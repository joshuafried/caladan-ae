/*
 * RPC server-side support
 */

#include <stdio.h>

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
#include <runtime/timer.h>
#include <runtime/runtime.h>

#include "util.h"
#include "proto.h"

/* the maximum supported window size */
#define SRPC_MAX_WINDOW_EXP	6
#define SRPC_MAX_WINDOW		64
/* the minimum runtime queuing delay */
#define SRPC_MIN_DELAY_US	80
#define SRPC_DROP_THRESH	160
/* round trip time in us */
#define SRPC_RTT_US		10

#define SRPC_AI			0.002
#define SRPC_MD			0.2

#define SRPC_TRACK_FLOW		false
#define SRPC_TRACK_FLOW_ID	1

#define BUFFER_SIZE_EXP		15
#define BUFFER_SIZE		(1 << BUFFER_SIZE_EXP)
#define BUFFER_MASK		(BUFFER_SIZE - 1)

#define EWMA_WEIGHT		0.1f

BUILD_ASSERT((1 << SRPC_MAX_WINDOW_EXP) == SRPC_MAX_WINDOW);

int nextIndex = 0;
FILE *signal_out = NULL;

struct Event {
	uint64_t timestamp;
	int win_avail;
	int win_used;
	int num_pending;
	int num_drained;
	int num_active;
	int num_sess;
	uint64_t delay;
	int num_cores;
};

/* the handler function for each RPC */
static srpc_fn_t srpc_handler;

/* total number of session */
atomic_t srpc_num_sess;

/* the number of drained session */
atomic_t srpc_num_drained;

/* the number of active sessions */
atomic_t srpc_num_active;

/* global window available */
atomic_t srpc_win_avail;

/* global window used */
atomic_t srpc_win_used;

/* the number of pending requests */
atomic_t srpc_num_pending;

/* average service time in us */
int srpc_avg_st;

/* drained session list */
struct srpc_drained_ {
	spinlock_t lock;
	struct list_head list_p;
	struct list_head list_u;
	void *pad[3];
};

BUILD_ASSERT(sizeof(struct srpc_drained_) == CACHE_LINE_SIZE);

static struct srpc_drained_ srpc_drained[NCPU]
		__attribute__((aligned(CACHE_LINE_SIZE)));

static struct Event events[BUFFER_SIZE];

struct srpc_session {
	int			id;
	tcpconn_t		*c;
	struct list_node	drained_link;
	/* drained_list's core number. -1 if not in the drained list */
	int			drained_core;
	bool			is_linked;
	bool			wake_up;
	waitgroup_t		send_waiter;
	int			win;
	int			advertised_win;
	int			num_pending;
	bool			need_winupdate;
	uint64_t		demand;
	uint64_t		last_winupdate_timestamp;
	bool			demand_sync;

	/* shared state between receiver and sender */
	DEFINE_BITMAP(avail_slots, SRPC_MAX_WINDOW);

