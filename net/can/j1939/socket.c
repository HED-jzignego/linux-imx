// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2010-2011 EIA Electronics, Pieter Beyens <pieter.beyens@eia.be>
// Copyright (c) 2010-2011 EIA Electronics, Kurt Van Dijck <kurt.van.dijck@eia.be>
// Copyright (c) 2017-2018 Pengutronix, Marc Kleine-Budde <kernel@pengutronix.de>
// Copyright (c) 2017-2018 Pengutronix, Oleksij Rempel <kernel@pengutronix.de>
// Copyright (c) 2018 Protonic, Robin van der Gracht <robin@protonic.nl>

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/can/core.h>
#include <linux/can/skb.h>
#include <linux/errqueue.h>
#include <linux/if_arp.h>

#include "j1939-priv.h"

#define J1939_MIN_NAMELEN REQUIRED_SIZE(struct sockaddr_can, can_addr.j1939)

struct j1939_sock {
	struct sock sk; /* must be first to skip with memset */
	struct list_head list;

#define J1939_SOCK_BOUND BIT(0)
#define J1939_SOCK_CONNECTED BIT(1)
#define J1939_SOCK_PROMISC BIT(2)
#define J1939_SOCK_RECV_OWN BIT(3)
#define J1939_SOCK_ERRQUEUE BIT(4)
	int state;

	int ifindex;
	struct j1939_addr addr;
	struct j1939_filter *filters;
	int nfilters;
	pgn_t pgn_rx_filter;

	size_t etp_tx_complete_size;
	size_t etp_tx_done_size;

	/* j1939 may emit equal PGN (!= equal CAN-id's) out of order
	 * when transport protocol comes in.
	 * To allow emitting in order, keep a 'pending' nr. of packets
	 */
	atomic_t skb_pending;
	wait_queue_head_t waitq;
};

static inline struct j1939_sock *j1939_sk(const struct sock *sk)
{
	return container_of(sk, struct j1939_sock, sk);
}

/* conversion function between struct sock::sk_priority from linux and
 * j1939 priority field
 */
static inline priority_t j1939_prio(u32 sk_priority)
{
	sk_priority = min(sk_priority, 7U);

	return 7 - sk_priority;
}

static inline u32 j1939_to_sk_priority(priority_t prio)
{
	return 7 - prio;
}

/* function to see if pgn is to be evaluated */
static inline bool j1939_pgn_is_valid(pgn_t pgn)
{
	return pgn <= J1939_PGN_MAX;
}

/* test function to avoid non-zero DA placeholder for pdu1 pgn's */
static inline bool j1939_pgn_is_clean_pdu(pgn_t pgn)
{
	if (j1939_pgn_is_pdu1(pgn))
		return !(pgn & 0xff);
	else
		return true;
}

static inline void j1939_sock_pending_add(struct sock *sk)
{
	struct j1939_sock *jsk = j1939_sk(sk);

	atomic_inc(&jsk->skb_pending);
}

static int j1939_sock_pending_get(struct sock *sk)
{
	struct j1939_sock *jsk = j1939_sk(sk);

	return atomic_read(&jsk->skb_pending);
}

void j1939_sock_pending_del(struct sock *sk)
{
	struct j1939_sock *jsk = j1939_sk(sk);

	/* atomic_dec_return returns the new value */
	if (!atomic_dec_return(&jsk->skb_pending))
		wake_up(&jsk->waitq);	/* no pending SKB's */
}

static bool j1939_sk_match_dst(struct j1939_sock *jsk,
			       const struct j1939_sk_buff_cb *skcb)
{
	if ((jsk->state & J1939_SOCK_PROMISC))
		return true;

	/* Destination address filter */
	if (jsk->addr.src_name && skcb->addr.dst_name) {
		if (jsk->addr.src_name != skcb->addr.dst_name)
			return false;
	} else {
		/* receive (all sockets) if
		 * - all packages that match our bind() address
		 * - all broadcast on a socket if SO_BROADCAST
		 *   is set
		 */
		if (j1939_address_is_unicast(skcb->addr.da)) {
			if (jsk->addr.sa != skcb->addr.da)
				return false;
		} else if (!sock_flag(&jsk->sk, SOCK_BROADCAST)) {
			/* receiving broadcast without SO_BROADCAST
			 * flag is not allowed */
			return false;
		}
	}

	/* Source address filter */
	if (jsk->state & J1939_SOCK_CONNECTED) {
		/* receive (all sockets) if
		 * - all packages that match our connect() name or address
		 */
		if (jsk->addr.dst_name && skcb->addr.src_name) {
			if (jsk->addr.dst_name != skcb->addr.src_name)
				return false;
		} else {
			if (jsk->addr.da != skcb->addr.sa)
				return false;
		}
	}

	/* PGN filter */
	if (j1939_pgn_is_valid(jsk->pgn_rx_filter) &&
	    jsk->pgn_rx_filter != skcb->addr.pgn)
		return false;

	return true;
}

