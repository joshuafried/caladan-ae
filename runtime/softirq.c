/*
 * softirq.c - handles high priority events (timers, ingress packets, etc.)
 */

#include <base/stddef.h>
#include <base/log.h>
#include <runtime/thread.h>

#include "defs.h"
#include "net/defs.h"
#include "net/verbs.h"

DECLARE_PERTHREAD(struct tcache_perthread, thread_pt);

struct softirq_work {
	unsigned int recv_cnt, join_cnt, timer_budget;
	struct kthread *k;
	struct rx_net_hdr *recv_reqs[SOFTIRQ_MAX_BUDGET];
	struct kthread *join_reqs[SOFTIRQ_MAX_BUDGET];
};

static void softirq_fn(void *arg)
{
	struct softirq_work *w = arg;
	int i;

	/* deliver new RX packets to the runtime */
	net_rx_softirq(w->recv_reqs, w->recv_cnt);

	/* handle any pending timeouts */
	if (timer_needed(w->k))
		timer_softirq(w->k, w->timer_budget);

	/* join parked kthreads */
	for (i = 0; i < w->join_cnt; i++)
		join_kthread(w->join_reqs[i]);
}

static void softirq_gather_work(struct softirq_work *w, struct kthread *k,
				unsigned int budget)
{
	unsigned int recv_cnt = 0, join_cnt = 0;
	unsigned long *qs;
	int j, budget_left, todo, rcvd;

	budget_left = min(budget, SOFTIRQ_MAX_BUDGET);
	while (budget_left) {
		uint64_t cmd;
		unsigned long payload;

		if (!lrpc_recv(&k->rxcmdq, &cmd, &payload))
			break;

		budget_left--;

		switch (cmd) {
		case RX_JOIN:
			w->join_reqs[join_cnt++] = (struct kthread *)payload;
			break;

		default:
			log_err_ratelimited("net: invalid RXQ cmd '%ld'", cmd);
		}
	}

	qs = get_queues(k);
	bitmap_for_each_set(qs, nrvqs, j) {
		if (!budget_left)
			break;

		if (!verbs_has_rx_packets(&vqs[j]))
			continue;

		if (!spin_try_lock(&vqs[j].lock))
			continue;

		todo = min(budget_left, SOFTIRQ_MAX_BUDGET - recv_cnt);
		rcvd = verbs_gather_rx(w->recv_reqs + recv_cnt, &vqs[j], todo);
		recv_cnt += rcvd;
		spin_unlock(&vqs[j].lock);
	}

	w->recv_cnt = recv_cnt;
	w->join_cnt = join_cnt;
	w->timer_budget = budget_left;
}


static __thread thread_t *next_irq;
static __thread struct softirq_work *next_irq_w;

static inline void softirq_work_init(struct softirq_work *w)
{
	w->recv_cnt = w->join_cnt = w->timer_budget = 0;
}

/**
 * softirq_run_thread - creates a closure for softirq handling
 * @k: the kthread from which to take RX queue commands
 * @budget: the maximum number of events to process
 *
 * Returns a thread that handles receive processing when executed or
 * NULL if no receive processing work is available.
 */
thread_t *softirq_run_thread(struct kthread *k, unsigned int budget)
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

	softirq_gather_work(next_irq_w, k, budget);

	if (!next_irq_w->recv_cnt && !next_irq_w->join_cnt && !timer_needed(k))
		return NULL;

	th = next_irq;
	next_irq_w->k = k;
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

	k = getk();

	spin_lock(&k->lock);
	softirq_gather_work(&w, k, budget);
	w.k = k;
	spin_unlock(&k->lock);
	putk();

	if (w.recv_cnt || w.join_cnt || timer_needed(k))
		softirq_fn(&w);
}
