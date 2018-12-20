#pragma once


struct io_bundle;

#if __has_include("spdk/nvme.h")

#define SPDK_STDINC_H
#include <spdk/nvme_spec.h>

struct storage_queue {
	struct spdk_nvme_qpair		*qp_handle;
	struct spdk_nvme_cpl		*cpl;
	uint32_t		*cq_head;
	uint32_t		queue_depth;
};

static inline bool storage_queue_has_work(struct storage_queue *q)
{
	struct spdk_nvme_cpl *cpl = q->cpl;
	uint32_t cq_head = ACCESS_ONCE(*q->cq_head);
	int parity = !!(cq_head & q->queue_depth);
	int idx = cq_head & (q->queue_depth - 1);
	uint16_t *status = (uint16_t *)&cpl[idx].status;
	return (ACCESS_ONCE(*status) & 0x1) == parity;
}

extern int storage_proc_completions(struct io_bundle *b,
	unsigned int budget, struct thread **wakeable_threads);

#else

struct storage_queue {};
static inline bool storage_queue_has_work(struct storage_queue *q)
{
	return false;
}

static inline int storage_proc_completions(struct io_bundle *b,
	unsigned int budget, struct thread **wakeable_threads)
{
	return 0;
}

#endif
