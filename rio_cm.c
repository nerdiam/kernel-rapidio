/*
 * rio_cm - RapidIO messaging channel manager
 *
 * Copyright 2013-2015 Integrated Device Technology, Inc.
 * Alexandre Bounine <alexandre.bounine@idt.com>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 * THIS PROGRAM IS DISTRIBUTED IN THE HOPE THAT IT WILL BE USEFUL,
 * BUT WITHOUT ANY WARRANTY; WITHOUT EVEN THE IMPLIED WARRANTY OF
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  SEE THE
 * GNU GENERAL PUBLIC LICENSE FOR MORE DETAILS.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include "include/rio.h"
#include "include/rio_drv.h"
#include <linux/slab.h>
#include <linux/idr.h>
#include <linux/interrupt.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/reboot.h>
#include <linux/bitops.h>
#include <linux/printk.h>
#include "include/rio_cm_cdev.h"

#define DRV_NAME        "rio_cm"
#define DRV_VERSION     "0.5"
#define DRV_AUTHOR      "Alexandre Bounine <alexandre.bounine@idt.com>"
#define DRV_DESC        "RapidIO Messaging Channel Manager"
#define DEV_NAME	"rio_cm"

/* Debug output filtering masks */
enum {
	DBG_NONE	= 0,
	DBG_INIT	= BIT(0), /* driver init */
	DBG_EXIT	= BIT(1), /* driver exit */
	DBG_MPORT	= BIT(2), /* mport add/remove */
	DBG_RDEV	= BIT(3), /* RapidIO device add/remove */
	DBG_CHOP	= BIT(4), /* channel operations */
	DBG_WAIT	= BIT(5), /* waiting for events */
	DBG_TX		= BIT(6), /* message TX */
	DBG_TX_EVENT	= BIT(7), /* message TX event */
	DBG_RX_DATA	= BIT(8), /* inbound data messages */
	DBG_RX_CMD	= BIT(9), /* inbound REQ/ACK/NACK messages */
	DBG_ALL		= ~0,
};

