/*
 * ksched.c - an accelerated scheduler interface for the IOKernel
 */

#include <asm/io.h>
#include <asm/local.h>
#include <asm/set_memory.h>
#include <asm/msr-index.h>
#include <asm/msr.h>
#include <asm/mwait.h>
#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/tlbflush.h>
#include <linux/capability.h>
#include <linux/cdev.h>
#include <linux/cpuidle.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/smp.h>
#include <linux/uaccess.h>

#include "ksched.h"

#define KSCHED_PMC_PROBE_DELAY (3)
#define CORE_PERF_GLOBAL_CTRL_ENABLE_PMC_0 (0x1)
#define CORE_PERF_GLOBAL_CTRL_ENABLE_PMC_1 (0x2)

MODULE_LICENSE("GPL");

/* the character device that provides the ksched IOCTL interface */
static struct cdev ksched_cdev;
/* the character device that mmaps the PCI configuration space */
static struct cdev pci_cfg_cdev;

/* shared memory between the IOKernel and the Linux Kernel */
static __read_mostly struct ksched_shm_cpu *shm;
#define SHM_SIZE (NR_CPUS * sizeof(struct ksched_shm_cpu))

struct ksched_percpu {
	unsigned int	last_gen;
	pid_t		tid;
	local_t		busy;
};

/* per-cpu data to coordinate context switching and signal delivery */
static DEFINE_PER_CPU(struct ksched_percpu, kp);

/**
 * ksched_lookup_task - retreives a task from a pid number
 * @nr: the pid number
 *
 * Returns a task pointer or NULL if none was found.
 */
static struct task_struct *ksched_lookup_task(pid_t nr)
{
	struct pid *pid;

	pid = find_vpid(nr);
	if (unlikely(!pid))
		return NULL;
	return pid_task(pid, PIDTYPE_PID);
}

static int ksched_wakeup_pid(int cpu, pid_t pid)
{
	struct task_struct *p;
	int ret;

	rcu_read_lock();
	p = ksched_lookup_task(pid);
	if (unlikely(!p)) {
		rcu_read_unlock();
		return -EINVAL;
	}

	if (WARN_ON_ONCE(p->on_cpu || p->state == TASK_WAKING ||
			 p->state == TASK_RUNNING)) {
		rcu_read_unlock();
		return -EINVAL;
	}

	ret = set_cpus_allowed_ptr(p, cpumask_of(cpu));
	if (unlikely(ret)) {
		rcu_read_unlock();
		return ret;
	}

	wake_up_process(p);
	rcu_read_unlock();

	return 0;
}

static int ksched_mwait_on_addr(const unsigned int *addr, unsigned int hint,
				unsigned int val)
{
	unsigned int cur;

	lockdep_assert_irqs_disabled();

	/* first see if the condition is met without waiting */
	cur = smp_load_acquire(addr);
	if (cur != val)
		return cur;

	/* then arm the monitor address and recheck to avoid a race */
	__monitor(addr, 0, 0);
	cur = smp_load_acquire(addr);
	if (cur != val)
		return cur;

	/* finally, execute mwait, and recheck after waking up */
	__mwait(hint, MWAIT_ECX_INTERRUPT_BREAK);
	return smp_load_acquire(addr);
}

static int __cpuidle ksched_idle(struct cpuidle_device *dev,
				 struct cpuidle_driver *drv, int index)
{
	struct ksched_percpu *p;
	struct ksched_shm_cpu *s;
	unsigned long gen;
	unsigned int hint;
	pid_t tid;
	int cpu;

	lockdep_assert_irqs_disabled();

	cpu = get_cpu();
	p = this_cpu_ptr(&kp);
	s = &shm[cpu];

	/* check if we entered the idle loop with a process still active */
	if (p->tid != 0 && ksched_lookup_task(p->tid) != NULL) {
		ksched_mwait_on_addr(&s->gen, 0, s->gen);
		put_cpu();

		return index;
	}

	/* mark the core as idle if a new request isn't waiting */
	local_set(&p->busy, false);
	if (s->busy && smp_load_acquire(&s->gen) == p->last_gen)
		WRITE_ONCE(s->busy, false);

	/* use the mwait instruction to efficiently wait for the next request */
	hint = READ_ONCE(s->mwait_hint);
	gen = ksched_mwait_on_addr(&s->gen, hint, p->last_gen);
	if (gen != p->last_gen) {
		tid = READ_ONCE(s->tid);
		p->last_gen = gen;
		/* if the TID is 0, then leave the core idle */
		if (tid != 0) {
			if (unlikely(ksched_wakeup_pid(cpu, tid)))
				tid = 0;
		}
		p->tid = tid;
		WRITE_ONCE(s->busy, tid != 0);
		local_set(&p->busy, true);
		smp_store_release(&s->last_gen, gen);
	}

	put_cpu();

	return index;
}

