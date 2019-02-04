/*
 * verbs.h - Verbs driver for Shenango's network statck
 */


#pragma once

#include <base/atomic.h>
#include <base/mempool.h>
#include <base/list.h>
#include <net/mbuf.h>

#include <infiniband/mlx5dv.h>
#include <infiniband/verbs.h>

struct verbs_queue_rx {
	/* handles to ibverbs cq and wq */
	struct ibv_cq_ex *rx_cq;
	struct ibv_wq *rx_wq;

	/* direct verbs rwq */
	void **buffers; // array of posted buffers
	struct mlx5dv_rwq rx_wq_dv;
	uint32_t wq_head;

	/* direct verbs cq */
	struct mlx5dv_cq rx_cq_dv;
	uint32_t *cq_head; // head pointer will be in shared memory

};

struct verbs_queue_tx {
	/* handles to ibverbs cq and qp */
	struct ibv_cq_ex *tx_cq;
	struct ibv_qp *tx_qp;

	/* direct verbs qp */
	struct mbuf **buffers; // pending DMA
	struct mlx5dv_qp tx_qp_dv;
	uint32_t sq_head;
	atomic_t pending_completions;

	/* direct verbs cq */
	struct mlx5dv_cq tx_cq_dv;
	uint32_t *cq_head; // head pointer will be in shared memory

};


static inline bool cqe_ready(struct mlx5_cqe64 *cqe, uint32_t cqe_cnt, uint32_t idx)
{
	uint16_t parity = idx & cqe_cnt;
	uint8_t op_own = ACCESS_ONCE(cqe->op_own);
	uint8_t op_owner = op_own & MLX5_CQE_OWNER_MASK;
	uint8_t op_code = (op_own & 0xf0) >> 4;

	return op_owner == !!parity && op_code != MLX5_CQE_INVALID;
}

static inline bool verbs_has_tx_completions(struct verbs_queue_tx *vq)
{
	uint32_t idx = *vq->cq_head;
	struct mlx5_cqe64 *cqes = vq->tx_cq_dv.buf;
	struct mlx5_cqe64 *cqe = &cqes[idx & (vq->tx_cq_dv.cqe_cnt - 1)];
	return cqe_ready(cqe, vq->tx_cq_dv.cqe_cnt, idx);
}

static inline bool verbs_has_rx_packets(struct verbs_queue_rx *vq)
{
	uint32_t idx = *vq->cq_head;
	struct mlx5_cqe64 *cqes = vq->rx_cq_dv.buf;
	struct mlx5_cqe64 *cqe = &cqes[idx & (vq->rx_cq_dv.cqe_cnt - 1)];
	return cqe_ready(cqe, vq->rx_cq_dv.cqe_cnt, idx);
}


void verbs_rx_completion(unsigned long completion_data);
int verbs_transmit_one(struct verbs_queue_tx *v, struct mbuf *m);

int verbs_gather_rx(struct rx_net_hdr **hdrs, struct verbs_queue_rx *v, unsigned int budget);
int verbs_gather_completions(struct mbuf **mbufs, struct verbs_queue_tx *v, unsigned int budget);

/* Initialization functions */
int verbs_init(struct mempool *mp, struct verbs_queue_rx *qs, int nrqs);
int verbs_init_thread(void);
int verbs_init_rx_queue(struct verbs_queue_rx *v);
int verbs_init_tx_queue(struct verbs_queue_tx *v);
size_t verbs_shm_space_needed(size_t rx_qs, size_t tx_qs);
