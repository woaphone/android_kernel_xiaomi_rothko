// SPDX-License-Identifier: GPL-2.0-only
/* Rothko MiWiLL Ethernet link and vendor userspace control ABI. */
#include <linux/bitmap.h>
#include <linux/etherdevice.h>
#include <linux/if_arp.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <net/dst.h>
#include <net/genetlink.h>
#include <net/net_namespace.h>
#include <net/rtnetlink.h>

#define MIWILL_ETHERTYPE		0xeee1
#define MIWILL_REDUNDANT	0x11
#define MIWILL_SINGLE		0x1e
#define MIWILL_MANAGEMENT	0x1f
#define MIWILL_MARK_MASK	0x0ce00000
#define MIWILL_MARK_SINGLE	0x08000000
#define MIWILL_PORTS		2
#define MIWILL_INIT_MS		2000
#define MIWILL_RETRY_MS		10000
#define MIWILL_HISTORY_MS	250
#define MIWILL_HISTORY_BANKS	4
#define MIWILL_SEQ_COUNT	(1U << 16)
#define MIWILL_ENABLE_UNCHANGED	U8_MAX

enum miwill_cmd {
	MIWILL_CMD_SET = 1,
	MIWILL_CMD_GET,
	MIWILL_CMD_EVENT,
	MIWILL_CMD_APP_FLOW,
};

enum miwill_attr {
	MIWILL_A_DEV = 1,
	MIWILL_A_ENABLED,
	MIWILL_A_MODE,
	MIWILL_A_GATEWAY,
	MIWILL_A_PORT_5G,
	MIWILL_A_PORT_24G,
	MIWILL_A_IPV4,
	MIWILL_A_RESET,
	MIWILL_A_APP_FLOW,
};

enum miwill_management {
	MIWILL_MGMT_HELLO = 1,
	MIWILL_MGMT_IPV4,
	MIWILL_MGMT_RESET,
};

struct miwill_hdr {
	u8 type;
	u8 subtype;
	__be16 seq;
	u8 id[2];
	__be16 proto;
};

static_assert(sizeof(struct miwill_hdr) == 8);

struct miwill {
	struct net_device *dev;
	struct net_device __rcu *port[MIWILL_PORTS];
	u8 gateway[ETH_ALEN] __aligned(2);
	bool redundancy;
	bool active;
	struct timer_list hello_timer;
	atomic_t tx_seq;
	atomic_t mgmt_seq;
	spinlock_t rx_lock;
	__be32 ipv4;
	unsigned int missed;
	unsigned int bank;
	unsigned long epoch;
	unsigned long (*history)[BITS_TO_LONGS(MIWILL_SEQ_COUNT)];
};

static struct genl_family miwill_family __ro_after_init;
static struct rtnl_link_ops miwill_link_ops;

static bool miwill_active(struct miwill *mw)
{
	/* Pairs with miwill_open() publishing the gateway and RX history. */
	return smp_load_acquire(&mw->active);
}

static void miwill_event(unsigned int attr, const void *value, size_t len)
{
	struct sk_buff *msg;
	void *hdr;

	msg = genlmsg_new(nla_total_size(len), GFP_ATOMIC);
	if (!msg)
		return;

	hdr = genlmsg_put(msg, 0, 0, &miwill_family, 0, MIWILL_CMD_EVENT);
	if (!hdr || nla_put(msg, attr, len, value)) {
		nlmsg_free(msg);
		return;
	}

	genlmsg_end(msg, hdr);
	genlmsg_multicast(&miwill_family, msg, 0, 0, GFP_ATOMIC);
}

static bool miwill_port_ready(const struct net_device *port)
{
	return port && netif_running(port) && netif_carrier_ok(port);
}

/* Keep four successive bitmaps of received 16-bit sequences. */
static bool miwill_duplicate(struct miwill *mw, u16 seq)
{
	const unsigned long interval = msecs_to_jiffies(MIWILL_HISTORY_MS);
	unsigned long now, elapsed;
	unsigned int i;
	bool duplicate = false;

	spin_lock_bh(&mw->rx_lock);
	now = jiffies;
	if (time_after_eq(now, mw->epoch + interval)) {
		elapsed = (now - mw->epoch) / interval;
		mw->epoch += elapsed * interval;
		for (i = 0; i < min_t(unsigned long, elapsed, MIWILL_HISTORY_BANKS); i++) {
			mw->bank = (mw->bank + 1) % MIWILL_HISTORY_BANKS;
			bitmap_zero(mw->history[mw->bank], MIWILL_SEQ_COUNT);
		}
	}
	for (i = 0; i < MIWILL_HISTORY_BANKS; i++) {
		if (test_bit(seq, mw->history[i])) {
			duplicate = true;
			break;
		}
	}
	if (!duplicate)
		__set_bit(seq, mw->history[mw->bank]);
	spin_unlock_bh(&mw->rx_lock);

	return duplicate;
}

