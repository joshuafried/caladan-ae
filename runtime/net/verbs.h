/*
 * verbs.h - Verbs driver for Shenango's network statck
 */


#pragma once

#include <base/mempool.h>
#include <base/list.h>
#include <net/mbuf.h>

#include <infiniband/mlx5dv.h>
#include <infiniband/verbs.h>

struct verbs_custom_cq {
	struct mlx5dv_cq dvcq;
	uint32_t cq_idx __aligned(CACHE_LINE_SIZE);
	uint32_t pad[15];
};

struct verbs_queue_rx {
	struct ibv_cq_ex *rx_cq; /* completion queue for rx_wq */
	struct ibv_wq *rx_wq; /* single RX work queue */

	/* Support for directly exposed queues */
	struct verbs_custom_cq *raw_cq;

	struct list_node link;
} __aligned(CACHE_LINE_SIZE);

struct verbs_queue_tx {
	struct ibv_cq_ex *tx_cq;
	struct ibv_qp *tx_qp;

	uint32_t pending_completions;
	uint32_t pad;

	/* Support for directly exposed queues */
	struct verbs_custom_cq *raw_cq;
};

void verbs_rx_completion(unsigned long completion_data);
int verbs_transmit_one(struct verbs_queue_tx *v, struct mbuf *m);

int verbs_gather_rx(struct rx_net_hdr **hdrs, struct verbs_queue_rx *v, unsigned int budget);
int verbs_gather_completions(struct mbuf **mbufs, struct verbs_queue_tx *v, unsigned int budget);

bool verbs_queue_is_empty(struct verbs_custom_cq *raw_cq);

/* Initialization functions */
int verbs_init(struct mempool *mp, struct verbs_queue_rx **qs, int nrqs);
int verbs_init_thread(void);
int verbs_init_rx_queue(struct verbs_queue_rx *v);
int verbs_init_tx_queue(struct verbs_queue_tx *v);
size_t verbs_shm_space_needed(size_t rx_qs, size_t tx_qs);
