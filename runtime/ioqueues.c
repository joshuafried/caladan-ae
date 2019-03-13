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

/* the egress buffer pool must be large enough to fill all the TXQs entirely */

struct iokernel_control iok;

struct io_bundle bundles[MAX_BUNDLES];
unsigned int nr_bundles;

// Could be a macro really, this is totally static :/
static size_t calculate_shm_space(void)
{
	size_t ret = 0, q;

	// Header + queue_spec information
	ret += sizeof(struct control_hdr);
	ret += sizeof(struct thread_spec) * maxks;
	ret += sizeof(struct bundle_spec) * nr_bundles;
	ret = align_up(ret, CACHE_LINE_SIZE);

	// RX command queues (wb is not included)
	q = sizeof(struct lrpc_msg) * COMMAND_QUEUE_MCOUNT;
	q = align_up(q, CACHE_LINE_SIZE);
	ret += q * maxks;

	// TX command queues
	q = sizeof(struct lrpc_msg) * COMMAND_QUEUE_MCOUNT;
	q = align_up(q, CACHE_LINE_SIZE);
	q += align_up(sizeof(uint32_t), CACHE_LINE_SIZE);
	ret += q * maxks;

	// Shared queue pointers for the iokernel to use to determine busyness
	q = align_up(sizeof(struct q_ptrs), CACHE_LINE_SIZE);
	ret += q * maxks;

	q = align_up(sizeof(struct bundle_vars), CACHE_LINE_SIZE);
	ret += q * nr_bundles;

	ret = align_up(ret, PGSIZE_2MB);

	ret += verbs_shm_space_needed(0, 0);

	return ret;
}

static void ioqueue_alloc(struct shm_region *r, struct queue_spec *q,
			  char **ptr, size_t msg_count, bool alloc_wb)
{
	q->msg_buf = ptr_to_shmptr(r, *ptr, sizeof(struct lrpc_msg) * msg_count);
	*ptr += align_up(sizeof(struct lrpc_msg) * msg_count, CACHE_LINE_SIZE);

	if (alloc_wb) {
		q->wb = ptr_to_shmptr(r, *ptr, sizeof(uint32_t));
		*ptr += align_up(sizeof(uint32_t), CACHE_LINE_SIZE);
	}

	q->msg_count = msg_count;
}

static void queue_pointers_alloc(struct shm_region *r,
		struct thread_spec *tspec, char **ptr)
{
	/* set wb for rxcmdq */
	tspec->rxcmdq.wb = ptr_to_shmptr(r, *ptr, sizeof(struct q_ptrs));

	tspec->q_ptrs = ptr_to_shmptr(r, *ptr, sizeof(struct q_ptrs));
	*((uint32_t *) *ptr) = 0;
	*ptr += align_up(sizeof(struct q_ptrs), CACHE_LINE_SIZE);
}


static int ioqueues_shm_setup(void)
{
	struct shm_region *r = &iok.shared_region;
	char *ptr;
	int i, ret;
	size_t shm_len;

	ret = fill_random_bytes(&iok.key, sizeof(iok.key));
	if (ret)
		return ret;

	/* map shared memory for control header, command queues, and egress pkts */
	shm_len = calculate_shm_space();
	r->len = shm_len;
	r->base = mem_map_shm(iok.key, NULL, shm_len, PGSIZE_2MB, true);
	if (r->base == MAP_FAILED) {
		log_err("control_setup: mem_map_shm() failed");
		return -1;
	}

	/* set up queues in shared memory */
	ptr = r->base;
	ptr += sizeof(struct control_hdr);
	iok.threads = (struct thread_spec *)ptr;
	ptr += sizeof(struct thread_spec) * maxks;
	iok.bundles = (struct bundle_spec *)ptr;
	ptr += sizeof(struct bundle_spec) * nr_bundles;
	ptr = (char *)align_up((uintptr_t)ptr, CACHE_LINE_SIZE);

	for (i = 0; i < maxks; i++) {
		struct thread_spec *tspec = &iok.threads[i];
		ioqueue_alloc(r, &tspec->rxcmdq, &ptr, COMMAND_QUEUE_MCOUNT, false);
		ioqueue_alloc(r, &tspec->txcmdq, &ptr, COMMAND_QUEUE_MCOUNT, true);

		queue_pointers_alloc(r, tspec, &ptr);
	}

	for (i = 0; i < nr_bundles; i++) {
		struct bundle_spec *bspec = &iok.bundles[i];
		bspec->b_vars = ptr_to_shmptr(r, ptr, sizeof(struct bundle_vars));
		bundles[i].b_vars = (struct bundle_vars *)ptr;
		ptr += align_up(sizeof(struct bundle_vars), CACHE_LINE_SIZE);
	}

	ptr = (char *)align_up((uintptr_t)ptr, PGSIZE_2MB);
	iok.verbs_mem_len = verbs_shm_space_needed(0, 0); // FIXME
	ptr_to_shmptr(r, ptr, iok.verbs_mem_len);
	iok.verbs_mem = ptr;

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
	BUG_ON(ret);

	ret = shm_init_lrpc_out(r, &ts->txcmdq, &myk()->txcmdq);
	BUG_ON(ret);

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