static void miwill_rx_management(struct miwill *mw, struct sk_buff *skb,
				 u8 subtype)
{
	__be32 ip;
	u8 reason = 2;

	switch (subtype) {
	case MIWILL_MGMT_IPV4:
		if (skb_copy_bits(skb, sizeof(struct miwill_hdr), &ip, sizeof(ip)))
			return;
		spin_lock_bh(&mw->rx_lock);
		WRITE_ONCE(mw->ipv4, ip);
		mw->missed = 0;
		spin_unlock_bh(&mw->rx_lock);
		miwill_event(MIWILL_A_IPV4, &ip, sizeof(ip));
		break;
	case MIWILL_MGMT_RESET:
		if (skb->len >= sizeof(struct miwill_hdr) + sizeof(ip))
			miwill_event(MIWILL_A_RESET, &reason, sizeof(reason));
		break;
	}
}

static rx_handler_result_t miwill_rx(struct sk_buff **pskb)
{
	struct sk_buff *skb = *pskb;
	struct miwill *mw = rcu_dereference(skb->dev->rx_handler_data);
	struct miwill_hdr *hdr;
	struct ethhdr *eth;
	__be16 proto;

	if (!miwill_active(mw) || skb->protocol != htons(MIWILL_ETHERTYPE))
		return RX_HANDLER_PASS;
	if (skb->dev != rcu_dereference(mw->port[0]) &&
	    skb->dev != rcu_dereference(mw->port[1]))
		return RX_HANDLER_PASS;
	if (skb->pkt_type == PACKET_OTHERHOST ||
	    !ether_addr_equal(eth_hdr(skb)->h_source, mw->gateway))
		return RX_HANDLER_PASS;

	skb = skb_share_check(skb, GFP_ATOMIC);
	if (!skb)
		goto drop_stat;
	*pskb = skb;
	if (!pskb_may_pull(skb, sizeof(*hdr)))
		goto drop;

	hdr = (struct miwill_hdr *)skb->data;
	if (hdr->type == MIWILL_MANAGEMENT) {
		miwill_rx_management(mw, skb, hdr->subtype);
		consume_skb(skb);
		return RX_HANDLER_CONSUMED;
	}
	if (hdr->type != MIWILL_REDUNDANT && hdr->type != MIWILL_SINGLE)
		goto drop;

	proto = hdr->proto;
	if (!eth_proto_is_802_3(proto) || proto == htons(MIWILL_ETHERTYPE))
		goto drop;
	if (skb_cow(skb, 0))
		goto drop;
	hdr = (struct miwill_hdr *)skb->data;
	if (hdr->type == MIWILL_REDUNDANT && miwill_duplicate(mw, ntohs(hdr->seq))) {
		consume_skb(skb);
		return RX_HANDLER_CONSUMED;
	}

	eth = (struct ethhdr *)(skb->data + sizeof(*hdr) - ETH_HLEN);
	memmove(eth, eth_hdr(skb), 2 * ETH_ALEN);
	eth->h_proto = proto;
	__skb_pull(skb, sizeof(*hdr));
	skb_set_mac_header(skb, -ETH_HLEN);
	skb_reset_mac_len(skb);
	skb_reset_network_header(skb);
	skb_reset_transport_header(skb);
	skb->ip_summed = CHECKSUM_NONE;
	skb->protocol = proto;
	skb->dev = mw->dev;
	dev_sw_netstats_rx_add(mw->dev, skb->len);
	return RX_HANDLER_ANOTHER;

drop:
	kfree_skb(skb);
drop_stat:
	dev_core_stats_rx_dropped_inc(mw->dev);
	return RX_HANDLER_CONSUMED;
}

