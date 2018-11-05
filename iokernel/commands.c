/*
 * commands.c - dataplane commands to/from runtimes
 */

#include <rte_mbuf.h>

#include <base/log.h>
#include <base/lrpc.h>
#include <iokernel/queue.h>

#include "defs.h"

static int commands_drain_queue(struct thread *t, struct rte_mbuf **bufs, int n)
{
	int i, n_bufs = 0;

	for (i = 0; i < n; i++) {
		uint64_t cmd;
		unsigned long payload;

		if (!lrpc_recv(&t->txcmdq, &cmd, &payload)) {
			if (unlikely(t->parked && lrpc_empty(&t->txpktq)))
				unpoll_thread(t);
			break;
		}

		switch (cmd) {
		case TXCMD_NET_COMPLETE:
			bufs[n_bufs++] = (struct rte_mbuf *)payload;
			/* TODO: validate pointer @buf */
			break;

		case TXCMD_PARKED_LAST:
			if (cores_park_kthread(t, false) &&
			    t->p->active_thread_count == 0 && payload) {
				t->p->pending_timer = true;
				t->p->deadline_us = microtime() + payload;
			}
			break;
		case TXCMD_PARKED:
			/* notify another kthread if the park was involuntary */
			if (cores_park_kthread(t, false) && payload != 0) {
				bool success = rx_send_to_runtime(t->p, t->p->next_thread_rr++, RX_JOIN, payload);
				if (unlikely(!success))
					STAT_INC(RX_JOIN_FAIL, 1);
			}
			break;

		default:
			/* kill the runtime? */
			BUG();
		}
	}

	return n_bufs;
}

/*
 * Process a batch of commands from runtimes.
 */
bool commands_rx(void)
{
	struct rte_mbuf *bufs[IOKERNEL_CMD_BURST_SIZE];
	int i, n_bufs = 0;
	static unsigned int pos = 0;

	/*
	 * Poll each thread in each runtime until all have been polled or we
	 * have processed CMD_BURST_SIZE commands.
	 */
	for (i = 0; i < nrts; i++) {
		unsigned int idx = (pos + i) % nrts;

		if (n_bufs >= IOKERNEL_CMD_BURST_SIZE)
			break;
		n_bufs += commands_drain_queue(ts[idx], &bufs[n_bufs],
				IOKERNEL_CMD_BURST_SIZE - n_bufs);
	}

	flush_wake_requests();

	STAT_INC(COMMANDS_PULLED, n_bufs);

	pos++;
	for (i = 0; i < n_bufs; i++)
		rte_pktmbuf_free(bufs[i]);
	return n_bufs > 0;
}
