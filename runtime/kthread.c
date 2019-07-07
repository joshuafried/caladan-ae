/*
 * kthread.c - support for adding and removing kernel threads
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>

#include <base/atomic.h>
#include <base/cpu.h>
#include <base/list.h>
#include <base/lock.h>
#include <base/log.h>
#include <base/random.h>
#include <runtime/sync.h>
#include <runtime/timer.h>

#include "defs.h"

#include "../ksched/ksched.h"

/* protects @ks and @nrks below */
DEFINE_SPINLOCK(klock);
/* the maximum number of kthreads */
unsigned int maxks;
/* the number of busy spinning kthreads (threads that don't park) */
unsigned int spinks;
/* the number of guaranteed kthreads (we can always have this many if we want,
 * must be >= 1) */
unsigned int guaranteedks = 1;
/* the number of active kthreads */
static atomic_t runningks;
/* an array of all kthreads, attached or detached (@maxks in total) */
struct kthread *allks[NCPU];
/* kernel thread-local data */
__thread struct kthread *mykthread;
/* Map of cpu to kthread */
struct cpu_record cpu_map[NCPU] __attribute__((aligned(CACHE_LINE_SIZE)));
/* Map of cpu to sibling */
unsigned long cpu_sibling[NCPU];
/* tracks current kthread id */
__thread unsigned int kthread_id;

static __thread int ksched_fd;

#if 0
static DEFINE_BITMAP(possible_cores, NCPU);
#endif

DEFINE_BITMAP(kthread_awake, NCPU);
atomic64_t kthread_gen __aligned(CACHE_LINE_SIZE);

static DEFINE_SPINLOCK(kthread_assignment_lock);
/* write access is protected by kthread_assignment_lock */
uint64_t kthread_assignment_gen;
static unsigned int bundle_assignments[MAX_BUNDLES];
void __update_assignments(void);

unsigned int preference_table[NCPU][MAX_BUNDLES];

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
	k->detached = true;

	return k;
}

/**
 * kthread_init_thread - initializes state for the kthread
 *
 * Returns 0 if successful, or -ENOMEM if out of memory.
 */
int kthread_init_thread(void)
{
	static int allksn = 0;

	mykthread = allock();
	if (!mykthread)
		return -ENOMEM;

	spin_lock_np(&klock);
	mykthread->kthread_idx = allksn;
	kthread_id = allksn;
	allks[allksn++] = mykthread;
	assert(allksn <= maxks);
	spin_unlock_np(&klock);

	return 0;
}

/**
 * kthread_attach - attaches the thread-local kthread to the runtime if it isn't
 * already attached
 *
 * An attached kthread participates in scheduling, RCU, and I/O.
 */
static void kthread_attach(void)
{
	struct kthread *k = myk();

	assert(k->parked == false);
	assert(k->detached == true);

	k->detached = false;
}

/**
 * kthread_detach_locked - detaches a kthread from the runtime
 * @r: the remote kthread to detach
 *
 * @r->lock must be held before calling this function.
 *
 * A detached kthread can no longer be stolen from. It must not receive I/O,
 * have outstanding timers, or participate in RCU.
 */
void kthread_detach(struct kthread *r)
{
	struct kthread *k = myk();

	assert_spin_lock_held(&r->lock);
	assert(r != k);
	assert(r->parked == true);
	assert(r->detached == false);

	/* steal all overflow packets and completions */
	mbufq_merge_to_tail(&k->txpktq_overflow, &r->txpktq_overflow);
	mbufq_merge_to_tail(&k->txcmdq_overflow, &r->txcmdq_overflow);

	/* verify the kthread is correctly detached */
	assert(r->rq_head == r->rq_tail);
	assert(list_empty(&r->rq_overflow));
	assert(mbufq_empty(&r->txpktq_overflow));
	assert(mbufq_empty(&r->txcmdq_overflow));

	/* set state */
	r->detached = true;
}

/*
 * kthread_yield_to_iokernel - block on eventfd until iokernel wakes us up
 */
