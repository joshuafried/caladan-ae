/*
 * RPC server-side support
 */

#include <base/atomic.h>
#include <base/stddef.h>
#include <base/time.h>
#include <base/list.h>
#include <base/log.h>
#include <runtime/rpc.h>
#include <runtime/tcp.h>
#include <runtime/sync.h>
#include <runtime/smalloc.h>
#include <runtime/thread.h>
#include <runtime/runtime.h>

#include "util.h"
#include "proto.h"

/* the maximum supported window size */
#define SRPC_MAX_WINDOW		64
/* the minimum runtime queuing delay */
#define SRPC_MIN_DELAY_US	20
/* the maximum runtime queuing delay */
#define SRPC_MAX_DELAY_US	60
/* round trip time in us */
#define SRPC_RTT_US		10

#define SRPC_TRACK_FLOW		false
#define SRPC_TRACK_FLOW_ID	0

/* the handler function for each RPC */
static srpc_fn_t srpc_handler;

/* total number of session */
atomic_t srpc_num_sess;

/* the number of drained session */
atomic_t srpc_num_drained;

/* drained session list */
struct srpc_drained_ {
	spinlock_t lock;
	struct list_head list;
	void *pad[5];
};

BUILD_ASSERT(sizeof(struct srpc_drained_) == CACHE_LINE_SIZE);

static struct srpc_drained_ srpc_drained[NCPU]
		__attribute__((aligned(CACHE_LINE_SIZE)));

struct srpc_session {
#if SRPC_TRACK_FLOW
	int			id;
#endif
	tcpconn_t		*c;
	struct list_node	drained_link;
	/* drained_list's core number. -1 if not in the drained list */
	int			drained_core;
	bool			is_linked;
	bool			need_winupdate;
	bool			wake_up;
	waitgroup_t		send_waiter;
	double			win;
	uint64_t		demand;
	uint64_t		last_demand_timestamp;

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

/* credit-related stats */
atomic64_t srpc_stat_winu_rx_;
atomic64_t srpc_stat_winu_tx_;
atomic64_t srpc_stat_win_tx_;
atomic64_t srpc_stat_req_rx_;
atomic64_t srpc_stat_resp_tx_;

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

static int srpc_winupdate(struct srpc_session *s)
{
	struct srpc_hdr shdr;
	int ret;

	/* craft the response header */
	shdr.magic = RPC_RESP_MAGIC;
	shdr.op = RPC_OP_WINUPDATE;
	shdr.len = 0;
	shdr.win = (uint64_t)s->win;

	/* send the packet */
	ret = tcp_write_full(s->c, &shdr, sizeof(shdr));
	if (unlikely(ret < 0))
		return ret;

	atomic64_inc(&srpc_stat_winu_tx_);
	atomic64_fetch_and_add(&srpc_stat_win_tx_, shdr.win);
#if SRPC_TRACK_FLOW
	if (s->id == SRPC_TRACK_FLOW_ID) {
		printf("[%lu] <== Winupdate: win = %lu\n",
		       microtime(), shdr.win);
	}
#endif
	return 0;
}

static void srpc_update_window(struct srpc_session *s)
{
	uint64_t us = runtime_queue_us();
	float alpha;
	int win = (int)s->win;

	/* Don't update the window if session is on the
	 * drained list */
	if (s->is_linked) {
		assert(s->win < 1.0);
		return;
	}

	/* update window (currently DCTCP-like) */
	if (us >= SRPC_MIN_DELAY_US) {
		us = MIN(SRPC_MAX_DELAY_US, us);
		alpha = (float)(us - SRPC_MIN_DELAY_US) /
			(float)(SRPC_MAX_DELAY_US - SRPC_MIN_DELAY_US);
		s->win = s->win * (1.0 - alpha / 2.0);
	} else {
		if (win == 0)
			s->win = 1.0;
		else
			s->win += 1.0 / (float)win;
	}

	/* clamp to supported values */
	/* now we allow zero window */
	s->win = MAX(s->win, 0.0);
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
	thread_t *th;

again:
	th = NULL;
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

	switch (chdr.op) {
	case RPC_OP_CALL:
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
			srpc_put_slot(s, idx);
			if (ret == 0)
				return -EIO;
			return ret;
		}
		s->slots[idx]->req_len = chdr.len;
		s->slots[idx]->resp_len = 0;
		ret = thread_spawn(srpc_worker, s->slots[idx]);
		BUG_ON(ret);
		spin_lock_np(&s->lock);
		s->demand = chdr.demand;
		s->last_demand_timestamp = microtime();
		if (s->drained_core >= 0) {
			spin_lock_np(&srpc_drained[s->drained_core].lock);
			if (s->is_linked) {
				list_del_from(&srpc_drained[s->drained_core].list,
					      &s->drained_link);
				s->is_linked = false;
				atomic_dec(&srpc_num_drained);
			}
			spin_unlock_np(&srpc_drained[s->drained_core].lock);
			s->drained_core = -1;
		}
		spin_unlock_np(&s->lock);

		atomic64_inc(&srpc_stat_req_rx_);

#if SRPC_TRACK_FLOW
		uint64_t now = microtime();
		if (s->id == SRPC_TRACK_FLOW_ID) {
			printf("[%lu] ===> Request: demand=%lu\n",
			       now, chdr.demand);
		}
#endif
		break;

	case RPC_OP_WINUPDATE:
		if (unlikely(chdr.len != 0)) {
			log_warn("srpc: winupdate has nonzero len");
			return -EINVAL;
		}
		assert(chdr.len == 0);

		spin_lock_np(&s->lock);
		if (s->drained_core == -1) {
			th = s->sender_th;
			s->sender_th = NULL;
			s->need_winupdate = true;
		}

		s->demand = chdr.demand;
		s->last_demand_timestamp = microtime();
		spin_unlock_np(&s->lock);

		if (chdr.demand == 0)
			panic("winupdate with zero demand\n");

		if (th)
			thread_ready(th);

		atomic64_inc(&srpc_stat_winu_rx_);

#if SRPC_TRACK_FLOW
		if (s->id == SRPC_TRACK_FLOW_ID) {
			printf("[%lu] ===> Winupdate: demand=%lu\n",
			       microtime(), chdr.demand);
		}
#endif
		goto again;
	default:
		log_warn("srpc: got invalid op %d", chdr.op);
		return -EINVAL;
	}

