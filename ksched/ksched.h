/*
 * ksched.h - the UAPI for ksched and its ioctl's
 */

#pragma once

#include <linux/types.h>
#include <linux/ioctl.h>

#include "shm.h"

#define KSCHED_MINOR		0

struct ksched_preempt_req {
	unsigned int		nr;
	int	cpus[];
};

struct ksched_init_args {
	unsigned long *bitmap;	/* pointer to bitmap of managed cpus */
	unsigned int size;			/* number of words in the bitmap */
};

struct ksched_park_args {
	uint64_t cmd;
	unsigned long payload;
};

#define KSCHED_MAGIC		0xF0
#define KSCHED_IOC_MAXNR	4

#define KSCHED_IOC_PARK		_IOW(KSCHED_MAGIC, 1, struct ksched_park_args)
#define KSCHED_IOC_START	_IO(KSCHED_MAGIC, 2)
#define KSCHED_IOC_PREEMPT		_IOW(KSCHED_MAGIC, 3, struct ksched_preempt_req)
#define KSCHED_IOC_INIT		_IOW(KSCHED_MAGIC, 4, struct ksched_init_args)