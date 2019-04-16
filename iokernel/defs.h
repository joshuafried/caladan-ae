/*
 * defs.h - shared definitions local to the iokernel
 */

#include <base/stddef.h>
#include <base/bitmap.h>
#include <base/gen.h>
#include <base/lrpc.h>
#include <base/mem.h>
#include <base/list.h>
#include <iokernel/control.h>
#include <net/ethernet.h>

#include "ref.h"

/* #define STATS 1 */

/*
 * Constant limits
 */
#define IOKERNEL_MAX_PROC		1024
#define IOKERNEL_CONTROL_BURST_SIZE	4

#define CORES_ADJUST_INTERVAL_US	5

/*
 * Process Support
 */

struct proc;

struct thread {
	struct proc		*p;
	unsigned int		parked:1;
	unsigned int		waking:1;
	struct lrpc_chan_out	rxcmdq;
	struct lrpc_chan_in	txcmdq;
	pid_t			tid;
	struct q_ptrs		*q_ptrs;
	uint32_t		last_rq_head;
	uint32_t		last_rxq_head;
	/* current or most recent core this thread ran on, depending on whether
	 * this thread is parked or not */
	unsigned int		core;
	/* the @ts index (if active) */
	unsigned int		ts_idx;
	/* the proc->active_threads index (if active) */
	unsigned int		at_idx;
	unsigned int		kthread_idx;
	/* list link for when idle */
	struct list_node	idle_link;
};

struct hwq {
	void *descriptor_table;
	uint32_t *consumer_idx;
	uint32_t descriptor_size;
	uint32_t nr_descriptors;
	uint32_t parity_byte_offset;
	uint32_t parity_bit_mask;
	uint32_t hwq_type;

	uint32_t cq_idx;
	bool cq_pending;
};

struct timer {
	unsigned int *timern;
	uint64_t *next_deadline_tsc;
};

#define MAX_HWQ 1
#define MAX_TIMER 1

struct bundle {
	/* shared variables from runtime */
	unsigned int hwq_count;
	unsigned int timer_count;
	struct hwq qs[MAX_HWQ];
	struct timer timers[MAX_TIMER];
};

struct proc {
	pid_t			pid;
	struct shm_region	region;
	bool			removed;
	struct ref		ref;
	unsigned int		kill:1;       /* the proc is being torn down */
	unsigned int		overloaded:1; /* the proc needs more cores */
	unsigned int		bursting:1;   /* the proc is using past resv. */
	unsigned int		launched:1;   /* executing the first time */

	/* intrusive list links */
	struct list_node	overloaded_link;
	struct list_node	bursting_link;

	/* scheduler data */
	struct sched_spec	sched_cfg;

	/* runtime threads */
	unsigned int		thread_count;
	unsigned int		active_thread_count;
	unsigned int		bundle_count;
	struct thread		threads[NCPU];
	struct bundle		bundles[NCPU];
	struct thread		*active_threads[NCPU];
	DEFINE_BITMAP(available_threads, NCPU);
	struct list_head	idle_threads;
	unsigned int		inflight_preempts;
	unsigned int		next_thread_rr; // for spraying join requests/overflow completions

	/* Unique identifier -- never recycled across runtimes*/
	uintptr_t		uniqid;

	/* table of physical addresses for shared memory */
	physaddr_t		page_paddrs[];

};

extern void proc_release(struct ref *r);

/**
 * proc_get - increments the proc reference count
 * @p: the proc to reference count
 *
 * Returns @p.
 */
static inline struct proc *proc_get(struct proc *p)
{
	ref_get(&p->ref);
	return p;
}

/**
 * proc_put - decrements the proc reference count, freeing if zero
 * @p: the proc to unreference count
 */
static inline void proc_put(struct proc *p)
{
	ref_put(&p->ref, proc_release);
}

/* the number of active threads to be polled (across all procs) */
extern unsigned int nrts;
/* an array of active threads to be polled (across all procs) */
extern struct thread *ts[NCPU];

/**
 * poll_thread - adds a thread to the queue polling array
 * @th: the thread to poll
 *
 * Can be called more than once.
 */
static inline void poll_thread(struct thread *th)
{
	if (th->ts_idx != -1)
		return;
	proc_get(th->p);
	ts[nrts] = th;
	th->ts_idx = nrts++;
}

/**
 * unpoll_thread - removes a thread from the queue polling array
 * @th: the thread to no longer poll
 */
static inline void unpoll_thread(struct thread *th)
{
	if (th->ts_idx == -1)
		return;
	ts[th->ts_idx] = ts[--nrts];
	ts[th->ts_idx]->ts_idx = th->ts_idx;
	th->ts_idx = -1;
	proc_put(th->p);
}

/*
 * Communication between control plane and data-plane in the I/O kernel
 */
#define CONTROL_DATAPLANE_QUEUE_SIZE	128
struct lrpc_params {
	struct lrpc_msg *buffer;
	uint32_t *wb;
};
extern struct lrpc_params lrpc_control_to_data_params;
extern struct lrpc_params lrpc_data_to_control_params;

/*
 * Commands from control plane to dataplane.
 */
enum {
	DATAPLANE_ADD_CLIENT,		/* points to a struct proc */
	DATAPLANE_REMOVE_CLIENT,	/* points to a struct proc */
	DATAPLANE_NR,			/* number of commands */
};

/*
 * Commands from dataplane to control plane.
 */
enum {
	CONTROL_PLANE_REMOVE_CLIENT,	/* points to a struct proc */
	CONTROL_PLANE_NR,		/* number of commands */
};

/*
 * Dataplane state
 */
struct dataplane {
	uint8_t			port;

	struct proc		*clients[IOKERNEL_MAX_PROC];
	int			nr_clients;
};

extern struct dataplane dp;

/*
 * Logical cores assigned to linux and the control and dataplane threads
 */
struct core_assignments {
	uint8_t linux_core;
	uint8_t ctrl_core;
	uint8_t dp_core;
};

extern struct core_assignments core_assign;


/*
 * Stats collected in the iokernel
 */
enum {
	RX_JOIN_FAIL = 0,

	COMMANDS_PULLED,

	IOKERNEL_LOOPS,

	RQ_GRANT,
	RX_GRANT,

	ADJUSTS,
	PREEMPTS,
	NR_STATS,

};

extern uint64_t stats[NR_STATS];
extern void print_stats(void);

#ifdef STATS
#define STAT_INC(stat_name, amt) do { stats[stat_name] += amt; } while (0);
#else
#define STAT_INC(stat_name, amt) ;
#endif

/*
 * RXQ command steering
 */

extern bool rx_send_to_runtime(struct proc *p, uint32_t hash, uint64_t cmd,
			       unsigned long payload);

/*
 * Initialization
 */

extern int cores_init(void);
extern int control_init(void);
extern int dp_clients_init();

/*
 * other dataplane functions
 */
extern void dp_clients_rx_control_lrpcs();

/*
 * functions for manipulating core assignments
 */
extern void cores_init_proc(struct proc *p);
extern void cores_free_proc(struct proc *p);
extern int cores_pin_thread(pid_t tid, int core);
extern bool cores_park_kthread(struct thread *t, bool force);
extern struct thread *cores_add_core(struct proc *p);
extern void cores_adjust_assignments();
extern void proc_set_overloaded(struct proc *p);
extern unsigned int get_nr_avail_cores(void);
extern unsigned int get_total_cores(void);
extern unsigned int get_available_cores(void);
extern void flush_wake_requests(void);
extern bool poll_core_queues(void);
