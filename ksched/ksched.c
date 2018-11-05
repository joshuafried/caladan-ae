/*
 * ksched.c - an accelerated scheduler interface for the IOKernel
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/cdev.h>
#include <linux/cpu.h>
#include <linux/kthread.h>
#include <linux/smp.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <asm/mwait.h>

#include <asm-generic/local.h>
#include "ksched.h"

MODULE_LICENSE("GPL");

/* the character device that provides the ksched IOCTL interface */
static struct cdev ksched_cdev;

#define PID_QUEUE_DEPTH 2
#define PIDS_MASK (PID_QUEUE_DEPTH - 1)

struct ksched_percpu {
	/* Make sure there is no false sharing for gen */
	unsigned long pad1[8];
	unsigned long	gen;
	unsigned long pad2[7 + 8];

	unsigned long waker_cached_gen;
	unsigned long pad3[7];

	unsigned long	last_gen;
	pid_t	pids[PID_QUEUE_DEPTH];

	local_t signalled;
	unsigned int running;
	struct task_struct *waiter_thread;

} ____cacheline_aligned_in_smp;

/* per-cpu data shared between parked cores and the waker core */
static DEFINE_PER_CPU(struct ksched_percpu, kp);

/**
 * ksched_lookup_task - retreives a task from a pid number
 * @nr: the pid number
 *
 * WARNING: must be called inside an RCU read critical section.
 *
 * Returns a task pointer or NULL if none was found.
 */
static struct task_struct *ksched_lookup_task(pid_t nr)
{
	return pid_task(find_vpid(nr), PIDTYPE_PID);
}

/**
 * deschedule - put the curent task to sleep and invoke schedule()
 */
static long deschedule(void)
{
	__set_current_state(TASK_INTERRUPTIBLE);
	schedule();
	__set_current_state(TASK_RUNNING);
	return 0;
}


/**
 * ksched_wakeup_pid - wakeup a task
 * @cpu: the core to pin the task to
 * @pid: the pid number
 *
 * Returns 1 if the process was woken up, 0 if it was already running,
 * or a number less than 0 if an error occurred.
 */
static int ksched_wakeup_pid(int cpu, pid_t pid)
{
	struct task_struct *p;
	int ret;

	rcu_read_lock();
	p = ksched_lookup_task(pid);
	if (!p) {
		rcu_read_unlock();
		return -ESRCH;
	}
	get_task_struct(p);
	rcu_read_unlock();

	ret = set_cpus_allowed_ptr(p, cpumask_of(cpu));
	if (ret) {
		put_task_struct(p);
		return ret;
	}

	ret = wake_up_process(p);
	put_task_struct(p);

	return ret;
}

/**
 * ksched_preempt - signal a task to park
 * @pid: the pid number
 *
 */
static void ksched_preempt(pid_t pid)
{
	struct task_struct *t;

	rcu_read_lock();
	t = ksched_lookup_task(pid);
	if (!t)
		goto out;

	send_sig(SIGUSR1, t, 0);

out:
	rcu_read_unlock();
}

/**
 * run_next - find and wake next task for core
 *
 * WARNING: must be called with interrupts disabled OR p->running == 0
 * to prevent running with the IPI handler
 *
 * Returns the awoken pid or 0 if none could be found
 */
static pid_t run_next(void)
{
	struct ksched_percpu *p;
	unsigned long gen;
	pid_t pid;

	p = this_cpu_ptr(&kp);

	gen = smp_load_acquire(&p->gen);
	if (gen == p->last_gen)
		return 0;

	pid = READ_ONCE(p->pids[p->last_gen++ & PIDS_MASK]);
	if (!pid)
		return 0;

	if (pid != current->pid)
		ksched_wakeup_pid(smp_processor_id(), pid);

	local_set(&p->signalled, 0);
	smp_store_release(&p->running, 1);

	/* if a preempt is pending, make sure that pid reparks */
	/* Now that p->running = 1, we may race with ipi handler */
	if (smp_load_acquire(&p->gen) != p->last_gen &&
			local_inc_return(&p->signalled) == 1)
		ksched_preempt(pid);

	return pid;

}

static int ksched_waiter_thread(void *arg)
{
	struct ksched_percpu *p = this_cpu_ptr(&kp);
	unsigned long gen;

	while (!kthread_should_stop()) {

		if (run_next())
			deschedule();

		gen = smp_load_acquire(&p->gen);
		if (gen != p->last_gen)
			continue;

		/* then arm the monitor address and recheck to avoid a race */
		__monitor(&p->gen, 0, 0);
		gen = smp_load_acquire(&p->gen);
		if (gen != p->last_gen)
			continue;

		/* finally, execute mwait, and recheck after waking up */
		__mwait(0, MWAIT_ECX_INTERRUPT_BREAK);
		gen = smp_load_acquire(&p->gen);
		if (gen != p->last_gen)
			continue;

		/* run another task if needed */
		if (need_resched())
			schedule();
	}

	return 0;
}

static long ksched_park(void)
{
	struct ksched_percpu *p;
	pid_t pid;
	int cpu;

	cpu = get_cpu();
	local_irq_disable();

	if (unlikely(signal_pending(current))) {
		local_irq_enable();
		put_cpu();
		return -ERESTARTSYS;
	}

	p = this_cpu_ptr(&kp);
	pid = run_next();

	/* Is the next pid immediately available? */
	if (pid) {
		local_irq_enable();
		put_cpu();
		if (pid == current->pid) {
			return 0;
		} else {
			return deschedule();
		}
	}

	p->running = 0;
	wake_up_process(p->waiter_thread);
	smp_wmb();
	local_irq_enable();
	put_cpu();
	return deschedule();
}

