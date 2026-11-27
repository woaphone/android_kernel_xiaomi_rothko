// SPDX-License-Identifier: GPL-2.0-only
/* FOD request/event subset of the Rothko MI display ABI. */
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/compat.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kfifo.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <drm/drm_device.h>
#include <drm/drm_vblank.h>

#include "mtk_drm_drv.h"
#include "mtk_dsi.h"
#include "rothko_fod.h"

struct fod_request {
	u32 flags;
	u32 display;
	u32 value;
};

struct fod_event {
	u32 display;
	u32 type;
	u32 length;
	u32 value;
};

#define FOD_GET_VERSION	_IOR('D', 0, struct fod_request)
#define FOD_SET_DOZE	_IOW('D', 2, struct fod_request)
#define FOD_GET_DOZE	_IOR('D', 3, struct fod_request)
#define FOD_SUBSCRIBE	_IOW('D', 7, struct fod_request)
#define FOD_UNSUBSCRIBE	_IOW('D', 8, struct fod_request)
#define FOD_SET_LHBM	_IOW('D', 14, struct fod_request)
#define FOD_ABI_VERSION	0x100
#define FOD_EVENT_READY	2
#define FOD_EVENT_DBV	5

struct fod_client {
	struct list_head node;
	DECLARE_KFIFO(events, struct fod_event, 32);
	u32 subscriptions;
	bool disconnected;
};

struct fod_endpoint {
	struct device dev;
	struct cdev cdev;
};

static DEFINE_MUTEX(fod_lock);
static DEFINE_MUTEX(fod_event_lock);
static LIST_HEAD(fod_clients);
static DECLARE_WAIT_QUEUE_HEAD(fod_wait);
static struct fod_endpoint *fod_endpoint;
static struct drm_device *fod_drm;
static bool fod_lhbm_supported;
static struct fod_client *fod_pending_client;
static u32 fod_pending_request;
static unsigned long fod_send_deadline;
static void fod_apply_pending(struct work_struct *work);
static DECLARE_DELAYED_WORK(fod_pending_work, fod_apply_pending);

static void fod_notify(struct drm_device *drm, u32 type, u32 value)
{
	struct fod_event event = {
		.display = 0,
		.type = type,
		.length = sizeof(event),
		.value = value,
	};
	struct fod_client *client;

	mutex_lock(&fod_event_lock);
	if (drm != READ_ONCE(fod_drm)) {
		mutex_unlock(&fod_event_lock);
		return;
	}
	list_for_each_entry(client, &fod_clients, node) {
		if (client->disconnected || !(client->subscriptions & BIT(type)))
			continue;
		if (kfifo_is_full(&client->events))
			kfifo_skip(&client->events);
		kfifo_put(&client->events, event);
	}
	mutex_unlock(&fod_event_lock);
	wake_up_interruptible_poll(&fod_wait, EPOLLIN | EPOLLRDNORM | EPOLLPRI);
}

void rothko_fod_backlight_done(struct drm_crtc *crtc, unsigned int dbv)
{
	fod_notify(crtc->dev, FOD_EVENT_DBV, dbv);
}

static int fod_doze_brightness(struct drm_device *drm, bool set, u32 *level)
{
	struct mtk_drm_private *priv = drm->dev_private;
	struct drm_crtc *crtc = priv->crtc[0];
	struct mtk_drm_crtc *mtk_crtc;
	struct mtk_ddp_comp *output;
	struct mtk_panel_ext *ext;
	struct mtk_dsi *dsi;
	int ret = -ENODEV;

	if (!crtc || (set && *level > 2))
		return crtc ? -EINVAL : -ENODEV;
	mtk_crtc = to_mtk_crtc(crtc);
	/* Match the lock order of display atomic commits. */
	mutex_lock(&priv->commit.lock);
	mutex_lock(&mtk_crtc->lock);
	if (atomic_read(&priv->kernel_pm.status) != KERNEL_PM_RESUME) {
		ret = -EAGAIN;
		goto out;
	}
	/* The primary DSI remains bound when the CRTC is in Doze Suspend. */
	output = priv->ddp_comp[DDP_COMPONENT_DSI0];
	if (!output)
		goto out;
	dsi = container_of(output, struct mtk_dsi, ddp_comp);
	if (!dsi->panel)
		goto out;
	ext = find_panel_ext(dsi->panel);
	ret = -EOPNOTSUPP;
	if (!ext || !ext->funcs)
		goto out;
	if (set && ext->funcs->set_doze_brightness)
		ret = ext->funcs->set_doze_brightness(dsi->panel, *level);
	if (!set && ext->funcs->get_doze_brightness)
		ret = ext->funcs->get_doze_brightness(dsi->panel, level);
out:
	mutex_unlock(&mtk_crtc->lock);
	mutex_unlock(&priv->commit.lock);
	return ret;
}

