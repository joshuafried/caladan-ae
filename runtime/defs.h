/*
 * defs.h - internal runtime definitions
 */

#pragma once

#include <base/stddef.h>
#include <base/list.h>
#include <base/mem.h>
#include <base/tcache.h>
#include <base/gen.h>
#include <base/lrpc.h>
#include <base/thread.h>
#include <base/time.h>
#include <net/ethernet.h>
#include <net/ip.h>
#include <iokernel/control.h>
#include <net/mbufq.h>
#include <runtime/thread.h>
#include <runtime/rcu.h>
#include <runtime/preempt.h>

#include "net/verbs.h"


/*
 * constant limits
 * TODO: make these configurable?
 */

#define RUNTIME_MAX_THREADS		100000
#define RUNTIME_STACK_SIZE		128 * KB
#define RUNTIME_GUARD_SIZE		128 * KB
#define RUNTIME_RQ_SIZE			32
#define RUNTIME_SOFTIRQ_BUDGET		16
#define RUNTIME_MAX_TIMERS		4096
#define RUNTIME_SCHED_POLL_ITERS	0
#define RUNTIME_SCHED_MIN_POLL_US	2
#define RUNTIME_WATCHDOG_US		50


/* Network parameters */
#define MAX_BUNDLES			NCPU
#define RQ_NUM_DESC			128
#define SQ_NUM_DESC			128

#define SQ_CLEAN_THRESH			RUNTIME_SOFTIRQ_BUDGET
#define SQ_CLEAN_MAX			SQ_CLEAN_THRESH

BUILD_ASSERT(SQ_CLEAN_THRESH <= SQ_NUM_DESC);

#define VERBS_RX_BUF_TC_MAG		RUNTIME_SOFTIRQ_BUDGET * 2
#define RUNTIME_SMALLOC_MAG_SIZE	RUNTIME_SOFTIRQ_BUDGET * 8
#define NET_TX_BUF_TC_MAG		RUNTIME_SOFTIRQ_BUDGET * 16

#define STACK_TC_MAG			TCACHE_DEFAULT_MAG_SIZE
#define THREAD_TC_MAG			TCACHE_DEFAULT_MAG_SIZE

#define EGRESS_POOL_SIZE(nks) \
	(4096 * MBUF_DEFAULT_LEN * max(16, (nks)) * 16UL)
#define RX_BUF_BOOL_SZ(nrqs) \
 (align_up(nrqs * RQ_NUM_DESC * 2UL * MBUF_DEFAULT_LEN, PGSIZE_2MB))
#define POW_TWO_ROUND_UP(x) \
 ((x) <= 1UL ? 1UL : 1UL << (64 - __builtin_clzl((x) - 1)))
#define NR_BUNDLES(maxks, guaranteedks) (POW_TWO_ROUND_UP(maxks))

#define MLX5_TCP_RSS 1


struct io_bundle {

	spinlock_t lock;

	/* cache line of variables shared with iokernel */
	struct bundle_vars *b_vars;

	/* ingress network queue */
	struct verbs_queue_rx rxq;

	/* timer wheel */
	unsigned int			timern;
	struct timer_idx		*timers;

	/* storage queue */

} __aligned(CACHE_LINE_SIZE);

extern struct io_bundle bundles[MAX_BUNDLES];
extern unsigned int nr_bundles;

/*
 * Trap frame support
 */

/*
 * See the "System V Application Binary Interface" for a full explation of
 * calling and argument passing conventions.
 */

struct thread_tf {
	/* argument registers, can be clobbered by callee */
	uint64_t rdi; /* first argument */
	uint64_t rsi;
	uint64_t rdx;
	uint64_t rcx;
	uint64_t r8;
	uint64_t r9;
	uint64_t r10;
	uint64_t r11;

	/* callee-saved registers */
	uint64_t rbx;
	uint64_t rbp;
	uint64_t r12;
	uint64_t r13;
	uint64_t r14;
	uint64_t r15;

	/* special-purpose registers */
	uint64_t rax;	/* holds return value */
	uint64_t rip;	/* instruction pointer */
	uint64_t rsp;	/* stack pointer */
};

#define ARG0(tf)        ((tf)->rdi)
#define ARG1(tf)        ((tf)->rsi)
#define ARG2(tf)        ((tf)->rdx)
#define ARG3(tf)        ((tf)->rcx)
#define ARG4(tf)        ((tf)->r8)
#define ARG5(tf)        ((tf)->r9)