static void miwill_header(struct miwill *mw, struct net_device *port,
			  struct sk_buff *skb, u8 type, u8 subtype, u16 seq,
			  __be16 proto)
{
	struct ethhdr *eth = (struct ethhdr *)skb->data;
	struct miwill_hdr *hdr = (struct miwill_hdr *)(skb->data + ETH_HLEN);

	ether_addr_copy(eth->h_dest, mw->gateway);
	ether_addr_copy(eth->h_source, port->dev_addr);
	eth->h_proto = htons(MIWILL_ETHERTYPE);
	hdr->type = type;
	hdr->subtype = subtype;
	hdr->seq = htons(seq);
	memcpy(hdr->id, mw->dev->dev_addr + ETH_ALEN - sizeof(hdr->id),
	       sizeof(hdr->id));
	hdr->proto = proto;
	skb_reset_mac_header(skb);
	skb->mac_len = ETH_HLEN;
	skb->protocol = eth->h_proto;
	skb->dev = port;
	skb_set_queue_mapping(skb, 0);
}

static void miwill_tx_management(struct miwill *mw, u8 subtype)
{
	struct net_device *port[MIWILL_PORTS], *out;
	unsigned int headroom, len;
	u8 addresses[MIWILL_PORTS][ETH_ALEN];
	struct sk_buff *skb;
	const void *payload;
	__be32 ip;
	int i;

	rcu_read_lock();
	for (i = 0; i < MIWILL_PORTS; i++)
		port[i] = rcu_dereference(mw->port[i]);
	out = miwill_port_ready(port[0]) ? port[0] : port[1];
	if (!miwill_port_ready(out))
		goto unlock;

	if (subtype == MIWILL_MGMT_HELLO) {
		if (!port[0] || !port[1])
			goto unlock;
		for (i = 0; i < MIWILL_PORTS; i++)
			memcpy(addresses[i], port[i]->dev_addr, ETH_ALEN);
		payload = addresses;
		len = sizeof(addresses);
	} else {
		ip = READ_ONCE(mw->ipv4);
		payload = &ip;
		len = sizeof(ip);
	}

	headroom = LL_RESERVED_SPACE(out);
	skb = alloc_skb(headroom + ETH_HLEN + sizeof(struct miwill_hdr) + len,
			GFP_ATOMIC);
	if (!skb)
		goto unlock;
	skb_reserve(skb, headroom);
	skb_put(skb, ETH_HLEN + sizeof(struct miwill_hdr));
	skb_put_data(skb, payload, len);
	skb_set_network_header(skb, ETH_HLEN);
	miwill_header(mw, out, skb, MIWILL_MANAGEMENT, subtype,
		      atomic_inc_return(&mw->mgmt_seq) - 1, htons(MIWILL_ETHERTYPE));
	dev_queue_xmit(skb);
unlock:
	rcu_read_unlock();
}

static void miwill_hello_timer(struct timer_list *timer)
{
	struct miwill *mw = timer_container_of(mw, timer, hello_timer);
	u8 reason = 1;
	bool timeout;

	if (!miwill_active(mw))
		return;

	spin_lock_bh(&mw->rx_lock);
	timeout = ++mw->missed >= 4;
	spin_unlock_bh(&mw->rx_lock);
	if (timeout)
		miwill_event(MIWILL_A_RESET, &reason, sizeof(reason));
	miwill_tx_management(mw, MIWILL_MGMT_HELLO);
	mod_timer(&mw->hello_timer, jiffies + msecs_to_jiffies(MIWILL_RETRY_MS));
}

static bool miwill_tx_one(struct miwill *mw, struct sk_buff *skb,
			  struct net_device *port, u8 type, u16 seq)
{
	__be16 proto = eth_hdr(skb)->h_proto;
	int ret;

	if (skb_cow_head(skb, LL_RESERVED_SPACE(port) + sizeof(struct miwill_hdr))) {
		kfree_skb(skb);
		return false;
	}
	__skb_push(skb, sizeof(struct miwill_hdr));
	miwill_header(mw, port, skb, type, 0, seq, proto);
	skb_dst_drop(skb);
	ret = dev_queue_xmit(skb);
	return !net_xmit_eval(ret);
}