static void ksched_ipi(void *unused)
{
	struct ksched_percpu *p = this_cpu_ptr(&kp);
	unsigned long gen;

	/* if last_gen is the current gen, ksched_park() beat us here */
	gen = smp_load_acquire(&p->gen);
	if (gen == p->last_gen)
		return;

	/* if !p->running, waiter thread will handle the preemption */
	if (!p->running)
		return;

	if (local_inc_return(&p->signalled) == 1) {
		ksched_preempt(p->pids[(p->last_gen - 1) & PIDS_MASK]);
	}
}

static long ksched_wake(struct ksched_wake_req __user *req)
{
	struct ksched_wakeup wakeup;
	struct ksched_percpu *p;
	cpumask_var_t mask;
	unsigned int nr;
	int ret, i;

	/* validate inputs */
	ret = copy_from_user(&nr, &req->nr, sizeof(nr));
	if (unlikely(ret))
		return ret;
	if (unlikely(!alloc_cpumask_var(&mask, GFP_KERNEL)))
		return -ENOMEM;
	cpumask_clear(mask);

	for (i = 0; i < nr; i++) {
		ret = copy_from_user(&wakeup, &req->wakeups[i],
				     sizeof(wakeup));
		if (unlikely(ret)) {
			free_cpumask_var(mask);
			return ret;
		}
		if (unlikely(!cpu_possible(wakeup.cpu))) {
			free_cpumask_var(mask);
			return -EINVAL;
		}

		p = per_cpu_ptr(&kp, wakeup.cpu);
		p->pids[p->waker_cached_gen++ & PIDS_MASK] = wakeup.next_tid;
		smp_store_release(&p->gen, p->waker_cached_gen);

		if (wakeup.preempt) {
			cpumask_set_cpu(wakeup.cpu, mask);
		}
	}

	if (!cpumask_empty(mask)) {
		get_cpu();
		smp_call_function_many(mask, ksched_ipi, NULL, false);
		put_cpu();
	}

	free_cpumask_var(mask);
	return 0;
}

static long
ksched_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	/* validate input */
	if (unlikely(_IOC_TYPE(cmd) != KSCHED_MAGIC))
		return -ENOTTY;
	if (unlikely(_IOC_NR(cmd) > KSCHED_IOC_MAXNR))
		return -ENOTTY;

	switch (cmd) {
	case KSCHED_IOC_PARK:
		return ksched_park();
	case KSCHED_IOC_START:
		return deschedule();
	case KSCHED_IOC_WAKE:
		return ksched_wake((void __user *)arg);
	default:
		break;
	}

	return -ENOTTY;
}

static int start_waiter_threads(void)
{
	struct ksched_percpu *p;
	struct task_struct *k;

	int  cpu;
	int ret = 0;
	get_online_cpus();

	for_each_online_cpu(cpu) {
		p = per_cpu_ptr(&kp, cpu);
		memset(p, 0, sizeof(*p));

		// TODO: have IOKernel tell us where to run
		if (cpu_to_node(cpu) > 0)
			continue;
		if (cpu == 2 || cpu == 26 || cpu % 2 == 1)
			continue;

		k = kthread_create_on_node(ksched_waiter_thread, NULL, 0, "ksched/0:%d", cpu);
		if (!IS_ERR(k)) {
			p->waiter_thread = k;
			kthread_bind(k, cpu);
			wake_up_process(k);
		} else {
			ret = PTR_ERR(k);
			goto out;
		}
	}

out:
	put_online_cpus();
	return ret;
}

static int stop_waiter_threads(void)
{
	struct ksched_percpu *p;
	int cpu;
	get_online_cpus();
	for_each_online_cpu(cpu) {
		p = per_cpu_ptr(&kp, cpu);
		if (p->waiter_thread) {
			kthread_stop(p->waiter_thread);
			p->waiter_thread = NULL;
		}
	}
	put_online_cpus();
	return 0;
}

static bool iokernel_running;

static int ksched_open(struct inode *inode, struct file *filp)
{
	if (filp->f_cred->uid.val == 0 && !iokernel_running) {
		iokernel_running = true;
		start_waiter_threads();
	} else {
		// todo filp->private_data = current->pid;
	}

	return 0;
}

static int ksched_release(struct inode *inode, struct file *filp)
{
	if (filp->f_cred->uid.val == 0 && iokernel_running) {
		iokernel_running = false;
		stop_waiter_threads();
	} else {
		// TODO: restart waiter thread on core
	}
	return 0;
}

static struct file_operations ksched_ops = {
	.owner =	THIS_MODULE,
	.unlocked_ioctl = ksched_ioctl,
	.open =		ksched_open,
	.release =	ksched_release,
};

static dev_t devno;

static int __init ksched_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&devno, 0, 1, "ksched");
	if (ret) {
		printk(KERN_ERR "ksched: failed to reserve char dev region\n");
		return ret;
	}

	cdev_init(&ksched_cdev, &ksched_ops);
	ret = cdev_add(&ksched_cdev, devno, 1);
	if (ret) {
		printk(KERN_ERR "ksched: failed to add char dev\n");
		return ret;
	}

	printk(KERN_INFO "ksched: API V1 ready");

	return 0;
}

static void __exit ksched_exit(void)
{
	stop_waiter_threads();
	cdev_del(&ksched_cdev);
	unregister_chrdev_region(devno, 1);
}

module_init(ksched_init);
module_exit(ksched_exit);
