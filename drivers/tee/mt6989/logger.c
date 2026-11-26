// SPDX-License-Identifier: GPL-2.0-only
/* Xiaomi MT6989 MiTEE shared-memory log ABI. */

#include <linux/arm_ffa.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/log2.h>
#include <linux/overflow.h>
#include <linux/panic_notifier.h>
#include <linux/poll.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "optee_private.h"

#define MITEE_FFA_GET_LOG_BUFFER	11
#define MITEE_LOG_DATA_OFFSET	0x3cb
#define MITEE_LOG_KEY_OFFSET	0x171
#define MITEE_LOG_KEY_SIZE	0x159
#define MITEE_LOG_LINE_SIZE	256

/* The producer reserves bytes with alloc and publishes them with put. */
struct mitee_log_ring {
	u32 alloc;
	u32 put;
	u32 size;
};

struct mitee_memlog {
	void *buffer;
	struct mitee_log_ring *ring[2];
	u8 *data[2];
	u32 read_pos[2];
	bool boot_done;
	atomic_t read_avail;
	atomic_t event;
	u32 poll_event;
	wait_queue_head_t wait;
	struct notifier_block call_notifier;
	struct notifier_block panic_notifier;
	struct proc_dir_entry *proc;
	struct proc_dir_entry *key_proc;
};

static void mitee_memlog_wake(struct mitee_memlog *log)
{
	atomic_inc(&log->event);
	wake_up_interruptible(&log->wait);
}

static int mitee_memlog_call_notify(struct notifier_block *nb,
				    unsigned long event, void *data)
{
	struct mitee_memlog *log = container_of(nb, struct mitee_memlog,
					       call_notifier);

	if (event != 1)
		return NOTIFY_DONE;
	mitee_memlog_wake(log);
	return NOTIFY_OK;
}

static int mitee_memlog_panic_notify(struct notifier_block *nb,
				     unsigned long event, void *data)
{
	struct mitee_memlog *log = container_of(nb, struct mitee_memlog,
					       panic_notifier);

	mitee_memlog_wake(log);
	return NOTIFY_OK;
}

static int mitee_memlog_open(struct inode *inode, struct file *file)
{
	struct mitee_memlog *log = pde_data(inode);
	int rc = nonseekable_open(inode, file);

	if (!rc) {
		file->private_data = log;
		WRITE_ONCE(log->poll_event, atomic_read(&log->event));
	}
	return rc;
}

static ssize_t mitee_memlog_read(struct file *file, char __user *buf,
				 size_t count, loff_t *pos)
{
	struct mitee_memlog *log = file->private_data;
	unsigned int index;
	struct mitee_log_ring *ring;
	u32 read;
	u32 put, alloc, size;
	size_t limit, copied = 0;
	char line[MITEE_LOG_LINE_SIZE];
	char *out;
	ssize_t rc;

	/* The stock proc ABI serializes readers and drains the saved ring first. */
	if (!atomic_xchg(&log->read_avail, 0))
		return 0;

	index = log->boot_done ? 0 : 1;
	ring = log->ring[index];
	read = log->read_pos[index];
	put = READ_ONCE(ring->put);
	rmb();
	alloc = READ_ONCE(ring->alloc);
	size = READ_ONCE(ring->size);
	if (alloc - read > size) {
		pr_notice("mitee: log overflow, lose some msg.\n");
		read = alloc - size;
	}
	if (put < read) {
		rc = -EFAULT;
		goto out_avail;
	}
	if (put == log->read_pos[index]) {
		if (index == 1)
			log->boot_done = true;
		rc = 0;
		goto out_avail;
	}
	limit = min_t(size_t, put - read, count);
	out = kmalloc(round_up(limit, MITEE_LOG_LINE_SIZE) +
		      MITEE_LOG_LINE_SIZE, GFP_KERNEL);
	if (!out) {
		rc = -ENOMEM;
		goto out_avail;
	}
	while (read != put) {
		size_t length = 0;
		u32 available = min_t(u32, put - read, sizeof(line) - 1);

		while (length < available) {
			char ch = READ_ONCE(log->data[index][(read + length) &
							     (size - 1)]);

			line[length++] = ch;
			if (ch == '\n')
				break;
		}
		line[length] = '\0';
		rmb();
		/* A read returns complete newline/255-byte chunks, as in mitee.ko. */
		if (copied + length > limit)
			break;
		memcpy(out + copied, line, length);
		copied += length;
		read += length;
	}
	if (copy_to_user(buf, out, copied)) {
		rc = -EFAULT;
	} else {
		WRITE_ONCE(log->read_pos[index], read);
		rc = copied;
	}
	kfree(out);
out_avail:
	WRITE_ONCE(log->poll_event, atomic_read(&log->event));
	atomic_set(&log->read_avail, 1);
	return rc;
}

static __poll_t mitee_memlog_poll(struct file *file, poll_table *wait)
{
	struct mitee_memlog *log = file->private_data;

	if (READ_ONCE(log->read_pos[0]) != READ_ONCE(log->ring[0]->put))
		return EPOLLIN | EPOLLRDNORM;
	poll_wait(file, &log->wait, wait);
	if (READ_ONCE(log->poll_event) != atomic_read(&log->event))
		return EPOLLIN | EPOLLRDNORM;
	return 0;
}