static long ksched_park(void)
{
	struct ksched_percpu *p;
	struct ksched_shm_cpu *s;
	unsigned long gen;
	pid_t tid;
	int cpu;

	cpu = get_cpu();
	p = this_cpu_ptr(&kp);
	s = &shm[cpu];

	local_set(&p->busy, false);

	/* check if a new request is available yet */
	gen = smp_load_acquire(&s->gen);
	if (gen == p->last_gen) {
		WRITE_ONCE(s->busy, false);
		p->tid = 0;
		put_cpu();
		goto park;
	}

	/* determine the next task to run */
	tid = READ_ONCE(s->tid);
	p->last_gen = gen;

	/* are we waking the current pid? */
	if (tid == task_pid_vnr(current)) {
		WRITE_ONCE(s->busy, true);
		local_set(&p->busy, true);
		smp_store_release(&s->last_gen, gen);
		put_cpu();
		return 0;
	}

	/* if the tid is zero, then simply idle this core */
	if (tid != 0) {
		if (unlikely(ksched_wakeup_pid(cpu, tid)))
			tid = 0;
	}
	p->tid = tid;
	WRITE_ONCE(s->busy, tid != 0);
	local_set(&p->busy, true);
	smp_store_release(&s->last_gen, gen);
	put_cpu();

park:
	/* put this task to sleep and reschedule so the next task can run */
	__set_current_state(TASK_INTERRUPTIBLE);
	schedule();
	__set_current_state(TASK_RUNNING);
	return smp_processor_id();
}

static long ksched_start(void)
{
	/* put this task to sleep and reschedule so the next task can run */
	__set_current_state(TASK_INTERRUPTIBLE);
	schedule();
	__set_current_state(TASK_RUNNING);
	return 0;
}

static void ksched_deliver_signal(struct ksched_percpu *p, unsigned int signum)
{
	struct task_struct *t;

	/* if core is already idle, don't bother delivering signals */
	if (!local_read(&p->busy)) {
		put_cpu();
		return;
	}

	/* lookup the current task assigned to this core */
	rcu_read_lock();
	t = ksched_lookup_task(p->tid);
	if (!t) {
		rcu_read_unlock();
		return;
	}

	/* send the signal */
	send_sig(signum, t, 0);
	rcu_read_unlock();
}

static u64 ksched_measure_pmc(u64 sel)
{
	u64 start, end;

	wrmsrl(MSR_P6_EVNTSEL0, sel);
	rdmsrl(MSR_P6_PERFCTR0, start);
	udelay(KSCHED_PMC_PROBE_DELAY);
	rdmsrl(MSR_P6_PERFCTR0, end);
	return end - start;
}

static void ksched_enable_pmc(void) {
        wrmsrl(MSR_CORE_PERF_GLOBAL_CTRL, CORE_PERF_GLOBAL_CTRL_ENABLE_PMC_0 |
                                          CORE_PERF_GLOBAL_CTRL_ENABLE_PMC_1);
}

static void ksched_ipi(void *unused)
{
	struct ksched_percpu *p;
	struct ksched_shm_cpu *s;
	static bool pmc_ctrl_enabled[NR_CPUS];
	int cpu, tmp;

	cpu = get_cpu();
	p = this_cpu_ptr(&kp);
	s = &shm[cpu];

	/* check if a signal has been requested */
	tmp = smp_load_acquire(&s->sig);
	if (tmp == p->last_gen)
		ksched_deliver_signal(p, READ_ONCE(s->signum));

	/* check if a performance counter has been requested */
	tmp = smp_load_acquire(&s->pmc);
	if (tmp == p->last_gen) {
		if (!pmc_ctrl_enabled[cpu]) {
	        	ksched_enable_pmc();
	        	pmc_ctrl_enabled[cpu] = true;
	        }
	  
		s->pmcval = ksched_measure_pmc(READ_ONCE(s->pmcsel));
		smp_store_release(&s->pmc, 0);
	}

	put_cpu();
}

static int get_user_cpu_mask(const unsigned long __user *user_mask_ptr,
			     unsigned len, struct cpumask *new_mask)
{
	if (len < cpumask_size())
		cpumask_clear(new_mask);
	else if (len > cpumask_size())
		len = cpumask_size();

	return copy_from_user(new_mask, user_mask_ptr, len) ? -EFAULT : 0;
}

static long ksched_intr(struct ksched_intr_req __user *ureq)
{
	cpumask_var_t mask;
	struct ksched_intr_req req;

	/* only the IOKernel can send interrupts (privileged) */
	if (unlikely(!capable(CAP_SYS_ADMIN)))
		return -EACCES;

	/* validate inputs */
	if (unlikely(copy_from_user(&req, ureq, sizeof(req))))
		return -EFAULT;
	if (unlikely(!alloc_cpumask_var(&mask, GFP_KERNEL)))
		return -ENOMEM;
	if (unlikely(get_user_cpu_mask((const unsigned long __user *)req.mask,
				       req.len, mask))) {
		free_cpumask_var(mask);
		return -EFAULT;
	}

	/* send interrupts */
	smp_call_function_many(mask, ksched_ipi, NULL, false);
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
	case KSCHED_IOC_START:
		return ksched_start();
	case KSCHED_IOC_PARK:
		return ksched_park();
	case KSCHED_IOC_INTR:
		return ksched_intr((void __user *)arg);
	default:
		break;
	}

	return -ENOTTY;
}

