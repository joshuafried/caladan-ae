/*
 * softirq.c - handles high priority events (timers, ingress packets, etc.)
 */

#include <base/stddef.h>
#include <base/log.h>
#include <runtime/thread.h>
#include <runtime/timer.h>

#include "defs.h"
#include "net/defs.h"
#include "net/verbs.h"

DECLARE_PERTHREAD(struct tcache_perthread, thread_pt);

struct softirq_work {
	unsigned int recv_cnt, join_cnt, timeout_cnt;
	struct rx_net_hdr *recv_reqs[SOFTIRQ_MAX_BUDGET];
	struct kthread *join_reqs[SOFTIRQ_MAX_BUDGET];
	struct timer_entry *timeouts[SOFTIRQ_MAX_BUDGET];
};

static void softirq_fn(void *arg)
{
	struct softirq_work *w = arg;
	int i;

	/* deliver new RX packets to the runtime */
	net_rx_softirq(w->recv_reqs, w->recv_cnt);

	/* handle any pending timeouts */
	for (i = 0; i < w->timeout_cnt; i++)
		w->timeouts[i]->fn(w->timeouts[i]->arg);

	/* join parked kthreads */
	for (i = 0; i < w->join_cnt; i++)
		join_kthread(w->join_reqs[i]);
}

static unsigned int poll_bundle(struct softirq_work *w, struct io_bundle *b, unsigned int budget)
{
	unsigned int recv_cnt, timeout_cnt;

	assert_preempt_disabled();

	BUG_ON(budget > SOFTIRQ_MAX_BUDGET - w->recv_cnt);
	BUG_ON(budget > SOFTIRQ_MAX_BUDGET - w->timeout_cnt);

	if (unlikely(!budget))
		return 0;

	if (unlikely(!verbs_has_rx_packets(&b->rxq) && !timer_needed(b)))
		return 0;

	if (unlikely(!spin_try_lock(&b->lock)))
		return 0;

	recv_cnt = verbs_gather_rx(w->recv_reqs + w->recv_cnt, b, budget);
	timeout_cnt = timer_gather(b, budget - recv_cnt, w->timeouts + w->timeout_cnt);

	spin_unlock(&b->lock);

	w->timeout_cnt += timeout_cnt;
	w->recv_cnt += recv_cnt;

	return recv_cnt + timeout_cnt;
}

static unsigned int poll_lrpc(struct softirq_work *w, struct kthread *k,
				unsigned int budget)
{
	unsigned int budget_left, join_cnt = 0;
	uint64_t cmd;
	unsigned long payload;

	assert_spin_lock_held(&k->lock);

	budget_left = min(budget, SOFTIRQ_MAX_BUDGET);
	while (budget_left) {
		if (!lrpc_recv(&k->rxcmdq, &cmd, &payload))
			break;

		budget_left--;
		BUG_ON(cmd != RX_JOIN);
		w->join_reqs[join_cnt++] = (struct kthread *)payload;
	}
	w->join_cnt = join_cnt;
	return join_cnt;
}

static void softirq_gather_bundles(struct softirq_work *w, struct kthread *k,
				unsigned int budget)
{
	unsigned long id;
	int j, budget_left, qidx;

	assert_preempt_disabled();

	budget_left = min(budget, SOFTIRQ_MAX_BUDGET);
	id = get_core_id(k);

	update_assignments();

	for (j = 0; j < nr_bundles; j++) {

		if (!budget_left)
			break;

		qidx = (j + k->pos_vq_rx) & (nr_bundles - 1);
		if (ACCESS_ONCE(bundle_assignments[qidx]) != id)
			continue;

		budget_left -= poll_bundle(w, &bundles[qidx], budget_left);
	}

	k->pos_vq_rx += j;
}


static __thread thread_t *next_irq;
static __thread struct softirq_work *next_irq_w;

static inline void softirq_work_init(struct softirq_work *w)
{
	w->recv_cnt = w->join_cnt = w->timeout_cnt = 0;
}

/**
 * softirq_run_thread - creates a closure for softirq handling
 * @k: the kthread from which to take RX queue commands
 * @budget: the maximum number of events to process
 *
 * Returns a thread that handles receive processing when executed or
 * NULL if no receive processing work is available.
 */
thread_t *softirq_run_local(unsigned int budget)
{
	thread_t *th;
	struct kthread *k = myk();
	struct softirq_work *w;

	assert_spin_lock_held(&k->lock);

	if (!next_irq) {
		next_irq = thread_create_with_buf(softirq_fn, (void **)&next_irq_w, sizeof(*w));
		if (unlikely(!next_irq))
			return NULL;
		softirq_work_init(next_irq_w);
	}

	poll_lrpc(next_irq_w, k, budget);
	softirq_gather_bundles(next_irq_w, k, budget);

	if (!next_irq_w->recv_cnt & !next_irq_w->join_cnt & !next_irq_w->timeout_cnt)
		return NULL;

	th = next_irq;
	th->state = THREAD_STATE_RUNNABLE;
	next_irq = NULL;
	return th;

}

thread_t *softirq_steal_lrpc(struct kthread *k, unsigned int budget)
{
	thread_t *th;
	struct softirq_work *w;

	assert_spin_lock_held(&k->lock);

	if (!next_irq) {
		next_irq = thread_create_with_buf(softirq_fn, (void **)&next_irq_w, sizeof(*w));
		if (unlikely(!next_irq))
			return NULL;
		softirq_work_init(next_irq_w);
	}

	poll_lrpc(next_irq_w, k, budget);

	if (!next_irq_w->join_cnt)
		return NULL;

	th = next_irq;
	th->state = THREAD_STATE_RUNNABLE;
	next_irq = NULL;
	return th;
}

thread_t *softirq_steal_bundle(struct kthread *k, unsigned int budget)
{
	thread_t *th;
	struct softirq_work *w;

	if (!next_irq) {
		next_irq = thread_create_with_buf(softirq_fn, (void **)&next_irq_w, sizeof(*w));
		if (unlikely(!next_irq))
			return NULL;
		softirq_work_init(next_irq_w);
	}

	softirq_gather_bundles(next_irq_w, k, budget);

	if (!next_irq_w->recv_cnt & !next_irq_w->timeout_cnt)
		return NULL;

	th = next_irq;
	th->state = THREAD_STATE_RUNNABLE;
	next_irq = NULL;
	return th;
}


/**
 * softirq_run - handles softirq processing in the current thread
 * @budget: the maximum number of events to process
 */
void softirq_run(unsigned int budget)
{
	struct kthread *k;
	struct softirq_work w;
	softirq_work_init(&w);

	k = getk();

	spin_lock(&k->lock);
	poll_lrpc(&w, k, budget);
	spin_unlock(&k->lock);
	softirq_gather_bundles(&w, k, budget);
	putk();

	if (w.recv_cnt || w.join_cnt || w.timeout_cnt)
		softirq_fn(&w);
}
