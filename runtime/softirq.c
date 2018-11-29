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
	unsigned int recv_cnt, compl_cnt, join_cnt, timer_budget;
	struct kthread *k;
	struct rx_net_hdr *recv_reqs[SOFTIRQ_MAX_BUDGET];
	struct mbuf *compl_reqs[SOFTIRQ_MAX_BUDGET];
	struct kthread *join_reqs[SOFTIRQ_MAX_BUDGET];
};

static void softirq_fn(void *arg)
{
	struct softirq_work *w = arg;
	int i;

	/* complete TX requests and free packets */
	for (i = 0; i < w->compl_cnt; i++)
		mbuf_free(w->compl_reqs[i]);

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
	unsigned int recv_cnt = 0, compl_cnt = 0, join_cnt = 0;
	int j, budget_left;

	budget_left = min(budget, SOFTIRQ_MAX_BUDGET);
	while (budget_left) {
		uint64_t cmd;
		unsigned long payload;

		if (!lrpc_recv(&k->rxq, &cmd, &payload))
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

	if (budget_left) {
		compl_cnt = verbs_gather_completions(w->compl_reqs, &k->vq_tx, budget_left);
		budget_left -= compl_cnt;
	}

	for (j = 0; budget_left && j < k->nr_vq_rx; j++) {
		int idx = (j + k->pos_vq_rx) % k->nr_vq_rx;
		if (j + 2 < k->nr_vq_rx)
			prefetch(k->vq_rx[(idx + 2) % k->nr_vq_rx]->raw_cq);
		int rcv = verbs_gather_rx(w->recv_reqs + recv_cnt, k->vq_rx[idx], budget_left);
		recv_cnt += rcv;
		budget_left -= rcv;
	}

	k->pos_vq_rx += j;

	w->k = k;
	w->recv_cnt = recv_cnt;
	w->compl_cnt = compl_cnt;
	w->join_cnt = join_cnt;
	w->timer_budget = budget_left;
}

static bool verbs_work_exists(struct kthread *k)
{
	int j = 0;

	if (!verbs_queue_is_empty(k->vq_tx.raw_cq))
		return true;

	for (j = 0; j < k->nr_vq_rx; j++) {
		int pos = (j + k->pos_vq_rx) % k->nr_vq_rx;
		if (j + 4 < k->nr_vq_rx)
			prefetch(k->vq_rx[(pos + 4) % k->nr_vq_rx]->raw_cq);
		if (!verbs_queue_is_empty(k->vq_rx[pos]->raw_cq))
			return true;
	}

	return false;
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

	/* check if there's any work available */
	if (lrpc_empty(&k->rxq) && !timer_needed(k) && !verbs_work_exists(k))
		return NULL;

	th = thread_create_with_buf(softirq_fn, (void **)&w, sizeof(*w));
	if (unlikely(!th))
		return NULL;

	softirq_gather_work(w, k, budget);
	th->state = THREAD_STATE_RUNNABLE;
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
	/* check if there's any work available */
	if (lrpc_empty(&k->rxq) && !timer_needed(k) && !verbs_work_exists(k)) {
		putk();
		return;
	}

	spin_lock(&k->lock);
	softirq_gather_work(&w, k, budget);
	spin_unlock(&k->lock);
	putk();

	softirq_fn(&w);
}
