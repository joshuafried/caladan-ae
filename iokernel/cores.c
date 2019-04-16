/*
 * cores.c - manages assignments of cores to runtimes, the iokernel, and linux
 */

#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <base/bitmap.h>
#include <base/cpu.h>
#include <base/hash.h>
#include <base/log.h>
#include <base/thread.h>
#include <iokernel/queue.h>

#include "defs.h"

#include "../ksched/ksched.h"

/*#define CORES_NOHT 1*/

static unsigned int nr_avail_cores;
static unsigned int total_cores;
static DEFINE_BITMAP(online_cores, NCPU);
static DEFINE_BITMAP(avail_cores, NCPU);
static DEFINE_BITMAP(online_cores, NCPU);
struct core_assignments core_assign;
unsigned int nrts = 0;
struct thread *ts[NCPU];

/* maps each cpu number to the number of its hyperthread buddy */
static int cpu_siblings[NCPU];

/* ksched interface */
static int ksched_fd;
static struct ksched_preempt_req *preempt_reqs;
static struct ksched_shm_percpu *ksched_shm;


unsigned int get_nr_avail_cores(void)
{
	return nr_avail_cores;
}

unsigned int get_total_cores(void)
{
	return total_cores;
}


/**
 * cpu_to_sibling_cpu - gets the sibling (hyperthread pair) of a cpu
 * @cpu: the number of the cpu
 *
 * Returns the number of the sibling cpu.
 */
static inline int cpu_to_sibling_cpu(int cpu)
{
	assert(cpu < cpu_count);
	return cpu_siblings[cpu];
}

/* Stores information about a core. Current is only valid if the core is in
 * use. */
struct core {
	struct thread	*current;
	struct thread	*prev;
	struct thread	*next; /* if non-NULL, thread that preempted this core */

	struct lrpc_chan_in cmdq_in;
	struct lrpc_chan_out cmdq_out;
	unsigned long cached_gen;
};

static struct core core_history[NCPU];

/**
 * core_is_preempting - returns true if core is mid-preemption.
 */
static inline bool core_is_preempting(unsigned int core)
{
	return core_history[core].next != NULL;
}


static void wake_single(struct thread *th, int core, bool preempt)
{

	while (unlikely(!lrpc_send(&core_history[core].cmdq_out, KSCHED_RUN_NEXT, th->tid)))
		WARN();
	store_release(&ksched_shm[core].gen, ++core_history[core].cached_gen);

	if (preempt)
		preempt_reqs->cpus[preempt_reqs->nr++] = core;
}

void flush_wake_requests(void)
{
	if (!preempt_reqs->nr)
		return;

	STAT_INC(PREEMPTS, preempt_reqs->nr);
	BUG_ON(ioctl(ksched_fd, KSCHED_IOC_PREEMPT, preempt_reqs));

	preempt_reqs->nr = 0;
}


/**
 * core_wake_idle - wake thread on idle core
 * @core: the core to use
 * @th: the thread that will use core
 */
static inline void core_wake_idle(unsigned int core, struct thread *th)
{
	assert(bitmap_test(avail_cores, core));

	bitmap_clear(avail_cores, core);
	nr_avail_cores--;

	assert(!core_is_preempting(core));

	proc_get(th->p);

	assert(!th->parked);

	if (core_history[core].prev)
		proc_put(core_history[core].prev->p);

	core_history[core].prev = core_history[core].current;
	core_history[core].current = th;
	core_history[core].next = NULL;

	wake_single(th, core, false);
}

/**
 * core_reserve_preempt - record that th is preempting on core
 * @core: the core to preempt
 * @th: the thread that will use core
 */
static inline void core_preempt(unsigned int core, struct thread *th)
{
	assert(!bitmap_test(avail_cores, core));
	assert(!core_is_preempting(core));
	assert(!th->parked);

	proc_get(th->p);
	core_history[core].next = th;
	core_history[core].current->p->inflight_preempts++;

	wake_single(th, core, true);
}

/**
 * core_finish_preempt - record that core has finished preemption
 * @core: the core that is completing preemption
 */
