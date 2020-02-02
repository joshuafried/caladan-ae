/*
 * RPC server-side support
 */

#include <base/stddef.h>
#include <base/list.h>
#include <base/log.h>
#include <runtime/rpc.h>
#include <runtime/tcp.h>
#include <runtime/sync.h>
#include <runtime/smalloc.h>
#include <runtime/thread.h>
#include <runtime/runtime.h>

#include "util.h"

/* the maximum supported window size */
#define SRPC_MAX_WINDOW		64
/* the minimum runtime queuing delay */
#define SRPC_MIN_DELAY_US	20
/* the maximum runtime queuing delay */
#define SRPC_MAX_DELAY_US	60

/* the handler function for each RPC */
static srpc_fn_t srpc_handler;
/* spin lock for srpc_drained list */
static DEFINE_SPINLOCK(drained_lock);
/* a list of zero window sessions */
static LIST_HEAD(srpc_drained);

struct srpc_session {
	tcpconn_t		*c;
	struct list_node	drained_link;
	bool			is_drained;
	bool			need_winupdate;
	waitgroup_t		send_waiter;
	uint32_t		win;

	/* shared state between receiver and sender */
	DEFINE_BITMAP(avail_slots, SRPC_MAX_WINDOW);

	/* shared state between workers and sender */
	spinlock_t		lock;
	int			closed;
	thread_t		*sender_th;
	DEFINE_BITMAP(completed_slots, SRPC_MAX_WINDOW);

	/* worker slots (one for each credit issued) */
	struct srpc_ctx		*slots[SRPC_MAX_WINDOW];
};

static int srpc_get_slot(struct srpc_session *s)
{
	int slot = __builtin_ffsl(s->avail_slots[0]) - 1;
	if (slot >= 0) {
		bitmap_atomic_clear(s->avail_slots, slot);
		s->slots[slot] = smalloc(sizeof(struct srpc_ctx));
		s->slots[slot]->s = s;
		s->slots[slot]->idx = slot;
	}
	return slot;
}

static void srpc_put_slot(struct srpc_session *s, int slot)
{
	sfree(s->slots[slot]);
	s->slots[slot] = NULL;
	bitmap_atomic_set(s->avail_slots, slot);
}

static int srpc_wakeup(struct srpc_session *s)
{
	struct srpc_hdr shdr;
	int ret;
	ssize_t pkt_len = sizeof(struct srpc_hdr);

	assert(s->win == 1);

	/* craft the response header */
	shdr.magic = RPC_RESP_MAGIC;
	shdr.op = RPC_OP_WINUPDATE;
	shdr.len = 0;
	shdr.win = s->win;

	/* send the packet */
	ret = tcp_write_full(s->c, &shdr, pkt_len);
	if (unlikely(ret < 0))
		return ret;

	return 0;
}

static void srpc_update_window(struct srpc_session *s)
{
	uint64_t us = runtime_standing_queue_us();
	float alpha;

	if (s->need_winupdate) {
		s->win = 1;
		return;
	}

	/* Don't update the window if session is on the
	 * drained list */
	if (s->win == 0) {
		return;
	}

	/* update window (currently DCTCP-like) */
	if (us >= SRPC_MIN_DELAY_US) {
		us = MIN(SRPC_MAX_DELAY_US, us);
		alpha = (float)(us - SRPC_MIN_DELAY_US) /
			(float)(SRPC_MAX_DELAY_US - SRPC_MIN_DELAY_US);
		s->win = (float)s->win * (1.0 - alpha / 2.0);
	} else {
		s->win++;
	}

	/* clamp to supported values */
	/* now we allow zero window */
	s->win = MAX(s->win, 0);
	s->win = MIN(s->win, SRPC_MAX_WINDOW - 1);
}

static void srpc_worker(void *arg)
{
	struct srpc_ctx *c = (struct srpc_ctx *)arg;
	struct srpc_session *s = c->s;
	thread_t *th;

	srpc_handler(c);

	spin_lock_np(&s->lock);
	bitmap_set(s->completed_slots, c->idx);
	th = s->sender_th;
	s->sender_th = NULL;
	spin_unlock_np(&s->lock);
	if (th)
		thread_ready(th);
}

