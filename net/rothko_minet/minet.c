// SPDX-License-Identifier: GPL-2.0-only
#include <linux/hashtable.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/jhash.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/netfilter.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/uidgid.h>
#include <linux/workqueue.h>
#include <net/genetlink.h>
#include <net/inet_sock.h>
#include <net/ip.h>
#include <net/netfilter/nf_conntrack_extend.h>

#define MINET_TYPE_APP		1
#define MINET_REPORT_DELAY	1250 /* jiffies */
/* Bound packet-driven memory across all UIDs while waiting for a report. */
#define MINET_MAX_PENDING	8192

#define MINET_CMD_SET		1
#define MINET_CMD_EVENT		2
#define MINET_ATTR_DATA		1

enum {
	MINET_OP_ADD = 1,
	MINET_OP_REMOVE,
	MINET_OP_CLEAR,
};

/* The daemon ABI uses native scalars and network-order address bytes. */
struct minet_request {
	u16 type;
	u16 op;
	u32 count;
	u32 uid[];
};

struct minet_record {
	u8 protocol;
	u8 reserved;
	u16 family;
	u16 remote_port;
	u16 local_port;
	union nf_inet_addr remote;
	union nf_inet_addr local;
	u32 rx_packets;
	u32 rx_bytes;
	u32 tx_packets;
	u32 tx_bytes;
};

struct minet_event {
	u16 type;
	u16 reserved;
	u32 uid;
	u16 count;
	u16 reserved2;
	struct minet_record records[];
};

static_assert(sizeof(struct minet_request) == 8);
static_assert(offsetof(struct minet_request, type) == 0);
static_assert(offsetof(struct minet_request, op) == 2);
static_assert(offsetof(struct minet_request, count) == 4);
static_assert(offsetof(struct minet_request, uid) == 8);
static_assert(sizeof(struct minet_event) == 12);
static_assert(offsetof(struct minet_event, type) == 0);
static_assert(offsetof(struct minet_event, uid) == 4);
static_assert(offsetof(struct minet_event, count) == 8);
static_assert(offsetof(struct minet_event, records) == 12);
static_assert(sizeof(struct minet_record) == 56);
static_assert(offsetof(struct minet_record, protocol) == 0);
static_assert(offsetof(struct minet_record, family) == 2);
static_assert(offsetof(struct minet_record, remote_port) == 4);
static_assert(offsetof(struct minet_record, local_port) == 6);
static_assert(offsetof(struct minet_record, remote) == 8);
static_assert(offsetof(struct minet_record, local) == 24);
static_assert(offsetof(struct minet_record, rx_packets) == 40);
static_assert(offsetof(struct minet_record, rx_bytes) == 44);
static_assert(offsetof(struct minet_record, tx_packets) == 48);
static_assert(offsetof(struct minet_record, tx_bytes) == 52);

/* Each single-UID attribute, including its header, must fit in u16. */
#define MINET_MAX_RECORDS	((U16_MAX - NLA_HDRLEN - sizeof(struct minet_event)) / \
				 sizeof(struct minet_record))

struct minet_flow {
	struct hlist_node node;
	struct list_head list;
	struct minet_record record;
};

struct minet_app {
	struct hlist_node node;
	struct list_head list;
	u32 uid;
	unsigned int count;
	DECLARE_HASHTABLE(flows, 6);
	struct list_head pending;
};

/* The control mutex serializes hook transitions. The report mutex keeps an
 * in-flight report ordered against UID removal. Packet hooks only take the
 * spinlock, which protects the UID index and pending counters.
 */
static DEFINE_MUTEX(minet_control_lock);
static DEFINE_MUTEX(minet_report_lock);
static DEFINE_SPINLOCK(minet_data_lock);
static DEFINE_HASHTABLE(minet_apps, 6);
static unsigned int minet_pending;
static bool minet_stopping;
static struct genl_family minet_family __ro_after_init;