#ifdef DEBUG
#define riocm_debug(level, fmt, arg...) \
	do { \
		if (DBG_##level & dbg_level) \
			pr_debug(DRV_NAME ": %s " fmt "\n", \
				__func__, ##arg); \
	} while (0)
#else
#define riocm_debug(level, fmt, arg...) \
		no_printk(KERN_DEBUG pr_fmt(DRV_NAME fmt "\n"), ##arg)
#endif

#define riocm_warn(fmt, arg...) \
	pr_warn(DRV_NAME ": %s WARNING " fmt "\n", __func__, ##arg)

#define riocm_error(fmt, arg...) \
	pr_err(DRV_NAME ": %s ERROR " fmt "\n", __func__, ##arg)


static int cmbox = 1;
module_param(cmbox, int, S_IRUGO);
MODULE_PARM_DESC(cmbox, "RapidIO Mailbox number (default 1)");

static int chstart = 256;
module_param(chstart, int, S_IRUGO);
MODULE_PARM_DESC(chstart,
		 "Start channel number for dynamic allocation (default 256)");

#ifdef DEBUG
static u32 dbg_level = DBG_NONE;
module_param(dbg_level, uint, S_IWUSR | S_IRUGO);
MODULE_PARM_DESC(dbg_level, "Debugging output level (default 0 = none)");
#endif

MODULE_AUTHOR(DRV_AUTHOR);
MODULE_DESCRIPTION(DRV_DESC);
MODULE_LICENSE("GPL");

#define RIOCM_TX_RING_SIZE	128
#define RIOCM_RX_RING_SIZE	128
#define RIOCM_CONNECT_TO	3 /* connect response TO (in sec) */

#define RIOCM_MAX_CHNUM		0xffff /* Use full range of u16 field */
#define RIOCM_CHNUM_AUTO	0

enum rio_cm_state {
	RIO_CM_IDLE,
	RIO_CM_CONNECT,
	RIO_CM_CONNECTED,
	RIO_CM_DISCONNECT,
	RIO_CM_CHAN_BOUND,
	RIO_CM_LISTEN,
	RIO_CM_DESTROYING,
};

enum rio_cm_pkt_type {
	RIO_CM_SYS	= 0xaa,
	RIO_CM_CHAN	= 0x55,
};

enum rio_cm_chop {
	CM_CONN_REQ,
	CM_CONN_ACK,
	CM_CONN_CLOSE,
	CM_DATA_MSG,
};

struct rio_ch_base_bhdr {
	u32 src_id;
	u32 dst_id;
#define RIO_HDR_LETTER_MASK 0xffff0000
#define RIO_HDR_MBOX_MASK   0x0000ffff
	u8  src_mbox;
	u8  dst_mbox;
	u8  type;
} __attribute__((__packed__));

struct rio_ch_chan_hdr {
	struct rio_ch_base_bhdr bhdr;
	u8 ch_op;
	u16 dst_ch;
	u16 src_ch;
	u16 msg_len; /* for NACK response acts as an error code */
	u16 rsrvd;
} __attribute__((__packed__));

struct tx_req {
	struct list_head node;
	struct rio_dev   *rdev;
	void		 *buffer;
	size_t		 len;
};

struct cm_dev {
	struct list_head	list;
	struct rio_mport	*mport;
	void			*rx_buf[RIOCM_RX_RING_SIZE];
	int			rx_slots;

	void			*tx_buf[RIOCM_TX_RING_SIZE];
	int			tx_slot;
	int			tx_cnt;
	int			tx_ack_slot;
	struct list_head	tx_reqs;

	spinlock_t		tx_lock;
	struct list_head	peers;
	int			npeers;
	struct tasklet_struct	rx_tasklet;
	struct tasklet_struct	tx_tasklet;
};

struct chan_rx_ring {
	void	*buf[RIOCM_RX_RING_SIZE];
	int	head;
	int	tail;
	int	count;

	/* Tracking RX buffers reported to upper level */
	void	*inuse[RIOCM_RX_RING_SIZE];
	int	inuse_cnt;
};

struct rio_channel {
	u16			id;	/* local channel ID */
	struct kref		ref;	/* channel refcount */
	struct file		*filp;
	struct cm_dev		*cmdev;	/* associated CM device object */
	struct rio_dev		*rdev;	/* remote RapidIO device */
	enum rio_cm_state	state;
	int			error;
	spinlock_t		lock;
	void			*context;
	u32			loc_destid;	/* local destID */
	u32			rem_destid;	/* remote destID */
	u16			rem_channel;	/* remote channel ID */
	struct list_head	accept_queue;
	struct list_head	ch_node;
	wait_queue_head_t	wait_q;
	struct completion	comp;
	struct chan_rx_ring	rx_ring;
};

struct cm_peer {
	struct list_head node;
	struct rio_dev *rdev;
};

struct rio_cm_work {
	struct work_struct work;
	struct cm_dev *cm;
	void *data;
};

struct conn_req {
	struct list_head node;
	u32 destid;	/* requester destID */
	u16 chan;	/* requester channel ID */
	struct cm_dev *cmdev;
};

/*
 * A channel_dev represents a structure on mport
 * @cdev	Character device
 * @dev		Associated device object
 */
struct channel_dev {
	struct cdev	cdev;
	struct device	*dev;
};

static struct rio_channel *riocm_ch_alloc(u16 ch_num);
static void riocm_ch_free(struct kref *ref);
static int riocm_post_send(struct cm_dev *cm, struct rio_dev *rdev,
			   void *buffer, size_t len, int req);
static int riocm_ch_close(struct rio_channel *ch);

static DEFINE_SPINLOCK(idr_lock);
static DEFINE_IDR(ch_idr);

static LIST_HEAD(cm_dev_list);
static DECLARE_RWSEM(rdev_sem);
static struct workqueue_struct *riocm_wq;

static struct class *dev_class;
static unsigned int dev_major;
static unsigned int dev_minor_base;
static dev_t dev_number;
static struct channel_dev riocm_cdev;

#define is_msg_capable(src_ops, dst_ops)			\
			((src_ops & RIO_SRC_OPS_DATA_MSG) &&	\
			 (dst_ops & RIO_DST_OPS_DATA_MSG))
#define dev_cm_capable(dev) \
	is_msg_capable(dev->src_ops, dev->dst_ops)

static int riocm_comp(struct rio_channel *ch, enum rio_cm_state comp)
{
	int ret;

	spin_lock_bh(&ch->lock);
	ret = (ch->state == comp);
	spin_unlock_bh(&ch->lock);
	return ret;
}

static int riocm_comp_exch(struct rio_channel *ch,
			   enum rio_cm_state comp, enum rio_cm_state exch)
{
	int ret;

	spin_lock_bh(&ch->lock);
	ret = (ch->state == comp);
	if (ret)
		ch->state = exch;
	spin_unlock_bh(&ch->lock);
	return ret;
}

static enum rio_cm_state riocm_exch(struct rio_channel *ch,
				    enum rio_cm_state exch)
{
	enum rio_cm_state old;

	spin_lock_bh(&ch->lock);
	old = ch->state;
	ch->state = exch;
	spin_unlock_bh(&ch->lock);
	return old;
}

static struct rio_channel *riocm_get_channel(u16 nr)
{
	struct rio_channel *ch;

	spin_lock_bh(&idr_lock);
	ch = idr_find(&ch_idr, nr);
	if (ch)
		kref_get(&ch->ref);
	spin_unlock_bh(&idr_lock);
	return ch;
}

static void riocm_put_channel(struct rio_channel *ch)
{
	kref_put(&ch->ref, riocm_ch_free);
}

static void *riocm_rx_get_msg(struct cm_dev *cm)
{
	void *msg;
	int i;

	msg = rio_get_inb_message(cm->mport, cmbox);
	if (msg) {
		for (i = 0; i < RIOCM_RX_RING_SIZE; i++) {
			if (cm->rx_buf[i] == msg) {
				cm->rx_buf[i] = NULL;
				cm->rx_slots++;
				break;
			}
		}

		if (i == RIOCM_RX_RING_SIZE)
			riocm_warn("no record for buffer 0x%p", msg);
	}

	return msg;
}

/*
 * riocm_rx_fill - fills a ring of receive buffers for given cm device
 * @cm: cm_dev object
 * @nent: max number of entries to fill
 *
 * Returns: none
 */
static void riocm_rx_fill(struct cm_dev *cm, int nent)
{
	int i;

	if (cm->rx_slots == 0)
		return;

	for (i = 0; i < RIOCM_RX_RING_SIZE && cm->rx_slots && nent; i++) {
		if (cm->rx_buf[i] == NULL) {
			cm->rx_buf[i] = kmalloc(RIO_MAX_MSG_SIZE, GFP_ATOMIC);
			if (cm->rx_buf[i] == NULL)
				break;
			rio_add_inb_buffer(cm->mport, cmbox, cm->rx_buf[i]);
			cm->rx_slots--;
			nent--;
		}
	}
}

/*
 * riocm_rx_free - frees all receive buffers associated with given cm device
 * @cm: cm_dev object
 *
 * Returns: none
 */
static void riocm_rx_free(struct cm_dev *cm)
{
	int i;

	for (i = 0; i < RIOCM_RX_RING_SIZE; i++) {
		if (cm->rx_buf[i] != NULL) {
			kfree(cm->rx_buf[i]);
			cm->rx_buf[i] = NULL;
		}
	}
}

static int riocm_req_handler(struct cm_dev *cm, void *req_data)
{
	struct rio_channel *ch;
	struct conn_req *req;
	struct rio_ch_chan_hdr *hh = req_data;
	u16 chnum;

	chnum = ntohs(hh->dst_ch);

	ch = riocm_get_channel(chnum);

	if (!ch)
		return -ENODEV;

	if (ch->state != RIO_CM_LISTEN) {
		riocm_debug(RX_CMD, "channel %d is not in listen state", chnum);
		riocm_put_channel(ch);
		return -EINVAL;
	}

	req = kzalloc(sizeof(struct conn_req), GFP_KERNEL);
	if (!req) {
		riocm_put_channel(ch);
		return -ENOMEM;
	}

	req->destid = ntohl(hh->bhdr.src_id);
	req->chan = ntohs(hh->src_ch);
	req->cmdev = cm;

	spin_lock_bh(&ch->lock);
	list_add_tail(&req->node, &ch->accept_queue);
	wake_up(&ch->wait_q);
	spin_unlock_bh(&ch->lock);
	riocm_put_channel(ch);

	return 0;
}

static int riocm_resp_handler(void *resp_data)
{
	struct rio_channel *ch;
	struct rio_ch_chan_hdr *hh = resp_data;
	u16 chnum;

	if (hh->ch_op != CM_CONN_ACK)
		return -EINVAL;

	chnum = ntohs(hh->dst_ch);
	ch = riocm_get_channel(chnum);
	if (!ch)
		return -ENODEV;

	if (ch->state != RIO_CM_CONNECT) {
		riocm_put_channel(ch);
		return -EINVAL;
	}

	riocm_exch(ch, RIO_CM_CONNECTED);
	ch->rem_channel = ntohs(hh->src_ch);
	wake_up(&ch->wait_q);
	riocm_put_channel(ch);

	return 0;
}

static int riocm_close_handler(void *data)
{
	struct rio_channel *ch;
	struct rio_ch_chan_hdr *hh = data;
	int ret;

	if (hh->ch_op != CM_CONN_CLOSE) {
		riocm_error("Invalid request header");
		return -EINVAL;
	}

	riocm_debug(RX_CMD, "for ch=%d", ntohs(hh->dst_ch));

	spin_lock_bh(&idr_lock);
	ch = idr_find(&ch_idr, ntohs(hh->dst_ch));
	if (!ch) {
		spin_unlock_bh(&idr_lock);
		return -ENODEV;
	}
	idr_remove(&ch_idr, ch->id);
	spin_unlock_bh(&idr_lock);

	riocm_exch(ch, RIO_CM_DISCONNECT);

	ret = riocm_ch_close(ch);
	if (ret)
		riocm_debug(RX_CMD, "riocm_ch_close() returned %d", ret);

	return 0;
}

static void rio_cm_handler(struct work_struct *_work)
{
	struct rio_cm_work *work;
	void *data;
	struct rio_ch_chan_hdr *hdr;

	work = container_of(_work, struct rio_cm_work, work);
	data = work->data;

	if (!rio_mport_is_running(work->cm->mport))
		goto out;

	hdr = (struct rio_ch_chan_hdr *)data;

	riocm_debug(RX_CMD, "OP=%x for ch=%d from %d",
		    hdr->ch_op, ntohs(hdr->dst_ch), ntohs(hdr->src_ch));

	switch (hdr->ch_op) {
	case CM_CONN_REQ:
		riocm_req_handler(work->cm, data);
		break;
	case CM_CONN_ACK:
		riocm_resp_handler(data);
		break;
	case CM_CONN_CLOSE:
		riocm_close_handler(data);
		break;
	default:
		riocm_error("Invalid packet header");
		break;
	}
out:
	kfree(data);
	kfree(work);
}

static int rio_rx_data_handler(struct cm_dev *cm, void *buf)
{
	struct rio_ch_chan_hdr *hdr;
	struct rio_channel *ch;

	hdr = (struct rio_ch_chan_hdr *)buf;

	riocm_debug(RX_DATA, "for ch=%d", ntohs(hdr->dst_ch));

	ch = riocm_get_channel(ntohs(hdr->dst_ch));
	if (!ch) {
		/* Discard data message for non-existing channel */
		kfree(buf);
		return -ENODEV;
	}
#if (0)
	riocm_debug(RX_DATA, "found ch=%d", ch->id);
	riocm_debug(RX_DATA, "msg=%s",
		 (char *)((u8 *)buf + sizeof(struct rio_ch_chan_hdr)));
#endif
	/* Place pointer to the buffer into channel's RX queue */
	spin_lock(&ch->lock);

	if (ch->state != RIO_CM_CONNECTED) {
		/* Channel is not ready to receive data, discard a packet */
		riocm_debug(RX_DATA, "ch=%d is in wrong state=%d",
			    ch->id, ch->state);
		spin_unlock(&ch->lock);
		kfree(buf);
		riocm_put_channel(ch);
		return -EIO;
	}

	if (ch->rx_ring.count == RIOCM_RX_RING_SIZE) {
		/* If RX ring is full, discard a packet */
		riocm_debug(RX_DATA, "ch=%d is full", ch->id);
		spin_unlock(&ch->lock);
		kfree(buf);
		riocm_put_channel(ch);
		return -ENOMEM;
	}

	ch->rx_ring.buf[ch->rx_ring.head] = buf;
	ch->rx_ring.head++;
	ch->rx_ring.count++;
	ch->rx_ring.head %= RIOCM_RX_RING_SIZE;

	wake_up(&ch->wait_q);

	spin_unlock(&ch->lock);
	riocm_put_channel(ch);

	return 0;
}

static void rio_ibmsg_handler(unsigned long context)
{
	struct cm_dev *cm = (struct cm_dev *)context;
	void *data;
	struct rio_ch_chan_hdr *hdr;
	int i;

	if (!rio_mport_is_running(cm->mport))
		return;

	for (i = 0; i < 8; i++) {
		data = riocm_rx_get_msg(cm);
		if (data)
			riocm_rx_fill(cm, 1);

		if (data == NULL)
			break;

		hdr = (struct rio_ch_chan_hdr *)data;

		if (hdr->bhdr.type != RIO_CM_CHAN) {
			/* For now simply discard packets other than channel */
			riocm_error("Unsupported TYPE code (0x%x). Msg dropped",
				    hdr->bhdr.type);
			kfree(data);
			continue;
		}

		/* Process a channel message */
		if (hdr->ch_op == CM_DATA_MSG) {
			rio_rx_data_handler(cm, data);
		} else {
			struct rio_cm_work *work;

			work = kmalloc(sizeof(*work), GFP_ATOMIC);
			if (!work) {
				/* Discard a packet if we cannot process it */
				riocm_error("Failed to alloc memory for work");
				kfree(data);
				continue;
			}

			INIT_WORK(&work->work, rio_cm_handler);
			work->data = data;
			work->cm = cm;
			queue_work(riocm_wq, &work->work);
		}
	}

	if (i == 8)
		tasklet_schedule(&cm->rx_tasklet);
}

static void riocm_inb_msg_event(struct rio_mport *mport, void *dev_id,
			 int mbox, int slot)
{
	struct cm_dev *cm = (struct cm_dev *)dev_id;

	if (rio_mport_is_running(cm->mport))
		tasklet_schedule(&cm->rx_tasklet);
}

static void rio_txcq_handler(struct cm_dev *cm, int slot)
{
	int ack_slot;

	/* FIXME: We do not need TX completion notification until direct buffer
	 * transfer is implemented. At this moment only correct tracking
	 * of tx_count is important.
	 */
	riocm_debug(TX_EVENT, "for mport_%d slot %d tx_cnt %d",
		    cm->mport->id, slot, cm->tx_cnt);

	spin_lock(&cm->tx_lock);
	ack_slot = cm->tx_ack_slot;

	if (ack_slot == slot)
		riocm_debug(TX_EVENT, "slot == ack_slot");

	while (cm->tx_cnt && ((ack_slot != slot) ||
	       (cm->tx_cnt == RIOCM_TX_RING_SIZE))) {

		cm->tx_buf[ack_slot] = NULL;
		++ack_slot;
		ack_slot &= (RIOCM_TX_RING_SIZE - 1);
		cm->tx_cnt--;
	}

	if (cm->tx_cnt < 0 || cm->tx_cnt > RIOCM_TX_RING_SIZE)
		riocm_error("tx_cnt %d out of sync", cm->tx_cnt);

	WARN_ON((cm->tx_cnt < 0) || (cm->tx_cnt > RIOCM_TX_RING_SIZE));

	cm->tx_ack_slot = ack_slot;

	if (!list_empty(&cm->tx_reqs) && (cm->tx_cnt < RIOCM_TX_RING_SIZE)) {
		struct tx_req *req, *_req;
		int rc;

		list_for_each_entry_safe(req, _req, &cm->tx_reqs, node) {
			list_del(&req->node);
			cm->tx_buf[cm->tx_slot] = req->buffer;
			rc = rio_add_outb_message(cm->mport, req->rdev, cmbox,
						  req->buffer, req->len);
			kfree(req->buffer);
			kfree(req);

			++cm->tx_cnt;
			++cm->tx_slot;
			cm->tx_slot &= (RIOCM_TX_RING_SIZE - 1);
			if (cm->tx_cnt == RIOCM_TX_RING_SIZE)
				break;
		}
	}

	spin_unlock(&cm->tx_lock);
}

static void riocm_outb_msg_event(struct rio_mport *mport, void *dev_id,
			  int mbox, int slot)
{
	struct cm_dev *cm = (struct cm_dev *)dev_id;

	if (cm && rio_mport_is_running(cm->mport))
		rio_txcq_handler(cm, slot);
}

static int riocm_post_send(struct cm_dev *cm, struct rio_dev *rdev,
			   void *buffer, size_t len, int req)
{
	int rc;
	unsigned long flags;

	spin_lock_irqsave(&cm->tx_lock, flags);

	if (cm->mport == NULL) {
		rc = -ENODEV;
		goto err_out;
	}

	if (cm->tx_cnt == RIOCM_TX_RING_SIZE) {
		if (req) {
			struct tx_req *treq;

			treq = kzalloc(sizeof(struct tx_req), GFP_ATOMIC);
			if (treq == NULL) {
				rc = -ENOMEM;
				goto err_out;
			}
			treq->rdev = rdev;
			treq->buffer = buffer;
			treq->len = len;
			list_add_tail(&treq->node, &cm->tx_reqs);
		}
		riocm_debug(TX, "Tx Queue is full");
		rc = -EBUSY;
		goto err_out;
	}

	cm->tx_buf[cm->tx_slot] = buffer;
	rc = rio_add_outb_message(cm->mport, rdev, cmbox, buffer, len);

	riocm_debug(TX, "Add buf@%p destid=%x tx_slot=%d tx_cnt=%d",
		 buffer, rdev->destid, cm->tx_slot, cm->tx_cnt);

	++cm->tx_cnt;
	++cm->tx_slot;
	cm->tx_slot &= (RIOCM_TX_RING_SIZE - 1);

err_out:
	spin_unlock_irqrestore(&cm->tx_lock, flags);
	return rc;
}

/*
 * riocm_ch_send - sends a data packet to a remote device
 * @ch_id: local channel ID
 * @buf: pointer to a data buffer to send (including CM header)
 * @len: length of data to transfer (including CM header)
 *
 * ATTN: ASSUMES THAT THE HEADER SPACE IS RESERVED PART OF THE DATA PACKET
 *
 * Returns: 0 if success, or
 *          -EINVAL if one or more input parameters is/are not valid,
 *          -ENODEV if cannot find a channel with specified ID,
 *          -EAGAIN if a channel is not in connected state,
 *	    error codes returned by HW send routine.
 */
static int riocm_ch_send(u16 ch_id, void *buf, int len)
{
	struct rio_channel *ch;
	struct rio_ch_chan_hdr *hdr;
	int ret;

	if (buf == NULL || ch_id == 0 || len == 0 || len > RIO_MAX_MSG_SIZE)
		return -EINVAL;

	ch = riocm_get_channel(ch_id);
	if (!ch)
		return -ENODEV;

	if (!riocm_comp(ch, RIO_CM_CONNECTED)) {
		ret = -EAGAIN;
		goto err_out;
	}

	/*
	 * Fill buffer header section with corresponding channel data
	 */
	hdr = (struct rio_ch_chan_hdr *)buf;

	hdr->bhdr.src_id = htonl(ch->loc_destid);
	hdr->bhdr.dst_id = htonl(ch->rem_destid);
	hdr->bhdr.src_mbox = cmbox;
	hdr->bhdr.dst_mbox = cmbox;
	hdr->bhdr.type = RIO_CM_CHAN;
	hdr->ch_op = CM_DATA_MSG;
	hdr->dst_ch = htons(ch->rem_channel);
	hdr->src_ch = htons(ch->id);
	hdr->msg_len = htons((u16)len);

	/* FIXME: the function call below relies on the fact that underlying
	 * add_outb_message() routine copies TX data into its internal transfer
	 * buffer. Needs to be reviewed if switched to direct buffer version.
	 */

	ret = riocm_post_send(ch->cmdev, ch->rdev, buf, len, 0);
	if (ret)
		riocm_debug(TX, "ch %d send_err=%d", ch->id, ret);
err_out:
	riocm_put_channel(ch);
	return ret;
}

/*
 * riocm_wait_for_rx_data - waits for received data message
 * @ch: channel object
 *
 * ATTN: THIS FUNCTION MUST BE CALLED WITH CHANNEL SPINLOCK HELD BY CALLER.
 *
 * Returns: 0 if success (accept queue is not empty), or
 *          -EINTR if wait was interrupted by signal
 */
static int riocm_wait_for_rx_data(struct rio_channel *ch, long timeout)
__releases(ch->lock)
__acquires(ch->lock)
{
	int err;
	DEFINE_WAIT(wait);

	riocm_debug(WAIT, "on %d", ch->id);

	for (;;) {
		prepare_to_wait_exclusive(&ch->wait_q, &wait,
					  TASK_INTERRUPTIBLE);
		spin_unlock_bh(&ch->lock);
		if (ch->rx_ring.count == 0)
			timeout = schedule_timeout(timeout);

		spin_lock_bh(&ch->lock);

		if (signal_pending(current)) {
			err = -EINTR;
			break;
		}
		if (ch->rx_ring.count) {
			err = 0;
			break;
		}
		if (ch->state != RIO_CM_CONNECTED) {
			err = -ECONNRESET;
			break;
		}
		if (!timeout) {
			err = -ETIME;
			break;
		}
	}

	finish_wait(&ch->wait_q, &wait);
	riocm_debug(WAIT, "on %d returns %d", ch->id, err);
	return err;
}

static int riocm_ch_free_rxbuf(struct rio_channel *ch, void *buf)
{
	int i, ret = -EINVAL;

	spin_lock_bh(&ch->lock);

	for (i = 0; i < RIOCM_RX_RING_SIZE; i++) {
		if (ch->rx_ring.inuse[i] == buf) {
			ch->rx_ring.inuse[i] = NULL;
			ch->rx_ring.inuse_cnt--;
			ret = 0;
			break;
		}
	}

	spin_unlock_bh(&ch->lock);

	if (!ret)
		kfree(buf);

	return ret;
}

static int riocm_ch_receive(struct rio_channel *ch, void **buf, int *len,
			      long timeout)
{
	void *rxmsg = NULL;
	int i, ret = 0;

	if (!riocm_comp(ch, RIO_CM_CONNECTED)) {
		ret = -EAGAIN;
		goto out;
	}

	if (ch->rx_ring.inuse_cnt == RIOCM_RX_RING_SIZE) {
		/* If we do not have entries to track buffers given to upper
		 * layer, reject request.
		 */
		ret = -ENOMEM;
		goto out;
	}

	spin_lock_bh(&ch->lock);

	if (ch->rx_ring.count == 0) {
		ret = riocm_wait_for_rx_data(ch, timeout);
		if (ret)
			goto out_wait;
	}

	rxmsg = ch->rx_ring.buf[ch->rx_ring.tail];
	ch->rx_ring.buf[ch->rx_ring.tail] = NULL;
	ch->rx_ring.count--;
	ch->rx_ring.tail++;
	ch->rx_ring.tail %= RIOCM_RX_RING_SIZE;
	ret = -ENOMEM;

	for (i = 0; i < RIOCM_RX_RING_SIZE; i++) {
		if (ch->rx_ring.inuse[i] == NULL) {
			ch->rx_ring.inuse[i] = rxmsg;
			ch->rx_ring.inuse_cnt++;
			ret = 0;
			break;
		}
	}

	if (ret) {
		/* We have no entry to store pending message: drop it */
		kfree(rxmsg);
		rxmsg = NULL;
	}

out_wait:
	spin_unlock_bh(&ch->lock);
out:
	*buf = rxmsg;
	return ret;
}

/*
 * riocm_wait_for_connect_resp - waits for connect response (ACK/NACK) from
 *                               a remote device
 * @ch: channel object
 * @timeo: timeout value in jiffies
 *
 * ATTN: THIS FUNCTION MUST BE CALLED WITH CHANNEL SPINLOCK HELD BY CALLER.
 *
 * Returns: 0 if success (accept queue is not empty), or
 *          -EINTR if wait was interrupted by signal,
 *          -ETIME if wait timeout expired.
 */
static int riocm_wait_for_connect_resp(struct rio_channel *ch, long timeo)
__releases(ch->lock)
__acquires(ch->lock)
{
	int err;
	DEFINE_WAIT(wait);

	riocm_debug(WAIT, "on %d", ch->id);

	for (;;) {
		prepare_to_wait_exclusive(&ch->wait_q, &wait,
					  TASK_INTERRUPTIBLE);
		spin_unlock_bh(&ch->lock);
		timeo = schedule_timeout(timeo);
		spin_lock_bh(&ch->lock);

		if (signal_pending(current)) {
			err = -EINTR;
			break;
		}
		if (ch->state != RIO_CM_CONNECT) {
			err = 0;
			break;
		}
		if (!timeo) {
			err = -ETIME;
			break;
		}
	}

	finish_wait(&ch->wait_q, &wait);
	riocm_debug(WAIT, "on %d returns %d", ch->id, err);
	return err;
}

/*
 * riocm_ch_connect - sends a connect request to a remote device
 * @loc_ch: local channel ID
 * @mport_id:  corresponding RapidIO mport device
 * @rem_destid: destination ID of target RapidIO device
 * @rem_ch: remote channel ID
 *
 * Returns: 0 if success, or
 *          -ENODEV if cannot find specified channel or mport,
 *          -EINVAL if the channel is not in IDLE state,
 *          -EAGAIN if no connection request available immediately.
 */
static int riocm_ch_connect(u16 loc_ch, u8 mport_id, u32 rem_destid, u16 rem_ch)
{
	struct rio_channel *ch = NULL;
	struct rio_ch_chan_hdr hdr;
	struct cm_dev *cm;
	struct cm_peer *peer;
	int found = 0;
	int ret;

	down_read(&rdev_sem);

	/* Find matching cm_dev object */
	list_for_each_entry(cm, &cm_dev_list, list) {
		if (cm->mport->id == mport_id) {
			found++;
			break;
		}
	}

	if (!found) {
		up_read(&rdev_sem);
		riocm_error("cm_dev not found");
		return -ENODEV;
	}

	/* Find corresponding RapidIO endpoint device object */

	found = 0;

	list_for_each_entry(peer, &cm->peers, node) {
		if (peer->rdev->destid == rem_destid) {
			found++;
			break;
		}
	}

	up_read(&rdev_sem);

	if (!found) {
		riocm_error("Target RapidIO device not found");
		return -ENODEV;
	}

	ch = riocm_get_channel(loc_ch);
	if (!ch)
		return -ENODEV;

	if (!riocm_comp_exch(ch, RIO_CM_IDLE, RIO_CM_CONNECT)) {
		riocm_put_channel(ch);
		return -EINVAL;
	}

	ch->cmdev = cm;
	ch->rdev = peer->rdev;
	ch->context = NULL;
	ch->loc_destid = cm->mport->host_deviceid;
	ch->rem_channel = rem_ch;

	/*
	 * Send connect request to the remote RapidIO device
	 */

	hdr.bhdr.src_id = htonl(ch->loc_destid);
	hdr.bhdr.dst_id = htonl(rem_destid);
	hdr.bhdr.src_mbox = cmbox;
	hdr.bhdr.dst_mbox = cmbox;
	hdr.bhdr.type = RIO_CM_CHAN;
	hdr.ch_op = CM_CONN_REQ;
	hdr.dst_ch = htons(rem_ch);
	hdr.src_ch = htons(loc_ch);

	/* FIXME: the function call below relies on the fact that underlying
	 * add_outb_message() routine copies TX data into its internal transfer
	 * buffer. Needs to be reviewed if switched to direct buffer version.
	 */
	ret = riocm_post_send(cm, peer->rdev, &hdr, sizeof(hdr), 1);

	if (ret && ret != -EBUSY) {
		riocm_comp_exch(ch, RIO_CM_CONNECT, RIO_CM_IDLE);
		goto conn_done;
	}

	/* Wait for connect response from the remote device */
	spin_lock_bh(&ch->lock);

	/* Check if we still in CONNECT state */
	if (ch->state == RIO_CM_CONNECT) {
		ret = riocm_wait_for_connect_resp(ch, RIOCM_CONNECT_TO * HZ);
		if (ret == 0)
			ret = (ch->state == RIO_CM_CONNECTED) ? 0 : -1;
	}

	spin_unlock_bh(&ch->lock);
conn_done:
	riocm_put_channel(ch);
	return ret;
}

/*
 * riocm_wait_for_connect_req - waits for connect request from a remote device
 * @ch: channel object
 * @timeo: timeout value in jiffies
 *
 * ATTN: THIS FUNCTION MUST BE CALLED WITH CHANNEL SPINLOCK HELD BY CALLER.
 *
 * Returns: 0 if success (accept queue is not empty), or
 *          -EINTR if wait was interrupted by signal,
 *          -ETIME if wait timeout expired.
 */
static int riocm_wait_for_connect_req(struct rio_channel *ch, long timeo)
__releases(ch->lock)
__acquires(ch->lock)
{
	int err;
	DEFINE_WAIT(wait);

	riocm_debug(WAIT, "on %d", ch->id);

	for (;;) {
		prepare_to_wait_exclusive(&ch->wait_q, &wait,
					  TASK_INTERRUPTIBLE);
		spin_unlock_bh(&ch->lock);
		if (list_empty(&ch->accept_queue) && ch->state == RIO_CM_LISTEN)
			timeo = schedule_timeout(timeo);

		spin_lock_bh(&ch->lock);
		if (signal_pending(current)) {
			err = -EINTR;
			break;
		}
		if (!list_empty(&ch->accept_queue)) {
			err = 0;
			break;
		}
		if (ch->state != RIO_CM_LISTEN) {
			err = -ECANCELED;
			break;
		}
		if (!timeo) {
			err = -ETIME;
			break;
		}
	}

	finish_wait(&ch->wait_q, &wait);
	riocm_debug(WAIT, "on %d returns %d", ch->id, err);
	return err;
}

static int riocm_send_ack(struct rio_channel *ch)
{
	struct rio_ch_chan_hdr hdr;
	int ret;

	hdr.bhdr.src_id = htonl(ch->loc_destid);
	hdr.bhdr.dst_id = htonl(ch->rem_destid);
	hdr.dst_ch = htons(ch->rem_channel);
	hdr.src_ch = htons(ch->id);
	hdr.bhdr.src_mbox = cmbox;
	hdr.bhdr.dst_mbox = cmbox;
	hdr.bhdr.type = RIO_CM_CHAN;
	hdr.ch_op = CM_CONN_ACK;

	/* FIXME: the function call below relies on the fact that underlying
	 * add_outb_message() routine copies TX data into its internal transfer
	 * buffer. Review if switching to direct buffer version.
	 */
	ret = riocm_post_send(ch->cmdev, ch->rdev, &hdr, sizeof(hdr), 1);
	if (ret == -EBUSY)
		ret = 0;
	if (ret)
		riocm_error("send ACK to ch_%d on %s failed (ret=%d)",
			    ch->id, rio_name(ch->rdev), ret);
	return ret;
}

/*
 * riocm_ch_accept - associate a channel object and an mport device
 * @ch_id: channel ID
 * @new_ch_id: local mport device
 * @timeout: wait timeout (if 0 non-blocking call, do not wait if connection
 *           request is not available).
 *
 * Returns: pointer to new channel struct if success, or error-valued pointer:
 *          -ENODEV if cannot find specified channel or mport,
 *          -EINVAL if the channel is not in IDLE state,
 *          -EAGAIN if no connection request available immediately.
 */
static struct rio_channel *riocm_ch_accept(u16 ch_id, u16 *new_ch_id,
					   long timeout)
{
	struct rio_channel *ch = NULL;
	struct rio_channel *new_ch = NULL;
	struct conn_req *req;
	struct cm_peer *peer;
	int found = 0;
	int err;

	ch = riocm_get_channel(ch_id);
	if (!ch)
		return ERR_PTR(-EINVAL);

	spin_lock_bh(&ch->lock);

	if (ch->state != RIO_CM_LISTEN) {
		err = -EINVAL;
		goto err_out;
	}

	/* Check if we have pending connection request */
	if (list_empty(&ch->accept_queue)) {

		/* Don't sleep if this is a non blocking call */
		if (!timeout) {
			err = -EAGAIN;
			goto err_out;
		}

		err = riocm_wait_for_connect_req(ch, timeout);
		if (err)
			goto err_out;
	}

	req = list_first_entry(&ch->accept_queue, struct conn_req, node);

	/* Create new channel for this connection */
	new_ch = riocm_ch_alloc(RIOCM_CHNUM_AUTO);

	if (IS_ERR(new_ch)) {
		riocm_error("failed to get channel for new req (%ld)",
			PTR_ERR(new_ch));
		err = -ENOMEM;
		goto err_out;
	}

	list_del(&req->node);
	new_ch->cmdev = ch->cmdev;
	new_ch->loc_destid = ch->loc_destid;
	new_ch->rem_destid = req->destid;
	new_ch->rem_channel = req->chan;

	spin_unlock_bh(&ch->lock);
	riocm_put_channel(ch);
	kfree(req);

	down_read(&rdev_sem);
	/* Find requester's device object */
	list_for_each_entry(peer, &new_ch->cmdev->peers, node) {
		if (peer->rdev->destid == new_ch->rem_destid) {
			riocm_debug(RX_CMD, "found matching device(%s)",
				    rio_name(peer->rdev));
			found = 1;
			break;
		}
	}
	up_read(&rdev_sem);

	if (!found) {
		/* If peer device object not found simply ignore the request */
		riocm_put_channel(new_ch);
		err = -ENODEV;
		goto err_nodev;
	}

	new_ch->rdev = peer->rdev;
	new_ch->state = RIO_CM_CONNECTED;
	spin_lock_init(&new_ch->lock);

	/* Acknowledge the connection request. */
	riocm_send_ack(new_ch);

	*new_ch_id = new_ch->id;
	return new_ch;
err_out:
	spin_unlock_bh(&ch->lock);
	riocm_put_channel(ch);
err_nodev:
	*new_ch_id = 0;
	return ERR_PTR(err);
}

/*
 * riocm_ch_listen - puts a channel into LISTEN state
 * @ch_id: channel ID
 *
 * Returns: 0 if success, or
 *          -EINVAL if the specified channel does not exists or
 *                  is not in CHAN_BOUND state.
 */
static int riocm_ch_listen(u16 ch_id)
{
	struct rio_channel *ch = NULL;
	int ret = 0;

	riocm_debug(CHOP, "(ch_%d)", ch_id);

	ch = riocm_get_channel(ch_id);
	if (!ch || !riocm_comp_exch(ch, RIO_CM_CHAN_BOUND, RIO_CM_LISTEN))
		ret = -EINVAL;
	riocm_put_channel(ch);
	return ret;
}

/*
 * riocm_ch_bind - associate a channel object and an mport device
 * @ch_id: channel ID
 * @mport_id: local mport device ID
 * @context: pointer to the additional caller's context (???)
 *
 * Returns: 0 if success, or
 *          -ENODEV if cannot find specified mport,
 *          -EINVAL if the specified channel does not exist or
 *                  is not in IDLE state.
 */
static int riocm_ch_bind(u16 ch_id, u8 mport_id, void *context)
{
	struct rio_channel *ch = NULL;
	struct cm_dev *cm;
	int rc = -ENODEV;

	riocm_debug(CHOP, "ch_%d to mport_%d", ch_id, mport_id);

	/* Find matching cm_dev object */
	down_read(&rdev_sem);
	list_for_each_entry(cm, &cm_dev_list, list) {
		if ((cm->mport->id == mport_id) &&
		     rio_mport_is_running(cm->mport)) {
			rc = 0;
			break;
		}
	}

	if (rc)
		goto exit;

	ch = riocm_get_channel(ch_id);
	if (!ch) {
		rc = -EINVAL;
		goto exit;
	}

	spin_lock_bh(&ch->lock);
	if (ch->state != RIO_CM_IDLE) {
		spin_unlock_bh(&ch->lock);
		rc = -EINVAL;
		goto err_put;
	}

	ch->cmdev = cm;
	ch->loc_destid = cm->mport->host_deviceid;
	ch->context = context;
	ch->state = RIO_CM_CHAN_BOUND;
	spin_unlock_bh(&ch->lock);
err_put:
	riocm_put_channel(ch);
exit:
	up_read(&rdev_sem);
	return rc;
}

/*
 * riocm_ch_alloc - channel object allocation helper routine
 * @ch_num: channel ID (1 ... RIOCM_MAX_CHNUM, 0 = automatic)
 *
 * Return value: pointer to newly created channel object, or error code
 */
static struct rio_channel *riocm_ch_alloc(u16 ch_num)
{
	int id;
	int start, end;
	struct rio_channel *ch;

	ch = kzalloc(sizeof(struct rio_channel), GFP_KERNEL);
	if (!ch)
		return ERR_PTR(-ENOMEM);

	if (ch_num) {
		/* If requested, try to obtain the specified channel ID */
		start = ch_num;
		end = ch_num + 1;
	} else {
		/* Obtain channel ID from the dynamic allocation range */
		start = chstart;
		end = RIOCM_MAX_CHNUM + 1;
	}

	spin_lock_bh(&idr_lock);
	id = idr_alloc(&ch_idr, ch, start, end, GFP_KERNEL);
	spin_unlock_bh(&idr_lock);

	if (id < 0) {
		kfree(ch);
		return ERR_PTR(id == -ENOSPC ? -EBUSY : id);
	}

	ch->id = (u16)id;
	ch->state = RIO_CM_IDLE;
	spin_lock_init(&ch->lock);
	INIT_LIST_HEAD(&ch->accept_queue);
	INIT_LIST_HEAD(&ch->ch_node);
	init_waitqueue_head(&ch->wait_q);
	init_completion(&ch->comp);
	kref_init(&ch->ref);
	ch->rx_ring.head = 0;
	ch->rx_ring.tail = 0;
	ch->rx_ring.count = 0;
	ch->rx_ring.inuse_cnt = 0;

	return ch;
}

/*
 * riocm_ch_create - creates a new channel object and allocates ID for it
 * @ch_num: channel ID (1 ... RIOCM_MAX_CHNUM, 0 = automatic)
 *
 * Allocates and initializes a new channel object. If the parameter ch_num > 0
 * and is within the valid range, riocm_ch_create tries to allocate the
 * specified ID for the new channel. If ch_num = 0, channel ID will be assigned
 * automatically from the range (chstart ... RIOCM_MAX_CHNUM).
 * Module parameter 'chstart' defines start of an ID range available for dynamic
 * allocation. Range below 'chstart' is reserved for pre-defined ID numbers.
 * Available channel numbers are limited by 16-bit size of channel numbers used
 * in the packet header.
 *
 * Return value: 0 if successful (with channel number updated via pointer) or
 *               -1 if error.
 * Return value: PTR to rio_channel structure if successful (with channel number
 *               updated via pointer) or NULL if error.
 */
static struct rio_channel *riocm_ch_create(u16 *ch_num)
{
	struct rio_channel *ch = NULL;

	ch = riocm_ch_alloc(*ch_num);

	if (IS_ERR(ch)) {
		riocm_error("Failed to allocate channel %d (err=%ld)",
			 *ch_num, PTR_ERR(ch));
		return NULL;
	}

	*ch_num = ch->id;
	return ch;
}

/*
 * riocm_ch_free - channel object release routine
 * @ref: pointer to a channel's kref structure
 */
static void riocm_ch_free(struct kref *ref)
{
	struct rio_channel *ch = container_of(ref, struct rio_channel, ref);
	int i;

	riocm_debug(CHOP, "(ch_%d)", ch->id);

	if (ch->rx_ring.inuse_cnt) {
		for (i = 0; i < RIOCM_RX_RING_SIZE; i++) {
			if (ch->rx_ring.inuse[i] != NULL)
				kfree(ch->rx_ring.inuse[i]);
		}
	}

	if (ch->rx_ring.count)
		for (i = 0; i < RIOCM_RX_RING_SIZE; i++)
			if (ch->rx_ring.buf[i] != NULL)
				kfree(ch->rx_ring.buf[i]);

	complete(&ch->comp);
}

static int riocm_send_close(struct rio_channel *ch)
{
	struct rio_ch_chan_hdr *hdr;
	int ret;

	/*
	 * Send CH_CLOSE notification to the remote RapidIO device
	 */

	hdr = kzalloc(sizeof(struct rio_ch_chan_hdr), GFP_KERNEL);
	if (hdr == NULL)
		return -ENOMEM;

	hdr->bhdr.src_id = htonl(ch->loc_destid);
	hdr->bhdr.dst_id = htonl(ch->rem_destid);
	hdr->bhdr.src_mbox = cmbox;
	hdr->bhdr.dst_mbox = cmbox;
	hdr->bhdr.type = RIO_CM_CHAN;
	hdr->ch_op = CM_CONN_CLOSE;
	hdr->dst_ch = htons(ch->rem_channel);
	hdr->src_ch = htons(ch->id);

	/* FIXME: the function call below relies on the fact that underlying
	 * add_outb_message() routine copies TX data into its internal transfer
	 * buffer. Needs to be reviewed if switched to direct buffer version.
	 */
	ret = riocm_post_send(ch->cmdev, ch->rdev, hdr, sizeof(*hdr), 1);
	if (ret == -EBUSY)
		ret = 0;
	else
		kfree(hdr);

	if (ret)
		riocm_error("ch(%d) send CLOSE failed (ret=%d)", ch->id, ret);

	return ret;
}

/*
 * riocm_ch_close - closes a channel object with specified ID (by local request)
 * @ch: channel to be closed
 */
static int riocm_ch_close(struct rio_channel *ch)
{
	unsigned long tmo = msecs_to_jiffies(3000);
	enum rio_cm_state state;
	long wret;
	int ret = 0;

	riocm_debug(CHOP, "(ch_%d)", ch->id);

	state = riocm_exch(ch, RIO_CM_DESTROYING);
	if (state == RIO_CM_CONNECTED)
		riocm_send_close(ch);

	wake_up_all(&ch->wait_q);

	riocm_put_channel(ch);
	wret = wait_for_completion_interruptible_timeout(&ch->comp, tmo);

	if (wret == 0) {
		/* Timeout on wait occurred */
		riocm_debug(CHOP, "%s(%d) timed out waiting for ch %d",
		       current->comm, task_pid_nr(current), ch->id);
		ret = -ETIMEDOUT;
	} else if (wret == -ERESTARTSYS) {
		/* Wait_for_completion was interrupted by a signal */
		riocm_debug(CHOP, "%s(%d) wait for ch %d was interrupted",
			current->comm, task_pid_nr(current), ch->id);
		ret = -EINTR;
	}

	if (!ret) {
		riocm_debug(CHOP, "ch_%d resources released", ch->id);
		kfree(ch);
	} else {
		riocm_debug(CHOP, "failed to release ch_%d resources", ch->id);
	}

	return ret;
}

/*
 * riocm_get_peer_list - report number of remote peer endpoints connected
 *                        to the specified mport device
 * @mport_id: mport device ID
 * @buf: peer list buffer
 * @nent: number of 32-bit entries in the buffer
 */
static int riocm_get_peer_list(u8 mport_id, void *buf, u32 *nent)
{
	struct cm_dev *cm;
	struct cm_peer *peer;
	u32 *entry_ptr = buf;
	int i = 0;

	/* Find a matching cm_dev object */
	down_read(&rdev_sem);
	list_for_each_entry(cm, &cm_dev_list, list)
		if (cm->mport->id == mport_id)
			goto found;

	up_read(&rdev_sem);
	*nent = 0;
	return -ENODEV;

found:
	list_for_each_entry(peer, &cm->peers, node) {
		*entry_ptr = (u32)peer->rdev->destid;
		entry_ptr++;
		if (++i >= *nent)
			break;
	}
	up_read(&rdev_sem);

	*nent = i;
	return 0;
}

/*
 * riocm_cdev_open() - Open character device (mport)
 */
static int riocm_cdev_open(struct inode *inode, struct file *filp)
{
	riocm_debug(INIT, "by filp=%p %s(%d)",
		   filp, current->comm, task_pid_nr(current));

	if (list_empty(&cm_dev_list))
		return -ENODEV;

	return 0;
}

/*
 * riocm_cdev_release() - Release character device
 */
static int riocm_cdev_release(struct inode *inode, struct file *filp)
{
	struct rio_channel *ch, *_c;
	unsigned int i;
	LIST_HEAD(list);

	riocm_debug(EXIT, "by filp=%p %s(%d)",
		   filp, current->comm, task_pid_nr(current));

	/* Check if there are channels associated with this file descriptor */
	spin_lock_bh(&idr_lock);
	idr_for_each_entry(&ch_idr, ch, i) {
		if (ch && ch->filp == filp) {
			riocm_debug(EXIT, "ch_%d not released by %s(%d)",
				    ch->id, current->comm,
				    task_pid_nr(current));
			idr_remove(&ch_idr, ch->id);
			list_add(&ch->ch_node, &list);
		}
	}
	spin_unlock_bh(&idr_lock);

	if (!list_empty(&list)) {
		list_for_each_entry_safe(ch, _c, &list, ch_node) {
			list_del(&ch->ch_node);
			riocm_ch_close(ch);
		}
	}

	return 0;
}

static unsigned int riocm_cdev_poll(struct file *file, poll_table *wait)
{
	unsigned int mask = 0;
#if 0
	mask |= POLLIN | POLLRDNORM;
#endif
	return mask;
}

/*
 * cm_ep_get_list_size() - Reports number of endpoints in the network
 */
static int cm_ep_get_list_size(void __user *arg)
{
	u16 __user *p = arg;
	u32 mport_id;
	u32 count = 0;
	struct cm_dev *cm;

	if (get_user(mport_id, p))
		return -EFAULT;

	/* Find a matching cm_dev object */
	down_read(&rdev_sem);
	list_for_each_entry(cm, &cm_dev_list, list) {
		if (cm->mport->id == mport_id) {
			count = cm->npeers;
			up_read(&rdev_sem);
			if (copy_to_user(arg, &count, sizeof(count)))
				return -EFAULT;
			return 0;
		}
	}
	up_read(&rdev_sem);

	return -ENODEV;
}

/*
 * cm_ep_get_list() - Returns list of attached endpoints
 */
static int cm_ep_get_list(void __user *arg)
{
	int ret = 0;
	uint32_t info[2];
	void *buf;

	if (copy_from_user(&info, arg, sizeof(info)))
		return -EFAULT;

	buf = kcalloc(info[0] + 2, sizeof(u32), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = riocm_get_peer_list(info[1], (u8 *)buf + 2*sizeof(u32), &info[0]);
	if (ret)
		goto out;

	((u32 *)buf)[0] = info[0]; /* report an updated number of entries */
	((u32 *)buf)[1] = info[1]; /* put back an mport ID */
	if (copy_to_user(arg, buf, sizeof(u32) * (info[0] + 2)))
		ret = -EFAULT;
out:
	kfree(buf);
	return ret;
}

/*
 * cm_mport_get_list() - Returns list of attached endpoints
 */
static int cm_mport_get_list(void __user *arg)
{
	int ret = 0;
	uint32_t entries;
	void *buf;
	struct cm_dev *cm;
	u32 *entry_ptr;
	int count = 0;

	if (copy_from_user(&entries, arg, sizeof(entries)))
		return -EFAULT;
	if (entries == 0)
		return -ENOMEM;
	buf = kcalloc(entries + 1, sizeof(u32), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	/* Scan all registered cm_dev objects */
	entry_ptr = (u32 *)((u8 *)buf + sizeof(u32));
	down_read(&rdev_sem);
	list_for_each_entry(cm, &cm_dev_list, list) {
		if (count++ < entries) {
			*entry_ptr = (cm->mport->id << 16) |
				      cm->mport->host_deviceid;
			entry_ptr++;
		}
	}
	up_read(&rdev_sem);

	*((u32 *)buf) = count; /* report a real number of entries */
	if (copy_to_user(arg, buf, sizeof(u32) * (count + 1)))
		ret = -EFAULT;

	kfree(buf);
	return ret;
}

/*
 * cm_chan_create() - Create a message exchange channel
 */
static int cm_chan_create(struct file *filp, void __user *arg)
{
	u16 __user *p = arg;
	u16 ch_num;
	struct rio_channel *ch;

	if (get_user(ch_num, p))
		return -EFAULT;
	ch = riocm_ch_create(&ch_num);
	if (ch == NULL)
		return -1;
	ch->filp = filp;
	riocm_debug(CHOP, "ch_%d by %p", ch_num, filp);
	return put_user(ch_num, p);
}

/*
 * cm_chan_close() - Close channel
 * @filp:	Pointer to file object
 * @arg:	Channel to close
 */
static int cm_chan_close(struct file *filp, void __user *arg)
{
	u16 __user *p = arg;
	u16 ch_num;
	struct rio_channel *ch;

	if (get_user(ch_num, p))
		return -EFAULT;

	riocm_debug(CHOP, "ch_%d by %p", ch_num, filp);

	spin_lock_bh(&idr_lock);
	ch = idr_find(&ch_idr, ch_num);
	if (!ch) {
		spin_unlock_bh(&idr_lock);
		return 0;
	}
	if (ch->filp != filp) {
		spin_unlock_bh(&idr_lock);
		return -EINVAL;
	}
	idr_remove(&ch_idr, ch->id);
	spin_unlock_bh(&idr_lock);

	return riocm_ch_close(ch);
}

/*
 * cm_chan_bind() - Bind channel
 * @arg:	Channel number
 */
static int cm_chan_bind(void __user *arg)
{
	struct rio_cm_channel chan;

	if (copy_from_user(&chan, arg, sizeof(struct rio_cm_channel)))
		return -EFAULT;

	return riocm_ch_bind(chan.id, chan.mport_id, NULL);
}

/*
 * cm_chan_listen() - Listen on channel
 * @arg:	Channel number
 */
static int cm_chan_listen(void __user *arg)
{
	u16 __user *p = arg;
	u16 ch_num;

	if (get_user(ch_num, p))
		return -EFAULT;

	return riocm_ch_listen(ch_num);
}

/*
 * cm_chan_accept() - Accept incoming connection
 * @filp:	Pointer to file object
 * @arg:	Channel number
 */
static int cm_chan_accept(struct file *filp, void __user *arg)
{
	struct rio_cm_accept param;
	long accept_to;
	struct rio_channel *ch;

	if (copy_from_user(&param, arg, sizeof(struct rio_cm_accept)))
		return -EFAULT;

	accept_to = param.wait_to ?
			msecs_to_jiffies(param.wait_to):MAX_SCHEDULE_TIMEOUT;

	ch = riocm_ch_accept(param.ch_num, &param.ch_num, accept_to);
	if (IS_ERR(ch))
		return PTR_ERR(ch);
	ch->filp = filp;

	riocm_debug(CHOP, "new ch_%d for %p", ch->id, filp);
	if (copy_to_user(arg, &param, sizeof(struct rio_cm_accept)))
		return -EFAULT;
	return 0;
}

/*
 * cm_chan_connect() - Connect on channel
 * @arg:	Channel information
 */
static int cm_chan_connect(void __user *arg)
{
	struct rio_cm_channel chan;

	if (copy_from_user(&chan, arg, sizeof(struct rio_cm_channel)))
		return -EFAULT;

	return riocm_ch_connect(chan.id, chan.mport_id,
				chan.remote_destid, chan.remote_channel);
}

/*
 * cm_chan_msg_send() - Connect on channel
 * @arg:	Outbound message information
 */
static int cm_chan_msg_send(void __user *arg)
{
	struct rio_cm_msg msg;
	void *buf;
	int ret = 0;

	if (copy_from_user(&msg, arg, sizeof(struct rio_cm_msg)))
		return -EFAULT;

	buf = kmalloc(RIO_MAX_MSG_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	if (copy_from_user(buf, msg.msg, msg.size)) {
		ret = -EFAULT;
		goto out;
	}

	ret = riocm_ch_send(msg.ch_num, buf, msg.size);
out:
	kfree(buf);
	return ret;
}

/*
 * cm_chan_msg_rcv() - Connect on channel
 * @arg:	Inbound message information
 */
static int cm_chan_msg_rcv(void __user *arg)
{
	struct rio_cm_msg msg;
	struct rio_channel *ch;
	void *buf;
	int msg_len = RIO_MAX_MSG_SIZE;
	long rxto;
	int ret = 0;

	if (copy_from_user(&msg, arg, sizeof(struct rio_cm_msg)))
		return -EFAULT;

	if (msg.ch_num == 0)
		return -EINVAL;

	ch = riocm_get_channel(msg.ch_num);
	if (!ch)
		return -ENODEV;

	rxto = msg.rxto?msecs_to_jiffies(msg.rxto):MAX_SCHEDULE_TIMEOUT;

	ret = riocm_ch_receive(ch, &buf, &msg_len, rxto);
	if (ret)
		goto out;

	 /* check msg.size for max allowed copy size ??? */
	if (copy_to_user(msg.msg, buf, RIO_MAX_MSG_SIZE))
		ret = -EFAULT;

	/* msg.size = RIO_MAX_MSG_SIZE;	*/
	riocm_ch_free_rxbuf(ch, buf);
out:
	riocm_put_channel(ch);
	return ret;
}

/*
 * riocm_cdev_ioctl() - IOCTLs for character device
 */
static long
riocm_cdev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case RIO_CM_EP_GET_LIST_SIZE:
		return cm_ep_get_list_size((void __user *)arg);
	case RIO_CM_EP_GET_LIST:
		return cm_ep_get_list((void __user *)arg);
	case RIO_CM_CHAN_CREATE:
		return cm_chan_create(filp, (void __user *)arg);
	case RIO_CM_CHAN_CLOSE:
		return cm_chan_close(filp, (void __user *)arg);
	case RIO_CM_CHAN_BIND:
		return cm_chan_bind((void __user *)arg);
	case RIO_CM_CHAN_LISTEN:
		return cm_chan_listen((void __user *)arg);
	case RIO_CM_CHAN_ACCEPT:
		return cm_chan_accept(filp, (void __user *)arg);
	case RIO_CM_CHAN_CONNECT:
		return cm_chan_connect((void __user *)arg);
	case RIO_CM_CHAN_SEND:
		return cm_chan_msg_send((void __user *)arg);
	case RIO_CM_CHAN_RECEIVE:
		return cm_chan_msg_rcv((void __user *)arg);
	case RIO_CM_MPORT_GET_LIST:
		return cm_mport_get_list((void __user *)arg);
	default:
		break;
	}

	return -EINVAL;
}

static const struct file_operations riocm_cdev_fops = {
	.owner		= THIS_MODULE,
	.open		= riocm_cdev_open,
	.release	= riocm_cdev_release,
	.poll		= riocm_cdev_poll,
	.unlocked_ioctl = riocm_cdev_ioctl,
};

/*
 * riocm_add_dev - add new remote RapidIO device into channel management core
 * @dev: device object associated with RapidIO device
 * @sif: subsystem interface
 *
 * Adds the specified RapidIO device (if applicable) into peers list of
 * the corresponding channel management device (cm_dev).
 */
static int riocm_add_dev(struct device *dev, struct subsys_interface *sif)
{
	struct cm_peer *peer;
	struct rio_dev *rdev = to_rio_dev(dev);
	struct cm_dev *cm;

	/* Check if the remote device has capabilities required to support CM */
	if (!dev_cm_capable(rdev))
		return 0;

	riocm_debug(RDEV, "(%s)", rio_name(rdev));

	peer = kmalloc(sizeof(struct cm_peer), GFP_KERNEL);
	if (!peer)
		return -ENOMEM;

	/* Find a corresponding cm_dev object */
	down_write(&rdev_sem);
	list_for_each_entry(cm, &cm_dev_list, list) {
		if (cm->mport == rdev->net->hport)
			goto found;
	}

	up_write(&rdev_sem);
	kfree(peer);
	return -ENODEV;

found:
	peer->rdev = rdev;
	list_add_tail(&peer->node, &cm->peers);
	cm->npeers++;

	up_write(&rdev_sem);
	return 0;
}

/*
 * riocm_remove_dev - remove remote RapidIO device from channel management core
 * @dev: device object associated with RapidIO device
 * @sif: subsystem interface
 *
 * Removes the specified RapidIO device (if applicable) from peers list of
 * the corresponding channel management device (cm_dev).
 */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4,3,0))
static void riocm_remove_dev(struct device *dev, struct subsys_interface *sif)
#else
static int riocm_remove_dev(struct device *dev, struct subsys_interface *sif)
#endif
{
	struct rio_dev *rdev = to_rio_dev(dev);
	struct cm_dev *cm;
	struct cm_peer *peer;
	struct rio_channel *ch, *_c;
	unsigned int i;
	bool found = false;
	LIST_HEAD(list);

	/* Check if the remote device has capabilities required to support CM */
	if (!dev_cm_capable(rdev))
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4,3,0))
		return;
#else
		return -ENODEV;
#endif

	riocm_debug(RDEV, "(%s)", rio_name(rdev));

	/* Find matching cm_dev object */
	down_write(&rdev_sem);
	list_for_each_entry(cm, &cm_dev_list, list) {
		if (cm->mport == rdev->net->hport) {
			found = true;
			break;
		}
	}

	if (!found) {
		up_write(&rdev_sem);
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4,3,0))
		return;
#else
		return -ENODEV;
#endif
	}

	/* Remove remote device from the list of peers */
	found = false;
	list_for_each_entry(peer, &cm->peers, node) {
		if (peer->rdev == rdev) {
			riocm_debug(RDEV, "removing peer %s", rio_name(rdev));
			found = true;
			list_del(&peer->node);
			cm->npeers--;
			kfree(peer);
			break;
		}
	}

	up_write(&rdev_sem);

	if (!found)
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4,3,0))
		return;