/* matches skb control buffer (addr) with a j1939 filter */
static bool j1939_sk_match_filter(struct j1939_sock *jsk,
				  const struct j1939_sk_buff_cb *skcb)
{
	const struct j1939_filter *f = jsk->filters;
	int nfilter = jsk->nfilters;

	if (!nfilter)
		/* receive all when no filters are assigned */
		return true;

	for (; nfilter; ++f, --nfilter) {
		if ((skcb->addr.pgn & f->pgn_mask) != f->pgn)
			continue;
		if ((skcb->addr.sa & f->addr_mask) != f->addr)
			continue;
		if ((skcb->addr.src_name & f->name_mask) != f->name)
			continue;
		return true;
	}
	return false;
}

static void j1939_sk_recv_one(struct j1939_sock *jsk, struct sk_buff *oskb)
{
	struct sk_buff *skb;
	const struct j1939_sk_buff_cb *oskcb = j1939_skb_to_cb(oskb);
	const struct can_skb_priv *oskb_prv = can_skb_prv(oskb);
	struct j1939_sk_buff_cb *skcb;

	if (!(jsk->state & (J1939_SOCK_BOUND | J1939_SOCK_CONNECTED)))
		return;

	if (jsk->ifindex != oskb_prv->ifindex)
		/* this socket does not take packets from this iface */
		return;

	if (oskcb->insock == &jsk->sk && !(jsk->state & J1939_SOCK_RECV_OWN))
		/* own message */
		return;

	if (!j1939_sk_match_dst(jsk, oskcb))
		return;

	if (!j1939_sk_match_filter(jsk, oskcb))
		return;

	skb = skb_clone(oskb, GFP_ATOMIC);
	if (!skb) {
		pr_warn("skb clone failed\n");
		return;
	}
	skcb = j1939_skb_to_cb(skb);
	skcb->msg_flags &= ~(MSG_DONTROUTE | MSG_CONFIRM);
	if (skcb->insock)
		skcb->msg_flags |= MSG_DONTROUTE;
	if (skcb->insock == &jsk->sk)
		skcb->msg_flags |= MSG_CONFIRM;

	if (sock_queue_rcv_skb(&jsk->sk, skb) < 0)
		kfree_skb(skb);
}

void j1939_sk_recv(struct j1939_priv *priv, struct sk_buff *skb)
{
	struct j1939_sock *jsk;

	spin_lock_bh(&priv->j1939_socks_lock);
	list_for_each_entry(jsk, &priv->j1939_socks, list) {
		j1939_sk_recv_one(jsk, skb);
	}
	spin_unlock_bh(&priv->j1939_socks_lock);
}

static int j1939_sk_init(struct sock *sk)
{
	struct j1939_sock *jsk = j1939_sk(sk);

	INIT_LIST_HEAD(&jsk->list);
	init_waitqueue_head(&jsk->waitq);
	jsk->sk.sk_priority = j1939_to_sk_priority(6);
	jsk->sk.sk_reuse = 1; /* per default */
	jsk->addr.sa = J1939_NO_ADDR;
	jsk->addr.da = J1939_NO_ADDR;
	jsk->addr.pgn = J1939_NO_PGN;
	jsk->pgn_rx_filter = J1939_NO_PGN;
	atomic_set(&jsk->skb_pending, 0);
	jsk->etp_tx_complete_size = 0;
	jsk->etp_tx_done_size = 0;

	return 0;
}

static int j1939_sk_sanity_check(struct sockaddr_can *addr, int len)
{
	if (!addr)
		return -EDESTADDRREQ;
	if (len < J1939_MIN_NAMELEN)
		return -EINVAL;
	if (addr->can_family != AF_CAN)
		return -EINVAL;
	if (!addr->can_ifindex)
		return -ENODEV;
	if (j1939_pgn_is_valid(addr->can_addr.j1939.pgn) &&
	    !j1939_pgn_is_clean_pdu(addr->can_addr.j1939.pgn))
		return -EINVAL;

	return 0;
}