static int fod_apply(struct drm_device *drm, u32 request)
{
	struct mtk_drm_private *priv = drm->dev_private;
	struct drm_crtc *crtc;
	struct mtk_drm_crtc *mtk_crtc;
	struct mtk_ddp_comp *output;
	struct mtk_dsi *dsi;
	struct mtk_panel_ext *ext;
	unsigned int mode;
	bool on, doze;
	u64 frame;
	int ret;

	switch (request) {
	case 0:
	case 1:
		mode = 0;
		break;
	case 2:
		mode = 1;
		break;
	case 3:
		mode = 4;
		break;
	default:
		return -EOPNOTSUPP;
	}
	on = mode != 0;
	crtc = list_first_entry_or_null(&drm->mode_config.crtc_list,
					struct drm_crtc, head);
	if (!crtc)
		return -ENODEV;
	mtk_crtc = to_mtk_crtc(crtc);
	mutex_lock(&mtk_crtc->lock);
	ret = -ENODEV;
	output = priv->ddp_comp[DDP_COMPONENT_DSI0];
	if (!output)
		goto unlock;
	dsi = container_of(output, struct mtk_dsi, ddp_comp);
	ret = -EAGAIN;
	if (!dsi->output_en || !mtk_crtc->enabled ||
	    mtk_crtc->ddp_mode == DDP_NO_USE ||
	    atomic_read(&priv->kernel_pm.status) != KERNEL_PM_RESUME)
		goto unlock;
	ret = -ENODEV;
	if (mtk_ddp_comp_request_output(mtk_crtc) != output)
		goto unlock;
	ret = -EOPNOTSUPP;
	if (!dsi->panel)
		goto unlock;
	ext = find_panel_ext(dsi->panel);
	if (!ext || !ext->funcs || !ext->funcs->set_local_hbm)
		goto unlock;

	doze = dsi->doze_enabled;
	if (doze) {
		mode = on ? request + 4 : request + 10;
		if (!on)
			fod_notify(drm, FOD_EVENT_READY, 0);
		ret = ext->funcs->set_local_hbm(dsi->panel, mode);
		if (!ret && on) {
			/* Stock 0A waits seven 120 Hz periods after the LP batch. */
			usleep_range(58331, 58341);
			fod_notify(drm, FOD_EVENT_READY, request == 3 ? 12 : 8);
		}
		goto unlock;
	}

	if (on) {
		ret = drm_crtc_vblank_get(crtc);
		if (ret)
			goto unlock;
	}
	ret = ext->funcs->set_local_hbm(dsi->panel, mode);
	if (!ret && on) {
		/* Do not notify until DDIC commands and a subsequent frame complete. */
		frame = drm_crtc_vblank_count(crtc);
		if (!wait_event_timeout(*drm_crtc_vblank_waitqueue(crtc),
				drm_crtc_vblank_count(crtc) != frame,
				msecs_to_jiffies(50))) {
			ext->funcs->set_local_hbm(dsi->panel, 0);
			ret = -ETIMEDOUT;
		}
	}
	if (on)
		drm_crtc_vblank_put(crtc);
	if (!ret)
		fod_notify(drm, FOD_EVENT_READY, on ? (request == 3 ? 12 : 8) : 0);
unlock:
	mutex_unlock(&mtk_crtc->lock);
	return ret;
}

static void fod_apply_pending(struct work_struct *work)
{
	int ret;

	mutex_lock(&fod_lock);
	if (!fod_pending_client || !fod_drm)
		goto out;
	if (time_after_eq(jiffies, fod_send_deadline)) {
		fod_pending_client = NULL;
		goto out;
	}
	ret = fod_apply(fod_drm, fod_pending_request);
	if (ret == -EAGAIN)
		mod_delayed_work(system_wq, &fod_pending_work, msecs_to_jiffies(20));
	else
		fod_pending_client = NULL;
out:
	mutex_unlock(&fod_lock);
}

static int fod_submit(struct fod_client *client, const struct fod_request *req)
{
	int ret;

	lockdep_assert_held(&fod_lock);
	if (!fod_lhbm_supported || req->value > 3)
		return -EOPNOTSUPP;

	/* A newer request replaces the pending transition, including finger-up. */
	fod_pending_client = NULL;
	ret = fod_apply(fod_drm, req->value);
	if (ret != -EAGAIN || !(req->flags & BIT(0)))
		return ret;

	/* Like the stock worker, wait for the display stack to turn DSI on. */
	fod_pending_client = client;
	fod_pending_request = req->value;
	fod_send_deadline = jiffies + msecs_to_jiffies(500);
	mod_delayed_work(system_wq, &fod_pending_work, msecs_to_jiffies(20));
	return 0;
}

