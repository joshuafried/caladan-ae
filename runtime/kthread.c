/*
 * kthread.c - support for adding and removing kernel threads
 */

#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>

#include <base/atomic.h>
#include <base/cpu.h>
#include <base/list.h>
#include <base/lock.h>
#include <base/log.h>
#include <runtime/sync.h>
#include <runtime/timer.h>

#define __user
#include "defs.h"
#include "../ksched/ksched.h"

/* protects @ks and @nrks below */
DEFINE_SPINLOCK(klock);
/* the maximum number of kthreads */
unsigned int maxks;
/* the total number of attached kthreads (i.e. the size of @ks) */
unsigned int nrks;
/* the number of busy spinning kthreads (threads that don't park) */
unsigned int spinks;
/* the number of guaranteed kthreads (we can always have this many if we want,
 * must be >= 1) */
unsigned int guaranteedks = 1;
/* the number of active kthreads */
atomic_t runningks;
/* an array of attached kthreads (@nrks in total) */
struct kthread *ks[NCPU];
/* kernel thread-local data */
__thread struct kthread *mykthread;
__thread unsigned int kthread_idx;
/* Map of cpu to kthread */
struct cpu_record cpu_map[NCPU] __attribute__((aligned(CACHE_LINE_SIZE)));
/* the file descriptor for the ksched module */
static int ksched_fd;

static struct kthread *allock(void)
{
	struct kthread *k;

	k = aligned_alloc(CACHE_LINE_SIZE,
			  align_up(sizeof(*k), CACHE_LINE_SIZE));
	if (!k)
		return NULL;

	memset(k, 0, sizeof(*k));
	spin_lock_init(&k->lock);
	list_head_init(&k->rq_overflow);
	mbufq_init(&k->txpktq_overflow);
	mbufq_init(&k->txcmdq_overflow);
	spin_lock_init(&k->timer_lock);
	k->park_efd = eventfd(0, 0);
	BUG_ON(k->park_efd < 0);
	return k;
}

/**
 * kthread_init_thread - initializes state for the kthread
 *
 * Returns 0 if successful, or -ENOMEM if out of memory.
 */
int kthread_init_thread(void)
{
	mykthread = allock();
	if (!mykthread)
		return -ENOMEM;

	spin_lock_np(&klock);
	mykthread->kthread_idx = nrks;
	ks[nrks++] = mykthread;
	assert(nrks <= maxks);
	spin_unlock_np(&klock);

	kthread_idx = mykthread->kthread_idx;

	return 0;
}

/*
 * kthread_yield_to_iokernel - block until iokernel wakes us up
 */
static __always_inline void kthread_yield_to_iokernel(void)
{
	struct kthread *k = myk();
	uint64_t assigned_core, last_core = k->curr_cpu;
	ssize_t s;

	clear_preempt_needed();

	s = read(k->park_efd, &assigned_core, sizeof(assigned_core));
	while (unlikely(s != sizeof(uint64_t) && errno == EINTR)) {
		clear_preempt_needed();
		s = read(k->park_efd, &assigned_core, sizeof(assigned_core));
	}


	// /* yield to the iokernel */
	// s = ioctl(ksched_fd, KSCHED_IOC_PARK, 0);
	// while (unlikely(s < 0 || preempt_needed())) {
	// 	/* preempted while yielding, yield again */
	// 	clear_preempt_needed();
	// 	s = ioctl(ksched_fd, KSCHED_IOC_PARK, 0);
	// }

	k->curr_cpu = assigned_core - 1;
	if (k->curr_cpu != last_core)
		STAT(CORE_MIGRATIONS)++;
	store_release(&cpu_map[assigned_core - 1].recent_kthread, k);
}


#ifdef DIRECTPATH

static atomic64_t kthread_gen;
static uint64_t flow_assignment_gen;
static DEFINE_SPINLOCK(flow_assignment_lock);
static DEFINE_BITMAP(kthread_awake, NCPU);