static inline void core_finish_preempt(unsigned int core)
{
	assert(!bitmap_test(avail_cores, core));
	assert(core_is_preempting(core));

	if (core_history[core].prev)
		proc_put(core_history[core].prev->p);

	core_history[core].current->p->inflight_preempts--;

	core_history[core].prev = core_history[core].current;
	core_history[core].current = core_history[core].next;
	core_history[core].next = NULL;
}

/**
 * core_cede - relinquish a core. It no longer has something running on it.
 * @core: the core to relinquish
 */
static inline void core_cede(unsigned int core)
{
	assert(!bitmap_test(avail_cores, core));

	bitmap_set(avail_cores, core);
	nr_avail_cores++;
}

/**
 * core_init - init a core.
 * @core: the core to init
 */
static inline void core_init(unsigned int core)
{
	bitmap_set(avail_cores, core);
	bitmap_set(online_cores, core);
	nr_avail_cores++;
	total_cores++;
	core_history[core].current = NULL;
	core_history[core].prev = NULL;
	core_history[core].next = NULL;
}

/**
 * core_available - returns true if core is available, false otherwise.
 */
static inline bool core_available(unsigned int core)
{
	return bitmap_test(avail_cores, core);
}

/* a list of procs that currently require more cores */
static LIST_HEAD(overloaded_procs);

/**
 * proc_set_overloaded - marks a process as overloaded
 * p: the process to mark as overloaded
 */
void proc_set_overloaded(struct proc *p)
{
	if (p->overloaded)
		return;

	list_add(&overloaded_procs, &p->overloaded_link);
	p->overloaded = true;
}

/**
 * proc_clear_overloaded - unmarks a process as overloaded
 * @p: the process to unmark as overloaded
 */
static inline void proc_clear_overloaded(struct proc *p)
{
	if (!p->overloaded)
		return;

	list_del_from(&overloaded_procs, &p->overloaded_link);
	p->overloaded = false;
}

/**
 * proc_is_overloaded - returns true if the process is overloaded
 * @p: the process to test if overloaded
 */
static inline bool proc_is_overloaded(struct proc *p)
{
	return p->overloaded;
}

/**
 * no_overloaded_procs - returns true if there are no overloaded procs, false
 * otherwise
 */
static inline bool no_overloaded_procs()
{
	return list_empty(&overloaded_procs);
}

/**
 * get_overloaded_proc - returns an overloaded proc or NULL if none exists
 */
static inline struct proc *get_overloaded_proc()
{
	return list_top(&overloaded_procs, struct proc, overloaded_link);
}

/* a list of procs that are using more cores than they have reserved */
static LIST_HEAD(bursting_procs);

/**
 * proc_set_bursting - marks a process as bursting
 * p: the process to mark as bursting
 */
static inline void proc_set_bursting(struct proc *p)
{
	if (p->bursting)
		return;

	list_add(&bursting_procs, &p->bursting_link);
	p->bursting = true;
}

/**
 * proc_clear_bursting - unmarks a process as bursting
 * @p: the process to unmark as bursting
 */
static inline void proc_clear_bursting(struct proc *p)
{
	if (!p->bursting)
		return;

	list_del_from(&bursting_procs, &p->bursting_link);
	p->bursting = false;
}

/**
 * proc_is_bursting - returns true if the process is bursting
 * @p: the process to test if bursting
 */
static inline bool proc_is_bursting(struct proc *p)
{
	return p->bursting;
}

/**
 * get_bursting_proc - returns a bursting proc or NULL if none exists
 */
static inline struct proc *get_bursting_proc()
{
	return list_top(&bursting_procs, struct proc, bursting_link);
}

/**
 * thread_reserve - record that thread th will now run on core.
 * @th: the thread to reserve
 * @core: the core this kthread will run on
 */
