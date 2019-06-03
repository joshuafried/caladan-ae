/*
 * ioqueues.c
 */

#include <sys/socket.h>
#include <sys/un.h>

#include <base/log.h>
#include <base/lrpc.h>
#include <base/mem.h>
#include <base/random.h>
#include <base/thread.h>

#include <iokernel/shm.h>

#include "defs.h"

#define COMMAND_QUEUE_MCOUNT	128

struct iokernel_control iok;

struct io_bundle bundles[MAX_BUNDLES];
unsigned int nr_bundles;

/* coarse-grained simple allocator for iokernel shm region */
/* all allocations are on cache line boundaries */
shmptr_t iok_shm_alloc(size_t size, size_t alignment, void **out)
{
	static DEFINE_SPINLOCK(shmlock);
	static size_t allocated;
	struct shm_region *r = &iok.shared_region;
	void *p;

	spin_lock(&shmlock);
	if (!iok.shared_region.base) {
		r->base = mem_map_shm(iok.key, NULL, 24 * PGSIZE_2MB, PGSIZE_2MB, true);
		r->len = PGSIZE_2MB * 24;
		BUG_ON(r->base == MAP_FAILED);
	}

	if (!alignment)
		alignment = CACHE_LINE_SIZE;

	allocated = align_up(allocated, alignment);
	p = r->base + allocated;
	allocated = align_up(allocated + size, CACHE_LINE_SIZE);

	// TODO: it is difficult to support remapping of shared huge-pages
	BUG_ON(allocated > iok.shared_region.len);

	spin_unlock(&shmlock);

	if (out)
		*out = p;

	return ptr_to_shmptr(r, p, size);
}

static void ioqueue_alloc(struct queue_spec *q,
			  size_t msg_count, bool alloc_wb)
{
	q->msg_buf = iok_shm_alloc(sizeof(struct lrpc_msg) * msg_count, 0, NULL);
	q->msg_count = msg_count;

	if (alloc_wb)
		q->wb = iok_shm_alloc(sizeof(uint32_t), 0, NULL);
}

static void queue_pointers_alloc(struct thread_spec *tspec)
{
	/* set wb for rxcmdq */
	tspec->rxcmdq.wb = iok_shm_alloc(sizeof(struct q_ptrs), 0, NULL);
	tspec->q_ptrs = iok_shm_alloc(sizeof(struct q_ptrs), 0, NULL);
}


static int ioqueues_shm_setup(void)
{
	int i, ret;
	struct shm_region *r = &iok.shared_region;
	struct control_hdr *hdr;

	ret = fill_random_bytes(&iok.key, sizeof(iok.key));
	if (ret)
		return ret;

	/* control header must be first in shm region */
	iok_shm_alloc(sizeof(struct control_hdr), 0, (void **)&hdr);
	BUG_ON(hdr != iok.shared_region.base);

	iok_shm_alloc(sizeof(struct thread_spec) * maxks, 0, (void **)&iok.threads);
	iok_shm_alloc(sizeof(struct bundle_spec) * nr_bundles, 0, (void **)&iok.bundles);

	for (i = 0; i < maxks; i++) {
		struct thread_spec *tspec = &iok.threads[i];
		ioqueue_alloc(&tspec->rxcmdq, COMMAND_QUEUE_MCOUNT, false);
		ioqueue_alloc(&tspec->txcmdq, COMMAND_QUEUE_MCOUNT, true);

		queue_pointers_alloc(tspec);
	}

	for (i = 0; i < nr_bundles; i++) {
		struct bundle_vars *bv;
		iok_shm_alloc(sizeof(*bv), 0, (void **)&bv);
		bundles[i].b_vars = bv;

		struct timer_spec *s;
		iok.bundles[i].timer_count = 1;
		iok.bundles[i].timer_specs = iok_shm_alloc(sizeof(*s), 0, (void **)&s);
		s->timern = ptr_to_shmptr(r, &bv->timern, sizeof(bv->timern));
		s->next_deadline_tsc = ptr_to_shmptr(r, &bv->next_deadline_tsc, sizeof(bv->next_deadline_tsc));

		struct hardware_queue_spec *hs;
		iok.bundles[i].hwq_count = 1;
		/* allocate for two, in case storage is enabled */
		iok.bundles[i].hwq_specs = iok_shm_alloc(sizeof(*hs) * 2, 0, NULL);
	}

	return 0;
}