#else
		return -ENODEV;
#endif

	/*
	 * Release channels associated with this peer
	 */

	spin_lock_bh(&idr_lock);
	idr_for_each_entry(&ch_idr, ch, i) {
		if (ch && ch->rdev == rdev) {
			if (atomic_read(&rdev->state) != RIO_DEVICE_SHUTDOWN)
				riocm_exch(ch, RIO_CM_DISCONNECT);
			idr_remove(&ch_idr, ch->id);
			list_add(&ch->ch_node, &list);
		}
	}
	spin_unlock_bh(&idr_lock);

	if (!list_empty(&list)) {
		list_for_each_entry_safe(ch, _c, &list, ch_node) {
			list_del(&ch->ch_node);
			riocm_ch_close(ch);
		}
	}

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4,3,0))
	return 0;
#endif
}


/*
 * riocm_cdev_add() - Create rio_cm char device
 * @devno: device number assigned to device (MAJ + MIN)
 */
static int riocm_cdev_add(dev_t devno)
{
	int ret;

	cdev_init(&riocm_cdev.cdev, &riocm_cdev_fops);
	riocm_cdev.cdev.owner = THIS_MODULE;
	ret = cdev_add(&riocm_cdev.cdev, devno, 1);
	if (ret < 0) {
		riocm_error("Cannot register a device with error %d", ret);
		return ret;
	}

	riocm_cdev.dev = device_create(dev_class, NULL, devno, NULL, DEV_NAME);
	if (IS_ERR(riocm_cdev.dev)) {
		cdev_del(&riocm_cdev.cdev);
		return PTR_ERR(riocm_cdev.dev);
	}

	riocm_debug(MPORT, "Added %s cdev(%d:%d)",
		    DEV_NAME, MAJOR(devno), MINOR(devno));

	return 0;
}

