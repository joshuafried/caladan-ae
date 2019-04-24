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
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/sched.h>
#include <linux/uaccess.h>

#include <asm-generic/local.h>

#include "klrpc.h"
#include "ksched.h"
#include "mwait.h"

MODULE_LICENSE("GPL");

/* the character device that provides the ksched IOCTL interface */
static struct cdev ksched_cdev;
static dev_t devno;

#define IOKERNEL_MAGIC ((void *)~0UL)
static bool iokernel_running;
static cpumask_var_t managed_cores;

static struct ksched_shm_percpu *shm;

struct ksched_percpu {

	unsigned long	last_gen;

	local_t signalled;
	pid_t running_pid;

	struct task_struct *waiter_thread;

	struct lrpc_chan_out cmdq_out;
	struct lrpc_chan_in cmdq_in;

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
 * ksched_preempt_pid - signal a task to park
 * @pid: the pid number
 *
 */
static void ksched_preempt_pid(pid_t pid)
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
 * WARNING: must be called with interrupts disabled OR p->running_pid == 0
 * to prevent running with the IPI handler
 *
 * Returns the awoken pid or 0 if none could be found
 */
static pid_t run_next(void)
{
	struct ksched_percpu *p;
	uint64_t cmd;
	unsigned long payload;
	pid_t pid;

	int cpu = smp_processor_id();

	p = this_cpu_ptr(&kp);

	if (!lrpc_recv(&p->cmdq_in, &cmd, &payload))
		return 0;

	WARN_ON(cmd != KSCHED_RUN_NEXT);

	p->last_gen++;
	pid = (pid_t)payload;

	if (!pid)
		return 0;

	if (pid != current->pid)
		if (ksched_wakeup_pid(cpu, pid) < 0)
			return 0;

	local_set(&p->signalled, 0);
	smp_store_release(&p->running_pid, pid);

	/* if a preempt is pending, make sure that pid reparks */
	/* Now that p->running = 1, we may race with ipi handler */
	if (!lrpc_empty(&p->cmdq_in) && local_inc_return(&p->signalled) == 1)
		ksched_preempt_pid(p->running_pid);

	return p->running_pid;

}

static int ksched_waiter_thread(void *arg)
{
	struct ksched_percpu *p = this_cpu_ptr(&kp);
	int cpu = smp_processor_id();

	while (!kthread_should_stop()) {

		if (run_next())
			deschedule();

		smp_store_release(&p->running_pid, 0);

		mwait(&shm[cpu].gen, p->last_gen);

		if (!lrpc_empty(&p->cmdq_in))
			continue;

		/* run another task if needed */
		if (need_resched())
			schedule();
	}

	return 0;
}

static long ksched_park(struct ksched_park_args __user *uargs)
{
	struct ksched_park_args args;
	struct ksched_percpu *p;
	struct task_struct *ts;
	pid_t pid;
	int cpu, ret;

	cpu = get_cpu();
	local_irq_disable();

	if (unlikely(signal_pending(current))) {
		local_irq_enable();
		put_cpu();
		return -ERESTARTSYS;
	}

	p = this_cpu_ptr(&kp);

	/* This core has moved on */
	if (p->running_pid != current->pid) {
		local_irq_enable();
		put_cpu();
		return deschedule();
	}

	ret = copy_from_user(&args, uargs, sizeof(args));
	if (ret) {
		send_sig(SIGKILL, current, 0);
		goto out;
	}

	while (!lrpc_send_mwait(&p->cmdq_out, args.cmd, args.payload))
		cpu_relax();

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

out:
	smp_store_release(&p->running_pid, 0);
	rcu_read_lock();
	ts = rcu_dereference(p->waiter_thread);
	if (ts)
		get_task_struct(ts);
	rcu_read_unlock();

	if (ts) {
		wake_up_process(ts);
		put_task_struct(ts);
	}

	smp_wmb();
	local_irq_enable();
	put_cpu();
	return deschedule();
}

static void ksched_ipi(void *unused)
{
	struct ksched_percpu *p = this_cpu_ptr(&kp);

	/* if there are no pending messages, ksched_park() beat us here */
	if (lrpc_empty(&p->cmdq_in))
		return;

	/* if !p->running_rid, waiter thread will handle the preemption */
	if (!p->running_pid)
		return;

	if (local_inc_return(&p->signalled) == 1) {
		ksched_preempt_pid(p->running_pid);
	}
}

static long ksched_preempt(struct file *filp, struct ksched_preempt_req __user *req)
{
	cpumask_var_t mask;
	unsigned int nr;
	int ret, i, cpu;

	if (filp->private_data != IOKERNEL_MAGIC)
		return -EPERM;

	/* validate inputs */
	ret = copy_from_user(&nr, &req->nr, sizeof(nr));
	if (unlikely(ret))
		return ret;
	if (unlikely(!alloc_cpumask_var(&mask, GFP_KERNEL)))
		return -ENOMEM;
	cpumask_clear(mask);

	for (i = 0; i < nr; i++) {
		ret = copy_from_user(&cpu, &req->cpus[i],
				     sizeof(cpu));
		if (unlikely(ret)) {
			free_cpumask_var(mask);
			return ret;
		}
		if (unlikely(!cpu_possible(cpu))) {
			free_cpumask_var(mask);
			return -EINVAL;
		}
		cpumask_set_cpu(cpu, mask);
	}

	if (!cpumask_empty(mask)) {
		get_cpu();
		smp_call_function_many(mask, ksched_ipi, NULL, false);
		put_cpu();
	}

	free_cpumask_var(mask);
	return 0;
}

static void shm_close(struct vm_area_struct *vma) {
	unsigned long offset;
	unsigned char *s = (unsigned char *)shm;

	for (offset = 0; offset < KSCHED_SHM_SIZE; offset += PAGE_SIZE)
		put_page(pfn_to_page(vmalloc_to_pfn(s + offset)));
}

static struct vm_operations_struct shm_ops = {
    .close = shm_close,
};

static int
ksched_mmap_shm(struct file *filp, struct vm_area_struct *vma)
{
	int ret;
	unsigned long offset;
	unsigned char *s = (unsigned char *)shm;

	if (filp->private_data != IOKERNEL_MAGIC)
		return -EPERM;

	if (vma->vm_end - vma->vm_start != KSCHED_SHM_SIZE)
		return -EINVAL;

	WARN_ON(KSCHED_SHM_SIZE % PAGE_SIZE != 0);
	for (offset = 0; offset < KSCHED_SHM_SIZE; offset += PAGE_SIZE) {
		get_page(pfn_to_page(vmalloc_to_pfn(s + offset)));
		ret = remap_pfn_range(vma, vma->vm_start + offset,
		  vmalloc_to_pfn(s + offset), PAGE_SIZE, vma->vm_page_prot);
		if (ret)
			return ret;
	}

	vma->vm_ops = &shm_ops;

	return 0;
}

static int ksched_init_core(int cpu)
{
	int ret;

	struct ksched_percpu *p;
	struct task_struct *k;

	p = per_cpu_ptr(&kp, cpu);
	memset(p, 0, sizeof(*p));
	cpumask_set_cpu(cpu, managed_cores);
	k = kthread_create_on_node(ksched_waiter_thread, NULL, cpu_to_node(cpu),
		  "ksched/%d:%d", cpu_to_node(cpu), cpu);
	if (IS_ERR(k))
		return PTR_ERR(k);
	get_task_struct(k);
	p->waiter_thread = k;
	kthread_bind(k, cpu);

	ret = lrpc_init_out(&p->cmdq_out,
			    (struct lrpc_msg *)&shm[cpu].core_to_iok.tbl,
			    KSCHED_PERCORE_TBL_SIZE, &shm[cpu].core_to_iok.wb);
	if (ret)
		return ret;

	ret = lrpc_init_in(&p->cmdq_in,
			   (struct lrpc_msg *)&shm[cpu].iok_to_core.tbl,
			   KSCHED_PERCORE_TBL_SIZE, &shm[cpu].iok_to_core.wb);
	if (ret)
		return ret;

	wake_up_process(k);

	return 0;

}

static long
ksched_iok_init(struct file *filp, struct ksched_init_args __user *uargs)
{
	int cpu, i, j, ret;
	unsigned long mask;
	struct ksched_init_args args;

	if (filp->f_cred->uid.val != 0)
		return -EPERM;

	if (iokernel_running)
		return -EBUSY;

	if (unlikely(!alloc_cpumask_var(&managed_cores, GFP_KERNEL)))
		return -ENOMEM;
	cpumask_clear(managed_cores);

	memset(shm, 0, KSCHED_SHM_SIZE);

	iokernel_running = true;
	filp->private_data = IOKERNEL_MAGIC;

	ret = copy_from_user(&args, uargs, sizeof(args));
	if (ret)
		return ret;

	cpu = 0;
	for (i = 0; i < args.size; i++) {
		ret = copy_from_user(&mask, args.bitmap + i, sizeof(mask));
		if (ret)
			return ret;

		for (j = 0; j < BITS_PER_LONG; j++, cpu++, mask >>= 1) {
			if (!(mask & 1))
				continue;


			if (unlikely(!cpu_possible(cpu)))
				return -EINVAL;

			ret = ksched_init_core(cpu);
			if (ret)
				return ret;
		}
	}

	return 0;
}

struct task_struct *cleanup_ks[KSCHED_NCPU];
static void ksched_cleanup(void)
{
	struct ksched_percpu *p;
	int cpu;

	if (!iokernel_running)
		return;

	for_each_cpu(cpu, managed_cores) {
		p = per_cpu_ptr(&kp, cpu);
		cleanup_ks[cpu] = p->waiter_thread;
		if (!cleanup_ks[cpu])
			continue;
		rcu_assign_pointer(p->waiter_thread, NULL);
		kthread_stop(cleanup_ks[cpu]);
	}

	synchronize_rcu();

	for_each_cpu(cpu, managed_cores) {
		if (cleanup_ks[cpu])
			put_task_struct(cleanup_ks[cpu]);
	}

	free_cpumask_var(managed_cores);
	iokernel_running = false;
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
		return ksched_park((void __user *)arg);
	case KSCHED_IOC_START:
		return deschedule();
	case KSCHED_IOC_PREEMPT:
		return ksched_preempt(filp, (void __user *)arg);
	case KSCHED_IOC_INIT:
		return ksched_iok_init(filp, (void __user *)arg);
	default:
		break;
	}