static inline void thread_reserve(struct thread *th, unsigned int core)
{
	struct proc *p = th->p;
	unsigned int kthread = th - p->threads;

	assert(th->parked == true);

	bitmap_clear(p->available_threads, kthread);
	th->core = core;
	p->active_threads[p->active_thread_count] = th;
	th->at_idx = p->active_thread_count++;
	list_del_from(&p->idle_threads, &th->idle_link);

	if (p->active_thread_count > p->sched_cfg.guaranteed_cores)
		proc_set_bursting(p);

	proc_clear_overloaded(p);

	/* add the thread to the polling array */
	th->parked = false;
	th->waking = true;
	poll_thread(th);
}

/**
 * thread_cede - relinquish a kthread. It is no longer running on a dedicated
 * core.
 * @th: the thread to relinquish
 */
static inline void thread_cede(struct thread *th)
{
	struct proc *p = th->p;
	unsigned int kthread = th - p->threads;

	assert(!th->parked);

	bitmap_set(p->available_threads, kthread);
	p->active_threads[th->at_idx] = p->active_threads[--p->active_thread_count];
	p->active_threads[th->at_idx]->at_idx = th->at_idx;
	list_add(&p->idle_threads, &th->idle_link);

	if (p->active_thread_count == p->sched_cfg.guaranteed_cores)
		proc_clear_bursting(p);

	/* remove the thread from the polling array (if queues are empty) */
	th->parked = true;
	if (lrpc_empty(&th->txcmdq))
		unpoll_thread(th);
}

/*
 * Debugging function that logs how each core is currently being used.
 */
__attribute__((unused))
static void cores_log_assignments()
{
	int i, j;
	struct proc *p;
	char buf[NCPU+1];

	for (i = 0; i < dp.nr_clients; i++) {
		p = dp.clients[i];

		DEFINE_BITMAP(proc_cores, cpu_count);
		bitmap_init(proc_cores, cpu_count, false);
		bitmap_for_each_cleared(p->available_threads, p->thread_count, j) {
			bitmap_set(proc_cores, p->threads[j].core);
		}

		for (j = 0; j < cpu_count; j++)
			buf[j] = bitmap_test(proc_cores, j) ? '1' : '0';
		buf[j] = '\0';

		log_debug("cores: %s used by runtime pid %d", &buf[0], p->pid);
	}

	for (i = 0; i < cpu_count; i++)
		buf[i] = core_available(i) ? '1' : '0';
	buf[i] = '\0';
	log_debug("cores: %s idle", &buf[0]);
}

/**
 *  pick_thread_for_proc - choose a thread to start up for this proc.
 *  @p: the process to choose a thread for
 *  @core: the core that will be given to the proc
 *
 *  Returns a struct thread *, or NULL if no thread is idle. This function must
 *  return the 0th index kthread the first time it is called for a proc,
 *  because the first runtime thread is added to the 0th kthread's runqueue and
 *  no kthreads are attached yet, so that thread cannot be stolen.
 */
static struct thread *pick_thread_for_proc(struct proc *p, int core)
{
	struct thread *lastth;

	/* must return the 0th kthread when a process first starts */
	if (unlikely(p->launched)) {
		p->launched = false;
		return list_top(&p->idle_threads, struct thread, idle_link);
	}

	/* if this proc was preempted down to 0 threads, reuse the kthread that
	   ran most recently, because it has all the queued threads and packets */
	if (proc_is_overloaded(p) && p->active_thread_count == 0)
		return list_top(&p->idle_threads, struct thread, idle_link);

	/* try to reuse the same kthread on this core */
	lastth = core_history[core].current;
	if (lastth && lastth->p == p && lastth->parked)
		return lastth;

	/* try to reuse the previous kthread on this core */
	lastth = core_history[core].prev;
	if (lastth && lastth->p == p && lastth->parked)
		return lastth;

	/* return the least recently parked kthread */
	return list_tail(&p->idle_threads, struct thread, idle_link);
}

/**
 * pick_core_for_proc - choose a core to allocate to proc p.
 * @p: the process to allocate a core to
 *
 * Returns an available core if one exists, or else a core to be preempted.
 */