/*
 * riocm_add_mport - add new local mport device into channel management core
 * @dev: device object associated with mport
 * @class_intf: class interface
 *
 * When a new mport device is added, CM immediately reserves inbound and
 * outbound RapidIO mailboxes that will be used.
 */
static int riocm_add_mport(struct device *dev,
			   struct class_interface *class_intf)
{
	int rc;
	int i;
	struct cm_dev *cm;
	struct rio_mport *mport = to_rio_mport(dev);

	riocm_debug(MPORT, "add mport %s", mport->name);

	cm = kzalloc(sizeof(*cm), GFP_KERNEL);
	if (!cm)
		return -ENOMEM;

	cm->mport = mport;

	rc = rio_request_outb_mbox(mport, (void *)cm, cmbox,
				   RIOCM_TX_RING_SIZE, riocm_outb_msg_event);
	if (rc) {
		riocm_error("failed to allocate OBMBOX_%d on %s",
			    cmbox, mport->name);
		kfree(cm);
		return -ENODEV;
	}

	rc = rio_request_inb_mbox(mport, (void *)cm, cmbox,
				  RIOCM_RX_RING_SIZE, riocm_inb_msg_event);
	if (rc) {
		riocm_error("failed to allocate IBMBOX_%d on %s",
			    cmbox, mport->name);
		rio_release_outb_mbox(mport, cmbox);
		kfree(cm);
		return -ENODEV;
	}