static int mitee_memlog_key_open(struct inode *inode, struct file *file)
{
	file->private_data = pde_data(inode);
	return nonseekable_open(inode, file);
}

static ssize_t mitee_memlog_key_read(struct file *file, char __user *buf,
				     size_t count, loff_t *pos)
{
	struct mitee_memlog *log = file->private_data;
	u8 key[MITEE_LOG_KEY_SIZE];

	if (count < sizeof(key))
		return -EINVAL;
	memcpy(key, log->buffer + MITEE_LOG_KEY_OFFSET, sizeof(key));
	if (copy_to_user(buf, key, sizeof(key)))
		return -EFAULT;
	return sizeof(key);
}

static __poll_t mitee_memlog_key_poll(struct file *file, poll_table *wait)
{
	return 0;
}

static const struct proc_ops mitee_memlog_ops = {
	.proc_open = mitee_memlog_open,
	.proc_read = mitee_memlog_read,
	.proc_poll = mitee_memlog_poll,
};

static const struct proc_ops mitee_memlog_key_ops = {
	.proc_open = mitee_memlog_key_open,
	.proc_read = mitee_memlog_key_read,
	.proc_poll = mitee_memlog_key_poll,
};

int mitee_memlog_probe(struct optee *optee)
{
	struct ffa_device *dev = optee->ffa.ffa_dev;
	struct ffa_send_direct_data msg = { .data0 = MITEE_FFA_GET_LOG_BUFFER };
	struct mitee_memlog *log;
	phys_addr_t base, end;
	u64 raw_end;
	u32 size, boot_size;
	int rc;

	log = kzalloc(sizeof(*log), GFP_KERNEL);
	if (!log)
		return -ENOMEM;
	rc = dev->ops->msg_ops->sync_send_receive(dev, &msg);
	if (rc) {
		pr_err("miteelog service not available %d\n", rc);
		rc = -EIO;
		goto err_free;
	}
	if (check_add_overflow((u64)msg.data0, (u64)msg.data1, &raw_end)) {
		rc = -EINVAL;
		goto err_free;
	}
	base = ALIGN(msg.data0, PAGE_SIZE);
	end = round_down(raw_end, PAGE_SIZE);
	if (end <= base || end - base < MITEE_LOG_DATA_OFFSET) {
		rc = -EINVAL;
		goto err_free;
	}
	log->buffer = memremap(base, end - base, MEMREMAP_WB);
	if (!log->buffer) {
		rc = -ENOMEM;
		goto err_free;
	}
	log->ring[0] = log->buffer;
	log->ring[1] = log->buffer + sizeof(struct mitee_log_ring);
	size = READ_ONCE(log->ring[0]->size);
	boot_size = READ_ONCE(log->ring[1]->size);
	if (!is_power_of_2(size) || (boot_size && !is_power_of_2(boot_size)) ||
	    (u64)size + boot_size > end - base - MITEE_LOG_DATA_OFFSET) {
		rc = -EINVAL;
		goto err_unmap;
	}
	log->data[0] = log->buffer + MITEE_LOG_DATA_OFFSET;
	log->data[1] = log->data[0] + size;
	init_waitqueue_head(&log->wait);
	atomic_set(&log->read_avail, 1);
	atomic_set(&log->event, 0);
	log->call_notifier.notifier_call = mitee_memlog_call_notify;
	log->panic_notifier.notifier_call = mitee_memlog_panic_notify;
	rc = atomic_notifier_chain_register(&optee->call_notifier,
					    &log->call_notifier);
	if (rc)
		goto err_unmap;
	rc = atomic_notifier_chain_register(&panic_notifier_list,
					    &log->panic_notifier);
	if (rc)
		goto err_call_notifier;
	log->proc = proc_create_data("mitee_log", 0444, NULL,
				     &mitee_memlog_ops, log);
	if (!log->proc) {
		rc = -ENOMEM;
		goto err_panic_notifier;
	}
	log->key_proc = proc_create_data("mitee_log_key", 0444, NULL,
					 &mitee_memlog_key_ops, log);
	if (!log->key_proc) {
		rc = -ENOMEM;
		goto err_proc;
	}
	optee->memlog = log;
	pr_info("miteelog paddr=%pa, size=%llu\n", &base, (u64)(end - base));
	return 0;

err_proc:
	proc_remove(log->proc);
err_panic_notifier:
	atomic_notifier_chain_unregister(&panic_notifier_list,
					      &log->panic_notifier);
err_call_notifier:
	atomic_notifier_chain_unregister(&optee->call_notifier,
					      &log->call_notifier);
err_unmap:
	memunmap(log->buffer);
err_free:
	kfree(log);
	return rc;
}

void mitee_memlog_remove(struct optee *optee)
{
	struct mitee_memlog *log = optee->memlog;

	if (!log)
		return;
	proc_remove(log->proc);
	proc_remove(log->key_proc);
	atomic_notifier_chain_unregister(&panic_notifier_list,
					      &log->panic_notifier);
	atomic_notifier_chain_unregister(&optee->call_notifier,
					      &log->call_notifier);
	memunmap(log->buffer);
	kfree(log);
	optee->memlog = NULL;
}