static struct minet_app *minet_find_app(u32 uid)
{
	struct minet_app *app;

	hash_for_each_possible(minet_apps, app, node, uid)
		if (app->uid == uid)
			return app;
	return NULL;
}

static void minet_account(u32 uid, const struct minet_record *record,
			  bool tx, unsigned int len)
{
	struct minet_flow *flow;
	struct minet_app *app;
	u32 key;

	spin_lock_bh(&minet_data_lock);
	app = minet_find_app(uid);
	if (!app)
		goto out;

	/* The OEM groups by remote endpoint and retains the first local one. */
	key = jhash(&record->remote, sizeof(record->remote),
		    (record->family << 24) | (record->protocol << 16) |
		    record->remote_port);
	hash_for_each_possible(app->flows, flow, node, key) {
		if (flow->record.protocol == record->protocol &&
		    flow->record.family == record->family &&
		    flow->record.remote_port == record->remote_port &&
		    !memcmp(&flow->record.remote, &record->remote,
			    sizeof(record->remote)))
			goto account;
	}

	if (minet_pending >= MINET_MAX_PENDING)
		goto out;
	flow = kmalloc(sizeof(*flow), GFP_ATOMIC);
	if (!flow)
		goto out;

	flow->record = *record;
	hash_add(app->flows, &flow->node, key);
	list_add_tail(&flow->list, &app->pending);
	app->count++;
	minet_pending++;

account:
	if (tx) {
		flow->record.tx_packets++;
		flow->record.tx_bytes += len;
	} else {
		flow->record.rx_packets++;
		flow->record.rx_bytes += len;
	}
out:
	spin_unlock_bh(&minet_data_lock);
}

static u32 minet_skb_uid(const struct sk_buff *skb, bool tx)
{
	enum ip_conntrack_info ctinfo;
	struct nf_conn_minet *ext;
	struct sock *sk;
	struct nf_conn *ct;
	u32 uid = U32_MAX;

	sk = skb_to_full_sk(skb);
	if (sk && (sk->sk_type == SOCK_STREAM || sk->sk_type == SOCK_DGRAM))
		uid = from_kuid(&init_user_ns, sk_uid(sk));

	ct = nf_ct_get(skb, &ctinfo);
	if (!ct)
		return uid;

	if (uid != U32_MAX) {
		if (tx && ctinfo == IP_CT_NEW) {
			ext = nf_ct_ext_find(ct, NF_CT_EXT_MINET);
			if (ext)
				WRITE_ONCE(ext->uid_plus_one, uid + 1);
		}
		return uid;
	}

	if (ctinfo != IP_CT_ESTABLISHED && ctinfo != IP_CT_ESTABLISHED_REPLY)
		return U32_MAX;
	ext = nf_ct_ext_find(ct, NF_CT_EXT_MINET);
	return ext ? READ_ONCE(ext->uid_plus_one) - 1 : U32_MAX;
}

static bool minet_ipv4_local(__be32 addr)
{
	return ipv4_is_private_10(addr) || ipv4_is_private_172(addr) ||
	       ipv4_is_private_192(addr) || ipv4_is_linklocal_169(addr) ||
	       ipv4_is_loopback(addr) || ipv4_is_multicast(addr) ||
	       ipv4_is_lbcast(addr);
}