	return ret;
}

static int srpc_send_one(struct srpc_session *s, struct srpc_ctx *c)
{
	struct iovec vec[2];
	struct srpc_hdr shdr;
	int ret;

	/* must have a response payload */
	if (unlikely(c->resp_len == 0))
		return -EINVAL;

	/* craft the response header */
	shdr.magic = RPC_RESP_MAGIC;
	shdr.op = RPC_OP_CALL;
	shdr.len = c->resp_len;
	shdr.win = (uint64_t)s->win;

	/* initialize the SG vector */
	vec[0].iov_base = &shdr;
	vec[0].iov_len = sizeof(shdr);
	vec[1].iov_base = c->resp_buf;
	vec[1].iov_len = c->resp_len;

	/* send the packet */
	ret = tcp_writev_full(s->c, vec, 2);
	if (unlikely(ret < 0))
		return ret;

	assert(ret == sizeof(shdr) + c->resp_len);

	atomic64_inc(&srpc_stat_resp_tx_);
	atomic64_fetch_and_add(&srpc_stat_win_tx_, shdr.win);
#if SRPC_TRACK_FLOW
	if (s->id == SRPC_TRACK_FLOW_ID) {
		printf("[%lu] <=== Response: win=%lu\n",
			microtime(), shdr.win);
	}
#endif
	return 0;
}

static struct srpc_session *srpc_choose_drained_session(int core_id)
{
	struct srpc_session *ret;
	uint64_t now;

	assert(core_id >= 0);
	assert(core_id < runtime_max_cores());

again:
	ret = NULL;