static netdev_tx_t miwill_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct miwill *mw = netdev_priv(dev);
	struct net_device *port[MIWILL_PORTS];
	unsigned int n = 0, len = skb->len;
	struct sk_buff *clone;
	bool sent = false;
	u16 seq;
	int i;

	rcu_read_lock();
	if (!miwill_active(mw) || !pskb_may_pull(skb, ETH_HLEN))
		goto drop;
	skb_reset_mac_header(skb);
	if (!eth_proto_is_802_3(eth_hdr(skb)->h_proto) ||
	    eth_hdr(skb)->h_proto == htons(MIWILL_ETHERTYPE))
		goto drop;

	for (i = 0; i < MIWILL_PORTS; i++) {
		struct net_device *lower = rcu_dereference(mw->port[i]);

		if (miwill_port_ready(lower) &&
		    len + sizeof(struct miwill_hdr) <= READ_ONCE(lower->mtu) + ETH_HLEN)
			port[n++] = lower;
	}

	if (!n)
		goto drop;
	/* Existing Android fwmarks select the stock single-port data path. */
	if ((skb->mark & MIWILL_MARK_MASK) == MIWILL_MARK_SINGLE) {
		sent = miwill_tx_one(mw, skb, port[0], MIWILL_SINGLE, 0);
	} else {
		seq = atomic_inc_return(&mw->tx_seq) - 1;
		if (n == MIWILL_PORTS) {
			clone = skb_clone(skb, GFP_ATOMIC);
			if (clone)
				sent = miwill_tx_one(mw, clone, port[1], MIWILL_REDUNDANT, seq);
		}
		sent |= miwill_tx_one(mw, skb, port[0], MIWILL_REDUNDANT, seq);
	}
	rcu_read_unlock();
	if (sent)
		dev_sw_netstats_tx_add(dev, 1, len);
	else
		dev_core_stats_tx_dropped_inc(dev);
	return NETDEV_TX_OK;

drop:
	rcu_read_unlock();
	dev_core_stats_tx_dropped_inc(dev);
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static void miwill_update_carrier(struct miwill *mw)
{
	if (READ_ONCE(mw->active) &&
	    (miwill_port_ready(rtnl_dereference(mw->port[0])) ||
	     miwill_port_ready(rtnl_dereference(mw->port[1]))))
		netif_carrier_on(mw->dev);
	else
		netif_carrier_off(mw->dev);
}

static void miwill_release_port(struct miwill *mw, struct net_device *port)
{
	netdev_rx_handler_unregister(port);
	netdev_upper_dev_unlink(port, mw->dev);
}

static void miwill_detach(struct miwill *mw, int i)
{
	struct net_device *port = rtnl_dereference(mw->port[i]);

	if (!port)
		return;
	RCU_INIT_POINTER(mw->port[i], NULL);
	miwill_release_port(mw, port);
}

static int miwill_attach(struct miwill *mw, struct net_device *port,
			 struct netlink_ext_ack *extack)
{
	int err;

	if (port->type != ARPHRD_ETHER || port->addr_len != ETH_ALEN ||
	    (port->flags & IFF_LOOPBACK) || port->rtnl_link_ops == &miwill_link_ops)
		return -EINVAL;

	err = netdev_master_upper_dev_link(port, mw->dev, NULL, NULL, extack);
	if (err)
		return err;
	err = netdev_rx_handler_register(port, miwill_rx, mw);
	if (err)
		netdev_upper_dev_unlink(port, mw->dev);
	return err;
}

static int miwill_open(struct net_device *dev)
{
	struct miwill *mw = netdev_priv(dev);
	unsigned int i;

	if (!mw->redundancy || !is_valid_ether_addr(mw->gateway))
		return -EINVAL;
	for (i = 0; i < MIWILL_PORTS; i++) {
		struct net_device *port = rtnl_dereference(mw->port[i]);

		if (!port)
			return -ENOLINK;
		if (dev->mtu + sizeof(struct miwill_hdr) > port->mtu)
			return -EMSGSIZE;
	}

	mw->history = kvcalloc(MIWILL_HISTORY_BANKS, sizeof(*mw->history), GFP_KERNEL);
	if (!mw->history)
		return -ENOMEM;
	mw->bank = 0;
	mw->epoch = jiffies;
	mw->missed = 0;
	mw->ipv4 = 0;
	atomic_set(&mw->tx_seq, 0);
	/* Publish the active configuration and RX history to packet processing. */
	smp_store_release(&mw->active, true);
	miwill_update_carrier(mw);
	mod_timer(&mw->hello_timer, jiffies + msecs_to_jiffies(MIWILL_INIT_MS));
	return 0;
}