static int pick_core_for_proc(struct proc *p)
{
	int buddy_core, core;
	int i;
	struct thread *t;
	struct proc *buddy_proc, *core_proc;

#ifndef CORES_NOHT
	/* try to allocate a hyperthread pair core */
	for (i = 0; i < p->active_thread_count; i++) {
		t = p->active_threads[i];
		buddy_core = cpu_to_sibling_cpu(t->core);

		if (core_available(buddy_core))
			return buddy_core;

		if (nr_avail_cores > 0 || core_is_preempting(buddy_core))
			continue;

		buddy_proc = core_history[buddy_core].current->p;
		if (buddy_proc != p && proc_is_bursting(buddy_proc))
			return buddy_core;
	}
#endif

	/* try the core that we most recently ran on */
	t = list_top(&p->idle_threads, struct thread, idle_link);
	core = t->core;
	if (core_available(core))
		return core;

	/* core is busy, should we preempt it? */
	if (nr_avail_cores == 0 && !core_is_preempting(core)) {
		core_proc = core_history[core].current->p;
		if (core_proc != p && proc_is_bursting(core_proc))
			return core;
	}

	/* pick the lowest available core */
	core = bitmap_find_next_set(avail_cores, cpu_count, 0);
	if (core != cpu_count)
		return core;

	/* no cores available, take from the first bursting proc */
	for (i = 0; i < cpu_count; i++) {
		if (!core_history[i].current)
			continue;
		if (!proc_is_bursting(core_history[i].current->p))
			continue;
		if (core_is_preempting(i))
			continue;
		return i;
	}

	log_err("pick_core_for_proc: no available cores!");
	BUG();
	return 0;
}

/**
 * pick_thread_for_core - choose a thread to grant this core to.
 * @core: the core to find a process for
 *
 * Returns a thread to grant this core to, or NULL if no process want another
 * core.
 */
static struct thread *pick_thread_for_core(int core)
{
	int buddy_core;
	struct proc *p;

	/* With ksched, this function is no longer called mid-preemption */
	assert(!core_is_preempting(core));

	/* try to find an overloaded proc to grant this core to */
	if (no_overloaded_procs())
		return NULL;

	/* try to allocate to the process that used this core most recently */
	if (core_history[core].current) {
		p = core_history[core].current->p;
		if (!p->removed && proc_is_overloaded(p))
			goto chose_proc;
	}

#ifndef CORES_NOHT
	/* try to allocate to the process running on the hyperthread pair core */
	buddy_core = cpu_to_sibling_cpu(core);
	if (core_history[buddy_core].current) {
		p = core_history[buddy_core].current->p;
		if (!p->removed && proc_is_overloaded(p))
			goto chose_proc;
	}
#endif

	/* try to allocate to the process that used this core previously */
	if (core_history[core].prev) {
		p = core_history[core].prev->p;
		if (!p->removed && proc_is_overloaded(p))
			goto chose_proc;
	}

	/* choose any overloaded proc */
	p = get_overloaded_proc();

chose_proc:
	return pick_thread_for_proc(p, core);
}


/**
 * cores_park_kthread - parks the given kthread and frees its core.
 * @th: thread to park
 * @force: true if this kthread should be parked regardless of pending tx pkts
 *
 */
void cores_park_kthread(struct thread *th, bool force)
{
	struct proc *p = th->p;
	unsigned int core = th->core;
	unsigned int kthread = th - p->threads;
	struct thread *th_new;

	assert(kthread < NCPU);

	/* make sure this core and kthread are currently reserved */
	assert(!bitmap_test(avail_cores, core));
	assert(!bitmap_test(p->available_threads, kthread));
	assert(core_history[core].current == th);

	thread_cede(th);
	if (core_is_preempting(core)) {
		core_finish_preempt(core);
		return;
	}

	/* mark core as available */
	core_cede(core);

	/* try to find another thread to run on this core */
	th_new = pick_thread_for_core(core);
	if (th_new) {
		thread_reserve(th_new, core);
		core_wake_idle(core, th_new);
	}
}

/**
 * cores_add_core - allocate a core for this process. If the core is idle, this
 * function immediately wakes a kthread on it. Otherwise, a kthread will be
 * woken on the core once the preempted kthread parks.
 * @p: the process to allocate a core to
 */
struct thread *cores_add_core(struct proc *p)
{
	int core;
	struct thread *th, *th_current;

