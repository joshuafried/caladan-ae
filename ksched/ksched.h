/*
 * ksched.h - the UAPI for ksched and its ioctl's
 */

#pragma once

#include <linux/types.h>
#include <linux/ioctl.h>


#define KSCHED_MINOR		0

struct ksched_wakeup {
	bool		preempt;
	unsigned int	cpu;
	pid_t		next_tid;
};

struct ksched_wake_req {
	unsigned int		nr;
	struct ksched_wakeup	wakeups[];
};


#define KSCHED_MAGIC		0xF0
#define KSCHED_IOC_MAXNR	3

#define KSCHED_IOC_PARK		_IO(KSCHED_MAGIC, 1)
#define KSCHED_IOC_START	_IO(KSCHED_MAGIC, 2)
#define KSCHED_IOC_WAKE		_IOW(KSCHED_MAGIC, 3, struct ksched_wake_req)