static unsigned int minet_hook(void *priv, struct sk_buff *skb,
			       const struct nf_hook_state *state)
{
	bool tx = state->hook == NF_INET_LOCAL_OUT;
	struct minet_record record = {};
	int offset = skb_network_offset(skb);
	const __be16 *ports;
	__be16 port_buf[2];
	u32 uid;

	if (state->pf == NFPROTO_IPV4) {
		const struct iphdr *iph;
		struct iphdr buf;

		iph = skb_header_pointer(skb, offset, sizeof(buf), &buf);
		if (!iph || iph->ihl < 5 || (iph->frag_off & htons(IP_OFFSET)))
			return NF_ACCEPT;
		record.protocol = iph->protocol;
		record.family = AF_INET;
		record.remote.ip = tx ? iph->daddr : iph->saddr;
		record.local.ip = tx ? iph->saddr : iph->daddr;
		offset += iph->ihl * 4;
	} else {
		const struct ipv6hdr *ip6h;
		struct ipv6hdr buf;

		ip6h = skb_header_pointer(skb, offset, sizeof(buf), &buf);
		if (!ip6h)
			return NF_ACCEPT;
		/* Match the OEM check of the IPv6 base header next header. */
		record.protocol = ip6h->nexthdr;
		record.family = AF_INET6;
		record.remote.in6 = tx ? ip6h->daddr : ip6h->saddr;
		record.local.in6 = tx ? ip6h->saddr : ip6h->daddr;
		offset += sizeof(*ip6h);
	}

	if (record.protocol != IPPROTO_TCP && record.protocol != IPPROTO_UDP)
		return NF_ACCEPT;
	ports = skb_header_pointer(skb, offset, sizeof(port_buf), port_buf);
	if (!ports)
		return NF_ACCEPT;

	uid = minet_skb_uid(skb, tx);
	if (uid == U32_MAX)
		return NF_ACCEPT;
	if (record.family == AF_INET && minet_ipv4_local(record.remote.ip))
		return NF_ACCEPT;

	record.remote_port = ntohs(ports[tx ? 1 : 0]);
	record.local_port = ntohs(ports[tx ? 0 : 1]);
	minet_account(uid, &record, tx, skb->len);
	return NF_ACCEPT;
}

static const struct nf_hook_ops minet_hooks[] = {
	{ .hook = minet_hook, .pf = NFPROTO_IPV4, .hooknum = NF_INET_LOCAL_IN,
	  .priority = 1 },
	{ .hook = minet_hook, .pf = NFPROTO_IPV4, .hooknum = NF_INET_LOCAL_OUT,
	  .priority = -1 },
	{ .hook = minet_hook, .pf = NFPROTO_IPV6, .hooknum = NF_INET_LOCAL_IN,
	  .priority = 1 },
	{ .hook = minet_hook, .pf = NFPROTO_IPV6, .hooknum = NF_INET_LOCAL_OUT,
	  .priority = -1 },
};

static void minet_free_flows(struct list_head *flows)
{
	struct minet_flow *flow, *next;

	list_for_each_entry_safe(flow, next, flows, list) {
		list_del(&flow->list);
		kfree(flow);
	}
}

static bool minet_notify_batch(const struct minet_app *app, struct list_head **pos,
			 unsigned int count)
{
	struct minet_event *event;
	struct minet_flow *flow;
	struct sk_buff *skb;
	struct nlattr *attr;
	unsigned int i;
	size_t len;
	void *hdr;

	len = struct_size(event, records, count);
	skb = genlmsg_new(nla_total_size(len), GFP_KERNEL);
	if (!skb)
		return false;
	hdr = genlmsg_put(skb, 0, 0, &minet_family, 0, MINET_CMD_EVENT);
	if (!hdr)
		goto free_skb;
	attr = nla_reserve(skb, MINET_ATTR_DATA, len);
	if (!attr)
		goto free_skb;

	event = nla_data(attr);
	event->type = MINET_TYPE_APP;
	event->reserved = 0;
	event->uid = app->uid;
	event->count = count;
	event->reserved2 = 0;
	for (i = 0; i < count; i++) {
		flow = list_entry(*pos, struct minet_flow, list);
		event->records[i] = flow->record;
		*pos = (*pos)->next;
	}

	genlmsg_end(skb, hdr);
	/* Multicast is best-effort, including when there are no listeners. */
	genlmsg_multicast(&minet_family, skb, 0, 0, GFP_KERNEL);
	return true;

free_skb:
	nlmsg_free(skb);
	return false;
}