	/*
	 * Allocate and register inbound messaging buffers to be ready
	 * to receive channel and system management requests
	 */
	for (i = 0; i < RIOCM_RX_RING_SIZE; i++)
		cm->rx_buf[i] = NULL;

	cm->rx_slots = RIOCM_RX_RING_SIZE;
	riocm_rx_fill(cm, RIOCM_RX_RING_SIZE);

	cm->tx_slot = 0;
	cm->tx_cnt = 0;
	cm->tx_ack_slot = 0;
	spin_lock_init(&cm->tx_lock);

	tasklet_init(&cm->rx_tasklet, rio_ibmsg_handler, (unsigned long)cm);
	INIT_LIST_HEAD(&cm->peers);
	cm->npeers = 0;
	INIT_LIST_HEAD(&cm->tx_reqs);

	down_write(&rdev_sem);
	list_add_tail(&cm->list, &cm_dev_list);
	up_write(&rdev_sem);

	return 0;
}

/*
 * riocm_remove_mport - remove local mport device from channel management core
 * @dev: device object associated with mport
 * @class_intf: class interface
 *
 * Removes a local mport device from the list of registered devices that provide
 * channel management services. Returns an error if the specified mport is not
 * registered with the CM core.
 */
static void riocm_remove_mport(struct device *dev,
			       struct class_interface *class_intf)
{
	struct rio_mport *mport = to_rio_mport(dev);
	struct cm_dev *cm;
	struct cm_peer *peer, *temp;
	struct rio_channel *ch, *_c;
	unsigned int i;
	bool found = false;
	LIST_HEAD(list);