	if (list_empty(&srpc_drained[core_id].list))
		return NULL;

	spin_lock_np(&srpc_drained[core_id].lock);
	if (list_empty(&srpc_drained[core_id].list)) {
		spin_unlock_np(&srpc_drained[core_id].lock);
		return NULL;
	}

	ret = list_pop(&srpc_drained[core_id].list,
		       struct srpc_session,
		       drained_link);
	assert(ret->is_linked);
	ret->is_linked = false;
	spin_unlock_np(&srpc_drained[core_id].lock);
	spin_lock_np(&ret->lock);
	ret->drained_core = -1;
	spin_unlock_np(&ret->lock);
	atomic_dec(&srpc_num_drained);
	now = microtime();
#if SRPC_TRACK_FLOW
	if (ret->id == SRPC_TRACK_FLOW_ID) {
		printf("[%lu] Session waken up: %lu\n",
			now, now - ret->last_demand_timestamp);
	}
#endif

	if (now - ret->last_demand_timestamp > 80) {
		goto again;
	}

	return ret;
}

static void srpc_sender(void *arg)
{
	DEFINE_BITMAP(tmp, SRPC_MAX_WINDOW);
	struct srpc_session *s = (struct srpc_session *)arg;
	struct srpc_session *wakes;
	int ret, i;
	bool sleep;
	bool need_winupdate;
	bool wake_up;
	thread_t *th;
	int max_cores = runtime_max_cores();
	unsigned int core_id;
	int num_resp;
	uint64_t demand;
	int drained_core;

	while (true) {
		/* find slots that have completed */
		spin_lock_np(&s->lock);
		while (true) {
			need_winupdate = s->need_winupdate;
			wake_up = s->wake_up;
			sleep = !s->closed && !need_winupdate && !wake_up &&
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
		demand = s->demand;

		if (s->wake_up && s->drained_core >= 0) {
			spin_lock_np(&srpc_drained[s->drained_core].lock);
			if (s->is_linked) {
				list_del_from(&srpc_drained[s->drained_core].list,
					      &s->drained_link);
				s->is_linked = false;
			}
			spin_unlock_np(&srpc_drained[s->drained_core].lock);
			s->drained_core = -1;
		}

		drained_core = s->drained_core;
		s->need_winupdate = false;
		s->wake_up = false;
		spin_unlock_np(&s->lock);
		num_resp = bitmap_popcount(tmp, SRPC_MAX_WINDOW);

		if (wake_up) {
			s->win = 1.0;
			ret = srpc_winupdate(s);
			if (unlikely(ret))
				goto close;
			continue;
		}

		/* initialize the window to zero */
		if (need_winupdate)
			s->win = 0.0;

		srpc_update_window(s);

		/* decide whether and which session to wake up */
		core_id = get_current_affinity();
		wakes = NULL;

		if (s->win >= 2.0 || (s->win >= 1.0 && demand == 0)) {
			/* try local list */
			wakes = srpc_choose_drained_session(core_id);

			/* try to steal from other cores */
			i = (core_id + 1) % max_cores;
			while (!wakes && i != core_id) {
				wakes = srpc_choose_drained_session(i);
				i = (i + 1) % max_cores;
			}
		}

		if (s == wakes)
			wakes = NULL;

		if (wakes) {
			s->win -= 1.0;
		}

		if (demand == 0)
			s->win = 0.0;

		/* send a response for each completed slot */
		bitmap_for_each_set(tmp, SRPC_MAX_WINDOW, i) {
			ret = srpc_send_one(s, s->slots[i]);
			if (unlikely(ret)) {
				bitmap_atomic_or(s->avail_slots, tmp,
						 SRPC_MAX_WINDOW);
				goto close;
			}
			srpc_put_slot(s, i);
		}

		/* Send WINUPDATE message */
		if (need_winupdate && num_resp == 0 && s->win >= 1.0) {
			ret = srpc_winupdate(s);
			if (unlikely(ret))
				goto close;
		}

		/* add to the drained list if (1) window becomes zero,
		 * (2) s is not in the list already,
		 * (3) it has no outstanding requests */
		if (s->win < 1.0 && demand > 0 && drained_core == -1 &&
		    bitmap_popcount(s->avail_slots, SRPC_MAX_WINDOW) ==
		    SRPC_MAX_WINDOW) {
			spin_lock_np(&s->lock);
			spin_lock_np(&srpc_drained[core_id].lock);
			assert(!s->is_linked);
			list_add_tail(&srpc_drained[core_id].list,
				      &s->drained_link);
			s->is_linked = true;
			spin_unlock_np(&srpc_drained[core_id].lock);
			s->drained_core = core_id;
			spin_unlock_np(&s->lock);
			atomic_inc(&srpc_num_drained);
#if SRPC_TRACK_FLOW
			if (s->id == SRPC_TRACK_FLOW_ID) {
				printf("[%lu] Session is drained: win=%lf, drained_core = %d\n",
				       microtime(), s->win, s->drained_core);
			}
#endif
		}

		/* wake up the session */
		if (wakes) {
			spin_lock_np(&wakes->lock);
			assert(wakes->win < 1.0);
			th = wakes->sender_th;
			wakes->sender_th = NULL;
			wakes->wake_up = true;
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
		s->sender_th = NULL;
	}

	/* remove from the drained list */
	if (s->drained_core >= 0) {
		spin_lock_np(&srpc_drained[s->drained_core].lock);
		if (s->is_linked) {
			list_del_from(&srpc_drained[s->drained_core].list,
				      &s->drained_link);
			s->is_linked = false;
			atomic_dec(&srpc_num_drained);
		}
		spin_unlock_np(&srpc_drained[s->drained_core].lock);
		s->drained_core = -1;
	}
	spin_unlock_np(&s->lock);

	/* free any left over slots */
	for (i = 0; i < SRPC_MAX_WINDOW; i++) {
		if (s->slots[i])
			srpc_put_slot(s, i);
	}

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
	s->drained_core = -1;
#if SRPC_TRACK_FLOW
	s->id = atomic_fetch_and_add(&srpc_num_sess, 1);
#else
	atomic_inc(&srpc_num_sess);
#endif
	bitmap_init(s->avail_slots, SRPC_MAX_WINDOW, true);
	waitgroup_init(&s->send_waiter);
	waitgroup_add(&s->send_waiter, 1);

#if SRPC_TRACK_FLOW
	if (s->id == SRPC_TRACK_FLOW_ID) {
		printf("[%lu] connection established.\n",
		       microtime());
	}
#endif

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
	atomic_dec(&srpc_num_sess);
	tcp_close(c);
	sfree(s);
}

static void srpc_listener(void *arg)
{
	struct netaddr laddr;
	tcpconn_t *c;
	tcpqueue_t *q;
	int ret;
	int i;

	for (i = 0 ; i < NCPU ; ++i) {
		spin_lock_init(&srpc_drained[i].lock);
		list_head_init(&srpc_drained[i].list);
	}

	atomic_write(&srpc_num_sess, 0);
	atomic_write(&srpc_num_drained, 0);

	/* init stats */
	atomic64_write(&srpc_stat_winu_rx_, 0);
	atomic64_write(&srpc_stat_winu_tx_, 0);
	atomic64_write(&srpc_stat_req_rx_, 0);
	atomic64_write(&srpc_stat_resp_tx_, 0);

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

uint64_t srpc_stat_winu_rx()
{
	return atomic64_read(&srpc_stat_winu_rx_);
}

uint64_t srpc_stat_winu_tx()
{
	return atomic64_read(&srpc_stat_winu_tx_);
}

uint64_t srpc_stat_win_tx()
{
	return atomic64_read(&srpc_stat_win_tx_);
}

uint64_t srpc_stat_req_rx()
{
	return atomic64_read(&srpc_stat_req_rx_);
}

uint64_t srpc_stat_resp_tx()
{
	return atomic64_read(&srpc_stat_resp_tx_);
}
