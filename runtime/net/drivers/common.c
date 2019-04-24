
#include <base/log.h>

#include "common.h"
#include "../../defs.h"

static struct rx_queue *rxq_out[MAX_BUNDLES];
static struct tx_queue *txq_out[NCPU];

int mlx5_init(struct rx_queue **rxq_out, struct tx_queue **txq_out,
	             unsigned int nr_rxq, unsigned int nr_txq);
int mlx4_init(struct rx_queue **rxq_out, struct tx_queue **txq_out,
	             unsigned int nr_rxq, unsigned int nr_txq);

int ethdev_init(void)
{
	int i, ret;
	struct io_bundle *b;
	struct bundle_spec *bs;
	struct hardware_queue_spec *hs;

	log_info("Creating %u rx queues", nr_bundles);

	/* try initializing mlx5 */
	ret = mlx5_init(rxq_out, txq_out, nr_bundles, maxks);
	if (ret)
		/* if that fails, try mlx4 */
		ret = mlx4_init(rxq_out, txq_out, nr_bundles, maxks);

	if (ret)
		return ret;

	for (i = 0; i < nr_bundles; i++) {
		b = &bundles[i];
		/* attach rxq to bundle */
		b->rxq = rxq_out[i];

		/* setup shadow tail pointer */
		b->rxq->shadow_tail = &b->b_vars->rx_cq_idx;

		/* publish specs for iokernel */
		bs = &iok.bundles[i];
		bs->hwq_count = 1;
		bs->hwq_specs = iok_shm_alloc(sizeof(*hs), 0, (void **)&hs);
		hs->descriptor_size = (1 << b->rxq->descriptor_log_size);
		hs->nr_descriptors = b->rxq->nr_descriptors;
		hs->descriptor_table = ptr_to_shmptr(&iok.shared_region, b->rxq->descriptor_table, hs->descriptor_size * hs->nr_descriptors);
		hs->parity_byte_offset = b->rxq->parity_byte_offset;
		hs->parity_bit_mask = b->rxq->parity_bit_mask;
		hs->hwq_type = b->rxq->hwq_type;
		hs->consumer_idx = ptr_to_shmptr(&iok.shared_region, b->rxq->shadow_tail, sizeof(uint32_t));
	}

	return 0;

}

int ethdev_init_thread(void)
{
	struct kthread *k = myk();

	k->txq = txq_out[k->kthread_idx];

	return 0;
}