static int j1939_sk_bind(struct socket *sock, struct sockaddr *uaddr, int len)
{
	struct sockaddr_can *addr = (struct sockaddr_can *)uaddr;
	struct j1939_sock *jsk = j1939_sk(sock->sk);
	struct sock *sk = sock->sk;
	struct net *net = sock_net(sk);
	struct net_device *ndev;
	struct j1939_priv *priv;
	int ret = 0;

	ret = j1939_sk_sanity_check(addr, len);
	if (ret)
		return ret;

	lock_sock(sock->sk);

	ndev = dev_get_by_index(net, addr->can_ifindex);
	if (!ndev) {
		ret = -ENODEV;
		goto out_release_sock;
	}

	/* Already bound to an interface? */
	if (jsk->state & J1939_SOCK_BOUND) {
		/* A re-bind() to a different interface is not
		 * supported.
		 */
		if (jsk->ifindex != addr->can_ifindex) {
			ret = -EINVAL;
			goto out_dev_put;
		}

		/* drop old references */
		priv = j1939_priv_get_by_ndev(ndev);
		j1939_local_ecu_put(priv, jsk->addr.src_name, jsk->addr.sa);
	} else {
		if (ndev->type != ARPHRD_CAN) {
			ret = -ENODEV;
			goto out_dev_put;
		}

		ret = j1939_netdev_start(net, ndev);
		if (ret < 0)
			goto out_dev_put;

		jsk->ifindex = addr->can_ifindex;
		priv = j1939_priv_get_by_ndev(ndev);
	}

	/* set default transmit pgn */
	if (j1939_pgn_is_valid(addr->can_addr.j1939.pgn))
		jsk->pgn_rx_filter = addr->can_addr.j1939.pgn;
	jsk->addr.src_name = addr->can_addr.j1939.name;
	jsk->addr.sa = addr->can_addr.j1939.addr;

	/* get new references */
	ret = j1939_local_ecu_get(priv, jsk->addr.src_name, jsk->addr.sa);
	if (ret) {
		j1939_netdev_stop(ndev);
		goto out_dev_put;
	}

	if (!(jsk->state & J1939_SOCK_BOUND)) {
		spin_lock_bh(&priv->j1939_socks_lock);
		list_add_tail(&jsk->list, &priv->j1939_socks);
		spin_unlock_bh(&priv->j1939_socks_lock);

		jsk->state |= J1939_SOCK_BOUND;
	}
	j1939_priv_put(priv);

 out_dev_put:	/* fallthrough */
	dev_put(ndev);
 out_release_sock:
	release_sock(sock->sk);

	return ret;
}

static int j1939_sk_connect(struct socket *sock, struct sockaddr *uaddr,
			    int len, int flags)
{
	struct sockaddr_can *addr = (struct sockaddr_can *)uaddr;
	struct j1939_sock *jsk = j1939_sk(sock->sk);
	int ret = 0;

	ret = j1939_sk_sanity_check(addr, len);
	if (ret)
		return ret;

	lock_sock(sock->sk);

	/* bind() before connect() is mandatory */
	if (!(jsk->state & J1939_SOCK_BOUND)) {
		ret = -EINVAL;
		goto out_release_sock;
	}

	/* A connect() to a different interface is not supported. */
	if (jsk->ifindex != addr->can_ifindex) {
		ret = -EINVAL;
		goto out_release_sock;
	}

	if (!addr->can_addr.j1939.name &&
	    addr->can_addr.j1939.addr == J1939_NO_ADDR &&
	    !sock_flag(&jsk->sk, SOCK_BROADCAST)) {
		/* broadcast, but SO_BROADCAST not set */
		ret = -EACCES;
		goto out_release_sock;
	}

	jsk->addr.dst_name = addr->can_addr.j1939.name;
	jsk->addr.da = addr->can_addr.j1939.addr;

	if (j1939_pgn_is_valid(addr->can_addr.j1939.pgn))
		jsk->addr.pgn = addr->can_addr.j1939.pgn;

	jsk->state |= J1939_SOCK_CONNECTED;

 out_release_sock: /* fallthrough */
	release_sock(sock->sk);
	return ret;
}

