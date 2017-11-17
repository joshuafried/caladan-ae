/*
 * rcu.h - support for read-copy-update
 *
 * TODO: x86 only for now
 */

#include <base/compiler.h>
#include <base/atomic.h>

extern __thread int rcu_read_count;

static inline bool rcu_read_lock_held(void)
{
	return rcu_read_count > 0;
}

static inline void rcu_read_lock(void)
{
	rcu_read_count++;
}

static inline void rcu_read_unlock(void)
{
	assert(rcu_read_lock_held());
	rcu_read_count--;
}

#ifdef __CHECKER__
#define rcu_check_type(p) \
	((void)(((typeof(*(p)) __rcu *)p) == p))
#else /* __CHECKER__ */
#define rcu_check_type(p)
#endif /* __CHECKER__ */

#define RCU_INITIALIZER(v) (typeof(*(v)) __force __rcu *)(v)

/**
 * RCU_INIT_POINTER - initializes an RCU pointer
 * @p: the RCU pointer
 * @v: the initialization value
 *
 * Use this variant at initialization time before the data is shared. Otherwise,
 * you must use rcu_assign_pointer().
 */
#define RCU_INIT_POINTER(p, v)				\
	do {						\
		rcu_check_type(p);			\
		ACCESS_ONCE(p) = RCU_INITIALIZER(v);	\
	} while (0)

/**
 * rcu_dereference_protected - safely dereferences an RCU pointer
 * @p: the RCU pointer
 * @c: a condition proving the access is safe, such as a check of whether a lock
 * is held.
 *
 * An RCU pointer can be safely dereferenced if either the condition @c passes
 * or an RCU read lock is held.
 *
 * Returns the RCU pointer value.
 */
#define rcu_dereference_protected(p, c)			\
	({						\
		rcu_check_type(p);			\
		assert(rcu_read_lock_held() || !!(c));	\
		load_acquire((typeof(*(p)) __force *)(p));\
	})

/**
 * rcu_assign_pointer - safely assigns a new value to an RCU pointer
 * @p: the RCU pointer
 * @v: the value to assign
 */
#define rcu_assign_pointer(p, v)			\
	do {						\
		rcu_check_type(p)			\
		store_release(&p, RCU_INITIALIZER(v));	\
	} while (0)

struct rcu_head;
typedef void (*rcu_callback_t)(struct rcu_head *head);

struct rcu_head {
	struct rcu_head *next;
	rcu_callback_t func;
};

extern void rcu_free(struct rcu_head, rcu_callback_t func);
extern void rcu_synchronize(void);