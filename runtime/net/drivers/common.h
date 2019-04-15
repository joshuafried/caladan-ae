#pragma once

#include <iokernel/control.h>
#include <net/mbuf.h>


struct rx_queue {
	int		id;

	/* runtime provides this memory location */
	uint32_t		*shadow_tail; /* shared with iokernel */
	uint32_t		consumer_idx;

	/* driver provides details for business monitoring */
	void		*descriptor_table;
	uint32_t		descriptor_log_size;
	uint32_t		nr_descriptors;
	uint32_t		parity_byte_offset;
	uint32_t		parity_bit_mask;
	uint32_t		hwq_type;
};

struct tx_queue {
	int id;
};

static inline bool rx_pending(struct rx_queue *rxq)
{
	uint32_t tail, idx, parity, hd_parity;
	unsigned char *addr;

	tail = ACCESS_ONCE(rxq->consumer_idx);
	idx = tail & (rxq->nr_descriptors - 1);
	parity = !!(tail & rxq->nr_descriptors);
	addr = rxq->descriptor_table + (idx << rxq->descriptor_log_size) + rxq->parity_byte_offset;
	hd_parity = !!(ACCESS_ONCE(*addr) & rxq->parity_bit_mask);

	return parity == hd_parity;
}

struct rx_net_hdr;
struct net_driver_ops {
	int (*rx_batch)(struct rx_queue *rxq, struct rx_net_hdr **bufs, unsigned int budget);
	int (*tx_single)(struct tx_queue *txq, struct mbuf *m);
	void (*rx_completion)(unsigned long completion_data);
};
