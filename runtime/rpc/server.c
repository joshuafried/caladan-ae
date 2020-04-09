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
#define SRPC_MIN_DELAY_US	20
/* the maximum runtime queuing delay */
#define SRPC_MAX_DELAY_US	60
/* round trip time in us */
#define SRPC_RTT_US		10

#define SRPC_TRACK_FLOW		false
#define SRPC_TRACK_FLOW_ID	1

#define BUFFER_SIZE_EXP		15
#define BUFFER_SIZE		(1 << BUFFER_SIZE_EXP)
#define BUFFER_MASK		(BUFFER_SIZE - 1)

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

atomic_t srpc_num_pending;

/* drained session list */
struct srpc_drained_ {
	spinlock_t lock;
	struct list_head list;
	void *pad[5];
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
	uint64_t		last_demand_timestamp;
	uint64_t		last_winupdate_timestamp;

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
	ssize_t ret;

	bitmap_for_each_set(slots, SRPC_MAX_WINDOW, i) {
		struct srpc_ctx *c = s->slots[i];

		shdr[nrhdr].magic = RPC_RESP_MAGIC;
		shdr[nrhdr].op = RPC_OP_CALL;
		shdr[nrhdr].len = c->resp_len;
		shdr[nrhdr].win = (uint64_t)s->win;

		v[nriov].iov_base = &shdr[nrhdr];
		v[nriov].iov_len = sizeof(struct srpc_hdr);
		nrhdr++;
		nriov++;

		BUG_ON(c->resp_len == 0);

		v[nriov].iov_base = c->resp_buf;
		v[nriov++].iov_len = c->resp_len;
	}

	if (nriov == 0)
		return 0;

	/* send the completion(s) */
	ret = tcp_writev_full(s->c, v, nriov);
	bitmap_for_each_set(slots, SRPC_MAX_WINDOW, i)
		srpc_put_slot(s, i);

#if SRPC_TRACK_FLOW
	if (s->id == SRPC_TRACK_FLOW_ID) {
		printf("[%lu] <=== Response (%d): win=%lu\n",
			microtime(), nrhdr, shdr.win);
	}
#endif
	atomic_sub_and_fetch(&srpc_num_pending, nrhdr);
	atomic64_fetch_and_add(&srpc_stat_resp_tx_, nrhdr);
	atomic64_fetch_and_add(&srpc_stat_win_tx_, s->win * nrhdr);

	if (unlikely(ret < 0))
		return ret;
	return 0;
}

