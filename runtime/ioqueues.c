/*
 * ioqueues.c
 */

#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <base/hash.h>
#include <base/log.h>
#include <base/lrpc.h>
#include <base/mem.h>
#include <base/thread.h>

#include <iokernel/shm.h>

#include <net/ethernet.h>
#include <net/mbuf.h>

#include "defs.h"

#define PACKET_QUEUE_MCOUNT	4096
#define COMMAND_QUEUE_MCOUNT	4096
/* the egress buffer pool must be large enough to fill all the TXQs entirely */

struct iokernel_control iok;

static int generate_random_mac(struct eth_addr *mac)
{
	int fd, ret;
	fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0)
		return -1;

	ret = read(fd, mac, sizeof(*mac));
	close(fd);
	if (ret != sizeof(*mac))
		return -1;

	mac->addr[0] &= ~ETH_ADDR_GROUP;
	mac->addr[0] |= ETH_ADDR_LOCAL_ADMIN;

	return 0;
}

// Could be a macro really, this is totally static :/
static size_t calculate_shm_space(unsigned int thread_count)
{
	size_t ret = 0, q;

	// Header + queue_spec information
	ret += sizeof(struct control_hdr);
	ret += sizeof(struct thread_spec) * thread_count;
	ret += sizeof(struct mlxq_spec) * NCPU;
	ret = align_up(ret, CACHE_LINE_SIZE);

	// RX queues (wb is not included)
	q = sizeof(struct lrpc_msg) * PACKET_QUEUE_MCOUNT;
	q = align_up(q, CACHE_LINE_SIZE);
	ret += q * thread_count;

	// TX packet queues
	q = sizeof(struct lrpc_msg) * PACKET_QUEUE_MCOUNT;
	q = align_up(q, CACHE_LINE_SIZE);
	q += align_up(sizeof(uint32_t), CACHE_LINE_SIZE);
	ret += q * thread_count;

	// TX command queues
	q = sizeof(struct lrpc_msg) * COMMAND_QUEUE_MCOUNT;
	q = align_up(q, CACHE_LINE_SIZE);
	q += align_up(sizeof(uint32_t), CACHE_LINE_SIZE);
	ret += q * thread_count;

	// Shared queue pointers for the iokernel to use to determine busyness
	q = align_up(sizeof(struct q_ptrs), CACHE_LINE_SIZE);
	ret += q * thread_count;

	ret = align_up(ret, PGSIZE_2MB);

	// Egress buffers
	BUILD_ASSERT(ETH_MAX_LEN + sizeof(struct tx_net_hdr) <=
			MBUF_DEFAULT_LEN);
	BUILD_ASSERT(PGSIZE_2MB % MBUF_DEFAULT_LEN == 0);
	ret += EGRESS_POOL_SIZE(thread_count);
	ret = align_up(ret, PGSIZE_2MB);

	// SHM for verbs queues
	// TODO: fixme
	ret += verbs_shm_space_needed(0, 0);
	ret = align_up(ret, PGSIZE_2MB);

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
	/* set wb for rxq */
	tspec->rxq.wb = ptr_to_shmptr(r, *ptr, sizeof(struct q_ptrs));

	tspec->q_ptrs = ptr_to_shmptr(r, *ptr, sizeof(struct q_ptrs));
	*((uint32_t *) *ptr) = 0;
	*ptr += align_up(sizeof(struct q_ptrs), CACHE_LINE_SIZE);
}

