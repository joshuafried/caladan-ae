/*
 * timer.c - support for timers
 *
 * So far we use a D-ary heap just like the Go runtime. We may want to consider
 * adding a lower-resolution shared timer wheel as well.
 */

#include <limits.h>
#include <stdlib.h>

#include <base/time.h>
#include <runtime/sync.h>
#include <runtime/thread.h>
#include <runtime/timer.h>

#include "defs.h"

/* the arity of the heap */
#define D	4

/**
 * is_valid_heap - checks that the timer heap is a valid min heap
 * @heap: the timer heap
 * @n: the number of timers in the heap
 *
 * Returns true if valid, false otherwise.
 */
static bool is_valid_heap(struct timer_idx *heap, int n)
{
	int i, p;

	/* check that each timer's deadline is later or equal to its parent's
	 * deadline */
	for (i = n-1; i > 1; i--) {
		p = (i - 1) / D;
		if (heap[p].deadline_us > heap[i].deadline_us)
			return false;
	}

	return true;
}

/**
 * timer_heap_is_valid - checks that this kthread's timer heap is a
 * valid min heap
 * @k: the kthread
 */
static void assert_timer_heap_is_valid(struct io_bundle *b)
{
	assert(is_valid_heap(b->timers, b->timern));
}

static void sift_up(struct timer_idx *heap, int i)
{
	struct timer_idx tmp = heap[i];
	int p;

	while (i > 0) {
		p = (i - 1) / D;
		if (tmp.deadline_us >= heap[p].deadline_us)
			break;
		heap[i] = heap[p];
		heap[i].e->idx = i;
		heap[p] = tmp;
		heap[p].e->idx = p;
		i = p;
	}
}

static void sift_down(struct timer_idx *heap, int i, int n)
{
	struct timer_idx tmp = heap[i];
	uint64_t w;
	int c, j;

	while (1) {
		w = tmp.deadline_us;
		c = INT_MAX;
		for (j = (i * D + 1); j <= (i * D + D); j++) {
			if (j >= n)
				break;
			if (heap[j].deadline_us < w) {
				w = heap[j].deadline_us;
				c = j;
			}
		}
		if (c == INT_MAX)
			break;
		heap[i] = heap[c];
		heap[i].e->idx = i;
		heap[c] = tmp;
		heap[c].e->idx = c;
		i = c;
	}
}



/**
 * timer_earliest_deadline - return the first deadline for this kthread or 0 if
 * there are no active timers.
 */
uint64_t timer_earliest_deadline(struct io_bundle *b)
{
	uint64_t deadline_us;

	/* deliberate race condition */
	if (b->timern == 0)
		deadline_us = 0;
	else
		deadline_us = b->timers[0].deadline_us;

	return deadline_us;
}

static void timer_start_locked(struct timer_entry *e, uint64_t deadline_us, struct io_bundle *b)
{
	int i;

	assert_spin_lock_held(&b->lock);

	/* can't insert a timer twice! */
	BUG_ON(e->armed);

	i = b->timern++;
	if (b->timern >= RUNTIME_MAX_TIMERS) {
		/* TODO: support unlimited timers */
		BUG();
	}

	b->timers[i].deadline_us = deadline_us;
	b->timers[i].e = e;
	e->idx = i;
	e->bundle = b;
	sift_up(b->timers, i);
	e->armed = true;
}

/**
 * timer_start - arms a timer
 * @e: the timer entry to start
 * @deadline_us: the deadline in microseconds
 *
 * @e must have been initialized with init_timer().
 */
void timer_start(struct timer_entry *e, uint64_t deadline_us)
{
	struct kthread *k = getk();
	struct io_bundle *b = get_first_bundle(k);

	spin_lock(&b->lock);
	timer_start_locked(e, deadline_us, b);
	spin_unlock(&b->lock);
	putk();
}

/**
 * timer_cancel - cancels a timer
 * @e: the timer entry to cancel
 *
 * Returns true if the timer was successfully cancelled, otherwise it has
 * already fired or was never armed.
 */
bool timer_cancel(struct timer_entry *e)
{
	int last;
	struct io_bundle *b = e->bundle;

	spin_lock_np(&b->lock);

	if (!e->armed) {
		spin_unlock_np(&b->lock);
		return false;
	}
	e->armed = false;

	last = --b->timern;
	if (e->idx == last) {
		spin_unlock_np(&b->lock);
		return true;
	}

	b->timers[e->idx] = b->timers[last];
	b->timers[e->idx].e->idx = e->idx;
	sift_up(b->timers, e->idx);
	sift_down(b->timers, e->idx, b->timern);
	spin_unlock_np(&b->lock);

	return true;
}

static void timer_finish_sleep(unsigned long arg)
{
	thread_t *th = (thread_t *)arg;
	thread_ready(th);
}

static void __timer_sleep(uint64_t deadline_us)
{
	struct io_bundle *b;
	struct kthread *k;
	struct timer_entry e;

	init_timer(&e, timer_finish_sleep, (unsigned long)thread_self());

	k = getk();
	b = get_first_bundle(k);

	spin_lock_np(&b->lock);
	putk();
	timer_start_locked(&e, deadline_us, b);
	thread_park_and_unlock_np(&b->lock);
}

/**
 * timer_sleep_until - sleeps until a deadline
 * @deadline_us: the deadline time in microseconds
 */
void timer_sleep_until(uint64_t deadline_us)
{
	if (unlikely(microtime() >= deadline_us))
		return;

	__timer_sleep(deadline_us);
}

/**
 * timer_sleep - sleeps for a duration
 * @duration_us: the duration time in microseconds
 */
void timer_sleep(uint64_t duration_us)
{
	__timer_sleep(microtime() + duration_us);
}

int timer_gather(struct io_bundle *b, unsigned int budget, struct timer_entry **entries)
{
	struct timer_entry *e;
	uint64_t now_us;
	int i, nr_timeouts = 0;

	assert_spin_lock_held(&b->lock);
	assert_timer_heap_is_valid(b);

	now_us = microtime();
	while (budget-- && b->timern > 0 &&
	       b->timers[0].deadline_us <= now_us) {
		i = --b->timern;
		e = b->timers[0].e;
		e->armed = false;
		if (i > 0) {
			b->timers[0] = b->timers[i];
			b->timers[0].e->idx = 0;
			sift_down(b->timers, 0, i);
		}
		entries[nr_timeouts++] = e;
	}

	return nr_timeouts;
}

int timer_init(void)
{
	int i;
	for (i = 0; i < nr_bundles; i++) {
		bundles[i].timern = 0;
		bundles[i].timers = aligned_alloc(CACHE_LINE_SIZE,
			align_up(sizeof(struct timer_idx) * RUNTIME_MAX_TIMERS,
				 CACHE_LINE_SIZE));
		if (!bundles[i].timers)
			return -ENOMEM;
	}

	return 0;
}