/*
 * Thread support
 */

enum {
	THREAD_STATE_RUNNING = 0,
	THREAD_STATE_RUNNABLE,
	THREAD_STATE_SLEEPING,
};

struct stack;

struct thread {
	struct thread_tf	tf;
	struct list_node	link;
	struct stack		*stack;
	unsigned int		main_thread:1;
	unsigned int		state;
	unsigned int		stack_busy;
};

typedef void (*runtime_fn_t)(void);

/* assembly helper routines from switch.S */
extern void __jmp_thread(struct thread_tf *tf) __noreturn;
extern void __jmp_thread_direct(struct thread_tf *oldtf,
				struct thread_tf *newtf,
				unsigned int *stack_busy);
extern void __jmp_runtime(struct thread_tf *tf, runtime_fn_t fn,
			  void *stack);
extern void __jmp_runtime_nosave(runtime_fn_t fn, void *stack) __noreturn;


/*
 * Stack support
 */

#define STACK_PTR_SIZE	(RUNTIME_STACK_SIZE / sizeof(uintptr_t))
#define GUARD_PTR_SIZE	(RUNTIME_GUARD_SIZE / sizeof(uintptr_t))

struct stack {
	uintptr_t	usable[STACK_PTR_SIZE];
	uintptr_t	guard[GUARD_PTR_SIZE]; /* unreadable and unwritable */
};

DECLARE_PERTHREAD(struct tcache_perthread, stack_pt);

/**
 * stack_alloc - allocates a stack
 *
 * Stack allocation is extremely cheap, think less than taking a lock.
 *
 * Returns an unitialized stack.
 */
static inline struct stack *stack_alloc(void)
{
	return tcache_alloc(&perthread_get(stack_pt));
}

/**
 * stack_free - frees a stack
 * @s: the stack to free
 */
static inline void stack_free(struct stack *s)
{
	tcache_free(&perthread_get(stack_pt), (void *)s);
}

#define RSP_ALIGNMENT	16

static inline void assert_rsp_aligned(uint64_t rsp)
{
	/*
	 * The stack must be 16-byte aligned at process entry according to
	 * the System V Application Binary Interface (section 3.4.1).
	 *
	 * The callee assumes a return address has been pushed on the aligned
	 * stack by CALL, so we look for an 8 byte offset.
	 */
	assert(rsp % RSP_ALIGNMENT == sizeof(void *));
}

/**
 * stack_init_to_rsp - sets up an exit handler and returns the top of the stack
 * @s: the stack to initialize
 * @exit_fn: exit handler that is called when the top of the call stack returns
 *
 * Returns the top of the stack as a stack pointer.
 */
static inline uint64_t stack_init_to_rsp(struct stack *s, void (*exit_fn)(void))
{
	uint64_t rsp;

	s->usable[STACK_PTR_SIZE - 1] = (uintptr_t)exit_fn;
	rsp = (uint64_t)&s->usable[STACK_PTR_SIZE - 1];
	assert_rsp_aligned(rsp);
	return rsp;
}

/**
 * stack_init_to_rsp_with_buf - sets up an exit handler and returns the top of
 * the stack, reserving space for a buffer above
 * @s: the stack to initialize
 * @buf: a pointer to store the buffer pointer
 * @buf_len: the length of the buffer to reserve
 * @exit_fn: exit handler that is called when the top of the call stack returns
 *
 * Returns the top of the stack as a stack pointer.
 */
static inline uint64_t
stack_init_to_rsp_with_buf(struct stack *s, void **buf, size_t buf_len,
			   void (*exit_fn)(void))
{
	uint64_t rsp, pos = STACK_PTR_SIZE;

	/* reserve the buffer */
	pos -= div_up(buf_len, sizeof(uint64_t));
	pos = align_down(pos, RSP_ALIGNMENT / sizeof(uint64_t));
	*buf = (void *)&s->usable[pos];

	/* setup for usage as stack */
	s->usable[--pos] = (uintptr_t)exit_fn;
	rsp = (uint64_t)&s->usable[pos];
	assert_rsp_aligned(rsp);
	return rsp;
}

/*
 * ioqueues
 */

struct iokernel_control {
	int fd;

	/* shared memory setup */
	mem_key_t key;
	struct shm_region	shared_region;
	void *verbs_mem;
	size_t verbs_mem_len;

