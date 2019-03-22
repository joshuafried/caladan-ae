/*
 * kthread.c - support for adding and removing kernel threads
 */

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
/* the total number of attached kthreads (i.e. the size of @ks) */
unsigned int nrks;
/* the number of busy spinning kthreads (threads that don't park) */
unsigned int spinks;
/* the number of guaranteed kthreads (we can always have this many if we want,
 * must be >= 1) */
unsigned int guaranteedks = 1;
/* the number of active kthreads */
static atomic_t runningks;
/* an array of attached kthreads (@nrks in total) */
struct kthread *ks[NCPU];
/* an array of all kthreads, attached or detached (@maxks in total) */
struct kthread *allks[NCPU];
/* kernel thread-local data */
__thread struct kthread *mykthread;
/* Map of cpu to kthread */
struct cpu_record cpu_map[NCPU] __attribute__((aligned(CACHE_LINE_SIZE)));

static __thread int ksched_fd;

static DEFINE_BITMAP(core_awake, NCPU);
 atomic64_t kthread_gen __aligned(CACHE_LINE_SIZE);

static unsigned int preference_table[NCPU][MAX_BUNDLES];
__thread uint64_t last_kthread_gen;
__thread unsigned long cached_assignments[NCPU][MAX_BUNDLES];
__thread unsigned long assignment_count[NCPU];



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

	spin_lock(&klock);
	assert(nrks < maxks);
	ks[nrks] = k;
	store_release(&nrks, nrks + 1);
	spin_unlock(&klock);
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
	int i;

	assert_spin_lock_held(&r->lock);
	assert(r != k);
	assert(r->parked == true);
	assert(r->detached == false);

	/* make sure the park rxcmd was processed */
	lrpc_poll_send_tail(&r->txcmdq);
	if (unlikely(lrpc_get_cached_length(&r->txcmdq) > 0))
		return;


	spin_lock(&klock);
	assert(r != k);
	assert(nrks > 0);
	for (i = 0; i < nrks; i++)
		if (ks[i] == r)
			goto found;
	BUG();

found:
	ks[i] = ks[--nrks];
	spin_unlock(&klock);

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
static void kthread_yield_to_iokernel(void)
{
	struct kthread *k = myk();

	clear_preempt_needed();
	uint64_t last_core = k->curr_cpu;

	bitmap_atomic_clear(core_awake, last_core);
	atomic64_inc(&kthread_gen);

	BUG_ON(ioctl(ksched_fd, KSCHED_IOC_PARK));
#if 0
	while (ioctl(ksched_fd, KSCHED_IOC_PARK)) {
		BUG_ON(errno != EINTR);
		BUG_ON(!preempt_needed());
		clear_preempt_needed();
	}
#endif

	k->curr_cpu = sched_getcpu();

	bitmap_atomic_set(core_awake, k->curr_cpu);
	atomic64_inc(&kthread_gen);

	if (k->curr_cpu != last_core)
		STAT(CORE_MIGRATIONS)++;
	store_release(&cpu_map[k->curr_cpu].recent_kthread, k);
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
	unsigned long payload = 0;
	uint64_t cmd = TXCMD_PARKED;

	if (!voluntary ||
	    !mbufq_empty(&k->txpktq_overflow) ||
	    !mbufq_empty(&k->txcmdq_overflow)) {
		payload = (unsigned long)k;
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

	/* signal to iokernel that we're about to park */
	while (!lrpc_send(&k->txcmdq, cmd, payload))
		cpu_relax();

	kthread_yield_to_iokernel();

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
	bitmap_atomic_set(core_awake, k->curr_cpu);
	atomic64_inc(&kthread_gen);

	/* attach the kthread for the first time */
	kthread_attach();
	atomic_inc(&runningks);
}

int kthread_init(void)
{
	int i, j, ret;

	unsigned char rands[nr_bundles];

	for (i = 0; i < cpu_count; i++) {
		if (bitmap_find_next_set(cpu_info_tbl[i].thread_siblings_mask,
			  cpu_count, 0) < i)
			continue;

		ret = fill_random_bytes(rands, nr_bundles);
		if (ret)
			return ret;

		for (j = 0; j < nr_bundles; j++)
			preference_table[i][j] = j;

		for (j = nr_bundles - 1; j >= 1; j--)
			swapvars(preference_table[i][j], preference_table[i][rands[j] % j]);
	}

	return 0;
}

static void compute_preferences(void)
{
	int i, j, q, phys_id, n = 0;

	DEFINE_BITMAP(q_done, nr_bundles);
	DEFINE_BITMAP(phys_awake, cpu_count);

	bitmap_init(q_done, nr_bundles, false);
	bitmap_init(phys_awake, cpu_count, false);
	memset(assignment_count, 0, sizeof(unsigned long) * cpu_count);

	bitmap_for_each_set(core_awake, cpu_count, i) {
		phys_id = min(i, cpu_map[i].sibling_core);
		bitmap_set(phys_awake, phys_id);
	}

	while (true) {
		bitmap_for_each_set(phys_awake, cpu_count, phys_id) {
			j = 0;
			do {
				assert(j < nr_bundles);
				q = preference_table[phys_id][j++];
			} while (bitmap_test(q_done, q));

			bitmap_set(q_done, q);
			cached_assignments[phys_id][assignment_count[phys_id]++] = q;
			if (++n == nr_bundles)
				return;
		}
	}
}


unsigned long *__get_queues(struct kthread *k, int *nrqs)
{
	int phys_id;

	last_kthread_gen = atomic64_read(&kthread_gen);
	compute_preferences();

	phys_id = min(k->curr_cpu, cpu_map[k->curr_cpu].sibling_core);
	*nrqs = assignment_count[phys_id];
	return cached_assignments[phys_id];
}

