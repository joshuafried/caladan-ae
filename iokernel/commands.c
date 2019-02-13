/*
 * commands.c - dataplane commands to/from runtimes
 */

#include <base/log.h>
#include <base/lrpc.h>
#include <iokernel/queue.h>

#include "defs.h"

static int commands_drain_queue(struct thread *t, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		uint64_t cmd;
		unsigned long payload;

		if (!lrpc_recv(&t->txcmdq, &cmd, &payload)) {
			if (unlikely(t->parked))
				unpoll_thread(t);
			break;
		}

		switch (cmd) {
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

	return i;
}

/*
 * Process a batch of commands from runtimes.
 */
bool commands_rx(void)
{
	int i, n_cmds = 0;
	static unsigned int pos = 0;

	/*
	 * Poll each thread in each runtime until all have been polled or we
	 * have processed CMD_BURST_SIZE commands.
	 */
	for (i = 0; i < nrts; i++) {
		unsigned int idx = (pos + i) % nrts;

		if (n_cmds >= IOKERNEL_CMD_BURST_SIZE)
			break;
		n_cmds += commands_drain_queue(ts[idx],
				IOKERNEL_CMD_BURST_SIZE - n_cmds);
	}

	flush_wake_requests();

	STAT_INC(COMMANDS_PULLED, n_cmds);

	pos++;

	return n_cmds > 0;
}