static void minet_notify(const struct minet_app *app, struct list_head *flows,
			 unsigned int count)
{
	struct list_head *pos = flows->next;

	while (count) {
		unsigned int batch = min_t(unsigned int, count, MINET_MAX_RECORDS);

		if (!minet_notify_batch(app, &pos, batch))
			break;
		count -= batch;
	}
}

static void minet_report(struct work_struct *work);
static DECLARE_DELAYED_WORK(minet_report_work, minet_report);

static void minet_report(struct work_struct *work)
{
	unsigned int bucket, count;
	struct minet_app *app;
	LIST_HEAD(flows);

	mutex_lock(&minet_report_lock);
	hash_for_each(minet_apps, bucket, app, node) {
		spin_lock_bh(&minet_data_lock);
		count = app->count;
		if (count) {
			list_splice_init(&app->pending, &flows);
			hash_init(app->flows);
			app->count = 0;
			minet_pending -= count;
		}
		spin_unlock_bh(&minet_data_lock);

		if (count) {
			minet_notify(app, &flows, count);
			minet_free_flows(&flows);
		}
	}
	mutex_unlock(&minet_report_lock);
	if (READ_ONCE(nf_ct_minet_active))
		schedule_delayed_work(&minet_report_work, MINET_REPORT_DELAY);
}

static int minet_start(void)
{
	int err;

	err = nf_ct_netns_get(&init_net, NFPROTO_INET);
	if (err)
		return err;

	WRITE_ONCE(nf_ct_minet_active, true);
	err = nf_register_net_hooks(&init_net, minet_hooks, ARRAY_SIZE(minet_hooks));
	if (err) {
		WRITE_ONCE(nf_ct_minet_active, false);
		synchronize_net();
		nf_ct_netns_put(&init_net, NFPROTO_INET);
	}
	return err;
}

static void minet_stop(void)
{
	nf_unregister_net_hooks(&init_net, minet_hooks, ARRAY_SIZE(minet_hooks));
	WRITE_ONCE(nf_ct_minet_active, false);
	synchronize_net();
	cancel_delayed_work_sync(&minet_report_work);
	nf_ct_netns_put(&init_net, NFPROTO_INET);
}

static void minet_free_apps(struct list_head *apps)
{
	struct minet_app *app, *next;

	list_for_each_entry_safe(app, next, apps, list) {
		list_del(&app->list);
		minet_free_flows(&app->pending);
		kfree(app);
	}
}

static int minet_add(const struct minet_request *req)
{
	struct minet_app *app, *next;
	unsigned int i;
	int err = 0;
	bool start;
	LIST_HEAD(staged);

	/* Stage all allocations before changing the monitored UID set. */
	for (i = 0; i < req->count; i++) {
		if (req->uid[i] == U32_MAX) {
			err = -EINVAL;
			goto out;
		}
		if (minet_find_app(req->uid[i]))
			continue;
		app = kzalloc(sizeof(*app), GFP_KERNEL);
		if (!app) {
			err = -ENOMEM;
			goto out;
		}
		app->uid = req->uid[i];
		INIT_LIST_HEAD(&app->pending);
		list_add_tail(&app->list, &staged);
	}
	if (list_empty(&staged))
		return 0;

	start = hash_empty(minet_apps);
	if (start) {
		err = minet_start();
		if (err)
			goto out;
	}

	mutex_lock(&minet_report_lock);
	list_for_each_entry_safe(app, next, &staged, list) {
		spin_lock_bh(&minet_data_lock);
		if (!minet_find_app(app->uid)) {
			hash_add(minet_apps, &app->node, app->uid);
			list_del_init(&app->list);
		}
		spin_unlock_bh(&minet_data_lock);
	}
	mutex_unlock(&minet_report_lock);
	if (start)
		schedule_delayed_work(&minet_report_work, MINET_REPORT_DELAY);

out:
	minet_free_apps(&staged);
	return err;
}