	/* threads + other queues register themselves here */
	struct thread_spec *threads;
	struct bundle_spec *bundles;
};

extern struct iokernel_control iok;


/*
 * Per-kernel-thread State
 */

/*
 * These are per-kthread stat counters. It's recommended that most counters be
 * monotonically increasing, as that decouples the counters from any particular
 * collection time period. However, it may not be possible to represent all
 * counters this way.
 *
 * Don't use these enums directly. Instead, use the STAT() macro.
 */
enum {
	/* scheduler counters */
	STAT_RESCHEDULES = 0,
	STAT_SCHED_CYCLES,
	STAT_PROGRAM_CYCLES,
	STAT_THREADS_STOLEN,
	STAT_SOFTIRQS_STOLEN,
	STAT_SOFTIRQS_LOCAL,
	STAT_PARKS,
	STAT_PREEMPTIONS,
	STAT_PREEMPTIONS_STOLEN,
	STAT_CORE_MIGRATIONS,

	/* network stack counters */
	STAT_RX_BYTES,
	STAT_RX_PACKETS,
	STAT_TX_BYTES,
	STAT_TX_PACKETS,
	STAT_DROPS,
	STAT_RX_TCP_IN_ORDER,
	STAT_RX_TCP_OUT_OF_ORDER,
	STAT_RX_TCP_TEXT_CYCLES,

	/* total number of counters */
	STAT_NR,
};

struct timer_idx;
struct verbs_queue;

struct kthread {
	/* 1st cache-line */
	spinlock_t		lock;
	uint32_t		generation;
	uint32_t		rq_head;
	uint32_t		rq_tail;
	struct list_head	rq_overflow;
	struct lrpc_chan_in	rxcmdq;
	unsigned int		parked:1;
	unsigned int		detached:1;
	int pad1;

	/* 2nd cache-line */
	struct q_ptrs		*q_ptrs;
	struct mbufq		txpktq_overflow;
	struct mbufq		txcmdq_overflow;
	unsigned int		rcu_gen;
	unsigned int		curr_cpu;
	uint64_t		park_us;
	unsigned long		kthread_idx;

	/* 3rd cache-line */
	struct lrpc_chan_out	txcmdq;
	unsigned long pad4[4];

	/* 4th-7th cache-line */
	thread_t		*rq[RUNTIME_RQ_SIZE];

	struct verbs_queue_tx vq_tx __aligned(CACHE_LINE_SIZE);
	unsigned int pos_vq_rx;
	unsigned long		pad3[3];

	/* 9th cache-line, statistics counters */
	uint64_t		stats[STAT_NR] __aligned(CACHE_LINE_SIZE);
};

/* compile-time verification of cache-line alignment */
BUILD_ASSERT(offsetof(struct kthread, lock) % CACHE_LINE_SIZE == 0);
BUILD_ASSERT(offsetof(struct kthread, q_ptrs) % CACHE_LINE_SIZE == 0);
BUILD_ASSERT(offsetof(struct kthread, txcmdq) % CACHE_LINE_SIZE == 0);
BUILD_ASSERT(offsetof(struct kthread, rq) % CACHE_LINE_SIZE == 0);
BUILD_ASSERT(offsetof(struct kthread, stats) % CACHE_LINE_SIZE == 0);

extern __thread struct kthread *mykthread;

/**
 * myk - returns the per-kernel-thread data
 */
static inline struct kthread *myk(void)
{
	return mykthread;
}

/**
 * getk - returns the per-kernel-thread data and disables preemption
 *
 * WARNING: If you're using myk() instead of getk(), that's a bug if preemption
 * is enabled. The local kthread can change at anytime.
 */
static inline struct kthread *getk(void)
{
	preempt_disable();
	return mykthread;
}

/**
 * putk - reenables preemption after calling getk()
 */
static inline void putk(void)
{
	preempt_enable();
}

DECLARE_SPINLOCK(klock);
extern unsigned int maxks;
extern unsigned int spinks;
extern unsigned int guaranteedks;
extern unsigned int nrks;
extern struct kthread *ks[NCPU];
extern struct kthread *allks[NCPU];

extern void kthread_detach(struct kthread *r);
extern void kthread_park(bool voluntary);
extern void kthread_wait_to_attach(void);

struct cpu_record {
	struct kthread *recent_kthread;
	unsigned long sibling_core;
	unsigned long pad[6];
};