static int miwill_stop(struct net_device *dev)
{
	struct miwill *mw = netdev_priv(dev);
	int i;

	WRITE_ONCE(mw->active, false);
	timer_delete_sync(&mw->hello_timer);
	miwill_tx_management(mw, MIWILL_MGMT_RESET);
	miwill_update_carrier(mw);
	for (i = 0; i < MIWILL_PORTS; i++)
		miwill_detach(mw, i);
	kvfree(mw->history);
	mw->history = NULL;
	mw->redundancy = false;
	return 0;
}

static void miwill_uninit(struct net_device *dev)
{
	struct miwill *mw = netdev_priv(dev);
	int i;

	for (i = 0; i < MIWILL_PORTS; i++)
		miwill_detach(mw, i);
}

static const struct net_device_ops miwill_netdev_ops = {
	.ndo_open		= miwill_open,
	.ndo_stop		= miwill_stop,
	.ndo_uninit		= miwill_uninit,
	.ndo_start_xmit		= miwill_xmit,
	.ndo_set_mac_address	= eth_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
};

static void miwill_setup(struct net_device *dev)
{
	struct miwill *mw = netdev_priv(dev);

	ether_setup(dev);
	dev->netdev_ops = &miwill_netdev_ops;
	dev->needs_free_netdev = true;
	dev->netns_immutable = true;
	dev->priv_flags |= IFF_NO_QUEUE;
	dev->priv_flags &= ~(IFF_XMIT_DST_RELEASE | IFF_TX_SKB_SHARING);
	dev->pcpu_stat_type = NETDEV_PCPU_STAT_TSTATS;
	dev->mtu = ETH_DATA_LEN - sizeof(struct miwill_hdr);
	dev->max_mtu = ETH_MAX_MTU - sizeof(struct miwill_hdr);
	dev->needed_headroom = sizeof(struct miwill_hdr);
	mw->dev = dev;
	spin_lock_init(&mw->rx_lock);
	timer_setup(&mw->hello_timer, miwill_hello_timer, 0);
	atomic_set(&mw->mgmt_seq, 1);
	netif_carrier_off(dev);
}

static int miwill_newlink(struct net_device *dev,
			  struct rtnl_newlink_params *params,
			  struct netlink_ext_ack *extack)
{
	if (!net_eq(params->src_net, &init_net) ||
	    !net_eq(dev_net(dev), &init_net))
		return -EOPNOTSUPP;
	if (!params->tb[IFLA_ADDRESS])
		eth_hw_addr_random(dev);
	return register_netdevice(dev);
}

static int miwill_validate(struct nlattr *tb[], struct nlattr *data[],
			   struct netlink_ext_ack *extack)
{
	if (tb[IFLA_ADDRESS] &&
	    (nla_len(tb[IFLA_ADDRESS]) != ETH_ALEN ||
	     !is_valid_ether_addr(nla_data(tb[IFLA_ADDRESS]))))
		return -EADDRNOTAVAIL;
	return 0;
}

static struct rtnl_link_ops miwill_link_ops = {
	.kind		= "miwill",
	.priv_size	= sizeof(struct miwill),
	.setup		= miwill_setup,
	.newlink	= miwill_newlink,
	.validate	= miwill_validate,
};

static int miwill_dev_event(struct notifier_block *nb,
			    unsigned long event, void *ptr)
{
	struct net_device *lower = netdev_notifier_info_to_dev(ptr);
	struct miwill *mw;
	int i;

	if (event != NETDEV_UP && event != NETDEV_DOWN &&
	    event != NETDEV_CHANGE && event != NETDEV_UNREGISTER &&
	    event != NETDEV_PRE_TYPE_CHANGE)
		return NOTIFY_DONE;
	if (rtnl_dereference(lower->rx_handler) != miwill_rx)
		return NOTIFY_DONE;
	if (event == NETDEV_PRE_TYPE_CHANGE)
		return NOTIFY_BAD;

	mw = rtnl_dereference(lower->rx_handler_data);
	if (event == NETDEV_UNREGISTER) {
		for (i = 0; i < MIWILL_PORTS; i++)
			if (rtnl_dereference(mw->port[i]) == lower)
				miwill_detach(mw, i);
	}
	miwill_update_carrier(mw);
	return NOTIFY_DONE;
}