static int fod_open(struct inode *inode, struct file *file)
{
	struct fod_client *client;

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return -ENOMEM;
	INIT_KFIFO(client->events);
	mutex_lock(&fod_lock);
	if (!fod_endpoint || &fod_endpoint->cdev != inode->i_cdev) {
		mutex_unlock(&fod_lock);
		kfree(client);
		return -ENODEV;
	}
	mutex_lock(&fod_event_lock);
	list_add_tail(&client->node, &fod_clients);
	mutex_unlock(&fod_event_lock);
	mutex_unlock(&fod_lock);
	file->private_data = client;
	return nonseekable_open(inode, file);
}

static int fod_release(struct inode *inode, struct file *file)
{
	struct fod_client *client = file->private_data;

	mutex_lock(&fod_lock);
	if (fod_pending_client == client)
		fod_pending_client = NULL;
	mutex_lock(&fod_event_lock);
	list_del(&client->node);
	mutex_unlock(&fod_event_lock);
	mutex_unlock(&fod_lock);
	kfree(client);
	return 0;
}

static bool fod_readable(struct fod_client *client)
{
	return READ_ONCE(client->disconnected) || !kfifo_is_empty(&client->events);
}

static __poll_t fod_poll(struct file *file, poll_table *wait)
{
	struct fod_client *client = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &fod_wait, wait);
	mutex_lock(&fod_event_lock);
	if (client->disconnected)
		mask = EPOLLHUP | EPOLLERR;
	else if (!kfifo_is_empty(&client->events))
		mask = EPOLLIN | EPOLLRDNORM | EPOLLPRI;
	mutex_unlock(&fod_event_lock);
	return mask;
}

static ssize_t fod_read(struct file *file, char __user *buf, size_t size, loff_t *off)
{
	struct fod_client *client = file->private_data;
	struct fod_event event;
	int ret;

	if (!size)
		return 0;
	if (size < sizeof(event))
		return -EINVAL;
	for (;;) {
		mutex_lock(&fod_event_lock);
		if (client->disconnected) {
			mutex_unlock(&fod_event_lock);
			return -ENODEV;
		}
		ret = kfifo_get(&client->events, &event);
		mutex_unlock(&fod_event_lock);
		if (ret)
			return copy_to_user(buf, &event, sizeof(event)) ? -EFAULT : sizeof(event);
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(fod_wait, fod_readable(client));
		if (ret)
			return ret;
	}
}

static long fod_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct fod_client *client = file->private_data;
	struct fod_request req = {};
	void __user *user = (void __user *)arg;
	int ret;

	switch (cmd) {
	case FOD_GET_VERSION:
		req.value = FOD_ABI_VERSION;
		return copy_to_user(user, &req, sizeof(req)) ? -EFAULT : 0;
	case FOD_GET_DOZE:
		/* OEM request selects a display even though the ioctl is _IOR. */
		if (get_user(req.display, &((struct fod_request __user *)user)->display))
			return -EFAULT;
		break;
	case FOD_SET_DOZE:
	case FOD_SUBSCRIBE:
	case FOD_UNSUBSCRIBE:
	case FOD_SET_LHBM:
		if (copy_from_user(&req, user, sizeof(req)))
			return -EFAULT;
		break;
	default:
		return -ENOTTY;
	}
	if (cmd == FOD_SET_LHBM || cmd == FOD_SET_DOZE || cmd == FOD_GET_DOZE) {
		if (cmd != FOD_GET_DOZE && !(file->f_mode & FMODE_WRITE))
			return -EBADF;
		if (req.display)
			return -EOPNOTSUPP;
		mutex_lock(&fod_lock);
		if (!fod_drm || client->disconnected)
			ret = -ENODEV;
		else if (cmd == FOD_SET_LHBM)
			ret = fod_submit(client, &req);
		else
			ret = fod_doze_brightness(fod_drm, cmd == FOD_SET_DOZE, &req.value);
		mutex_unlock(&fod_lock);
		if (!ret && cmd == FOD_GET_DOZE && copy_to_user(user, &req, sizeof(req)))
			return -EFAULT;
		return ret;
	}

	/* mfp-daemon subscribes for both displays and to DBV as well as ready. */
	if (req.display > 1 || (req.value != FOD_EVENT_READY && req.value != FOD_EVENT_DBV))
		return -EOPNOTSUPP;
	mutex_lock(&fod_event_lock);
	ret = client->disconnected ? -ENODEV : 0;
	if (!ret && !req.display) {
		if (cmd == FOD_SUBSCRIBE)
			client->subscriptions |= BIT(req.value);
		else
			client->subscriptions &= ~BIT(req.value);
	}
	mutex_unlock(&fod_event_lock);
	return ret;
}

