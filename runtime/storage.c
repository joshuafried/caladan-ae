/*
 * storage.c
 */
#if __has_include("spdk/nvme.h")
#include <stdio.h>
#include <base/hash.h>
#include <base/log.h>
#include <base/mempool.h>
#include <runtime/storage.h>
#include <runtime/sync.h>

#define SPDK_STDINC_H
#include <spdk/nvme.h>
#include <spdk/env.h>

#include "defs.h"

static struct spdk_nvme_ctrlr *controller;
static struct spdk_nvme_ns *namespace;
static unsigned int block_size;
static unsigned int num_blocks;

static __thread struct thread **cb_ths;
static __thread unsigned int nrcb_ths;

/* 4KB storage request buffers */
#define REQUEST_BUF_POOL_SZ (PGSIZE_2MB * 20)
#define REQUEST_BUF_SZ (4 * KB)
struct mempool storage_buf_mp;
static struct tcache *storage_buf_tcache;
static DEFINE_PERTHREAD(struct tcache_perthread, storage_buf_pt);

/**
 * seq_complete - callback run after spdk nvme operation is complete
 *
 */
static void
seq_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct thread *th = arg;
	cb_ths[nrcb_ths++] = th;
}

/**
 * probe_cb - callback run after nvme devices have been probed
 *
 */
static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	struct spdk_nvme_ctrlr_opts *opts)
{
	opts->io_queue_size = UINT16_MAX;
	return true;
}

/**
 * attach_cb - callback run after nvme device has been attached
 *
 */
static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	int num_ns;

	num_ns = spdk_nvme_ctrlr_get_num_ns(ctrlr);
	if (num_ns > 1) {
		perror("more than 1 storage devices");
		exit(1);
	}
	if (num_ns == 0) {
		perror("no storage device");
		exit(1);
	}
	controller = ctrlr;
	namespace = spdk_nvme_ctrlr_get_ns(ctrlr, 1);
	block_size = (unsigned int)spdk_nvme_ns_get_sector_size(namespace);
	num_blocks = (unsigned int)spdk_nvme_ns_get_num_sectors(namespace);
}

static void *spdk_custom_allocator(size_t size, size_t align, uint64_t *physaddr_out) {
	void *out;
	iok_shm_alloc(size, align, &out);

	if (out && physaddr_out)
		mem_lookup_page_phys_addr(out, PGSIZE_2MB, physaddr_out);

	return out;
}


static int storage_init_bundle(int bundle_idx);
/**
 * storage_init - initializes storage
 *
 */
int storage_init(void)
{
	int i, rc;
	struct spdk_env_opts opts;
	void *buf;

	spdk_env_opts_init(&opts);
	opts.name = "shenango runtime";

	spdk_nvme_allocator_hook = spdk_custom_allocator;

	if (spdk_env_init(&opts) < 0) {
		log_err("Unable to initialize SPDK env");
		return 1;
	}

	rc = spdk_nvme_probe(NULL, NULL, probe_cb, attach_cb, NULL);
	if (rc != 0) {
		log_err("spdk_nvme_probe() failed");
		return 1;
	}

	if (controller == NULL) {
		log_err("no NVMe controllers found");
		return 1;
	}

	for (i = 0; i < nr_bundles; i++) {
		rc = storage_init_bundle(i);
		if (rc)
			return rc;
	}

	buf = mem_map_anom(NULL, REQUEST_BUF_POOL_SZ, PGSIZE_2MB, 0);
	if (buf == MAP_FAILED)
		return -ENOMEM;

	rc = spdk_mem_register(buf, REQUEST_BUF_POOL_SZ);
	if (rc)
		return rc;

	rc = mempool_create(&storage_buf_mp, buf, REQUEST_BUF_POOL_SZ,
		  PGSIZE_2MB, REQUEST_BUF_SZ);
	if (rc)
		return rc;

	storage_buf_tcache = mempool_create_tcache(&storage_buf_mp,
		  "storagebufs", TCACHE_DEFAULT_MAG_SIZE);
	if (!storage_buf_tcache)
		return -ENOMEM;

	return 0;
}

int storage_init_thread(void)
{
	tcache_init_perthread(storage_buf_tcache, &perthread_get(storage_buf_pt));
	return 0;
}

/**
 * storage_init_bundle - initializes storage (per-bundle)
 */