static struct notifier_block miwill_notifier = {
	.notifier_call = miwill_dev_event,
};

static const struct nla_policy miwill_policy[MIWILL_A_APP_FLOW + 1] = {
	[MIWILL_A_DEV]		= { .type = NLA_STRING, .len = IFNAMSIZ - 1 },
	[MIWILL_A_ENABLED]	= { .type = NLA_U8 },
	[MIWILL_A_MODE]		= { .type = NLA_STRING, .len = IFNAMSIZ - 1 },
	[MIWILL_A_GATEWAY]	= NLA_POLICY_ETH_ADDR,
	[MIWILL_A_PORT_5G]	= { .type = NLA_STRING, .len = IFNAMSIZ - 1 },
	[MIWILL_A_PORT_24G]	= { .type = NLA_STRING, .len = IFNAMSIZ - 1 },
	[MIWILL_A_IPV4]		= { .type = NLA_REJECT },
	[MIWILL_A_RESET]		= { .type = NLA_REJECT },
	[MIWILL_A_APP_FLOW]	= { .type = NLA_REJECT },
};

static int miwill_configure(struct miwill *mw, struct genl_info *info)
{
	struct net_device *old[MIWILL_PORTS], *new[MIWILL_PORTS];
	struct nlattr **attrs = info->attrs;
	bool old_mode = mw->redundancy;
	u8 old_gateway[ETH_ALEN];
	const u8 *gateway = NULL;
	u8 enabled = MIWILL_ENABLE_UNCHANGED;
	int err, i;

	if (attrs[MIWILL_A_ENABLED])
		enabled = nla_get_u8(attrs[MIWILL_A_ENABLED]);
	if (!enabled) {
		if (netif_running(mw->dev))
			dev_close(mw->dev);
		else {
			for (i = 0; i < MIWILL_PORTS; i++)
				miwill_detach(mw, i);
			mw->redundancy = false;
		}
		return 0;
	}

	if (attrs[MIWILL_A_MODE] &&
	    nla_strcmp(attrs[MIWILL_A_MODE], "redudancy"))
		return -EOPNOTSUPP;

	if (attrs[MIWILL_A_GATEWAY]) {
		gateway = nla_data(attrs[MIWILL_A_GATEWAY]);
		if (!is_valid_ether_addr(gateway))
			return -EADDRNOTAVAIL;
	}
	if (netif_running(mw->dev)) {
		if (gateway || attrs[MIWILL_A_MODE] ||
		    attrs[MIWILL_A_PORT_5G] || attrs[MIWILL_A_PORT_24G])
			return -EBUSY;
		return 0;
	}

	for (i = 0; i < MIWILL_PORTS; i++) {
		struct nlattr *attr = attrs[MIWILL_A_PORT_5G + i];

		old[i] = rtnl_dereference(mw->port[i]);
		new[i] = old[i];
		if (attr) {
			char name[IFNAMSIZ];

			if (nla_strscpy(name, attr, sizeof(name)) < 0)
				return -EINVAL;
			new[i] = __dev_get_by_name(&init_net, name);
			if (!new[i])
				return -ENODEV;
		}
	}
	if (new[0] && new[0] == new[1])
		return -EINVAL;

	for (i = 0; i < MIWILL_PORTS; i++) {
		if (!new[i] || new[i] == old[i])
			continue;
		err = miwill_attach(mw, new[i], info->extack);
		if (err)
			goto unwind_ports;
	}

	memcpy(old_gateway, mw->gateway, ETH_ALEN);
	if (gateway)
		memcpy(mw->gateway, gateway, ETH_ALEN);
	if (attrs[MIWILL_A_MODE])
		mw->redundancy = true;
	for (i = 0; i < MIWILL_PORTS; i++)
		rcu_assign_pointer(mw->port[i], new[i]);

	if (enabled != MIWILL_ENABLE_UNCHANGED) {
		err = dev_open(mw->dev, info->extack);
		if (err) {
			for (i = 0; i < MIWILL_PORTS; i++)
				rcu_assign_pointer(mw->port[i], old[i]);
			mw->redundancy = old_mode;
			memcpy(mw->gateway, old_gateway, ETH_ALEN);
			goto unwind_ports;
		}
	}

	for (i = 0; i < MIWILL_PORTS; i++)
		if (old[i] && old[i] != new[i])
			miwill_release_port(mw, old[i]);
	return 0;

unwind_ports:
	while (i--)
		if (new[i] && new[i] != old[i])
			miwill_release_port(mw, new[i]);
	return err;
}