static int srpc_recv_one(struct srpc_session *s)
{
	struct crpc_hdr chdr;
	int idx, ret;

	/* read the client header */
	ret = tcp_read_full(s->c, &chdr, sizeof(chdr));
	if (unlikely(ret <= 0)) {
		if (ret == 0)
			return -EIO;
		return ret;
	}

	/* parse the client header */
	if (unlikely(chdr.magic != RPC_REQ_MAGIC)) {
		log_warn("srpc: got invalid magic %x", chdr.magic);
		return -EINVAL;
	}
	if (unlikely(chdr.len > SRPC_BUF_SIZE)) {
		log_warn("srpc: request len %ld too large (limit %d)",
			 chdr.len, SRPC_BUF_SIZE);
		return -EINVAL;
	}
	if (unlikely(chdr.op != RPC_OP_CALL)) {
		log_warn("srpc: got invalid op %d", chdr.op);
		return -EINVAL;
	}

	/* reserve a slot */
	idx = srpc_get_slot(s);
	if (unlikely(idx < 0)) {
		log_warn("srpc: client tried to use more than %d slots",
			 SRPC_MAX_WINDOW);
		return -ENOENT;
	}

	/* retrieve the payload */
	ret = tcp_read_full(s->c, s->slots[idx]->req_buf, chdr.len);
	if (unlikely(ret <= 0)) {
		if (ret == 0)
			return -EIO;
		return ret;
	}
	s->slots[idx]->req_len = chdr.len;
	s->slots[idx]->resp_len = 0;
	ret = thread_spawn(srpc_worker, s->slots[idx]);
	BUG_ON(ret);
	return ret;
}

static int srpc_send_one(struct srpc_session *s, struct srpc_ctx *c)
{
	struct srpc_hdr *shdr = &c->resp_hdr;
	int ret;
	ssize_t pkt_len = sizeof(struct srpc_hdr) + c->resp_len;

	/* must have a response payload */
	if (unlikely(c->resp_len == 0))
		return -EINVAL;

	/* craft the response header */
	shdr->magic = RPC_RESP_MAGIC;
	shdr->op = RPC_OP_CALL;
	shdr->len = c->resp_len;
	shdr->win = s->win;

	/* send the packet */
	ret = tcp_write_full(s->c, shdr, pkt_len);
	if (unlikely(ret < 0))
		return ret;

	return 0;
}

static void srpc_sender(void *arg)
{
	DEFINE_BITMAP(tmp, SRPC_MAX_WINDOW);
	struct srpc_session *s = (struct srpc_session *)arg;
	struct srpc_session *wakes;
	int ret, i;
	bool sleep;
	bool need_winupdate;
	thread_t *th;

	while (true) {
		/* find slots that have completed */
		spin_lock_np(&s->lock);
		while (true) {
			need_winupdate = s->need_winupdate;
			sleep = !s->closed && !need_winupdate &&
				bitmap_popcount(s->completed_slots,
						SRPC_MAX_WINDOW) == 0;
			if (!sleep) {
				s->sender_th = NULL;
				break;
			}
			s->sender_th = thread_self();
			thread_park_and_unlock_np(&s->lock);
			spin_lock_np(&s->lock);
		}
		if (unlikely(s->closed)) {
			spin_unlock_np(&s->lock);
			break;
		}
		memcpy(tmp, s->completed_slots, sizeof(tmp));
		bitmap_init(s->completed_slots, SRPC_MAX_WINDOW, false);
		srpc_update_window(s);
		s->need_winupdate = false;
		spin_unlock_np(&s->lock);

		/* yield a window to another session if
		 * (1) it has window more than 1,
		 * (2) drained list is not empty */
		wakes = NULL;
		spin_lock_np(&drained_lock);
		if (s->win > 1 && !list_empty(&srpc_drained)) {
			s->win--;
			assert(s->win >= 1);
			wakes = list_pop(&srpc_drained, struct srpc_session,
					 drained_link);
			assert(wakes->is_drained == true);
			assert(wakes->drained_link.prev != NULL);
			assert(wakes->drained_link.next != NULL);
			wakes->drained_link.prev = NULL;
			wakes->drained_link.next = NULL;
		}
		spin_unlock_np(&drained_lock);

		if (s == wakes) {
			s->win++;
			wakes = NULL;
		}

		/* send a response for each completed slot */
		bitmap_for_each_set(tmp, SRPC_MAX_WINDOW, i) {
			ret = srpc_send_one(s, s->slots[i]);
			srpc_put_slot(s, i);
			if (unlikely(ret)) {
				bitmap_atomic_or(s->avail_slots, tmp,
						 SRPC_MAX_WINDOW);
				goto close;
			}
		}

		/* Send WINUPDATE message */
		if (need_winupdate) {
			s->is_drained = false;
			ret = srpc_wakeup(s);
			if (unlikely(ret))
				goto close;
		}

		/* add to the drained list if (1) window becomes zero,
		 * (2) s is not in the list already,
		 * (3) this is the last iteration */
		spin_lock_np(&drained_lock);
		if (s->win == 0 && !s->is_drained &&
		    bitmap_popcount(s->avail_slots, SRPC_MAX_WINDOW) ==
		    SRPC_MAX_WINDOW) {
			assert(s->drained_link.prev == NULL &&
			       s->drained_link.next == NULL);
			s->is_drained = true;
			list_add_tail(&srpc_drained, &s->drained_link);
		}
		spin_unlock_np(&drained_lock);

		/* wake up the session */
		if (wakes) {
			spin_lock_np(&wakes->lock);
			assert(wakes->win == 0);
			th = wakes->sender_th;
			wakes->sender_th = NULL;
			wakes->need_winupdate = true;
			spin_unlock_np(&wakes->lock);

			if (th)
				thread_ready(th);
		}
	}

close:
	/* wait for in-flight completions to finish */
	spin_lock_np(&s->lock);
	while (!s->closed ||
	       bitmap_popcount(s->avail_slots, SRPC_MAX_WINDOW) +
	       bitmap_popcount(s->completed_slots, SRPC_MAX_WINDOW) <
	       SRPC_MAX_WINDOW) {
		s->sender_th = thread_self();
		thread_park_and_unlock_np(&s->lock);
		spin_lock_np(&s->lock);
	}
	spin_unlock_np(&s->lock);

	/* free any remaining slots */
	bitmap_for_each_set(s->completed_slots, SRPC_MAX_WINDOW, i)
		srpc_put_slot(s, i);

	/* notify server thread that the sender is done */
	waitgroup_done(&s->send_waiter);
}