static void minet_remove(const struct minet_request *req)
{
	struct minet_app *app;
	unsigned int i, bucket;
	LIST_HEAD(removed);

	/* The control mutex owns app->list. Keep all UIDs visible until a
	 * possible hook shutdown has completed.
	 */
	for (i = 0; i < req->count; i++) {
		app = minet_find_app(req->uid[i]);
		if (app && list_empty(&app->list))
			list_add(&app->list, &removed);
	}
	if (list_empty(&removed))
		return;

	hash_for_each(minet_apps, bucket, app, node) {
		if (list_empty(&app->list))
			goto detach;
	}
	minet_stop();

detach:
	mutex_lock(&minet_report_lock);
	list_for_each_entry(app, &removed, list) {
		spin_lock_bh(&minet_data_lock);
		hash_del(&app->node);
		minet_pending -= app->count;
		spin_unlock_bh(&minet_data_lock);
	}
	mutex_unlock(&minet_report_lock);
	minet_free_apps(&removed);
}

static void minet_clear(void)
{
	struct minet_app *app;
	struct hlist_node *next;
	unsigned int bucket;
	LIST_HEAD(removed);

	if (hash_empty(minet_apps))
		return;

	minet_stop();
	/* Packet and report readers are quiesced and commands are serialized. */
	hash_for_each_safe(minet_apps, bucket, next, app, node) {
		hash_del(&app->node);
		list_add(&app->list, &removed);
	}
	minet_pending = 0;
	minet_free_apps(&removed);
}

static int minet_set(struct sk_buff *skb, struct genl_info *info)
{
	const struct nlattr *attr = info->attrs[MINET_ATTR_DATA];
	const struct minet_request *req;
	int err = 0;

	if (!attr)
		return -EINVAL;
	req = nla_data(attr);
	if (req->type != MINET_TYPE_APP)
		return -EINVAL;
	if (nla_len(attr) != struct_size(req, uid, req->count))
		return -EMSGSIZE;

	mutex_lock(&minet_control_lock);
	if (minet_stopping) {
		err = -ESHUTDOWN;
		goto out;
	}

	switch (req->op) {
	case MINET_OP_ADD:
		err = minet_add(req);
		break;
	case MINET_OP_REMOVE:
		minet_remove(req);
		break;
	case MINET_OP_CLEAR:
		minet_clear();
		break;
	default:
		err = -EINVAL;
		break;
	}
out:
	mutex_unlock(&minet_control_lock);
	return err;
}

static const struct nla_policy minet_policy[MINET_ATTR_DATA + 1] = {
	[MINET_ATTR_DATA] = NLA_POLICY_MIN_LEN(sizeof(struct minet_request)),
};

static const struct genl_ops minet_ops[] = {
	{
		.cmd = MINET_CMD_SET,
		.flags = GENL_ADMIN_PERM,
		.doit = minet_set,
	},
};

static const struct genl_multicast_group minet_groups[] = {
	{ .name = "minet_event" },
};

static struct genl_family minet_family __ro_after_init = {
	.name = "minet_genl",
	.version = 1,
	.maxattr = MINET_ATTR_DATA,
	.policy = minet_policy,
	.module = THIS_MODULE,
	.ops = minet_ops,
	.n_ops = ARRAY_SIZE(minet_ops),
	.mcgrps = minet_groups,
	.n_mcgrps = ARRAY_SIZE(minet_groups),
};

static int __init minet_init(void)
{
	return genl_register_family(&minet_family);
}

static void __exit minet_exit(void)
{
	mutex_lock(&minet_control_lock);
	minet_stopping = true;
	minet_clear();
	mutex_unlock(&minet_control_lock);
	genl_unregister_family(&minet_family);
}

module_init(minet_init);
module_exit(minet_exit);
MODULE_DESCRIPTION("Rothko per-UID traffic accounting");
MODULE_LICENSE("GPL");