static int storage_init_bundle(int bundle_idx)
{
	uint32_t max_xfer_size, entries, depth;

	struct io_bundle *b = &bundles[bundle_idx];
	struct bundle_spec *bs = &iok.bundles[bundle_idx];
	struct hardware_queue_spec *hs;

	struct spdk_nvme_io_qpair_opts opts;

	spdk_nvme_ctrlr_get_default_io_qpair_opts(controller,
			&opts, sizeof(opts));
	max_xfer_size = spdk_nvme_ns_get_max_io_xfer_size(namespace);
	entries = (4096 - 1) / max_xfer_size + 2;
	depth = 64;
	if ((depth * entries) > opts.io_queue_size) {
		log_info("controller IO queue size %u less than required",
			opts.io_queue_size);
		log_info("Consider using lower queue depth or small IO size because "
			"IO requests may be queued at the NVMe driver.");
	}
	entries += 1;


	if (depth * entries > opts.io_queue_requests)
		opts.io_queue_requests = depth * entries;

	b->sq.qp_handle = spdk_nvme_ctrlr_alloc_io_qpair(controller, &opts, sizeof(opts));
	if (!b->sq.qp_handle) {
		log_err("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed");
		return 1;
	}

	nvme_get_qp_info(b->sq.qp_handle, &b->sq.cpl, &b->sq.cq_head, &b->sq.queue_depth);
	BUG_ON(!is_power_of_two(b->sq.queue_depth));

	hs = shmptr_to_ptr(&iok.shared_region, bs->hwq_specs, sizeof(*hs));
	BUG_ON(!hs);
	hs += 1;
	bs->hwq_count += 1;

	hs->descriptor_size = sizeof(struct spdk_nvme_cpl);
	hs->nr_descriptors = b->sq.queue_depth;
	hs->descriptor_table = ptr_to_shmptr(&iok.shared_region, b->sq.cpl, hs->descriptor_size * hs->nr_descriptors);
	hs->parity_byte_offset = offsetof(struct spdk_nvme_cpl, status);
	hs->parity_bit_mask = 0x1;
	hs->hwq_type = HWQ_SPDK_NVME;
	hs->consumer_idx = ptr_to_shmptr(&iok.shared_region, b->sq.cq_head, sizeof(uint32_t));

	return 0;
}

/**
 * storage_proc_completions - process `budget` number of completions
 */
int storage_proc_completions(struct io_bundle *b,
	unsigned int budget, struct thread **wakeable_threads)
{
	assert_preempt_disabled();
	assert_spin_lock_held(&b->lock);

	cb_ths = wakeable_threads;
	nrcb_ths = 0;
	spdk_nvme_qpair_process_completions(b->sq.qp_handle, budget);
	return nrcb_ths;
}

/*
 * storage_write - write a payload to the nvme device
 *                 expects lba_count*storage_block_size() bytes to be allocated in the buffer
 *
 * returns -ENOMEM if no available memory, and -EIO if the write operation failed
 */
int storage_write(const void *payload, uint64_t lba, uint32_t lba_count)
{
	int rc;
	bool use_tc = lba_count * block_size <= REQUEST_BUF_SZ;
	struct kthread *k;
	struct io_bundle *b;
	void *spdk_payload;

	k = getk();
	b = get_first_bundle(k);

	if (likely(use_tc)) {
		spdk_payload = tcache_alloc(&perthread_get(storage_buf_pt));
	} else {
		spdk_payload = spdk_zmalloc(lba_count * block_size, 0, NULL,
			 SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	}

	if (unlikely(spdk_payload == NULL)) {
		putk();
		return -ENOMEM;
	}

	memcpy(spdk_payload, payload, lba_count * block_size);

	spin_lock(&b->lock);
	rc = spdk_nvme_ns_cmd_write(namespace, b->sq.qp_handle, spdk_payload,
			lba, /* LBA start */
			lba_count, /* number of LBAs */
			seq_complete, thread_self(), 0);
	if (unlikely(rc != 0)) {
		spin_unlock(&b->lock);
		rc = -EIO;
		goto done_np;
	}

	thread_park_and_unlock_np(&b->lock);

	preempt_disable();

done_np:
	if (likely(use_tc))
		tcache_free(&perthread_get(storage_buf_pt), spdk_payload);
	else
		spdk_free(spdk_payload);

	preempt_enable();

	return rc;
}

/*
 * storage_read - read a payload from the nvme device
 *                expects lba_count*storage_block_size() bytes to be allocated in the buffer
 *
 * returns -ENOMEM if no available memory, and -EIO if the write operation failed
 */
int storage_read(void *dest, uint64_t lba, uint32_t lba_count)
{
	int rc;
	bool use_tc = lba_count * block_size <= REQUEST_BUF_SZ;
	struct kthread *k;
	struct io_bundle *b;
	void *spdk_dest;

	k = getk();
	b = get_first_bundle(k);

	if (likely(use_tc)) {
		spdk_dest = tcache_alloc(&perthread_get(storage_buf_pt));
	} else {
		spdk_dest = spdk_zmalloc(lba_count * block_size, 0, NULL,
			 SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	}

	if (unlikely(spdk_dest == NULL)) {
		putk();
		return -ENOMEM;
	}

	spin_lock(&b->lock);
	rc = spdk_nvme_ns_cmd_read(namespace, b->sq.qp_handle, spdk_dest,
			lba, /* LBA start */
			lba_count, /* number of LBAs */
			seq_complete, thread_self(), 0);
	if (unlikely(rc != 0)) {
		spin_unlock(&b->lock);
		rc = -EIO;
		goto done_np;
	}

	thread_park_and_unlock_np(&b->lock);
	memcpy(dest, spdk_dest, lba_count * block_size);
	preempt_disable();

done_np:
	if (likely(use_tc))
		tcache_free(&perthread_get(storage_buf_pt), spdk_dest);
	else
		spdk_free(spdk_dest);
	preempt_enable();
	return 0;
}

/*
 * storage_block_size - get the size of a block from the nvme device
 */
unsigned int storage_block_size(void)
{
	return block_size;
}

/*
 * storage_num_blocks - gets the number of blocks from the nvme device
 */
unsigned int storage_num_blocks(void)
{
	return num_blocks;
}

#else

#include "defs.h"

int storage_init(void)
{
	return 0;
}

int storage_init_thread(void)
{
	return 0;
}

#endif