BUILD_ASSERT(sizeof(struct cpu_record) == CACHE_LINE_SIZE);

extern struct cpu_record cpu_map[NCPU];

/**
 * STAT - gets a stat counter
 *
 * e.g. STAT(DROPS)++;
 *
 * Deliberately could race with preemption.
 */
#define STAT(counter) (myk()->stats[STAT_ ## counter])



/*
 * Softirq support
 */

/* the maximum number of events to handle in a softirq invocation */
#define SOFTIRQ_MAX_BUDGET	128

extern bool disable_watchdog;

extern thread_t *softirq_steal_bundle(struct kthread *k, unsigned int budget);
extern thread_t *softirq_steal_lrpc(struct kthread *k, unsigned int budget);

extern thread_t *softirq_run_local(unsigned int budget);
extern void softirq_run(unsigned int budget);


/*
 * Network stack
 */

struct net_cfg {
	uint32_t		addr;
	uint32_t		netmask;
	uint32_t		gateway;
	struct eth_addr		mac;
	uint8_t			pad[14 + 32];
} __packed;

BUILD_ASSERT(sizeof(struct net_cfg) == CACHE_LINE_SIZE);

extern struct net_cfg netcfg;

#define MAX_ARP_STATIC_ENTRIES 1024
struct cfg_arp_static_entry {
	uint32_t ip;
	struct eth_addr addr;
};
extern int arp_static_count;
extern struct cfg_arp_static_entry static_entries[MAX_ARP_STATIC_ENTRIES];

struct rx_net_hdr;

extern void __net_recurrent(void);
extern void net_rx_softirq(struct rx_net_hdr **hdrs, unsigned int nr);


/*
 * Timer support
 */
struct timer_entry;

extern int timer_gather(struct io_bundle *b, unsigned int budget, struct timer_entry **entries);

struct timer_idx {
	uint64_t		deadline_us;
	struct timer_entry	*e;
};

/**
 * timer_needed - returns true if pending timers have to be handled
 * @b: the io bundle to check
 */
static inline bool timer_needed(struct io_bundle *b)
{
	/* deliberate race condition */
	return b->timern > 0 && b->timers[0].deadline_us <= microtime();
}


/* Queue shuffling support */
extern atomic64_t kthread_gen __aligned(CACHE_LINE_SIZE);
extern __thread uint64_t last_kthread_gen;
extern __thread unsigned long cached_assignments[NCPU][MAX_BUNDLES];
extern __thread unsigned long assignment_count[NCPU];
extern unsigned long *__get_queues(struct kthread *k, int *nrqs);

static inline unsigned long *get_queues(struct kthread *k, int *nrqs)
{
	int phys_id;

	if (unlikely(atomic64_read(&kthread_gen) != last_kthread_gen))
		return __get_queues(k, nrqs);

	phys_id = min(k->curr_cpu, cpu_map[k->curr_cpu].sibling_core);
	*nrqs = assignment_count[phys_id];
	return cached_assignments[phys_id];
}

static inline struct io_bundle *get_first_bundle(struct kthread *k)
{
	int pos;
	unsigned long *qs;

	assert_preempt_disabled();
	qs = get_queues(k, &pos);
	BUG_ON(pos == 0);

	return &bundles[qs[0]];
}

/*
 * Init
 */

/* per-thread initialization */
extern int kthread_init_thread(void);
extern int ioqueues_init_thread(void);
extern int stack_init_thread(void);
extern int sched_init_thread(void);
extern int net_init_thread(void);
extern int smalloc_init_thread(void);
extern int verbs_init_thread(void);

/* global initialization */
extern int ioqueues_init(void);
extern int stack_init(void);
extern int sched_init(void);
extern int preempt_init(void);
extern int net_init(void);
extern int arp_init(void);
extern int trans_init(void);
extern int smalloc_init(void);
extern int kthread_init(void);
extern int verbs_init(void);
extern int timer_init(void);

/* late initialization */
extern int ioqueues_register_iokernel(void);
extern int arp_init_late(void);
extern int stat_init_late(void);
extern int tcp_init_late(void);
extern int rcu_init_late(void);

/* configuration loading */
extern int cfg_load(const char *path);

/* runtime entry helpers */
extern void sched_start(void) __noreturn;
extern int thread_spawn_main(thread_fn_t fn, void *arg);
extern void thread_yield_kthread();
extern void join_kthread(struct kthread *k);