static const struct file_operations fod_fops = {
	.owner = THIS_MODULE,
	.open = fod_open,
	.release = fod_release,
	.read = fod_read,
	.poll = fod_poll,
	.unlocked_ioctl = fod_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = compat_ptr_ioctl,
#endif
};

static void fod_disconnect(void)
{
	struct fod_client *client;

	lockdep_assert_held(&fod_lock);
	fod_pending_client = NULL;
	cancel_delayed_work(&fod_pending_work);
	WRITE_ONCE(fod_drm, NULL);
	mutex_lock(&fod_event_lock);
	list_for_each_entry(client, &fod_clients, node)
		WRITE_ONCE(client->disconnected, true);
	mutex_unlock(&fod_event_lock);
	wake_up_interruptible_poll(&fod_wait, EPOLLHUP | EPOLLERR);
}

static char *fod_devnode(const struct device *dev, umode_t *mode)
{
	return kasprintf(GFP_KERNEL, "mi_display/%s", dev_name(dev));
}

static void fod_device_release(struct device *dev)
{
	kfree(container_of(dev, struct fod_endpoint, dev));
}

struct device *rothko_fod_device_create(void)
{
	struct fod_endpoint *endpoint;
	struct class *class;
	dev_t devt;
	int ret;

	class = class_create("mi_display");
	if (IS_ERR(class))
		return ERR_CAST(class);
	class->devnode = fod_devnode;
	ret = alloc_chrdev_region(&devt, 0, 1, "disp_feature");
	if (ret)
		goto destroy_class;
	endpoint = kzalloc(sizeof(*endpoint), GFP_KERNEL);
	if (!endpoint) {
		ret = -ENOMEM;
		goto unregister_devt;
	}
	device_initialize(&endpoint->dev);
	endpoint->dev.class = class;
	endpoint->dev.devt = devt;
	endpoint->dev.release = fod_device_release;
	ret = dev_set_name(&endpoint->dev, "disp_feature");
	if (ret)
		goto put_endpoint;
	cdev_init(&endpoint->cdev, &fod_fops);
	endpoint->cdev.owner = THIS_MODULE;
	ret = cdev_device_add(&endpoint->cdev, &endpoint->dev);
	if (ret)
		goto put_endpoint;

	mutex_lock(&fod_lock);
	fod_endpoint = endpoint;
	mutex_unlock(&fod_lock);
	return &endpoint->dev;

put_endpoint:
	put_device(&endpoint->dev);
unregister_devt:
	unregister_chrdev_region(devt, 1);
destroy_class:
	class_destroy(class);
	return ERR_PTR(ret);
}

void rothko_fod_device_destroy(struct device *dev)
{
	struct fod_endpoint *endpoint = container_of(dev, struct fod_endpoint, dev);
	const struct class *class = dev->class;
	dev_t devt = dev->devt;

	mutex_lock(&fod_lock);
	fod_disconnect();
	fod_endpoint = NULL;
	mutex_unlock(&fod_lock);
	cdev_device_del(&endpoint->cdev, dev);
	unregister_chrdev_region(devt, 1);
	class_destroy(class);
	put_device(dev);
}

int rothko_fod_register(struct drm_device *drm)
{
	struct mtk_drm_private *priv = drm->dev_private;
	struct mtk_ddp_comp *output = priv->ddp_comp[DDP_COMPONENT_DSI0];
	struct device_node *node;
	struct mtk_dsi *dsi;
	bool lhbm;
	int ret = 0;

	if (!IS_ENABLED(CONFIG_DRM_ROTHKO_N12) || !output)
		return 0;
	dsi = container_of(output, struct mtk_dsi, ddp_comp);
	if (!dsi->panel)
		return 0;
	node = dsi->panel->dev->of_node;
	lhbm = of_device_is_compatible(node, "n12_42_02_0a_dsc_cmd,lcm");
	if (!lhbm && !of_device_is_compatible(node, "n12_36_02_0b_dsc_cmd,lcm"))
		return 0;

	mutex_lock(&fod_lock);
	if (!fod_endpoint) {
		ret = -ENODEV;
	} else if (!fod_drm) {
		fod_lhbm_supported = lhbm;
		WRITE_ONCE(fod_drm, drm);
	}
	mutex_unlock(&fod_lock);
	return ret;
}

void rothko_fod_unregister(struct drm_device *drm)
{
	mutex_lock(&fod_lock);
	if (drm && fod_drm == drm)
		fod_disconnect();
	mutex_unlock(&fod_lock);
}