static void j1939_sk_sock2sockaddr_can(struct sockaddr_can *addr,
				       const struct j1939_sock *jsk, int peer)
{
	addr->can_family = AF_CAN;
	addr->can_ifindex = jsk->ifindex;
	addr->can_addr.j1939.pgn = jsk->addr.pgn;
	if (peer) {
		addr->can_addr.j1939.name = jsk->addr.dst_name;
		addr->can_addr.j1939.addr = jsk->addr.da;
	} else {
		addr->can_addr.j1939.name = jsk->addr.src_name;
		addr->can_addr.j1939.addr = jsk->addr.sa;
	}
}

static int j1939_sk_getname(struct socket *sock, struct sockaddr *uaddr,
			    int *sockaddr_len, int peer)
{
	struct sockaddr_can *addr = (struct sockaddr_can *)uaddr;
	struct sock *sk = sock->sk;
	struct j1939_sock *jsk = j1939_sk(sk);
	int ret = 0;

	lock_sock(sk);

	if (peer && !(jsk->state & J1939_SOCK_CONNECTED)) {
		ret = -EADDRNOTAVAIL;
		goto failure;
	}

	j1939_sk_sock2sockaddr_can(addr, jsk, peer);
	ret = J1939_MIN_NAMELEN;

 failure:
	release_sock(sk);

	*sockaddr_len = ret;
	return ret;
}

static int j1939_sk_release(struct socket *sock)
{
	struct sock *sk = sock->sk;
	struct j1939_sock *jsk;

	if (!sk)
		return 0;

	jsk = j1939_sk(sk);
	lock_sock(sk);

	if (jsk->state & J1939_SOCK_BOUND) {
		struct j1939_priv *priv;
		struct net_device *ndev;

		wait_event_interruptible(jsk->waitq,
					 j1939_sock_pending_get(&jsk->sk) == 0);

		ndev = dev_get_by_index(sock_net(sk), jsk->ifindex);
		priv = j1939_priv_get_by_ndev(ndev);

		spin_lock_bh(&priv->j1939_socks_lock);
		list_del_init(&jsk->list);
		spin_unlock_bh(&priv->j1939_socks_lock);

		j1939_local_ecu_put(priv, jsk->addr.src_name,
				    jsk->addr.sa);
		j1939_priv_put(priv);

		j1939_netdev_stop(ndev);
		dev_put(ndev);
	}

	sock_orphan(sk);
	sock->sk = NULL;

	release_sock(sk);
	sock_put(sk);

	return 0;
}

static int j1939_sk_setsockopt_flag(struct j1939_sock *jsk, char __user *optval,
				    unsigned int optlen, int flag)
{
	int tmp;

	if (optlen != sizeof(tmp))
		return -EINVAL;
	if (copy_from_user(&tmp, optval, optlen))
		return -EFAULT;
	lock_sock(&jsk->sk);
	if (tmp)
		jsk->state |= flag;
	else
		jsk->state &= ~flag;
	release_sock(&jsk->sk);
	return tmp;
}

static int j1939_sk_setsockopt(struct socket *sock, int level, int optname,
			       char __user *optval, unsigned int optlen)
{
	struct sock *sk = sock->sk;
	struct j1939_sock *jsk = j1939_sk(sk);
	int tmp, count = 0, ret = 0;
	struct j1939_filter *filters = NULL, *ofilters;

	if (level != SOL_CAN_J1939)
		return -EINVAL;

	switch (optname) {
	case SO_J1939_FILTER:
		if (optval) {
			struct j1939_filter *f;
			int c;

			if (optlen % sizeof(*filters) != 0)
				return -EINVAL;

			if (optlen > J1939_FILTER_MAX *
			    sizeof(struct j1939_filter))
				return -EINVAL;

			count = optlen / sizeof(*filters);
			filters = memdup_user(optval, optlen);
			if (IS_ERR(filters))
				return PTR_ERR(filters);

			for (f = filters, c = count; c; f++, c--) {
				f->name &= f->name_mask;
				f->pgn &= f->pgn_mask;
				f->addr &= f->addr_mask;
			}
		}

		lock_sock(&jsk->sk);
		ofilters = jsk->filters;
		jsk->filters = filters;
		jsk->nfilters = count;
		release_sock(&jsk->sk);
		kfree(ofilters);
		return 0;
	case SO_J1939_PROMISC:
		return j1939_sk_setsockopt_flag(jsk, optval, optlen,
						J1939_SOCK_PROMISC);
	case SO_J1939_RECV_OWN:
		return j1939_sk_setsockopt_flag(jsk, optval, optlen,
						J1939_SOCK_RECV_OWN);
	case SO_J1939_ERRQUEUE:
		ret = j1939_sk_setsockopt_flag(jsk, optval, optlen,
					       J1939_SOCK_ERRQUEUE);
		if (ret < 0)
			return ret;

		if (!(jsk->state & J1939_SOCK_ERRQUEUE))
			skb_queue_purge(&sk->sk_error_queue);
		return ret;
	case SO_J1939_SEND_PRIO:
		if (optlen != sizeof(tmp))
			return -EINVAL;
		if (copy_from_user(&tmp, optval, optlen))
			return -EFAULT;
		if (tmp < 0 || tmp > 7)
			return -EDOM;
		if (tmp < 2 && !capable(CAP_NET_ADMIN))
			return -EPERM;
		lock_sock(&jsk->sk);
		jsk->sk.sk_priority = j1939_to_sk_priority(tmp);
		release_sock(&jsk->sk);
		return 0;
	default:
		return -ENOPROTOOPT;
	}
}