	riocm_debug(MPORT, "%s", mport->name);

	/* Find a matching cm_dev object */
	down_write(&rdev_sem);
	list_for_each_entry(cm, &cm_dev_list, list) {
		if (cm->mport == mport) {
			list_del(&cm->list);
			found = true;
			break;
		}
	}
	up_write(&rdev_sem);
	if (!found)
		return;

	tasklet_kill(&cm->rx_tasklet);
	tasklet_kill(&cm->tx_tasklet);
	flush_workqueue(riocm_wq);

	/* Release channels bound to this mport */
	spin_lock_bh(&idr_lock);
	idr_for_each_entry(&ch_idr, ch, i) {
		if (ch->cmdev == cm) {
			riocm_debug(RDEV, "%s drop ch_%d",
				    mport->name, ch->id);
			idr_remove(&ch_idr, ch->id);
			list_add(&ch->ch_node, &list);
		}
	}
	spin_unlock_bh(&idr_lock);

	if (!list_empty(&list)) {
		list_for_each_entry_safe(ch, _c, &list, ch_node) {
			list_del(&ch->ch_node);
			riocm_ch_close(ch);
		}
	}

	rio_release_inb_mbox(mport, cmbox);
	rio_release_outb_mbox(mport, cmbox);

	/* Remove and free peer entries */
	if (!list_empty(&cm->peers))
		riocm_debug(RDEV, "ATTN: peer list not empty");
	list_for_each_entry_safe(peer, temp, &cm->peers, node) {
		riocm_debug(RDEV, "removing peer %s", rio_name(peer->rdev));
		list_del(&peer->node);
		kfree(peer);
	}