	/* shared statnhocho@hp159.utah.cloudlab.use between workers and sender */
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
atomic64_t srpc_stat_req_dropped_;
atomic64_t srpc_stat_resp_tx_;

static void printRecord()
{
	int i;

	if (!signal_out)
		signal_out = fopen("signal.csv", "w");

	for (i = 0; i < BUFFER_SIZE; ++i) {
		struct Event *event = &events[i];
		fprintf(signal_out, "%lu,%d,%d,%d,%d,%d,%d,%lu,%d\n",
			event->timestamp, event->win_avail,
			event->win_used, event->num_pending,
			event->num_drained, event->num_active,
			event->num_sess, event->delay,
			event->num_cores);
	}
	fflush(signal_out);
}

static void record(int win_avail, uint64_t delay)
{
	struct Event *event = &events[nextIndex];
	nextIndex = (nextIndex + 1) & BUFFER_MASK;

	event->timestamp = microtime();
	event->win_avail = win_avail;
	event->win_used = atomic_read(&srpc_win_used);
	event->num_pending = atomic_read(&srpc_num_pending);
	event->num_drained = atomic_read(&srpc_num_drained);
	event->num_active = atomic_read(&srpc_num_active);
	event->num_sess = atomic_read(&srpc_num_sess);
	event->delay = delay;
	event->num_cores = runtime_active_cores();

	if (nextIndex == 0)
		printRecord();
}

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

static int srpc_send_completion_vector(struct srpc_session *s,
				       unsigned long *slots)
{
	struct srpc_hdr shdr[SRPC_MAX_WINDOW];
	struct iovec v[SRPC_MAX_WINDOW * 2];
	int nriov = 0;
	int nrhdr = 0;
	int i;
	ssize_t ret = 0;
	int num_drop = 0;

	bitmap_for_each_set(slots, SRPC_MAX_WINDOW, i) {
		struct srpc_ctx *c = s->slots[i];

		shdr[nrhdr].magic = RPC_RESP_MAGIC;
		shdr[nrhdr].op = RPC_OP_CALL;
		shdr[nrhdr].len = c->resp_len;
		shdr[nrhdr].id = c->id;
		shdr[nrhdr].win = (uint64_t)s->win;

		v[nriov].iov_base = &shdr[nrhdr];
		v[nriov].iov_len = sizeof(struct srpc_hdr);
		nrhdr++;
		nriov++;

		if (c->resp_len > 0) {
			v[nriov].iov_base = c->resp_buf;
			v[nriov++].iov_len = c->resp_len;
		}
	}

	/* send the completion(s) */
	if (nriov == 0)
		return 0;
	ret = tcp_writev_full(s->c, v, nriov);
	bitmap_for_each_set(slots, SRPC_MAX_WINDOW, i)
		srpc_put_slot(s, i);

#if SRPC_TRACK_FLOW
	if (s->id == SRPC_TRACK_FLOW_ID) {
		printf("[%lu] <=== Response (%d): win=%d\n",
			microtime(), nrhdr, s->win);
	}
#endif
	atomic_sub_and_fetch(&srpc_num_pending, nrhdr);
	atomic64_fetch_and_add(&srpc_stat_resp_tx_, nrhdr);
	atomic64_fetch_and_add(&srpc_stat_win_tx_, s->win * nrhdr);

	if (unlikely(ret < 0))
		return ret;
	return 0;
}

static void srpc_update_window(struct srpc_session *s, bool req_dropped)
{
	int win_avail = atomic_read(&srpc_win_avail);
	int win_used = atomic_read(&srpc_win_used);
	int num_sess = atomic_read(&srpc_num_sess);
	int old_win = s->win;
	int win_diff;
	int open_window;
	int max_overprovision;

	assert_spin_lock_held(&s->lock);

	if (s->drained_core != -1)
		return;

	open_window = win_avail - win_used;
	max_overprovision = ((int)(open_window / num_sess), 1);
	if (win_used < win_avail) {
		s->win = MIN(s->num_pending + s->demand + max_overprovision,
			     s->win + open_window);
	} else if (win_used > win_avail) {
		s->win--;
	}

	if (s->wake_up || num_sess <= runtime_max_cores())
		s->win = MAX(s->win, max_overprovision);

	// prioritize the session
	if (old_win > 0 && s->win == 0 && !req_dropped && !s->demand_sync)
		s->win = max_overprovision;

	/* clamp to supported values */
	/* now we allow zero window */
	s->win = MAX(s->win, s->num_pending);
	s->win = MIN(s->win, SRPC_MAX_WINDOW - 1);

	/*
	if (s->demand_sync)
		s->win = MIN(s->win, s->num_pending + s->demand);
	else*/
	s->win = MIN(s->win, s->num_pending + s->demand + max_overprovision);

finish:
	win_diff = s->win - old_win;
	atomic_fetch_and_add(&srpc_win_used, win_diff);
#if SRPC_TRACK_FLOW
	if (s->id == SRPC_TRACK_FLOW_ID) {
		printf("[%lu] window update: win_avail = %d, win_used = %d, req_dropped = %d, num_pending = %d, demand = %d, num_sess = %d, old_win = %d, new_win = %d\n",
		       microtime(), win_avail, win_used, req_dropped, s->num_pending, s->demand, num_sess, old_win, s->win);
	}
#endif
}

static struct srpc_session *srpc_choose_drained_session_p(int core_id)
{
	struct srpc_session *ret;
	uint64_t now;

	assert(core_id >= 0);
	assert(core_id < runtime_max_cores());

again:
	ret = NULL;

	if (list_empty(&srpc_drained[core_id].list_p))
		return NULL;

	spin_lock_np(&srpc_drained[core_id].lock);
	if (list_empty(&srpc_drained[core_id].list_p)) {
		spin_unlock_np(&srpc_drained[core_id].lock);
		return NULL;
	}

	ret = list_pop(&srpc_drained[core_id].list_p,
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
		printf("[%lu] Session waken up\n", now);
	}
#endif

	return ret;
}

static struct srpc_session *srpc_choose_drained_session_u(int core_id)
{
	struct srpc_session *ret;
	uint64_t now;

	assert(core_id >= 0);
	assert(core_id < runtime_max_cores());

again:
	ret = NULL;

