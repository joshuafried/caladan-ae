/*
 * control.h - the control interface for the I/O kernel
 */

#pragma once

#include <sys/types.h>

#include <base/limits.h>
#include <iokernel/shm.h>
#include <iokernel/verbs.h>
#include <net/ethernet.h>

/* The abstract namespace path for the control socket. */
#define CONTROL_SOCK_PATH	"\0/control/iokernel.sock"

/* describes a queue */
struct q_ptrs {
	uint32_t rxq_wb; /* must be first */
	uint32_t rq_head;
	uint32_t rq_tail;
};

/* describes an io bundle */
struct bundle_spec {
	shmptr_t rx_cq_buf;
	uint32_t cqe_cnt;
	shmptr_t b_vars;
};

/* shared variables for an io bundle */
struct bundle_vars {
	uint32_t rx_cq_idx;
	unsigned int timern;
	uint64_t next_deadline_tsc;
};

/* describes a runtime kernel thread */
struct thread_spec {
	struct queue_spec	rxcmdq;
	struct queue_spec	txcmdq;
	shmptr_t		q_ptrs;
	pid_t			tid;
};

enum {
	SCHED_PRIORITY_SYSTEM = 0, /* high priority, system-level services */
	SCHED_PRIORITY_NORMAL,     /* normal priority, typical tasks */
	SCHED_PRIORITY_BATCH,      /* low priority, batch processing */
};

/* describes scheduler options */
struct sched_spec {
	unsigned int		priority;
	unsigned int		max_cores;
	unsigned int		guaranteed_cores;
	unsigned int		congestion_latency_us;
	unsigned int		scaleout_latency_us;
};

#define CONTROL_HDR_MAGIC	0x696f6b3a /* "iok:" */

/* the main control header */
struct control_hdr {
	unsigned int		magic;
	unsigned int		thread_count;
	struct sched_spec	sched_cfg;
	unsigned int		bundle_count;
	shmptr_t		thread_specs;
	shmptr_t		bundle_specs;
};