	return -ENOTTY;
}

static int ksched_open(struct inode *inode, struct file *filp)
{
	/* associate this file with this thread */
	filp->private_data = (void *)(uintptr_t)current->pid;

	/* ensure no conflict between a possible pid and IOKERNEL_MAGIC */
	BUILD_BUG_ON(sizeof(current->pid) >= sizeof(filp->private_data));
	BUILD_BUG_ON(sizeof(IOKERNEL_MAGIC) != sizeof(filp->private_data));

	return 0;
}

/* FIXME: this is a rather crude way of cleaning up. */
static void ksched_core_release(pid_t pid)
{
	int cpu;
	struct ksched_percpu *p;
	struct task_struct *ts;

	for_each_cpu(cpu, managed_cores) {
		p = per_cpu_ptr(&kp, cpu);
		/* rescue a core that was running this pid */
		if (smp_load_acquire(&p->running_pid) == pid) {
				rcu_read_lock();
				ts = rcu_dereference(p->waiter_thread);
				if (ts)
					get_task_struct(ts);
				rcu_read_unlock();

				if (ts) {
					wake_up_process(ts);
					put_task_struct(ts);
				}
		}
	}
}

static int ksched_release(struct inode *inode, struct file *filp)
{
	if (filp->private_data == IOKERNEL_MAGIC)
		ksched_cleanup();
	else
		ksched_core_release((pid_t)(uintptr_t)filp->private_data);
	return 0;
}

static struct file_operations ksched_ops = {
	.owner =	THIS_MODULE,
	.unlocked_ioctl = ksched_ioctl,
	.open =		ksched_open,
	.release =	ksched_release,
	.mmap =		ksched_mmap_shm,
};

static int __init ksched_init(void)
{
	int ret;

	if (!cpu_has(&boot_cpu_data, X86_FEATURE_MWAIT)) {
		printk(KERN_ERR "ksched: mwait support is required");
		return -ENOTSUPP;
	}

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

	shm = vzalloc_node(KSCHED_SHM_SIZE, 0);
	if (!shm)
		return -ENOMEM;

	printk(KERN_INFO "ksched: API V1 ready");

	return 0;
}

static void __exit ksched_exit(void)
{
	ksched_cleanup();
	vfree(shm);
	cdev_del(&ksched_cdev);
	unregister_chrdev_region(devno, 1);
}

module_init(ksched_init);
module_exit(ksched_exit);