static int j1939_sk_getsockopt(struct socket *sock, int level, int optname,
			       char __user *optval, int __user *optlen)
{
	struct sock *sk = sock->sk;
	struct j1939_sock *jsk = j1939_sk(sk);
	int ret, ulen;
	/* set defaults for using 'int' properties */
	int tmp = 0;
	int len = sizeof(tmp);
	void *val = &tmp;

	if (level != SOL_CAN_J1939)
		return -EINVAL;
	if (get_user(ulen, optlen))
		return -EFAULT;
	if (ulen < 0)
		return -EINVAL;

	lock_sock(&jsk->sk);
	switch (optname) {
	case SO_J1939_PROMISC:
		tmp = (jsk->state & J1939_SOCK_PROMISC) ? 1 : 0;
		break;
	case SO_J1939_RECV_OWN:
		tmp = (jsk->state & J1939_SOCK_RECV_OWN) ? 1 : 0;
		break;
	case SO_J1939_ERRQUEUE:
		tmp = (jsk->state & J1939_SOCK_ERRQUEUE) ? 1 : 0;
		break;
	case SO_J1939_SEND_PRIO:
		tmp = j1939_prio(jsk->sk.sk_priority);
		break;
	default:
		ret = -ENOPROTOOPT;
		goto no_copy;
	}

	/* copy to user, based on 'len' & 'val'
	 * but most sockopt's are 'int' properties, and have 'len' & 'val'
	 * left unchanged, but instead modified 'tmp'
	 */
	if (len > ulen)
		ret = -EFAULT;
	else if (put_user(len, optlen))
		ret = -EFAULT;
	else if (copy_to_user(optval, val, len))
		ret = -EFAULT;
	else
		ret = 0;
 no_copy:
	release_sock(&jsk->sk);
	return ret;
}

static int j1939_sk_recvmsg(struct socket *sock, struct msghdr *msg,
			    size_t size, int flags)
{
	struct sock *sk = sock->sk;
	struct sk_buff *skb;
	struct j1939_sk_buff_cb *skcb;
	int ret = 0;

	if (flags & ~(MSG_DONTWAIT | MSG_ERRQUEUE))
		return -EINVAL;

	if (flags & MSG_ERRQUEUE)
		return sock_recv_errqueue(sock->sk, msg, size, SOL_CAN_J1939,
					  SCM_J1939_ERRQUEUE);

	skb = skb_recv_datagram(sk, flags, 0, &ret);
	if (!skb)
		return ret;

	if (size < skb->len)
		msg->msg_flags |= MSG_TRUNC;
	else
		size = skb->len;

	ret = memcpy_to_msg(msg, skb->data, size);
	if (ret < 0) {
		skb_free_datagram(sk, skb);
		return ret;
	}

	skcb = j1939_skb_to_cb(skb);
	if (j1939_address_is_valid(skcb->addr.da))
		put_cmsg(msg, SOL_CAN_J1939, SCM_J1939_DEST_ADDR,
			 sizeof(skcb->addr.da), &skcb->addr.da);

	if (skcb->addr.dst_name)
		put_cmsg(msg, SOL_CAN_J1939, SCM_J1939_DEST_NAME,
			 sizeof(skcb->addr.dst_name), &skcb->addr.dst_name);

	put_cmsg(msg, SOL_CAN_J1939, SCM_J1939_PRIO,
		 sizeof(skcb->priority), &skcb->priority);

	if (msg->msg_name) {
		struct sockaddr_can *paddr = msg->msg_name;

		msg->msg_namelen = J1939_MIN_NAMELEN;
		memset(msg->msg_name, 0, msg->msg_namelen);
		paddr->can_family = AF_CAN;
		paddr->can_ifindex = skb->skb_iif;
		paddr->can_addr.j1939.name = skcb->addr.src_name;
		paddr->can_addr.j1939.addr = skcb->addr.sa;
		paddr->can_addr.j1939.pgn = skcb->addr.pgn;
	}

	sock_recv_ts_and_drops(msg, sk, skb);
	msg->msg_flags |= skcb->msg_flags;
	skb_free_datagram(sk, skb);

	return size;
}