static void srpc_server(void *arg)
{
	tcpconn_t *c = (tcpconn_t *)arg;
	struct srpc_session *s;
	thread_t *th;
	int ret;

	s = smalloc(sizeof(*s));
	BUG_ON(!s);
	memset(s, 0, sizeof(*s));
	s->c = c;
	s->win = 1;
	bitmap_init(s->avail_slots, SRPC_MAX_WINDOW, true);
	waitgroup_init(&s->send_waiter);
	waitgroup_add(&s->send_waiter, 1);

	ret = thread_spawn(srpc_sender, s);
	BUG_ON(ret);

	while (true) {
		ret = srpc_recv_one(s);
		if (ret)
			break;
	}

	spin_lock_np(&s->lock);
	th = s->sender_th;
	s->sender_th = NULL;
	s->closed = true;
	spin_unlock_np(&s->lock);

	if (th)
		thread_ready(th);

	waitgroup_wait(&s->send_waiter);
	tcp_close(c);

	/* remove the session from the drained list */
	spin_lock_np(&drained_lock);
	if (s->drained_link.prev != NULL ||
	    s->drained_link.next != NULL) {
		list_del_from(&srpc_drained, &s->drained_link);
		s->is_drained = false;
		s->drained_link.prev = NULL;
		s->drained_link.next = NULL;
	}
	spin_unlock_np(&drained_lock);

	sfree(s);
}

static void srpc_listener(void *arg)
{
	struct netaddr laddr;
	tcpconn_t *c;
	tcpqueue_t *q;
	int ret;

	laddr.ip = 0;
	laddr.port = SRPC_PORT;

	ret = tcp_listen(laddr, 4096, &q);
	BUG_ON(ret);

	while (true) {
		ret = tcp_accept(q, &c);
		if (WARN_ON(ret))
			continue;
		ret = thread_spawn(srpc_server, c);
		WARN_ON(ret);
	}
}

/**
 * srpc_enable - starts the RPC server
 * @handler: the handler function to call for each RPC.
 *
 * Returns 0 if successful.
 */
int srpc_enable(srpc_fn_t handler)
{
	static DEFINE_SPINLOCK(l);
	int ret;

	spin_lock_np(&l);
	if (srpc_handler) {
		spin_unlock_np(&l);
		return -EBUSY;
	}
	srpc_handler = handler;
	spin_unlock_np(&l);

	ret = thread_spawn(srpc_listener, NULL);
	BUG_ON(ret);
	return 0;
}
