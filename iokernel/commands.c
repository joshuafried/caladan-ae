/*
 * commands.c - dataplane commands to/from runtimes
 */

#include <base/log.h>
#include <base/lrpc.h>
#include <iokernel/queue.h>

#include "defs.h"

/**
 * rx_send_to_runtime - enqueues a command to an RXQ for a runtime
 * @p: the runtime's proc structure
 * @hash: the 5-tuple hash for the flow the command is related to
 * @cmd: the command to send
 * @payload: the command payload to send
 *
 * Returns true if the command was enqueued, otherwise a thread is not running
 * and can't be woken or the queue was full.
 */
bool rx_send_to_runtime(struct proc *p, uint32_t hash, uint64_t cmd,
			unsigned long payload)
{
	struct thread *th;

	if (likely(p->active_thread_count > 0)) {
		/* load balance between active threads */
		th = p->active_threads[hash % p->active_thread_count];
	} else if (p->sched_cfg.guaranteed_cores > 0 || get_nr_avail_cores() > 0) {
		th = cores_add_core(p);
		if (unlikely(!th))
			return false;
	} else {
		/* enqueue to the first idle thread, which will be woken next */
		th = list_top(&p->idle_threads, struct thread, idle_link);
		proc_set_overloaded(p);
	}

	return lrpc_send(&th->rxcmdq, cmd, payload);
}

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