static int ksched_mmap(struct file *file, struct vm_area_struct *vma)
{
	/* only the IOKernel can access the shared region (privileged) */
	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;
	return remap_vmalloc_range(vma, (void *)shm, vma->vm_pgoff);
}

static int ksched_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static int ksched_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static struct file_operations ksched_ops = {
	.owner		= THIS_MODULE,
	.mmap		= ksched_mmap,
	.unlocked_ioctl	= ksched_ioctl,
	.open		= ksched_open,
	.release	= ksched_release,
};

static int pci_cfg_mmap(struct file *file, struct vm_area_struct *vma) {
        if (!capable(CAP_SYS_ADMIN))
                return -EACCES;
        vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
        if (io_remap_pfn_range(vma, vma->vm_start, PCI_CFG_ADDRESS >> PAGE_SHIFT,
			    vma->vm_end - vma->vm_start, vma->vm_page_prot)) {
	        return -EAGAIN;
	}
	return 0;
}

static struct file_operations pci_cfg_ops = {
	.owner		= THIS_MODULE,
	.mmap		= pci_cfg_mmap,
};

/* TODO: This is a total hack to make ksched work as a module */
static struct cpuidle_state backup_state;
static int backup_state_count;

static int __init ksched_cpuidle_hijack(void)
{
	struct cpuidle_driver *drv;

	drv = cpuidle_get_driver();
	if (!drv)
		return -ENOENT;
	if (drv->state_count <= 0 || drv->states[0].disabled)
		return -EINVAL;

	cpuidle_pause_and_lock();
	backup_state = drv->states[0];
	backup_state_count = drv->state_count;
	drv->states[0].enter = ksched_idle;
	drv->state_count = 1;
	cpuidle_resume_and_unlock();

	return 0;
}

static void __exit ksched_cpuidle_unhijack(void)
{
	struct cpuidle_driver *drv;

	drv = cpuidle_get_driver();
	if (!drv)
		return;

	cpuidle_pause_and_lock();
	drv->states[0] = backup_state;
	drv->state_count = backup_state_count;
	cpuidle_resume_and_unlock();
}

static int __init ksched_init(void)
{
	dev_t devno_ksched = MKDEV(KSCHED_MAJOR, KSCHED_MINOR);
        dev_t devno_pci_cfg;
        int ret;

	if (!cpu_has(&boot_cpu_data, X86_FEATURE_MWAIT)) {
		printk(KERN_ERR "ksched: mwait support is required");
		return -ENOTSUPP;
	}

	ret = register_chrdev_region(devno_ksched, 1, "ksched");
	if (ret)
		return ret;

	cdev_init(&ksched_cdev, &ksched_ops);
	ret = cdev_add(&ksched_cdev, devno_ksched, 1);
	if (ret)
		goto fail_ksched_cdev_add;

	shm = vmalloc_user(SHM_SIZE);
	if (!shm) {
		ret = -ENOMEM;
		goto fail_shm;
	}
	memset(shm, 0, SHM_SIZE);

	ret = ksched_cpuidle_hijack();
	if (ret)
		goto fail_hijack;

	printk(KERN_INFO "ksched: API V2 enabled");

        devno_pci_cfg = MKDEV(PCI_CFG_MAJOR, PCI_CFG_MINOR);
	ret = register_chrdev_region(devno_pci_cfg, 1, "pcicfg");
	if (ret) {
	  goto fail_pci_cfg_reg_cdev_region;
	}
	cdev_init(&pci_cfg_cdev, &pci_cfg_ops);
	ret = cdev_add(&pci_cfg_cdev, devno_pci_cfg, 1);
	if (ret) {
	  goto fail_pci_cfg_cdev_add;
	}

	return 0;
	
fail_pci_cfg_cdev_add:
	unregister_chrdev_region(devno_pci_cfg, 1);
fail_pci_cfg_reg_cdev_region:
fail_hijack:
	vfree(shm);
fail_shm:
	cdev_del(&ksched_cdev);
fail_ksched_cdev_add:
	unregister_chrdev_region(devno_ksched, 1);
	return ret;
}

static void __exit ksched_exit(void)
{
	dev_t devno_ksched = MKDEV(KSCHED_MAJOR, KSCHED_MINOR);
        dev_t devno_pci_cfg = MKDEV(PCI_CFG_MAJOR, PCI_CFG_MINOR);

        ksched_cpuidle_unhijack();
	vfree(shm);
	cdev_del(&ksched_cdev);
	unregister_chrdev_region(devno_ksched, 1);
	cdev_del(&pci_cfg_cdev);
	unregister_chrdev_region(devno_pci_cfg, 1);
}

module_init(ksched_init);
module_exit(ksched_exit);