static int ioqueues_shm_setup(unsigned int threads)
{
	struct shm_region *r = &netcfg.tx_region, *ingress_region = &netcfg.rx_region;
	char *ptr;
	int i, ret;
	size_t shm_len;

	if (!netcfg.mac.addr[0]) {
		ret = generate_random_mac(&netcfg.mac);
		if (ret < 0)
			return ret;
	}

	BUILD_ASSERT(sizeof(netcfg.mac) >= sizeof(mem_key_t));
	iok.key = *(mem_key_t*)(&netcfg.mac);
	iok.key = rand_crc32c(iok.key);

	/* map shared memory for control header, command queues, and egress pkts */
	shm_len = calculate_shm_space(threads);
	r->len = shm_len;
	r->base = mem_map_shm(iok.key, NULL, shm_len, PGSIZE_2MB, true);
	if (r->base == MAP_FAILED) {
		log_err("control_setup: mem_map_shm() failed");
		return -1;
	}

	/* map ingress memory */
	ingress_region->base =
	    mem_map_shm(INGRESS_MBUF_SHM_KEY, NULL, INGRESS_MBUF_SHM_SIZE,
			PGSIZE_2MB, false);
	if (ingress_region->base == MAP_FAILED) {
		log_err("control_setup: failed to map ingress region");
		mem_unmap_shm(r->base);
		return -1;
	}
	ingress_region->len = INGRESS_MBUF_SHM_SIZE;

	/* set up queues in shared memory */
	iok.thread_count = threads;
	ptr = r->base;
	ptr += sizeof(struct control_hdr) + sizeof(struct thread_spec) * threads + sizeof(struct mlxq_spec) * NCPU;
	ptr = (char *)align_up((uintptr_t)ptr, CACHE_LINE_SIZE);

	for (i = 0; i < threads; i++) {
		struct thread_spec *tspec = &iok.threads[i];
		ioqueue_alloc(r, &tspec->rxq, &ptr, PACKET_QUEUE_MCOUNT, false);
		ioqueue_alloc(r, &tspec->txpktq, &ptr, PACKET_QUEUE_MCOUNT, true);
		ioqueue_alloc(r, &tspec->txcmdq, &ptr, COMMAND_QUEUE_MCOUNT, true);

		queue_pointers_alloc(r, tspec, &ptr);
	}

	iok.tx_len = EGRESS_POOL_SIZE(threads);
	ptr = (char *)align_up((uintptr_t)ptr, PGSIZE_2MB);
	ptr_to_shmptr(r, ptr, iok.tx_len);
	iok.tx_buf = ptr;
	ptr += align_up(iok.tx_len, PGSIZE_2MB);

	iok.verbs_mem_len = verbs_shm_space_needed(0, 0); // FIXME
	ptr_to_shmptr(r, ptr, iok.verbs_mem_len);
	iok.verbs_mem = ptr;
	ptr += align_up(iok.verbs_mem_len, PGSIZE_2MB);

	iok.next_free = ptr_to_shmptr(r, ptr, 0);

	return 0;
}

static void ioqueues_shm_cleanup(void)
{
	mem_unmap_shm(netcfg.tx_region.base);
	mem_unmap_shm(netcfg.rx_region.base);
}

/*
 * Register this runtime with the IOKernel. All threads must complete their
 * per-thread ioqueues initialization before this function is called.
 */
int ioqueues_register_iokernel(void)
{
	struct control_hdr *hdr;
	struct shm_region *r = &netcfg.tx_region;
	struct sockaddr_un addr;
	int ret;

	/* initialize control header */
	hdr = r->base;
	hdr->magic = CONTROL_HDR_MAGIC;
	hdr->egress_buf_count = EGRESS_POOL_SIZE(iok.thread_count) / MBUF_DEFAULT_LEN;
	hdr->thread_count = iok.thread_count;
	hdr->mac = netcfg.mac;

	hdr->sched_cfg.priority = SCHED_PRIORITY_NORMAL;
	hdr->sched_cfg.max_cores = iok.thread_count;
	hdr->sched_cfg.guaranteed_cores = guaranteedks;
	hdr->sched_cfg.congestion_latency_us = 0;
	hdr->sched_cfg.scaleout_latency_us = 0;

	struct thread_spec *threads = (struct thread_spec *)((unsigned char *)r->base + sizeof(*hdr));
	hdr->thread_specs = ptr_to_shmptr(&netcfg.tx_region, threads, sizeof(*threads) * iok.thread_count);
	memcpy(threads, iok.threads, sizeof(*threads) * iok.thread_count);

	struct mlxq_spec *qs = (struct mlxq_spec *)((unsigned char *)threads + sizeof(*threads) * iok.thread_count);
	hdr->mlxq_specs = ptr_to_shmptr(&netcfg.tx_region, qs, sizeof(*qs) * iok.mlxq_count);
	memcpy(qs, iok.qspec, sizeof(*qs) * iok.mlxq_count);
	hdr->mlxq_count = iok.mlxq_count;

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

	ret = write(iok.fd, &netcfg.tx_region.len, sizeof(netcfg.tx_region.len));
	if (ret != sizeof(netcfg.tx_region.len)) {
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
	struct shm_region *r = &netcfg.tx_region;

	assert(myk()->kthread_idx < iok.thread_count);
	struct thread_spec *ts = &iok.threads[myk()->kthread_idx];
	ts->tid = tid;

	ret = shm_init_lrpc_in(r, &ts->rxq, &myk()->rxq);
	BUG_ON(ret);

	ret = shm_init_lrpc_out(r, &ts->txpktq, &myk()->txpktq);
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
int ioqueues_init(unsigned int threads)
{
	int ret;

	ret = ioqueues_shm_setup(threads);
	if (ret) {
		log_err("ioqueues_init: ioqueues_shm_setup() failed, ret = %d", ret);
		return ret;
	}

	return 0;
}