	/* can't add cores if we're already using all available kthreads */
	if (p->active_thread_count == p->thread_count)
		return NULL;

	/* pick a core to add and a thread to run on it */
	core = pick_core_for_proc(p);
	th = pick_thread_for_proc(p, core);
	if (!th) {
		log_err("cores: proc already has max allowed kthreads (%d)",
			p->thread_count);
		BUG();
	}
	BUG_ON(!bitmap_test(p->available_threads, th - p->threads));

	thread_reserve(th, core);

	if (core_available(core)) {
		/* core is idle, immediately wake a kthread on it */
		core_wake_idle(core, th);
		return th;
	}

	/* core is busy, preempt the currently running thread */
	th_current = core_history[core].current;
	proc_set_overloaded(th_current->p);
	BUG_ON(core_is_preempting(core));
	core_preempt(core, th);

	return th;
}

/*
 * Pins thread tid to core. Returns 0 on success and < 0 on error. Note that
 * this function can always fail with error ESRCH, because threads can be
 * killed at any time.
 */
int cores_pin_thread(pid_t tid, int core)
{
	cpu_set_t cpuset;
	int ret;

	CPU_ZERO(&cpuset);
	CPU_SET(core, &cpuset);

	ret = sched_setaffinity(tid, sizeof(cpu_set_t), &cpuset);
	if (ret < 0) {
		log_warn("cores: failed to set affinity for thread %d with err %d",
				tid, errno);
		return -errno;
	}

	return 0;
}

/*
 * Initialize proc state for managing cores.
 */
void cores_init_proc(struct proc *p)
{
	int i;

	/* all threads are initially pinned to the linux core and will park
	 * themselves immediately */
	p->active_thread_count = 0;
	bitmap_init(p->available_threads, p->thread_count, true);
	list_head_init(&p->idle_threads);
	p->inflight_preempts = 0;
	for (i = 0; i < p->thread_count; i++) {
		/* init core to 0 - this will result in incorrect cache locality
		 * decisions at first but saves us from always checking if this thread
		 * has run yet */
		p->threads[i].core = bitmap_find_next_set(online_cores, NCPU, 0);
		list_add_tail(&p->idle_threads, &p->threads[i].idle_link);
	}

	p->bursting = false;
	p->overloaded = false;
	p->launched = true;

	/* wake the first kthread so the runtime can run the main_fn */
	cores_add_core(p);
}

/*
 * Free cores used by a proc that is exiting.
 */
void cores_free_proc(struct proc *p)
{
	int i;

	proc_clear_bursting(p);
	proc_clear_overloaded(p);

	// FIXME: this needs to interact with ksched in a reasonable way
	bitmap_for_each_cleared(p->available_threads, p->thread_count, i)
		cores_park_kthread(&p->threads[i], true);
}

static bool hwq_busy(struct hwq *h, uint32_t cq_idx)
{
	uint32_t idx, parity, hd_parity;
	unsigned char *addr;

	idx = cq_idx & (h->nr_descriptors - 1);
	parity = !!(cq_idx & h->nr_descriptors);
	addr = h->descriptor_table + (h->descriptor_size * idx) + h->parity_byte_offset;
	hd_parity = !!(ACCESS_ONCE(*addr) & h->parity_bit_mask);

	return parity == hd_parity;
}