static void srpc_update_window(struct srpc_session *s)
{
	int win_avail = atomic_read(&srpc_win_avail);
	int win_used = atomic_read(&srpc_win_used);
	int num_active = atomic_read(&srpc_num_active);
	int guaranteed_win;
	int old_win = s->win;
	int win_diff;
	int open_window;

	assert_spin_lock_held(&s->lock);

	if (s->num_pending + s->demand == 0){
		s->win = 0;
		goto finish;
	}

	assert(num_active > 0);
	guaranteed_win = win_avail / num_active;

	// Start with guaranteed win
	s->win = MAX(s->win, guaranteed_win);

	if (win_used < win_avail) {
		open_window = win_avail - win_used;
		s->win = MIN(s->num_pending + s->demand, s->win + open_window);
	}

	if (s->wake_up || num_active <= runtime_max_cores())
		s->win = MAX(s->win, 1);

	/* clamp to supported values */
	/* now we allow zero window */
	s->win = MAX(s->win, s->num_pending);
	s->win = MIN(s->win, SRPC_MAX_WINDOW - 1);
	s->win = MIN(s->win, s->num_pending + s->demand);

finish:
	win_diff = s->win - old_win;
	atomic_fetch_and_add(&srpc_win_used, win_diff);
#if SRPC_TRACK_FLOW
	if (s->id == SRPC_TRACK_FLOW_ID) {
		printf("[%lu] window update: win_avail = %d, win_used = %d, num_pending = %d, demand = %d, num_active = %d, guaranteed_win = %d, old_win = %d, new_win = %d\n",
		       microtime(), win_avail, win_used, s->num_pending, s->demand, num_active, guaranteed_win, old_win, s->win);
	}
#endif
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

static void srpc_remove_from_drained_list(struct srpc_session *s)
{
	assert_spin_lock_held(&s->lock);

	if (s->drained_core == -1)
		return;

	spin_lock_np(&srpc_drained[s->drained_core].lock);
	if (s->is_linked) {
		list_del_from(&srpc_drained[s->drained_core].list,
			      &s->drained_link);
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
	uint64_t old_demand;
	int old_active;
	int new_active;
	int num_pending;
	int win_diff;

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

		spin_lock_np(&s->lock);
		old_demand = s->demand;
		s->demand = chdr.demand;
		s->last_demand_timestamp = microtime();
		srpc_remove_from_drained_list(s);
		if (s->num_pending + old_demand == 0)
			atomic_inc(&srpc_num_active);
		s->num_pending++;
		/* adjust window if demand changed */
		if (s->win > s->num_pending + s->demand) {
			win_diff = s->win - (s->num_pending + s->demand);
			s->win = s->num_pending + s->demand;
			atomic_sub_and_fetch(&srpc_win_used, win_diff);
		}
		spin_unlock_np(&s->lock);

		atomic_inc(&srpc_num_pending);
		ret = thread_spawn(srpc_worker, s->slots[idx]);
		BUG_ON(ret);

		atomic64_inc(&srpc_stat_req_rx_);
#if SRPC_TRACK_FLOW
		uint64_t now = microtime();
		if (s->id == SRPC_TRACK_FLOW_ID) {
			printf("[%lu] ===> Request: demand=%lu, delay=%lu\n",
			       now, chdr.demand, now - s->last_winupdate_timestamp);
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
		s->last_demand_timestamp = microtime();

		if (old_demand > 0 && s->demand == 0) {
			srpc_remove_from_drained_list(s);
		} else if (old_demand == 0 && s->demand > 0 &&
			   s->drained_core == -1) {
			th = s->sender_th;
			s->sender_th = NULL;
			s->need_winupdate = true;
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
		if (old_active == 0 && new_active > 0)
			atomic_inc(&srpc_num_active);
		else if (old_active > 0 && new_active == 0)
			atomic_dec(&srpc_num_active);
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
		memcpy(tmp, s->completed_slots, sizeof(tmp));
		bitmap_init(s->completed_slots, SRPC_MAX_WINDOW, false);
		demand = s->demand;

		if (s->wake_up)
			srpc_remove_from_drained_list(s);

		drained_core = s->drained_core;
		num_resp = bitmap_popcount(tmp, SRPC_MAX_WINDOW);
		s->num_pending -= num_resp;
		srpc_update_window(s);

		win = s->win;

		num_pending = s->num_pending;

		send_winupdate = s->need_winupdate && num_resp == 0 &&
			s->advertised_win < s->win;

		if (num_resp > 0 || send_winupdate)
			s->advertised_win = s->win;

		s->need_winupdate = false;
		s->wake_up = false;

		if (send_winupdate)
			s->last_winupdate_timestamp = microtime();
		if (num_resp > 0 && s->num_pending + s->demand == 0)
			atomic_dec(&srpc_num_active);
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
		if (win == 0 && demand > 0 && drained_core == -1 &&
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
	if (s->num_pending + s->demand > 0)
		atomic_dec(&srpc_num_active);
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
		assert(atomic_read(&srpc_num_active) == 0);
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
	struct srpc_session *ds;
	unsigned int max_cores = runtime_max_cores();
	unsigned int core_id, i;
	thread_t *th;

	while (true) {
		timer_sleep(SRPC_RTT_US);
		us = runtime_queue_us();
		new_win = atomic_read(&srpc_win_avail);

		if (us >= SRPC_MIN_DELAY_US) {
			us = MIN(SRPC_MAX_DELAY_US, us);
			alpha = (float)(us - SRPC_MIN_DELAY_US) /
				(float)(SRPC_MAX_DELAY_US - SRPC_MIN_DELAY_US);
			new_win = (int)(new_win * (1.0 - alpha / 2.0));
		} else {
			new_win++;
		}

		new_win = MAX(new_win, max_cores);
		new_win = MIN(new_win, atomic_read(&srpc_num_active) << SRPC_MAX_WINDOW_EXP);

		// Wake up threads from drained list
		win_used = atomic_read(&srpc_win_used);
		win_open = new_win - win_used;
		core_id = get_current_affinity();

		while (win_open > 0) {
			ds = srpc_choose_drained_session(core_id);

			i = (core_id + 1) % max_cores;
			while (!ds && i != core_id) {
				ds = srpc_choose_drained_session(i);
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
		list_head_init(&srpc_drained[i].list);
	}

	atomic_write(&srpc_num_sess, 0);
	atomic_write(&srpc_num_drained, 0);
	atomic_write(&srpc_num_active, 0);

	atomic_write(&srpc_win_avail, runtime_max_cores());
	atomic_write(&srpc_win_used, 0);
	atomic_write(&srpc_num_pending, 0);

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

uint64_t srpc_stat_resp_tx()
{
	return atomic64_read(&srpc_stat_resp_tx_);
}