static int miwill_cmd_set(struct sk_buff *skb, struct genl_info *info)
{
	struct net_device *dev;
	char name[IFNAMSIZ];
	int ret = -ENODEV;

	if (!net_eq(genl_info_net(info), &init_net))
		return -EOPNOTSUPP;
	if (!info->attrs[MIWILL_A_DEV] ||
	    nla_strscpy(name, info->attrs[MIWILL_A_DEV], sizeof(name)) < 0)
		return -EINVAL;

	rtnl_lock();
	dev = __dev_get_by_name(&init_net, name);
	if (dev && dev->rtnl_link_ops == &miwill_link_ops)
		ret = miwill_configure(netdev_priv(dev), info);
	rtnl_unlock();
	return ret;
}

static int miwill_cmd_unsupported(struct sk_buff *skb, struct genl_info *info)
{
	return -EOPNOTSUPP;
}

static const struct genl_ops miwill_genl_ops[] = {
	{
		.cmd = MIWILL_CMD_SET,
		.flags = GENL_ADMIN_PERM,
		.policy = miwill_policy,
		.maxattr = MIWILL_A_APP_FLOW,
		.doit = miwill_cmd_set,
	},
	{
		/* The stock GET implementation never returns configuration. */
		.cmd = MIWILL_CMD_GET,
		.flags = GENL_ADMIN_PERM,
		.validate = GENL_DONT_VALIDATE_STRICT,
		.doit = miwill_cmd_unsupported,
	},
	{
		/* App UID accounting requires the stock MiNet flow hooks. */
		.cmd = MIWILL_CMD_APP_FLOW,
		.flags = GENL_ADMIN_PERM,
		.validate = GENL_DONT_VALIDATE_STRICT,
		.doit = miwill_cmd_unsupported,
	},
};

static const struct genl_multicast_group miwill_groups[] = {
	{ .name = "miwill_event", .flags = GENL_MCAST_CAP_NET_ADMIN },
};

static struct genl_family miwill_family __ro_after_init = {
	.name		= "miwill_genl",
	.version	= 1,
	.maxattr	= MIWILL_A_APP_FLOW,
	.module		= THIS_MODULE,
	.ops		= miwill_genl_ops,
	.n_ops		= ARRAY_SIZE(miwill_genl_ops),
	.mcgrps		= miwill_groups,
	.n_mcgrps	= ARRAY_SIZE(miwill_groups),
	.resv_start_op	= MIWILL_CMD_APP_FLOW + 1,
};

static int __init miwill_init(void)
{
	struct net_device *dev;
	int err;

	err = genl_register_family(&miwill_family);
	if (err)
		return err;
	err = register_netdevice_notifier(&miwill_notifier);
	if (err)
		goto unregister_family;
	err = rtnl_link_register(&miwill_link_ops);
	if (err)
		goto unregister_notifier;

	dev = alloc_netdev(sizeof(struct miwill), "miw_oem%d",
			   NET_NAME_ENUM, miwill_setup);
	if (!dev) {
		err = -ENOMEM;
		goto unregister_link;
	}
	dev->rtnl_link_ops = &miwill_link_ops;
	eth_hw_addr_random(dev);
	err = register_netdev(dev);
	if (!err)
		return 0;
	free_netdev(dev);

unregister_link:
	rtnl_link_unregister(&miwill_link_ops);
unregister_notifier:
	unregister_netdevice_notifier(&miwill_notifier);
unregister_family:
	genl_unregister_family(&miwill_family);
	return err;
}

static void __exit miwill_exit(void)
{
	rtnl_link_unregister(&miwill_link_ops);
	unregister_netdevice_notifier(&miwill_notifier);
	genl_unregister_family(&miwill_family);
}

module_init(miwill_init);
module_exit(miwill_exit);
MODULE_ALIAS_RTNL_LINK("miwill");
MODULE_DESCRIPTION("Rothko MiWiLL redundancy network driver");
MODULE_LICENSE("GPL");