static void flows_update(void)
{
	int i, pos, nrawake;
	uint64_t start = rdtsc(), cur_gen;
	unsigned int fg_map[maxks];
	unsigned int awakeks[maxks];
	DEFINE_BITMAP(kawake_local, NCPU);

again:

	if (!spin_try_lock_np(&flow_assignment_lock))
		goto done;

	cur_gen = atomic64_read(&kthread_gen);
	if (cur_gen == flow_assignment_gen) {
		spin_unlock_np(&flow_assignment_lock);
		goto done;
	}

	ACCESS_ONCE(flow_assignment_gen) = cur_gen;

	/* make a copy of kthread_awake */
	for (i = 0; i < BITMAP_LONG_SIZE(NCPU); i++)
		kawake_local[i] = ACCESS_ONCE(kthread_awake[i]);

	nrawake = 0;
	bitmap_for_each_set(kawake_local, maxks, i) {
		fg_map[i] = i;
		awakeks[nrawake++] = i;
	}

	if (!nrawake)
		goto out;

	pos = 0;
	bitmap_for_each_cleared(kawake_local, maxks, i) {
		/* steer packets away from this kthread */
		fg_map[i] = awakeks[pos++];
		if (pos == nrawake)
			pos = 0;
	}

	net_ops.steer_flows(fg_map);

out:
	spin_unlock_np(&flow_assignment_lock);

	if (unlikely(ACCESS_ONCE(flow_assignment_gen) != atomic64_read(&kthread_gen)))
		goto again;

done:
	STAT(FLOW_STEERING_CYCLES) += rdtsc() - start;
}

static void flows_notify_waking(void)
{
	if (!cfg_directpath_enabled)
		return;

	bitmap_atomic_set(kthread_awake, myk()->kthread_idx);
	atomic64_inc(&kthread_gen);
	flows_update();
}

static void flows_notify_parking(bool voluntary)
{

	if (!cfg_directpath_enabled)
		return;

	bitmap_atomic_clear(kthread_awake, myk()->kthread_idx);
	atomic64_inc(&kthread_gen);
	if (voluntary)
		flows_update();
}

#else
static inline void flows_notify_waking(void) {}
static inline void flows_notify_parking(bool voluntary) {}
#endif


/*
 * kthread_park - block this kthread until the iokernel wakes it up.
 * @voluntary: true if this kthread parked because it had no work left
 *
 * This variant must be called with the local kthread lock held. It is intended
 * for use by the scheduler and for use by signal handlers.
 */
void kthread_park(bool voluntary)
{
	struct kthread *k = myk();
	assert_preempt_disabled();

	/* atomically verify we have at least @spinks kthreads running */
	if (voluntary && atomic_read(&runningks) <= spinks)
		return;
	int remaining_ks = atomic_sub_and_fetch(&runningks, 1);
	if (voluntary && unlikely(remaining_ks < spinks)) {
		atomic_inc(&runningks);
		return;
	}

	flows_notify_parking(voluntary);

	STAT(PARKS)++;

	/* signal to iokernel that we're about to park */
	while (!lrpc_send(&k->txcmdq, TXCMD_PARKED, 0))
		cpu_relax();

	/* perform the actual parking */
	kthread_yield_to_iokernel();

	/* iokernel has unparked us */
	atomic_inc(&runningks);

	flows_notify_waking();
}

/**
 * kthread_wait_to_attach - block this kthread until the iokernel wakes it up.
 *
 * This variant is intended for initialization.
 */
void kthread_wait_to_attach(void)
{
	struct kthread *k = myk();
	int s;

	kthread_yield_to_iokernel();

	// s = ioctl(ksched_fd, KSCHED_IOC_START, 0);
	// BUG_ON(s < 0);

	// k->curr_cpu = s;
	// store_release(&cpu_map[s].recent_kthread, k);

	/* attach the kthread for the first time */
	atomic_inc(&runningks);

	flows_notify_waking();
}

/**
 * kthread_init - intitializes the kthread subsystem
 *
 * Returns 0 if successful.
 */
int kthread_init(void)
{
	return 0;
	ksched_fd = open("/dev/ksched", O_RDWR);
	if (ksched_fd < 0)
		return -errno;
	return 0;
}