static void kthread_yield_to_iokernel(struct ksched_park_args *args)
{
	struct kthread *k = myk();

	clear_preempt_needed();
	uint64_t last_core = k->curr_cpu;

	bitmap_atomic_clear(kthread_awake, k->kthread_idx);
	atomic64_inc(&kthread_gen);

	BUG_ON(ioctl(ksched_fd, KSCHED_IOC_PARK, args));
#if 0
	while (ioctl(ksched_fd, KSCHED_IOC_PARK)) {
		BUG_ON(errno != EINTR);
		BUG_ON(!preempt_needed());
		clear_preempt_needed();
	}
#endif

	k->curr_cpu = sched_getcpu();

	store_release(&cpu_map[k->curr_cpu].recent_kthread, k);
	bitmap_atomic_set(kthread_awake, k->kthread_idx);
	atomic64_inc(&kthread_gen);

	__update_assignments();

	if (k->curr_cpu != last_core)
		STAT(CORE_MIGRATIONS)++;

}

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
	struct ksched_park_args args = {
		.cmd = TXCMD_PARKED,
		.payload = 0,
	};

	if (!voluntary ||
	    !mbufq_empty(&k->txpktq_overflow) ||
	    !mbufq_empty(&k->txcmdq_overflow)) {
		args.payload = k->kthread_idx + 1;
	}

	assert_spin_lock_held(&k->lock);
	assert(k->parked == false);

	/* atomically verify we have at least @spinks kthreads running */
	if (atomic_read(&runningks) <= spinks)
		return;
	if (unlikely(atomic_sub_and_fetch(&runningks, 1) < spinks)) {
		atomic_inc(&runningks);
		return;
	}

	uint64_t now = microtime();

	k->parked = true;
	k->park_us = now;
	STAT(PARKS)++;
	spin_unlock(&k->lock);

	kthread_yield_to_iokernel(&args);

	/* iokernel has unparked us */

	spin_lock(&k->lock);
	k->parked = false;
	atomic_inc(&runningks);

	/* reattach kthread if necessary */
	if (k->detached)
		kthread_attach();
}

/**
 * kthread_wait_to_attach - block this kthread until the iokernel wakes it up.
 *
 * This variant is intended for initialization.
 */
void kthread_wait_to_attach(void)
{
	struct kthread *k = myk();
	ksched_fd = open("/dev/ksched", O_RDONLY);
	BUG_ON(ksched_fd < 0);

	BUG_ON(ioctl(ksched_fd, KSCHED_IOC_START));
	k->curr_cpu = sched_getcpu();
	store_release(&cpu_map[k->curr_cpu].recent_kthread, k);
	bitmap_atomic_set(kthread_awake, k->kthread_idx);
	atomic64_inc(&kthread_gen);
	__update_assignments();

	/* attach the kthread for the first time */
	kthread_attach();
	atomic_inc(&runningks);
}

#if 0
static int discover_cpus(void)
{
	int ret;
	char buf[4096];
	FILE *f;

	f = fopen(IOK_ONLINE_CORE_PATH, "r");
	if (f == NULL)
		return -errno;

	if (!fgets(buf, 4096, f)) {
		ret = -errno;
		goto done;
	}

	buf[strlen(buf) - 1] = 0;

	ret = string_to_bitmap(buf, possible_cores, NCPU);

done:
	fclose(f);
	return ret;
}
#endif

int kthread_init(void)
{
	int i, j;

	for (i = 0; i < maxks; i++)
		for (j = 0; j < nr_bundles; j++)
			preference_table[i][j] = (i + j) % nr_bundles;

	return 0;
}

static inline void assign_bundle(unsigned int bundle_idx, unsigned int kthread_idx)
{
	struct io_bundle *b = &bundles[bundle_idx];

	if (bundle_assignments[bundle_idx] == kthread_idx)
		return;

	rcu_hlist_del(&b->link);
	rcu_hlist_add_head(&bundle_assignment_list[kthread_idx], &b->link);
	bundle_assignments[bundle_idx] = kthread_idx;
}

void __update_assignments(void)
{
	int i, q = 0, idx, sib, n = 0;
	uint64_t cur_gen;
	struct kthread *k;

	DEFINE_BITMAP(q_done, nr_bundles);
	bitmap_init(q_done, nr_bundles, false);

	if (!spin_try_lock_np(&kthread_assignment_lock))
		return;

	cur_gen = atomic64_read(&kthread_gen);

	if (cur_gen == kthread_assignment_gen) {
		spin_unlock_np(&kthread_assignment_lock);
		return;
	}

	ACCESS_ONCE(kthread_assignment_gen) = cur_gen;

	/* assign "identity" queues to awake kthreads */
	bitmap_for_each_set(kthread_awake, maxks, idx) {
		assign_bundle(idx, idx);
		bitmap_set(q_done, idx);
		n++;

		/* absorb bundle from recently-parked sibling core */
		sib = cpu_sibling[allks[idx]->curr_cpu];
		k = cpu_map[sib].recent_kthread;
		if (!k || bitmap_test(kthread_awake, k->kthread_idx))
			continue;

		assign_bundle(k->kthread_idx, idx);
		bitmap_set(q_done, k->kthread_idx);
		n++;
	}

	while (n < nr_bundles) {
		bitmap_for_each_set(kthread_awake, maxks, idx) {
			for (i = 0; i < nr_bundles; i++) {
				q = preference_table[idx][i];
				if (!bitmap_test(q_done, q))
					break;
			}
			BUG_ON(i == nr_bundles);
			bitmap_set(q_done, q);
			assign_bundle(q, idx);
			if (++n == nr_bundles) {
				goto out;
			}
		}
	}
out:
	spin_unlock_np(&kthread_assignment_lock);
	return;
}