static bool cores_is_proc_congested(struct proc *p)
{
	struct thread *th;
	struct bundle *b;
	uint32_t rq_tail, rxq_tail, last_rq_head, last_rxq_head;
	unsigned int timern;
	uint64_t cur_tsc, next_deadline_tsc;
	bool last_pending, congested = false, bundle_congestion = false, inflight_wakeup = false;
	int i, j;

	for (i = 0; i < p->active_thread_count; i++) {
		th = p->active_threads[i];

		/* update the queue positions */
		rq_tail = load_acquire(&th->q_ptrs->rq_tail);
		rxq_tail = lrpc_poll_send_tail(&th->rxcmdq);
		last_rq_head = th->last_rq_head;
		last_rxq_head = th->last_rxq_head;
		th->last_rq_head = ACCESS_ONCE(th->q_ptrs->rq_head);
		th->last_rxq_head = ACCESS_ONCE(th->rxcmdq.send_head);

		/* if one prior queue was congested, no need to find more */
		if (congested)
			continue;

		inflight_wakeup |= th->waking;
		/* if the thread just woke up, give it a pass this round */
		if (th->waking) {
			th->waking = false;
			continue;
		}

		/* check if the runqueue is congested */
		if (wraps_lt(rq_tail, last_rq_head)) {
			STAT_INC(RQ_GRANT, 1);
			congested = true;
			continue;
		}

		/* check if the RX queue is congested */
		if (wraps_lt(rxq_tail, last_rxq_head)) {
			STAT_INC(RX_GRANT, 1);
			congested = true;
			continue;
		}
	}

	/* Check bundles for queued I/O */
	cur_tsc = rdtsc();
	for (i = 0; i < p->bundle_count; i++) {
		b = &p->bundles[i];

		for (j = 0; j < b->hwq_count; j++) {
			struct hwq *h = &b->qs[j];
			last_rxq_head = h->cq_idx;
			h->cq_idx = ACCESS_ONCE(*h->consumer_idx);
			last_pending = h->cq_pending;
			h->cq_pending = hwq_busy(h, h->cq_idx);

			/* fast path */
			if (!p->active_thread_count && h->cq_pending) {
				congested = true;
				continue;
			}

			if (last_rxq_head == h->cq_idx && last_pending && h->cq_pending) {
				bundle_congestion = true;
				continue;
			}
		}

		for (j = 0; j < b->timer_count; j++) {
			struct timer *t = &b->timers[j];
			timern = ACCESS_ONCE(*t->timern);
			next_deadline_tsc = ACCESS_ONCE(*t->next_deadline_tsc);

			/* check timer */
			if (!timern || next_deadline_tsc > cur_tsc)
				continue;

			if (!p->active_thread_count) {
				congested = true;
				continue;
			}

			if (next_deadline_tsc + CORES_ADJUST_INTERVAL_US * cycles_per_us < cur_tsc) {
				bundle_congestion = true;
				continue;
			}
		}

	}

	bundle_congestion &= !inflight_wakeup;

	return congested | bundle_congestion;
}

bool poll_core_queues(void)
{
	int cpu;
	bool work_done = false;
	uint64_t cmd;
	unsigned long payload;

	bitmap_for_each_set(online_cores, NCPU, cpu) {
		while (lrpc_recv(&core_history[cpu].cmdq_in, &cmd, &payload)) {
			struct thread *t = core_history[cpu].current;
			switch (cmd) {
				case TXCMD_PARKED:
					cores_park_kthread(t, false);
					/* notify another kthread if the park was involuntary */
					if (payload != 0)
						rx_send_to_runtime(t->p, 0, RX_JOIN, payload);
					break;
				default:
					BUG();
			}
			work_done = true;
		}
	}

	flush_wake_requests();

	return work_done;
}

/*
 * Rebalances the allocation of cores to runtimes. Grants more cores to
 * runtimes that would benefit from them.
 */
void cores_adjust_assignments(void)
{
	struct proc *p, *next;
	int i;

	/* determine which procs need more cores to meet their guarantees, and
	   which procs want more burstable cores */
	for (i = 0; i < dp.nr_clients; i++) {
		p = dp.clients[i];

		/* clear overloaded flag as long as we haven't been preempted
		   down to 0 cores */
		if (p->active_thread_count - p->inflight_preempts > 0)
			proc_clear_overloaded(p);

		if (!cores_is_proc_congested(p))
			continue;

		/* the proc is congested, add cores if possible */
		if (p->active_thread_count < p->sched_cfg.guaranteed_cores)
			cores_add_core(p);
		else
			proc_set_overloaded(p);
	}

	/* grant cores to procs that are bursting until we run out of cores */
	list_for_each_safe(&overloaded_procs, p, next, overloaded_link) {
		if (nr_avail_cores == 0)
			break;

		cores_add_core(p);
	}

	flush_wake_requests();
}

