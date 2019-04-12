/*
 * shm.h - defines layout of shared memory between ksched and iokernel
 */


#pragma once

#include <linux/kernel.h>

#ifndef __KERNEL__
#include <x86_64-linux-gnu/sys/user.h>
#endif

#define KSCHED_NCPU 256
#define KSCHED_PERCORE_TBL_SIZE 4
#define KSCHED_SHM_SIZE \
	(__ALIGN_KERNEL(sizeof(struct ksched_shm_percpu) * KSCHED_NCPU, PAGE_SIZE))


struct ksched_lrpc_mem {
	uint32_t wb;
	uint32_t pad[15];
	struct lrpc_msg tbl[KSCHED_PERCORE_TBL_SIZE];
};

struct ksched_shm_percpu {
		struct ksched_lrpc_mem core_to_iok;
		struct ksched_lrpc_mem iok_to_core;

		unsigned long pad1[8];
		unsigned long gen;
		unsigned long pad2[15];
};

enum {
	KSCHED_RUN_NEXT = 0,
};
