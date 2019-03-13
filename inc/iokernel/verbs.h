#pragma once


#include <infiniband/mlx5dv.h>
#include <infiniband/verbs.h>

static inline bool cq_is_empty(void *buf, uint32_t cqe_cnt, uint32_t cq_idx)
{
	struct mlx5_cqe64 *cqes = buf;
	struct mlx5_cqe64 *cqe = &cqes[cq_idx & (cqe_cnt - 1)];
	uint16_t parity = cq_idx & cqe_cnt;
	uint8_t op_own = ACCESS_ONCE(cqe->op_own);
	uint8_t op_owner = op_own & MLX5_CQE_OWNER_MASK;
	uint8_t op_code = (op_own & 0xf0) >> 4;

	return op_owner != !!parity || op_code == MLX5_CQE_INVALID;
}