/*
 * Intialize ksched interface
 */
static int ksched_init(void)
{
	int ret, cpu;

	ksched_fd = open("/dev/ksched", O_RDWR);
	if (ksched_fd < 0)
		return ksched_fd;

	struct ksched_init_args args = {
		.size = ARRAY_SIZE(online_cores),
		.bitmap = online_cores,
	};

	ret = ioctl(ksched_fd, KSCHED_IOC_INIT, &args);
	if (ret)
		return ret;

	ksched_shm = mmap(NULL, KSCHED_SHM_SIZE, PROT_READ|PROT_WRITE,
		  MAP_SHARED, ksched_fd, 0);
	if (ksched_shm == MAP_FAILED) {
		log_err("ksched_init: map failed");
		return -EINVAL;
	}

	bitmap_for_each_set(online_cores, NCPU, cpu) {
		ret = lrpc_init_in(
		    &core_history[cpu].cmdq_in,
		    (struct lrpc_msg *)&ksched_shm[cpu].core_to_iok.tbl,
		    KSCHED_PERCORE_TBL_SIZE, &ksched_shm[cpu].core_to_iok.wb);
		if (ret)
			return ret;

		ret = lrpc_init_out(
		    &core_history[cpu].cmdq_out,
		    (struct lrpc_msg *)&ksched_shm[cpu].iok_to_core.tbl,
		    KSCHED_PERCORE_TBL_SIZE, &ksched_shm[cpu].iok_to_core.wb);
		if (ret)
			return ret;

		core_history[cpu].cached_gen = 0;
	}

	preempt_reqs = malloc(sizeof(struct ksched_preempt_req) + NCPU * sizeof(int));
	if (!preempt_reqs)
		return -ENOMEM;

	preempt_reqs->nr = 0;

	return 0;
}

/*
 * Initialize core state.
 */
int cores_init(void)
{
	int i, j, ret;

	/* assign first non-zero core on socket 0 to the dataplane thread */
	for (i = 1; i < cpu_count; i++) {
		if (cpu_info_tbl[i].package == 0)
			break;
	}
	if (i == cpu_count)
		panic("cores: couldn't find any cores on package 0");
	core_assign.dp_core = i;

	cores_pin_thread(gettid(), core_assign.dp_core);

	/* parse hyperthread information */
	for (i = 0; i < cpu_count; i++) {
		int siblings = 0;

		bitmap_for_each_set(cpu_info_tbl[i].thread_siblings_mask,
				    cpu_count, j) {
			if (i == j)
				continue;
			if (siblings++) {
				panic("cores: can't support more than two "
				      "hyperthreads per core.");
			}

			cpu_siblings[i] = j;
		}

		if (siblings == 0)
			panic("cores: must have hyperthreads enabled");
	}

	/* assign the dataplane's sibling to linux and the control thread */
	core_assign.linux_core = cpu_to_sibling_cpu(core_assign.dp_core);
	core_assign.ctrl_core = cpu_to_sibling_cpu(core_assign.dp_core);

	/* mark all cores as unavailable */
	bitmap_init(avail_cores, cpu_count, false);
	bitmap_init(online_cores, NCPU, false);

	/* mark all cores as offline */
	bitmap_init(online_cores, cpu_count, false);


	/* find cores on socket 0 that are not already in use */
	for (i = 0; i < cpu_count; i++) {
		if (i == core_assign.linux_core ||
		    i == core_assign.ctrl_core ||
		    i == core_assign.dp_core) {
			continue;
		}

#ifdef CORES_NOHT
		/* disable hyperthreads */
		j = bitmap_find_next_set(cpu_info_tbl[i].thread_siblings_mask,
					 cpu_count, 0);
		if (i != j)
			continue;
#endif

		if (cpu_info_tbl[i].package == 0)
			core_init(i);
	}

	ret = ksched_init();
	if (ret)
		return ret;


	log_info("cores: linux on core %d, control on %d, dataplane on %d",
		 core_assign.linux_core, core_assign.ctrl_core,
		 core_assign.dp_core);

	return 0;
}
