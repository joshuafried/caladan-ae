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
/* tracks current cpu id */
__thread unsigned int curr_cpu;
__thread unsigned int curr_phys_cpu;

static __thread int ksched_fd;

static DEFINE_BITMAP(possible_cores, NCPU);

DEFINE_BITMAP(core_awake, NCPU);
atomic64_t kthread_gen __aligned(CACHE_LINE_SIZE);

static DEFINE_SPINLOCK(kthread_assignment_lock);
/* write access is protected by kthread_assignment_lock */
uint64_t kthread_assignment_gen;
unsigned int bundle_assignments[MAX_BUNDLES];
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

	bitmap_atomic_clear(core_awake, last_core);
	atomic64_inc(&kthread_gen);

	BUG_ON(ioctl(ksched_fd, KSCHED_IOC_PARK, args));
#if 0
	while (ioctl(ksched_fd, KSCHED_IOC_PARK)) {
		BUG_ON(errno != EINTR);
		BUG_ON(!preempt_needed());
		clear_preempt_needed();
	}
#endif

	k->curr_cpu = curr_cpu = sched_getcpu();
	curr_phys_cpu = min(curr_cpu, cpu_map[curr_cpu].sibling_core);

	store_release(&cpu_map[k->curr_cpu].recent_kthread, k);
	bitmap_atomic_set(core_awake, k->curr_cpu);
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
	k->curr_cpu = curr_cpu = sched_getcpu();
	curr_phys_cpu = min(curr_cpu, cpu_map[curr_cpu].sibling_core);
	store_release(&cpu_map[k->curr_cpu].recent_kthread, k);
	bitmap_atomic_set(core_awake, k->curr_cpu);
	atomic64_inc(&kthread_gen);
	__update_assignments();

	/* attach the kthread for the first time */
	kthread_attach();
	atomic_inc(&runningks);
}

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

int kthread_init(void)
{
	int i, j, sibling, core, ret, total = 0;

	ret = discover_cpus();
	if (ret)
		return ret;

	bitmap_for_each_set(possible_cores, NCPU, i) {
		sibling = bitmap_find_next_set(cpu_info_tbl[i].thread_siblings_mask,
			  cpu_count, 0);

		if (sibling < i && bitmap_test(possible_cores, sibling))
			continue;

		core = min(i, sibling);

		for (j = 0; j < nr_bundles; j++)
			preference_table[core][j] = (total + j) % nr_bundles;

		total++;
	}
	return 0;
}

void __update_assignments(void)
{
	int i, q = 0, phys_id, n = 0;
	uint64_t cur_gen;

	DEFINE_BITMAP(q_done, nr_bundles);
	DEFINE_BITMAP(phys_awake, cpu_count);

	bitmap_init(q_done, nr_bundles, false);
	bitmap_init(phys_awake, cpu_count, false);

	if (!spin_try_lock_np(&kthread_assignment_lock))
		return;

	cur_gen = atomic64_read(&kthread_gen);

	if (cur_gen == kthread_assignment_gen) {
		spin_unlock_np(&kthread_assignment_lock);
		return;
	}

	store_release(&kthread_assignment_gen, cur_gen);

	bitmap_for_each_set(core_awake, cpu_count, i) {
		phys_id = min(i, cpu_map[i].sibling_core);
		bitmap_set(phys_awake, phys_id);
	}

	while (true) {
		bitmap_for_each_set(phys_awake, cpu_count, phys_id) {
#ifdef RANDOM_ASSIGN
			q = (n + cur_gen) & (nr_bundles - 1);
#else
			for (i = 0; i < nr_bundles; i++) {
				q = preference_table[phys_id][i];
				if (!bitmap_test(q_done, q))
					break;
			}
			assert(i < nr_bundles);
			bitmap_set(q_done, q);
#endif
			ACCESS_ONCE(bundle_assignments[q]) = phys_id;
			if (++n == nr_bundles) {
				spin_unlock_np(&kthread_assignment_lock);
				return;
			}
		}
	}
}