	riocm_rx_free(cm);
	kfree(cm);
	riocm_debug(MPORT, "%s done", mport->name);
}

static int rio_cm_shutdown(struct notifier_block *nb, unsigned long code,
	void *unused)
{
	struct rio_channel *ch;
	unsigned int i;

	riocm_debug(EXIT, ".");

	spin_lock_bh(&idr_lock);
	idr_for_each_entry(&ch_idr, ch, i) {
		riocm_debug(EXIT, "close ch %d", ch->id);
		if (ch->state == RIO_CM_CONNECTED)
			riocm_send_close(ch);
	}
	spin_unlock_bh(&idr_lock);

	return NOTIFY_DONE;
}

/*
 * riocm_interface handles addition/removal of remote RapidIO devices
 */
static struct subsys_interface riocm_interface = {
	.name		= "rio_cm",
	.subsys		= &rio_bus_type,
	.add_dev	= riocm_add_dev,
	.remove_dev	= riocm_remove_dev,
};

/*
 * rio_mport_interface handles addition/removal local mport devices
 */
static struct class_interface rio_mport_interface __refdata = {
	.class = &rio_mport_class,
	.add_dev = riocm_add_mport,
	.remove_dev = riocm_remove_mport,
};

static struct notifier_block rio_cm_notifier = {
	.notifier_call = rio_cm_shutdown,
};