static struct sk_buff *j1939_sk_alloc_skb(struct net_device *ndev,
					  struct sock *sk,
					  struct msghdr *msg, size_t size,
					  int *errcode)
{
	struct j1939_sock *jsk = j1939_sk(sk);
	struct j1939_sk_buff_cb *skcb;
	struct sk_buff *skb;
	int ret;

	skb = sock_alloc_send_skb(sk,
				  size +
				  sizeof(struct can_frame) -
				  sizeof(((struct can_frame *)NULL)->data) +
				  sizeof(struct can_skb_priv),
				  msg->msg_flags & MSG_DONTWAIT, &ret);
	if (!skb)
		goto failure;

	can_skb_reserve(skb);
	can_skb_prv(skb)->ifindex = ndev->ifindex;
	can_skb_prv(skb)->skbcnt = 0;
	skb_reserve(skb, offsetof(struct can_frame, data));

	ret = memcpy_from_msg(skb_put(skb, size), msg, size);
	if (ret < 0)
		goto free_skb;
	sock_tx_timestamp(sk, skb->sk->sk_tsflags, &skb_shinfo(skb)->tx_flags);

	skb->dev = ndev;

	skcb = j1939_skb_to_cb(skb);
	memset(skcb, 0, sizeof(*skcb));
	skcb->addr = jsk->addr;
	skcb->priority = j1939_prio(sk->sk_priority);
	skcb->msg_flags = msg->msg_flags;

	if (msg->msg_name) {
		struct sockaddr_can *addr = msg->msg_name;

		if (addr->can_addr.j1939.name ||
		    addr->can_addr.j1939.addr != J1939_NO_ADDR) {
			skcb->addr.dst_name = addr->can_addr.j1939.name;
			skcb->addr.da = addr->can_addr.j1939.addr;
		}
		if (j1939_pgn_is_valid(addr->can_addr.j1939.pgn))
			skcb->addr.pgn = addr->can_addr.j1939.pgn;
	}

	*errcode = ret;
	return skb;

free_skb:
	kfree_skb(skb);
failure:
	*errcode = ret;
	return NULL;
}

static size_t j1939_sk_opt_stats_get_size(void)
{
	return
		nla_total_size(sizeof(u32)) + /* J1939_NLA_BYTES_ACKED */
		0;
}

static struct sk_buff *j1939_sk_get_timestamping_opt_stats(struct j1939_session *session)
{
	struct sk_buff *stats;

	stats = alloc_skb(j1939_sk_opt_stats_get_size(), GFP_ATOMIC);
	if (!stats)
		return NULL;

	nla_put_u32(stats, J1939_NLA_BYTES_ACKED,
		    min(session->pkt.done * 7, session->total_message_size));

	return stats;
}

void j1939_sk_errqueue(struct j1939_session *session,
		       enum j1939_sk_errqueue_type type)
{
	struct j1939_priv *priv = session->priv;
	struct sock *sk = session->sk;
	struct j1939_sock *jsk;
	struct sock_exterr_skb *serr;
	struct sk_buff *skb;
	int err;

	/* currently we have no sk for the RX session */
	if (!sk)
		return;

	jsk = j1939_sk(sk);

	if (!(jsk->state & J1939_SOCK_ERRQUEUE))
		return;

	skb = j1939_sk_get_timestamping_opt_stats(session);
	if (!skb)
		return;

	skb->tstamp = ktime_get_real();

	BUILD_BUG_ON(sizeof(struct sock_exterr_skb) > sizeof(skb->cb));

