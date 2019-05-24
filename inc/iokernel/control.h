/*
 * control.h - the control interface for the I/O kernel
 */

#pragma once

#include <sys/types.h>

#include <base/limits.h>
#include <iokernel/shm.h>
#include <net/ethernet.h>

/* The abstract namespace path for the control socket. */
#define CONTROL_SOCK_PATH	"\0/control/iokernel.sock"

#define IOK_ONLINE_CORE_PATH "/run/iokernel_managed_cores"

/* describes a queue */
struct q_ptrs {
	uint32_t rxq_wb; /* must be first */
	uint32_t rq_head;
	uint32_t rq_tail;
};

/* unused for now */
enum {
	HWQ_MLX5 = 0,
	HWQ_MLX4,
	HWQ_SPDK_NVME,
	NR_HWQ,
};

struct hardware_queue_spec {
	shmptr_t descriptor_table;
	shmptr_t consumer_idx;
	uint32_t descriptor_size;
	uint32_t nr_descriptors;
	uint32_t parity_byte_offset;
	uint32_t parity_bit_mask;
	uint32_t hwq_type;
};

struct timer_spec {
	shmptr_t timern;
	shmptr_t next_deadline_tsc;
};

/* describes an io bundle aka affinity group */
/* a bundle consists of a set of hardware queues and timer queues */
struct bundle_spec {
	uint32_t hwq_count;
	uint32_t timer_count;
	shmptr_t hwq_specs;
	shmptr_t timer_specs;
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
	struct sched_spec	sched_cfg;
	unsigned int		thread_count;
	unsigned int		bundle_count;
	shmptr_t		thread_specs;
	shmptr_t		bundle_specs;
};