static int __init riocm_init(void)
{
	int ret;

	/* Create device class needed by udev */
	dev_class = class_create(THIS_MODULE, DRV_NAME);
	if (!dev_class) {
		riocm_error("Cannot create " DRV_NAME " class");
		return -EINVAL;
	}

	ret = alloc_chrdev_region(&dev_number, 0, 1, DRV_NAME);
	if (ret) {
		class_destroy(dev_class);
		return ret;
	}

	dev_major = MAJOR(dev_number);
	dev_minor_base = MINOR(dev_number);
	riocm_debug(INIT, "Registered class with %d major", dev_major);

	riocm_wq = create_singlethread_workqueue("riocm_wq");
	if (!riocm_wq)
		return -ENOMEM;

	/*
	 * Register as rapidio_port class interface to get notifications about
	 * mport additions and removals.
	 */
	ret = class_interface_register(&rio_mport_interface);
	if (ret) {
		riocm_error("class_interface_register error: %d", ret);
		goto err_wq;
	}

	/*
	 * Register as RapidIO bus interface to get notifications about
	 * addition/removal of remote RapidIO devices.
	 */
	ret = subsys_interface_register(&riocm_interface);
	if (ret) {
		riocm_error("subsys_interface_register error: %d", ret);
		goto err_cl;
	}

	ret = register_reboot_notifier(&rio_cm_notifier);
	if (ret) {
		riocm_error("failed to register reboot notifier (err=%d)", ret);
		goto err_sif;
	}

	ret = riocm_cdev_add(dev_number);
	if (ret) {
		unregister_reboot_notifier(&rio_cm_notifier);
		ret = -ENODEV;
		goto err_sif;
	}

	return 0;
err_sif:
	subsys_interface_unregister(&riocm_interface);
err_cl:
	class_interface_unregister(&rio_mport_interface);
err_wq:
	destroy_workqueue(riocm_wq);
	return ret;
}

static void __exit riocm_exit(void)
{
	riocm_debug(EXIT, "enter");
	unregister_reboot_notifier(&rio_cm_notifier);
	subsys_interface_unregister(&riocm_interface);
	class_interface_unregister(&rio_mport_interface);
	destroy_workqueue(riocm_wq);
	idr_destroy(&ch_idr);

	device_unregister(riocm_cdev.dev);
	cdev_del(&(riocm_cdev.cdev));

	class_destroy(dev_class);
	unregister_chrdev_region(dev_number, 1);
}

late_initcall(riocm_init);
module_exit(riocm_exit);