	serr = SKB_EXT_ERR(skb);
	memset(serr, 0, sizeof(*serr));
	switch (type) {
	case J1939_ERRQUEUE_ACK:
		serr->ee.ee_errno = ENOMSG;
		serr->ee.ee_origin = SO_EE_ORIGIN_TIMESTAMPING;
		serr->ee.ee_info = SCM_TSTAMP_ACK;
		break;
	case J1939_ERRQUEUE_SCHED:
		serr->ee.ee_errno = ENOMSG;
		serr->ee.ee_origin = SO_EE_ORIGIN_TIMESTAMPING;
		serr->ee.ee_info = SCM_TSTAMP_SCHED;
		break;
	case J1939_ERRQUEUE_ABORT:
		serr->ee.ee_errno = session->err;
		serr->ee.ee_origin = SO_EE_ORIGIN_LOCAL;
		serr->ee.ee_info = J1939_EE_INFO_TX_ABORT;
		break;
	default:
		netdev_err(priv->ndev, "Unknown errqueue type %i\n", type);
	}

	serr->opt_stats = true;
	if (sk->sk_tsflags & SOF_TIMESTAMPING_OPT_ID)
		serr->ee.ee_data = session->tskey;

	err = sock_queue_err_skb(sk, skb);

	if (err)
		kfree_skb(skb);
};

void j1939_sk_send_multi_abort(struct j1939_priv *priv, struct sock *sk,
			       int err)
{
	sk->sk_err = err;

	sk->sk_error_report(sk);
}

static int j1939_sk_send_multi(struct j1939_priv *priv,  struct sock *sk,
			       struct msghdr *msg, size_t size)

{
	struct j1939_sock *jsk = j1939_sk(sk);
	struct j1939_session *session = NULL;
	struct sk_buff *skb;
	size_t segment_size, todo_size;
	int ret = 0;

	if (!jsk->etp_tx_done_size) {
		j1939_sock_pending_add(&jsk->sk);
		jsk->etp_tx_complete_size = size;
	} else if (jsk->etp_tx_complete_size != jsk->etp_tx_done_size + size) {
		return -EIO;
	}

	todo_size = size;

	while (todo_size) {
		struct j1939_sk_buff_cb *skcb;

		if (todo_size > J1939_MAX_TP_PACKET_SIZE)
			segment_size = J1939_MAX_TP_PACKET_SIZE;
		else
			segment_size = todo_size;

		/* Allocate skb for one segment */
		skb = j1939_sk_alloc_skb(priv->ndev, sk, msg, segment_size,
					 &ret);
		if (ret)
			break;

		skcb = j1939_skb_to_cb(skb);
		skcb->offset = jsk->etp_tx_done_size;

		if (!session) {
			if (jsk->etp_tx_done_size) {
				bool extd = J1939_REGULAR;

				if (jsk->etp_tx_complete_size >
				    J1939_MAX_TP_PACKET_SIZE)
					extd = J1939_EXTENDED;

				session = j1939_session_get_by_skcb(priv, skcb,
								    extd,
								    false);
				if (IS_ERR(session)) {
					ret = PTR_ERR(session);
					goto kfree_skb;
				} else if (!session) {
					ret = -ENOENT;
					goto kfree_skb;
				}

				j1939_session_skb_queue(session, skb);
			} else {
				/* create new session with
				 * etp_tx_complete_size and attach skb
				 * segment
				 */
				session = j1939_tp_send(priv, skb,
							jsk->etp_tx_complete_size);
				if (IS_ERR(session)) {
					ret = PTR_ERR(session);
					goto kfree_skb;
				}
			}
		} else {
			j1939_session_skb_queue(session, skb);
		}

		todo_size -= segment_size;
		jsk->etp_tx_done_size += segment_size;
	}

	switch (ret) {
	case 0: /* OK */
		if (todo_size)
			netdev_warn(priv->ndev,
				    "no error found and not completely queued?! %zu\n",
				    todo_size);
		ret = size;
		jsk->etp_tx_done_size = 0;
		break;
	case -ERESTARTSYS:
		ret = -EINTR;
		/* fall trough */
	case -EAGAIN: /* OK */
		if (todo_size != size)
			ret = size - todo_size;
		break;
	default: /* ERROR */
		/* skb session queue will be purged if we are the last user */
		jsk->etp_tx_done_size = 0;
	}

	if (session)
		j1939_session_put(session);

	return ret;

 kfree_skb:
	kfree_skb(skb);
	jsk->etp_tx_done_size = 0;
	return ret;
}

static int j1939_sk_send_one(struct j1939_priv *priv,  struct sock *sk,
			     struct msghdr *msg, size_t size)

{
	struct sk_buff *skb;
	int ret;

	skb = j1939_sk_alloc_skb(priv->ndev, sk, msg, size, &ret);
	if (ret)
		return ret;