static void ioqueues_shm_cleanup(void)
{
	mem_unmap_shm(iok.shared_region.base);
}

/*
 * Register this runtime with the IOKernel. All threads must complete their
 * per-thread ioqueues initialization before this function is called.
 */
int ioqueues_register_iokernel(void)
{
	struct control_hdr *hdr;
	struct shm_region *r = &iok.shared_region;
	struct sockaddr_un addr;
	int ret;

	/* initialize control header */
	hdr = r->base;
	hdr->magic = CONTROL_HDR_MAGIC;
	hdr->thread_count = maxks;
	hdr->bundle_count = nr_bundles;

	hdr->sched_cfg.priority = SCHED_PRIORITY_NORMAL;
	hdr->sched_cfg.max_cores = maxks;
	hdr->sched_cfg.guaranteed_cores = guaranteedks;
	hdr->sched_cfg.congestion_latency_us = 0;
	hdr->sched_cfg.scaleout_latency_us = 0;

	hdr->bundle_specs = ptr_to_shmptr(r, iok.bundles, sizeof(struct bundle_spec) * nr_bundles);
	hdr->thread_specs = ptr_to_shmptr(r, iok.threads, sizeof(struct thread_spec) * maxks);

	/* register with iokernel */
	BUILD_ASSERT(strlen(CONTROL_SOCK_PATH) <= sizeof(addr.sun_path) - 1);
	memset(&addr, 0x0, sizeof(struct sockaddr_un));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, CONTROL_SOCK_PATH, sizeof(addr.sun_path) - 1);

	iok.fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (iok.fd == -1) {
		log_err("register_iokernel: socket() failed [%s]", strerror(errno));
		goto fail;
	}

	if (connect(iok.fd, (struct sockaddr *)&addr,
		 sizeof(struct sockaddr_un)) == -1) {
		log_err("register_iokernel: connect() failed [%s]", strerror(errno));
		goto fail_close_fd;
	}

	ret = write(iok.fd, &iok.key, sizeof(iok.key));
	if (ret != sizeof(iok.key)) {
		log_err("register_iokernel: write() failed [%s]", strerror(errno));
		goto fail_close_fd;
	}

	ret = write(iok.fd, &r->len, sizeof(r->len));
	if (ret != sizeof(r->len)) {
		log_err("register_iokernel: write() failed [%s]", strerror(errno));
		goto fail_close_fd;
	}

	return 0;

fail_close_fd:
	close(iok.fd);
fail:
	ioqueues_shm_cleanup();
	return -errno;
}

int ioqueues_init_thread(void)
{
	int ret;
	pid_t tid = gettid();
	struct shm_region *r = &iok.shared_region;

	assert(myk()->kthread_idx < maxks);
	struct thread_spec *ts = &iok.threads[myk()->kthread_idx];
	ts->tid = tid;

	ret = shm_init_lrpc_in(r, &ts->rxcmdq, &myk()->rxcmdq);
	if (ret)
		return ret;

	ret = shm_init_lrpc_out(r, &ts->txcmdq, &myk()->txcmdq);
	if (ret)
		return ret;

	myk()->q_ptrs = (struct q_ptrs *) shmptr_to_ptr(r, ts->q_ptrs,
			sizeof(uint32_t));
	BUG_ON(!myk()->q_ptrs);

	return 0;
}

/*
 * General initialization for runtime <-> iokernel communication. Must be
 * called before per-thread ioqueues initialization.
 */
int ioqueues_init(void)
{
	int i, ret;

	nr_bundles = NR_BUNDLES(maxks, guaranteedks);

	for (i = 0; i < nr_bundles; i++)
		spin_lock_init(&bundles[i].lock);

	ret = ioqueues_shm_setup();
	if (ret) {
		log_err("ioqueues_init: ioqueues_shm_setup() failed, ret = %d", ret);
		return ret;
	}

	return 0;
}
