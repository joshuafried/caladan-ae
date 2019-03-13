/*
 * queue.h - shared memory queues between the iokernel and the runtimes
 */

#pragma once

#include <base/stddef.h>

/*
 * RX command queues: IOKERNEL -> RUNTIMES
 * These queues multiplex several different types of requests.
 */
enum {
	RX_JOIN = 0,		/* immediate detach request for a kthread */
	RX_CALL_NR,		/* number of commands */
};


/*
 * TX command queues: RUNTIMES -> IOKERNEL
 * These queues handle a variety of commands, and typically they are handled
 * much faster by the IOKERNEL than packets, so no HOL blocking.
 */
enum {
	TXCMD_PARKED = 0,		/* hint to iokernel that kthread is parked */
	TXCMD_NR,		/* number of commands */
};
