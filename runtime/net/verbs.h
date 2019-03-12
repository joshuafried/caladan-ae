/*
 * verbs.h - Verbs driver for Shenango's network statck
 */


#pragma once

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
	uint32_t cq_head;
	uint32_t *cq_head_ptr; // head pointer will be in shared memory

} __aligned(CACHE_LINE_SIZE);

struct verbs_queue_tx {
	/* handles to ibverbs cq and qp */
	struct ibv_cq_ex *tx_cq;
	struct ibv_qp *tx_qp;

	/* direct verbs qp */
	struct mbuf **buffers; // pending DMA
	struct mlx5dv_qp tx_qp_dv;
	uint32_t sq_head;

	/* direct verbs cq */
	struct mlx5dv_cq tx_cq_dv __aligned(CACHE_LINE_SIZE);
	uint32_t cq_head;
	uint32_t *cq_head_ptr; // head pointer will be in shared memory

} __aligned(CACHE_LINE_SIZE);

static inline unsigned int nr_inflight_tx(struct verbs_queue_tx *v)
{
	return v->sq_head - v->cq_head;
}

/*
 * cqe_status - retrieves status of completion queue element
 * @cqe: pointer to element
 * @cqe_cnt: total number of elements
 * @idx: index as stored in head pointer
 *
 * returns CQE status enum (MLX5_CQE_INVALID is -1)
 */
static inline uint8_t cqe_status(struct mlx5_cqe64 *cqe, uint32_t cqe_cnt, uint32_t head)
{
	uint16_t parity = head & cqe_cnt;
	uint8_t op_own = ACCESS_ONCE(cqe->op_own);
	uint8_t op_owner = op_own & MLX5_CQE_OWNER_MASK;
	uint8_t op_code = (op_own & 0xf0) >> 4;

	return ((op_owner == !parity) * MLX5_CQE_INVALID) | op_code;
}

static inline bool verbs_has_tx_completions(struct verbs_queue_tx *vq)
{
	uint32_t head = vq->cq_head;
	struct mlx5_cqe64 *cqes = vq->tx_cq_dv.buf;
	struct mlx5_cqe64 *cqe = &cqes[head & (vq->tx_cq_dv.cqe_cnt - 1)];
	return cqe_status(cqe, vq->tx_cq_dv.cqe_cnt, head) != MLX5_CQE_INVALID;
}

static inline bool verbs_has_rx_packets(struct verbs_queue_rx *vq)
{
	uint32_t head = vq->cq_head;
	struct mlx5_cqe64 *cqes = vq->rx_cq_dv.buf;
	struct mlx5_cqe64 *cqe = &cqes[head & (vq->rx_cq_dv.cqe_cnt - 1)];
	return cqe_status(cqe, vq->rx_cq_dv.cqe_cnt, head) != MLX5_CQE_INVALID;
}

struct rx_net_hdr;

void verbs_rx_completion(unsigned long completion_data);
int verbs_transmit_one(struct verbs_queue_tx *v, struct mbuf *m);
int verbs_gather_rx(struct rx_net_hdr **hdrs, struct verbs_queue_rx *v, unsigned int budget);

/* Initialization functions */
size_t verbs_shm_space_needed(size_t rx_qs, size_t tx_qs);