	ret = j1939_send_one(priv, skb);

	return ret ? ret : size;
}

static int j1939_sk_sendmsg(struct socket *sock, struct msghdr *msg,
			    size_t size)
{
	struct sock *sk = sock->sk;
	struct j1939_sock *jsk = j1939_sk(sk);
	struct j1939_priv *priv;
	struct net_device *ndev;
	int ifindex;
	int ret;

	/* various socket state tests */
	if (!(jsk->state & J1939_SOCK_BOUND))
		return -EBADFD;

	ifindex = jsk->ifindex;

	if (!jsk->addr.src_name && jsk->addr.sa == J1939_NO_ADDR)
		/* no source address assigned yet */
		return -EBADFD;

	/* deal with provided destination address info */
	if (msg->msg_name) {
		struct sockaddr_can *addr = msg->msg_name;

		if (msg->msg_namelen < J1939_MIN_NAMELEN)
			return -EINVAL;

		if (addr->can_family != AF_CAN)
			return -EINVAL;

		if (addr->can_ifindex && addr->can_ifindex != ifindex)
			return -EBADFD;

		if (j1939_pgn_is_valid(addr->can_addr.j1939.pgn) &&
		    !j1939_pgn_is_clean_pdu(addr->can_addr.j1939.pgn))
			return -EINVAL;

		if (!addr->can_addr.j1939.name &&
		    addr->can_addr.j1939.addr == J1939_NO_ADDR &&
		    !sock_flag(sk, SOCK_BROADCAST))
			/* broadcast, but SO_BROADCAST not set */
			return -EACCES;
	} else {
		if (!jsk->addr.dst_name && jsk->addr.da == J1939_NO_ADDR &&
		    !sock_flag(sk, SOCK_BROADCAST))
			/* broadcast, but SO_BROADCAST not set */
			return -EACCES;
	}

	ndev = dev_get_by_index(sock_net(sk), ifindex);
	if (!ndev)
		return -ENXIO;

	priv = j1939_priv_get_by_ndev(ndev);
	if (!priv)
		return -EINVAL;

	if (size > 8)
		/* re-route via transport protocol */
		ret = j1939_sk_send_multi(priv, sk, msg, size);
	else
		ret = j1939_sk_send_one(priv, sk, msg, size);

	j1939_priv_put(priv);

	dev_put(ndev);
	return ret;
}

void j1939_sk_netdev_event(struct net_device *ndev, int error_code)
{
	struct j1939_priv *priv = j1939_priv_get_by_ndev(ndev);
	struct j1939_sock *jsk;

	spin_lock_bh(&priv->j1939_socks_lock);
	list_for_each_entry(jsk, &priv->j1939_socks, list) {
		jsk->sk.sk_err = error_code;
		if (!sock_flag(&jsk->sk, SOCK_DEAD))
			jsk->sk.sk_error_report(&jsk->sk);

		if (error_code == ENODEV) {
			j1939_local_ecu_put(priv, jsk->addr.src_name,
					    jsk->addr.sa);

			j1939_netdev_stop(ndev);
		}
		/* do not remove filters here */
	}
	spin_unlock_bh(&priv->j1939_socks_lock);
	j1939_priv_put(priv);
}

static const struct proto_ops j1939_ops = {
	.family = PF_CAN,
	.release = j1939_sk_release,
	.bind = j1939_sk_bind,
	.connect = j1939_sk_connect,
	.socketpair = sock_no_socketpair,
	.accept = sock_no_accept,
	.getname = j1939_sk_getname,
	.poll = datagram_poll,
	.ioctl = can_ioctl,
	.listen = sock_no_listen,
	.shutdown = sock_no_shutdown,
	.setsockopt = j1939_sk_setsockopt,
	.getsockopt = j1939_sk_getsockopt,
	.sendmsg = j1939_sk_sendmsg,
	.recvmsg = j1939_sk_recvmsg,
	.mmap = sock_no_mmap,
	.sendpage = sock_no_sendpage,
};

static struct proto j1939_proto __read_mostly = {
	.name = "CAN_J1939",
	.owner = THIS_MODULE,
	.obj_size = sizeof(struct j1939_sock),
	.init = j1939_sk_init,
};

const struct can_proto j1939_can_proto = {
	.type = SOCK_DGRAM,
	.protocol = CAN_J1939,
	.ops = &j1939_ops,
	.prot = &j1939_proto,
};
