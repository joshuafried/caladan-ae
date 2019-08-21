/*
 * runtime.h - runtime initialization and metrics
 */

#pragma once

#include <base/stddef.h>
#include <runtime/thread.h>


/* main initialization */
typedef int (*initializer_fn_t)(void);

extern int runtime_set_initializers(initializer_fn_t global_fn,
				    initializer_fn_t perthread_fn,
				    initializer_fn_t late_fn);
extern int runtime_init(const char *cfgpath, thread_fn_t main_fn, void *arg);


extern struct congestion_info *runtime_congestion;

/**
 * runtime_queueing_delay_us - returns the queueing delay of rq and rxq
 */
static inline uint64_t runtime_queueing_delay_us(void)
{
  return ACCESS_ONCE(runtime_congestion->queueing_delay);
}

/**
 * runtime_load - returns the current CPU usage load
 */
static inline float runtime_load(void)
{
	return ACCESS_ONCE(runtime_congestion->load);
}

extern unsigned int maxks;
extern unsigned int guaranteedks;
extern atomic_t runningks;
extern __thread unsigned int kthread_idx;

/**
 * runtime_active_cores - returns the number of currently active cores
 *
 */
static inline int runtime_active_cores(void)
{
	return atomic_read(&runningks);
}

/**
 * runtime_max_cores - returns the maximum number of cores
 *
 * The runtime could be given at most this number of cores by the IOKernel.
 */
static inline int runtime_max_cores(void)
{
	return maxks;
}

/**
 * runtime_guaranteed_cores - returns the guaranteed number of cores
 *
 * The runtime will get at least this number of cores by the IOKernel if it
 * requires them.
 */
static inline int runtime_guaranteed_cores(void)
{
	return guaranteedks;
}

/**
 * runtime_kthread_idx - returns the index of the kthread
 *
 */
static inline int runtime_kthread_idx(void)
{
  return kthread_idx;
}