	if (list_empty(&srpc_drained[core_id].list_u))
		return NULL;

	spin_lock_np(&srpc_drained[core_id].lock);
	if (list_empty(&srpc_drained[core_id].list_u)) {
		spin_unlock_np(&srpc_drained[core_id].lock);
		return NULL;
	}

	ret = list_pop(&srpc_drained[core_id].list_u,
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
		printf("[%lu] Session waken up\n", now);
	}
#endif

	return ret;
}

static void srpc_remove_from_drained_list(struct srpc_session *s)
{
	assert_spin_lock_held(&s->lock);

	if (s->drained_core == -1)
		return;

	spin_lock_np(&srpc_drained[s->drained_core].lock);
	if (s->is_linked) {
		list_del(&s->drained_link);
		s->is_linked = false;
		atomic_dec(&srpc_num_drained);
#if SRPC_TRACK_FLOW
		if (s->id == SRPC_TRACK_FLOW_ID) {
			printf("[%lu] Seesion is removed from drained list\n",
			       microtime());
		}
#endif
	}
	spin_unlock_np(&srpc_drained[s->drained_core].lock);
	s->drained_core = -1;
}

static void srpc_worker(void *arg)
{
	struct srpc_ctx *c = (struct srpc_ctx *)arg;
	struct srpc_session *s = c->s;
	uint64_t st;
	thread_t *th;
/*
	uint64_t us = runtime_queue_us();

	if (us >= SRPC_DROP_THRESH) {
		c->resp_len = 0;
		c->drop = true;
#if SRPC_TRACK_FLOW
		if (s->id == SRPC_TRACK_FLOW_ID) {
			printf("[%lu] Request dropped: delay = %lu\n",
			       microtime(), us);
		}
#endif
		atomic64_inc(&srpc_stat_req_dropped_);
		goto done;
	}
*/
	c->drop = false;
	srpc_handler(c);
	st = microtime() - st;

	srpc_avg_st = (int)((1 - EWMA_WEIGHT) * srpc_avg_st + EWMA_WEIGHT * st);
done:
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
	uint64_t old_demand;
	int old_active;
	int new_active;
	int num_pending;
	int win_diff;
	unsigned int core_id;
	char buf_tmp[SRPC_BUF_SIZE];

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
		atomic64_inc(&srpc_stat_req_rx_);
		/* reserve a slot */
		idx = srpc_get_slot(s);
		if (unlikely(idx < 0)) {
			tcp_read_full(s->c, buf_tmp, chdr.len);
			atomic64_inc(&srpc_stat_req_dropped_);
			return 0;
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
		s->slots[idx]->id = chdr.id;

		spin_lock_np(&s->lock);
		old_demand = s->demand;
		s->demand = chdr.demand;
		s->demand_sync = chdr.sync;
		srpc_remove_from_drained_list(s);
		s->num_pending++;
		/* adjust window if demand changed */
		if (s->win > s->num_pending + s->demand) {
			win_diff = s->win - (s->num_pending + s->demand);
			s->win = s->num_pending + s->demand;
			atomic_sub_and_fetch(&srpc_win_used, win_diff);
		}

		atomic_inc(&srpc_num_pending);

		if (runtime_queue_us() >= SRPC_DROP_THRESH) {
			thread_t *th;

			s->slots[idx]->drop = true;
			bitmap_set(s->completed_slots, idx);
			th = s->sender_th;
			s->sender_th = NULL;
			spin_unlock_np(&s->lock);
			if (th)
				thread_ready(th);
			atomic64_inc(&srpc_stat_req_dropped_);
			goto again;
		}

		spin_unlock_np(&s->lock);

		ret = thread_spawn(srpc_worker, s->slots[idx]);
		BUG_ON(ret);

#if SRPC_TRACK_FLOW
		uint64_t now = microtime();
		if (s->id == SRPC_TRACK_FLOW_ID) {
			printf("[%lu] ===> Request: id=%lu, demand=%lu, delay=%lu\n",
			       now, chdr.id, chdr.demand, now - s->last_winupdate_timestamp);
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
		old_demand = s->demand;
		s->demand = chdr.demand;
		s->demand_sync = chdr.sync;
		core_id = get_current_affinity();

		if (old_demand > 0 && s->demand == 0) {
			srpc_remove_from_drained_list(s);
		} else if (old_demand == 0 && s->demand > 0) {
			srpc_remove_from_drained_list(s);
			if (s->num_pending == 0) {
				th = s->sender_th;
				s->sender_th = NULL;
				s->need_winupdate = true;
			}
		}

		old_active = s->num_pending + old_demand;
		new_active = s->num_pending + chdr.demand;

		if (s->demand == 0)
			s->advertised_win = 0;

		/* adjust window if demand changed */
		if (s->win > s->num_pending + s->demand) {
			win_diff = s->win - (s->num_pending + s->demand);
			s->win = s->num_pending + s->demand;
			atomic_sub_and_fetch(&srpc_win_used, win_diff);
		}
		spin_unlock_np(&s->lock);

		if (th)
			thread_ready(th);

		atomic64_inc(&srpc_stat_winu_rx_);
#if SRPC_TRACK_FLOW
		if (s->id == SRPC_TRACK_FLOW_ID) {
			printf("[%lu] ===> Winupdate: demand=%lu, \n",
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

static void srpc_sender(void *arg)
{
	DEFINE_BITMAP(tmp, SRPC_MAX_WINDOW);
	struct srpc_session *s = (struct srpc_session *)arg;
	int ret, i;
	bool sleep;
	int num_resp;
	unsigned int core_id;
	unsigned int max_cores = runtime_max_cores();
	uint64_t demand;
	bool send_winupdate;
	int drained_core;
	int win;
	int num_pending;
	bool req_dropped;

	while (true) {
		/* find slots that have completed */
		spin_lock_np(&s->lock);
		while (true) {
			sleep = !s->closed && !s->need_winupdate && !s->wake_up &&
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
		req_dropped = false;
		memcpy(tmp, s->completed_slots, sizeof(tmp));
		bitmap_init(s->completed_slots, SRPC_MAX_WINDOW, false);
		demand = s->demand;

		bitmap_for_each_set(tmp, SRPC_MAX_WINDOW, i) {
			struct srpc_ctx *c = s->slots[i];
			if (c->drop) {
				req_dropped = true;
				break;
			}
		}

		if (s->wake_up)
			srpc_remove_from_drained_list(s);

		drained_core = s->drained_core;
		num_resp = bitmap_popcount(tmp, SRPC_MAX_WINDOW);
		s->num_pending -= num_resp;
		srpc_update_window(s, req_dropped);

		win = s->win;

		num_pending = s->num_pending;

		send_winupdate = (s->need_winupdate || s->wake_up) &&
			num_resp == 0 && s->advertised_win < s->win;

		if (num_resp > 0 || send_winupdate)
			s->advertised_win = s->win;

		s->need_winupdate = false;
		s->wake_up = false;

		if (send_winupdate)
			s->last_winupdate_timestamp = microtime();
		spin_unlock_np(&s->lock);

		/* Send WINUPDATE message */
		if (send_winupdate) {
			ret = srpc_winupdate(s);
			if (unlikely(ret))
				goto close;
			continue;
		}

		/* send a response for each completed slot */
		ret = srpc_send_completion_vector(s, tmp);
		core_id = get_current_affinity();

		/* add to the drained list if (1) window becomes zero,
		 * (2) s is not in the list already,
		 * (3) it has no outstanding requests */
		if (win == 0 && drained_core == -1 &&
		    bitmap_popcount(s->avail_slots, SRPC_MAX_WINDOW) ==
		    SRPC_MAX_WINDOW) {
			spin_lock_np(&s->lock);
			if (!s->demand_sync || s->demand > 0) {
				spin_lock_np(&srpc_drained[core_id].lock);
				assert(!s->is_linked);
				BUG_ON(s->win > 0);
				if (!s->demand_sync) {
					list_add_tail(&srpc_drained[core_id].list_u,
						      &s->drained_link);
				} else if (s->demand > 0) {
					list_add_tail(&srpc_drained[core_id].list_p,
						      &s->drained_link);
				} else {
					printf("oops!\n");
				}
				s->is_linked = true;
				spin_unlock_np(&srpc_drained[core_id].lock);
				s->drained_core = core_id;
				atomic_inc(&srpc_num_drained);
			}
			spin_unlock_np(&s->lock);
#if SRPC_TRACK_FLOW
			if (s->id == SRPC_TRACK_FLOW_ID) {
				printf("[%lu] Session is drained: win=%d, drained_core = %d\n",
				       microtime(), win, s->drained_core);
			}
#endif
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
	srpc_remove_from_drained_list(s);
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
	s->id = atomic_fetch_and_add(&srpc_num_sess, 1) + 1;
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
	if (s->is_linked)
		srpc_remove_from_drained_list(s);
	atomic_sub_and_fetch(&srpc_win_used, s->win);
	atomic_sub_and_fetch(&srpc_num_pending, s->num_pending);
	s->num_pending = 0;
	s->demand = 0;
	s->win = 0;
	spin_unlock_np(&s->lock);

	if (th)
		thread_ready(th);

	atomic_dec(&srpc_num_sess);
	waitgroup_wait(&s->send_waiter);
	tcp_close(c);
	sfree(s);

	/* initialize windows */
	if (atomic_read(&srpc_num_sess) == 0) {
		assert(atomic_read(&srpc_win_used) == 0);
		assert(atomic_read(&srpc_num_drained) == 0);
		atomic_write(&srpc_win_used, 0);
		atomic_write(&srpc_win_avail, runtime_max_cores());
		fflush(stdout);
	}
}

static void srpc_cc_worker(void *arg)
{
	uint64_t us;
	float alpha;
        int new_win;
	int win_used;
	int win_open;
	int num_sess;
	struct srpc_session *ds;
	unsigned int max_cores = runtime_max_cores();
	unsigned int core_id, i;
	thread_t *th;
	bool have_p;

	while (true) {
		timer_sleep(SRPC_RTT_US);
		us = runtime_queue_us();
		new_win = atomic_read(&srpc_win_avail);
		num_sess = atomic_read(&srpc_num_sess);

		if (us >= SRPC_MIN_DELAY_US) {
			alpha = (us - SRPC_MIN_DELAY_US) / (float)SRPC_MIN_DELAY_US;
			alpha = alpha * SRPC_MD;
			alpha = MAX(1.0 - alpha, 0.5);

			new_win = (int)(new_win * alpha);
		} else {
			/*
			have_p = false;
			for(i = 0; i < max_cores; ++i) {
				if (!list_empty(&srpc_drained[i].list_p)) {
					have_p = true;
					break;
				}
			}
			if (have_p)
				new_win += 1;
			else
				new_win += MAX((int)(num_sess / 500), 1);
			*/
			new_win += MAX((int)(num_sess * SRPC_AI), 1);
		}

		new_win = MAX(new_win, max_cores);
		new_win = MIN(new_win, atomic_read(&srpc_num_sess) << SRPC_MAX_WINDOW_EXP);

		// Wake up threads from drained list
		win_used = atomic_read(&srpc_win_used);
		win_open = new_win - win_used;
		core_id = get_current_affinity();

		while (win_open > 0) {
/*
			ds = srpc_choose_drained_session_p(core_id);

			i = (core_id + 1) % max_cores;
			while (!ds && i != core_id) {
				ds = srpc_choose_drained_session_p(i);
				i = (i + 1) % max_cores;
			}

			if (!ds) {
				ds = srpc_choose_drained_session_u(core_id);
			}
*/
			ds = srpc_choose_drained_session_u(core_id);

			i = (core_id + 1) % max_cores;
			while (!ds && i != core_id) {
				ds = srpc_choose_drained_session_u(i);
				i = (i + 1) % max_cores;
			}

			if (!ds)
				break;

			spin_lock_np(&ds->lock);
			BUG_ON(ds->win > 0);
			th = ds->sender_th;
			ds->sender_th = NULL;
			ds->wake_up = true;
			ds->win = 1;
			spin_unlock_np(&ds->lock);

			atomic_inc(&srpc_win_used);

			if (th)
				thread_ready(th);
			win_open--;
		}

		atomic_write(&srpc_win_avail, new_win);

		//record(new_win, us);
	}
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
		list_head_init(&srpc_drained[i].list_p);
		list_head_init(&srpc_drained[i].list_u);
	}

	atomic_write(&srpc_num_sess, 0);
	atomic_write(&srpc_num_drained, 0);

	atomic_write(&srpc_win_avail, runtime_max_cores());
	atomic_write(&srpc_win_used, 0);
	atomic_write(&srpc_num_pending, 0);
	srpc_avg_st = 0;

	/* init stats */
	atomic64_write(&srpc_stat_winu_rx_, 0);
	atomic64_write(&srpc_stat_winu_tx_, 0);
	atomic64_write(&srpc_stat_req_rx_, 0);
	atomic64_write(&srpc_stat_resp_tx_, 0);

	laddr.ip = 0;
	laddr.port = SRPC_PORT;

	ret = tcp_listen(laddr, 4096, &q);
	BUG_ON(ret);

	ret = thread_spawn(srpc_cc_worker, NULL);
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

uint64_t srpc_stat_req_dropped()
{
	return atomic64_read(&srpc_stat_req_dropped_);
}

uint64_t srpc_stat_resp_tx()
{
	return atomic64_read(&srpc_stat_resp_tx_);
}
