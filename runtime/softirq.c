/*
 * softirq.c - handles high priority events (timers, ingress packets, etc.)
 */

#include <base/stddef.h>
#include <base/log.h>
#include <runtime/thread.h>
#include <runtime/timer.h>

#include "defs.h"
#include "net/defs.h"

struct softirq_work {
	unsigned int total_cnt, recv_cnt, join_cnt, timeout_cnt, storage_cnt;
	struct rx_net_hdr *recv_reqs[SOFTIRQ_MAX_BUDGET];
	struct kthread *join_reqs[SOFTIRQ_MAX_BUDGET];
	struct timer_entry *timeouts[SOFTIRQ_MAX_BUDGET];
	struct thread *storage_threads[SOFTIRQ_MAX_BUDGET];
};

static void softirq_fn(void *arg)
{
	struct softirq_work *w = arg;
	int i;

	for (i = 0; i < w->storage_cnt; i++)
		thread_ready(w->storage_threads[i]);

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
	unsigned int recv_cnt, timeout_cnt, storage_cnt, total;

	assert_preempt_disabled();

	BUG_ON(budget > SOFTIRQ_MAX_BUDGET - w->recv_cnt);
	BUG_ON(budget > SOFTIRQ_MAX_BUDGET - w->timeout_cnt);
	BUG_ON(budget > SOFTIRQ_MAX_BUDGET - w->storage_cnt);

	if (unlikely(!budget))
		return 0;

	if (unlikely(!rx_pending(b->rxq) && !timer_needed(b) && !storage_queue_has_work(&b->sq)))
		return 0;

	if (unlikely(!spin_try_lock(&b->lock)))
		return 0;

	storage_cnt = storage_proc_completions(b, budget, w->storage_threads);
	budget -= storage_cnt;

	recv_cnt = netcfg.ops.rx_batch(b->rxq, w->recv_reqs + w->recv_cnt, budget);
	budget -= recv_cnt;

	timeout_cnt = timer_gather(b, budget, w->timeouts + w->timeout_cnt);

	spin_unlock(&b->lock);

	w->storage_cnt += storage_cnt;
	w->timeout_cnt += timeout_cnt;
	w->recv_cnt += recv_cnt;

	total = storage_cnt + timeout_cnt + recv_cnt;
	w->total_cnt += total;

	return total;
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
		BUG_ON(payload - 1 >= maxks);
		w->join_reqs[join_cnt++] = allks[payload - 1];
	}
	w->join_cnt = join_cnt;
	w->total_cnt += join_cnt;
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

		qidx = (j + k->next_rx_poll) & (nr_bundles - 1);
		if (ACCESS_ONCE(bundle_assignments[qidx]) != id)
			continue;

		budget_left -= poll_bundle(w, &bundles[qidx], budget_left);
	}

	k->next_rx_poll += j;
}


static __thread thread_t *next_irq;
static __thread struct softirq_work *next_irq_w;

static inline void softirq_work_init(struct softirq_work *w)
{
	w->total_cnt = w->recv_cnt = w->join_cnt = w->timeout_cnt = w->storage_cnt = 0;
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

	if (!next_irq_w->total_cnt)
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

	if (!next_irq_w->total_cnt)
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

	if (!next_irq_w->total_cnt)
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

	if (w.total_cnt)
		softirq_fn(&w);
}
