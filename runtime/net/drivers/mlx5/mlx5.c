/*
 * mlx5.c - MLX5 driver for Shenango's network statck
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <base/log.h>
#include <base/mempool.h>

#include <util/mmio.h>
#include <util/udma_barrier.h>

#include "../../../defs.h"
#include "../../defs.h"
#include "../common.h"
#include "mlx5.h"
#include "../verbs.h"

#define PORT_NUM 1 // TODO: make this dynamic

#define RX_BUF_RESERVED \
 (align_up(sizeof(struct rx_net_hdr), CACHE_LINE_SIZE))

static struct mlx5_rxq rxqs[MAX_BUNDLES];
static struct mlx5_txq txqs[NCPU];

void mlx5_rx_completion(unsigned long completion_data)
{
	preempt_disable();
	tcache_free(&perthread_get(net_rx_buf_pt), (void *)completion_data);
	preempt_enable();
}

static inline unsigned char *mlx5_rx_alloc_buf(void)
{
	return tcache_alloc(&perthread_get(net_rx_buf_pt));
}


/*
 * mlx5_refill_rxqueue - replenish RX queue with nrdesc bufs
 * @vq: queue to refill
 * @nrdesc: number of buffers to fill
 *
 * WARNING: nrdesc must not exceed the number of free slots in the RXq
 * returns 0 on success, errno on error
 */
static inline int mlx5_refill_rxqueue(struct mlx5_rxq *vq, int nrdesc)
{
	unsigned int i;
	uint32_t index;
	unsigned char *buf;
	struct mlx5_wqe_data_seg *seg;

	struct mlx5dv_rwq *wq = &vq->rx_wq_dv;

	assert(nrdesc + vq->wq_head >= vq->rxq.consumer_idx + wq->wqe_cnt);

	for (i = 0; i < nrdesc; i++) {
		buf = mlx5_rx_alloc_buf();
		if (unlikely(!buf))
			return -ENOMEM;

		index = vq->wq_head++ & (wq->wqe_cnt - 1);
		seg = wq->buf + (index << vq->rx_wq_log_stride);
		seg->addr = htobe64((unsigned long)buf + RX_BUF_RESERVED);
		vq->buffers[index] = buf;
	}

	udma_to_device_barrier();
	wq->dbrec[0] = htobe32(vq->wq_head & 0xffff);

	return 0;

}

static void mlx5_init_tx_segment(struct mlx5_txq *v, unsigned int idx)
{
	int size;
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct mlx5_wqe_eth_seg *eseg;
	struct mlx5_wqe_data_seg *dpseg;
	void *segment;

	segment = v->tx_qp_dv.sq.buf + idx * v->tx_qp_dv.sq.stride;
	ctrl = segment;
	eseg = segment + sizeof(*ctrl);
	dpseg = (void *)eseg + (offsetof(struct mlx5_wqe_eth_seg, inline_hdr) & ~0xf);

	size = (sizeof(*ctrl) / 16) +
	       (offsetof(struct mlx5_wqe_eth_seg, inline_hdr)) / 16 +
	       sizeof(struct mlx5_wqe_data_seg) / 16;

	/* set ctrl segment */
	*(uint32_t *)(segment + 8) = 0;
	ctrl->imm = 0;
	ctrl->fm_ce_se = MLX5_WQE_CTRL_CQ_UPDATE;
	ctrl->qpn_ds = htobe32(size | (v->tx_qp->qp_num << 8));

	/* set eseg */
	memset(eseg, 0, sizeof(struct mlx5_wqe_eth_seg));
	eseg->cs_flags |= MLX5_ETH_WQE_L3_CSUM | MLX5_ETH_WQE_L4_CSUM;

	/* set dpseg */
	dpseg->lkey = htobe32(mr_tx->lkey);
}

/*
 * mlx5_gather_completions - collect up to budget received packets and completions
 */
static int mlx5_gather_completions(struct mbuf **mbufs, struct mlx5_txq *v, unsigned int budget)
{
	struct mlx5dv_cq *cq = &v->tx_cq_dv;
	struct mlx5_cqe64 *cqe, *cqes = cq->buf;

	unsigned int compl_cnt;
	uint8_t opcode;
	uint16_t wqe_idx;

	for (compl_cnt = 0; compl_cnt < budget; compl_cnt++, v->cq_head++) {
		cqe = &cqes[v->cq_head & (cq->cqe_cnt - 1)];
		opcode = cqe_status(cqe, cq->cqe_cnt, v->cq_head);

		if (opcode == MLX5_CQE_INVALID)
			break;

		BUG_ON(opcode != MLX5_CQE_REQ);

		assert(mlx5_get_cqe_format(cqe) != 0x3);

		wqe_idx = be16toh(cqe->wqe_counter) & (v->tx_qp_dv.sq.wqe_cnt - 1);
		mbufs[compl_cnt] = load_acquire(&v->buffers[wqe_idx]);
	}

	cq->dbrec[0] = htobe32(v->cq_head & 0xffffff);

	return compl_cnt;
}

/*
 * verbs_transmit_one - send one mbuf
 * @v: queue to use
 * @m: mbuf to send
 *
 * returns 0 on success, errno on error
 */
static int mlx5_transmit_one(struct tx_queue *t, struct mbuf *m)
{
	int i, compl = 0;
	struct mlx5_txq *v = container_of(t, struct mlx5_txq, txq);
	uint32_t idx = v->sq_head & (v->tx_qp_dv.sq.wqe_cnt - 1);
	struct mbuf *mbs[SQ_CLEAN_MAX];
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct mlx5_wqe_eth_seg *eseg;
	struct mlx5_wqe_data_seg *dpseg;
	void *segment;

	if (nr_inflight_tx(v) >= SQ_CLEAN_THRESH) {
		compl = mlx5_gather_completions(mbs, v, SQ_CLEAN_MAX);
		for (i = 0; i < compl; i++)
			mbuf_free(mbs[i]);
		if (unlikely(nr_inflight_tx(v) >= v->tx_qp_dv.sq.wqe_cnt)) {
			log_warn_ratelimited("txq full");
			return 1;
		}
	}

	segment = v->tx_qp_dv.sq.buf + (idx << v->tx_sq_log_stride);
	ctrl = segment;
	eseg = segment + sizeof(*ctrl);
	dpseg = (void *)eseg + (offsetof(struct mlx5_wqe_eth_seg, inline_hdr) & ~0xf);

	ctrl->opmod_idx_opcode = htobe32(((v->sq_head & 0xffff) << 8) |
					       MLX5_OPCODE_SEND);


	dpseg->byte_count = htobe32(mbuf_length(m));
	dpseg->addr = htobe64((uint64_t)mbuf_data(m));

	/* record buffer */
	store_release(&v->buffers[v->sq_head & (v->tx_qp_dv.sq.wqe_cnt - 1)], m);
	v->sq_head++;

	/* write doorbell record */
	udma_to_device_barrier();
	v->tx_qp_dv.dbrec[MLX5_SND_DBR] = htobe32(v->sq_head & 0xffff);

	/* ring bf doorbell */
	mmio_wc_start();
	mmio_write64_be(v->tx_qp_dv.bf.reg, *(__be64 *)ctrl);
	mmio_flush_writes();

	return 0;

}

static int mlx5_gather_rx(struct rx_queue *rxq, struct rx_net_hdr **hdrs, unsigned int budget)
{
	uint8_t opcode;
	uint16_t wqe_idx;
	int rx_cnt;

	struct mlx5_rxq *v = container_of(rxq, struct mlx5_rxq, rxq);
	struct mlx5dv_rwq *wq = &v->rx_wq_dv;
	struct mlx5dv_cq *cq = &v->rx_cq_dv;

	struct mlx5_cqe64 *cqe, *cqes = cq->buf;
	unsigned char *buf;
	struct rx_net_hdr *hdr;

	for (rx_cnt = 0; rx_cnt < budget; rx_cnt++, v->rxq.consumer_idx++) {
		cqe = &cqes[v->rxq.consumer_idx & (cq->cqe_cnt - 1)];
		opcode = cqe_status(cqe, cq->cqe_cnt, v->rxq.consumer_idx);

		if (opcode == MLX5_CQE_INVALID)
			break;

		if (unlikely(opcode != MLX5_CQE_RESP_SEND)) {
			log_err("got opcode %02X", opcode);
			BUG();
		}

		assert(mlx5_get_cqe_format(cqe) != 0x3); // not compressed

		wqe_idx = be16toh(cqe->wqe_counter) & (wq->wqe_cnt - 1);
		buf = v->buffers[wqe_idx];
		hdr = (struct rx_net_hdr *)(buf + RX_BUF_RESERVED - sizeof(*hdr));
		hdr->completion_data = (unsigned long)buf;
		hdr->len = be32toh(cqe->byte_cnt);
		hdr->csum_type = mlx5_csum_ok(cqe);
		hdr->rss_hash = mlx5_get_rss_result(cqe);
		hdrs[rx_cnt] = hdr;
	}

	if (unlikely(!rx_cnt))
		return rx_cnt;

	ACCESS_ONCE(*rxq->shadow_tail) = v->rxq.consumer_idx;

	cq->dbrec[0] = htobe32(v->rxq.consumer_idx & 0xffffff);
	BUG_ON(mlx5_refill_rxqueue(v, rx_cnt));

	return rx_cnt;

}


/*
 * simple_alloc - simple memory allocator for internal MLX5 structures
 */
static void *simple_alloc(size_t size, void *priv_data)
{
	void *out;
	iok_shm_alloc(size, PGSIZE_4KB, &out);
	return out;
}

static void simple_free(void *ptr, void *priv_data) {}

static struct mlx5dv_ctx_allocators dv_allocators = {
	.alloc = simple_alloc,
	.free = simple_free,
};

static int mlx5_create_rxq(int index, struct mlx5_rxq *v)
{
	int i, ret;
	unsigned char *buf;

	v->rxq.id = index;

	/* Create a CQ */
	struct ibv_cq_init_attr_ex cq_attr = {
		.cqe = RQ_NUM_DESC,
		.channel = NULL,
		.comp_vector = 0,
		.wc_flags = IBV_WC_EX_WITH_BYTE_LEN,
		.comp_mask = IBV_CQ_INIT_ATTR_MASK_FLAGS,
		.flags = IBV_CREATE_CQ_ATTR_SINGLE_THREADED,
	};
	struct mlx5dv_cq_init_attr dv_cq_attr = {
		.comp_mask = 0,
	};
	v->rx_cq = mlx5dv_create_cq(context, &cq_attr, &dv_cq_attr);
	if (!v->rx_cq)
		return -errno;

	/* Create the work queue for RX */
	struct ibv_wq_init_attr wq_init_attr = {
		.wq_type = IBV_WQT_RQ,
		.max_wr = RQ_NUM_DESC,
		.max_sge = 1,
		.pd = pd,
		.cq = ibv_cq_ex_to_cq(v->rx_cq),
		.comp_mask = IBV_WQ_INIT_ATTR_FLAGS,
		.create_flags = IBV_WQ_FLAGS_DELAY_DROP,
	};
	struct mlx5dv_wq_init_attr dv_wq_attr = {
		.comp_mask = 0,
	};
	v->rx_wq = mlx5dv_create_wq(context, &wq_init_attr, &dv_wq_attr);
	if (!v->rx_wq)
		return -errno;

	if (wq_init_attr.max_wr != RQ_NUM_DESC)
		log_warn("Ring size is larger than anticipated");

	/* Set the WQ state to ready */
	struct ibv_wq_attr wq_attr;
	memset(&wq_attr, 0, sizeof(wq_attr));
	wq_attr.attr_mask = IBV_WQ_ATTR_STATE;
	wq_attr.wq_state = IBV_WQS_RDY;
	ret = ibv_modify_wq(v->rx_wq, &wq_attr);
	if (ret)
		return -ret;

	/* expose direct verbs objects */
	struct mlx5dv_obj obj = {
		.cq = {
			.in = ibv_cq_ex_to_cq(v->rx_cq),
			.out = &v->rx_cq_dv,
		},
		.rwq = {
			.in = v->rx_wq,
			.out = &v->rx_wq_dv,
		},
	};
	ret = mlx5dv_init_obj(&obj, MLX5DV_OBJ_CQ | MLX5DV_OBJ_RWQ);
	if (ret)
		return -ret;

	BUG_ON(!is_power_of_two(v->rx_wq_dv.stride));
	BUG_ON(!is_power_of_two(v->rx_cq_dv.cqe_size));
	v->rx_wq_log_stride = __builtin_ctz(v->rx_wq_dv.stride);
	v->rx_cq_log_stride = __builtin_ctz(v->rx_cq_dv.cqe_size);

	/* allocate list of posted buffers */
	v->buffers = aligned_alloc(CACHE_LINE_SIZE, v->rx_wq_dv.wqe_cnt * sizeof(void *));
	if (!v->buffers)
		return -ENOMEM;

	v->rxq.descriptor_table = v->rx_cq_dv.buf;
	v->rxq.nr_descriptors = v->rx_cq_dv.cqe_cnt;
	v->rxq.descriptor_log_size = __builtin_ctz(sizeof(struct mlx5_cqe64));
	v->rxq.parity_byte_offset = offsetof(struct mlx5_cqe64, op_own);
	v->rxq.parity_bit_mask = MLX5_CQE_OWNER_MASK;
	v->rxq.hwq_type = HWQ_MLX5;

	/* set byte_count and lkey for all descriptors once */
	struct mlx5dv_rwq *wq = &v->rx_wq_dv;
	for (i = 0; i < wq->wqe_cnt; i++) {
		struct mlx5_wqe_data_seg *seg = wq->buf + i * wq->stride;
		seg->byte_count =  htobe32(MBUF_DEFAULT_LEN - RX_BUF_RESERVED);
		seg->lkey = htobe32(mr_rx->lkey);

		/* fill queue with buffers */
		buf = mempool_alloc(&net_rx_buf_mp);
		if (!buf)
			return -ENOMEM;

		seg->addr = htobe64((unsigned long)buf + RX_BUF_RESERVED);
		v->buffers[i] = buf;
		v->wq_head++;
	}

	/* set ownership of cqes to "hardware" */
	struct mlx5dv_cq *cq = &v->rx_cq_dv;
	for (i = 0; i < cq->cqe_cnt; i++) {
		struct mlx5_cqe64 *cqe = cq->buf + i * cq->cqe_size;
		mlx5dv_set_cqe_owner(cqe, 1);
	}

	udma_to_device_barrier();
	wq->dbrec[0] = htobe32(v->wq_head & 0xffff);

	return 0;
}

static int mlx5_init_txq(int index, struct mlx5_txq *v)
{
	int i, ret;

	v->txq.id = index;

	/* Create a CQ */
	struct ibv_cq_init_attr_ex cq_attr = {
		.cqe = SQ_NUM_DESC,
		.channel = NULL,
		.comp_vector = 0,
		.wc_flags = 0,
		.comp_mask = IBV_CQ_INIT_ATTR_MASK_FLAGS,
		.flags = IBV_CREATE_CQ_ATTR_SINGLE_THREADED,
	};
	struct mlx5dv_cq_init_attr dv_cq_attr = {
		.comp_mask = 0,
	};
	v->tx_cq = mlx5dv_create_cq(context, &cq_attr, &dv_cq_attr);
	if (!v->tx_cq)
		return -errno;

	/* Create a 1-sided queue pair for sending packets */
	struct ibv_qp_init_attr_ex qp_init_attr = {
		.send_cq = ibv_cq_ex_to_cq(v->tx_cq),
		.recv_cq = ibv_cq_ex_to_cq(v->tx_cq),
		.cap = {
			.max_send_wr = SQ_NUM_DESC,
			.max_recv_wr = 0,
			.max_send_sge = 1,
			.max_inline_data = 0, // TODO: should inline some data?
		},
		.qp_type = IBV_QPT_RAW_PACKET,
		.sq_sig_all = 1,
		.pd = pd,
		.comp_mask = IBV_QP_INIT_ATTR_PD
	};
	struct mlx5dv_qp_init_attr dv_qp_attr = {
		.comp_mask = 0,
	};
	v->tx_qp = mlx5dv_create_qp(context, &qp_init_attr, &dv_qp_attr);
	if (!v->tx_qp)
		return -errno;

	/* Turn on TX QP in 3 steps */
	struct ibv_qp_attr qp_attr;
	memset(&qp_attr, 0, sizeof(qp_attr));
	qp_attr.qp_state = IBV_QPS_INIT;
	qp_attr.port_num = 1;
	ret = ibv_modify_qp(v->tx_qp, &qp_attr, IBV_QP_STATE | IBV_QP_PORT);
	if (ret)
		return -ret;

	memset(&qp_attr, 0, sizeof(qp_attr));
	qp_attr.qp_state = IBV_QPS_RTR;
	ret = ibv_modify_qp(v->tx_qp, &qp_attr, IBV_QP_STATE);
	if (ret)
		return -ret;

	memset(&qp_attr, 0, sizeof(qp_attr));
	qp_attr.qp_state = IBV_QPS_RTS;
	ret = ibv_modify_qp(v->tx_qp, &qp_attr, IBV_QP_STATE);
	if (ret)
		return -ret;

	struct mlx5dv_obj obj = {
		.cq = {
			.in = ibv_cq_ex_to_cq(v->tx_cq),
			.out = &v->tx_cq_dv,
		},
		.qp = {
			.in = v->tx_qp,
			.out = &v->tx_qp_dv,
		},
	};
	ret = mlx5dv_init_obj(&obj, MLX5DV_OBJ_CQ | MLX5DV_OBJ_QP);
	if (ret)
		return -ret;

	BUG_ON(!is_power_of_two(v->tx_cq_dv.cqe_size));
	BUG_ON(!is_power_of_two(v->tx_qp_dv.sq.stride));
	v->tx_sq_log_stride = __builtin_ctz(v->tx_qp_dv.sq.stride);
	v->tx_cq_log_stride = __builtin_ctz(v->tx_cq_dv.cqe_size);

	/* allocate list of posted buffers */
	v->buffers = aligned_alloc(CACHE_LINE_SIZE, v->tx_qp_dv.sq.wqe_cnt * sizeof(*v->buffers));
	if (!v->buffers)
		return -ENOMEM;

	for (i = 0; i < v->tx_qp_dv.sq.wqe_cnt; i++)
		mlx5_init_tx_segment(v, i);

	return 0;
}


struct net_driver_ops mlx5_ops = {
	.rx_batch = mlx5_gather_rx,
	.tx_single = mlx5_transmit_one,
	.rx_completion = mlx5_rx_completion,
};

/*
 * mlx5_init - intialize all TX/RX queues
 */
int mlx5_init(struct rx_queue **rxq_out, struct tx_queue **txq_out,
	             unsigned int nr_rxq, unsigned int nr_txq)
{
	int i, ret;

	struct ibv_device **dev_list;
	struct ibv_device *ib_dev;
	struct ibv_wq *ind_tbl[MAX_BUNDLES];

	if (!is_power_of_two(nr_rxq) || nr_rxq > MAX_BUNDLES)
		return -EINVAL;

	dev_list = ibv_get_device_list(NULL);
	if (!dev_list) {
		perror("Failed to get IB devices list");
		return -1;
	}

	i = 0;
	while ((ib_dev = dev_list[i])) {
		if (strncmp(ibv_get_device_name(ib_dev), "mlx5", 4) == 0)
			break;
		i++;
	}

	if (!ib_dev) {
		log_err("mlx5_init: IB device not found");
		return -1;
	}

	struct mlx5dv_context_attr attr;
	memset(&attr, 0, sizeof(attr));
	context = mlx5dv_open_device(ib_dev, &attr);
	if (!context) {
		log_err("mlx5_init: Couldn't get context for %s",
			ibv_get_device_name(ib_dev));
		return -1;
	}

	ibv_free_device_list(dev_list);

	ret = mlx5dv_set_context_attr(context,
		  MLX5DV_CTX_ATTR_BUF_ALLOCATORS, &dv_allocators);
	if (ret) {
		log_err("mlx5_init: error setting memory allocator");
		return -1;
	}

	pd = ibv_alloc_pd(context);
	if (!pd) {
		log_err("mlx5_init: Couldn't allocate PD");
		return -1;
	}

	/* Register memory for TX buffers */
	mr_tx = ibv_reg_mr(pd, net_tx_buf_mp.buf, net_tx_buf_mp.len, IBV_ACCESS_LOCAL_WRITE);
	if (!mr_tx) {
		log_err("mlx5_init: Couldn't register mr");
		return -1;
	}

	mr_rx = ibv_reg_mr(pd, net_rx_buf_mp.buf, net_rx_buf_mp.len, IBV_ACCESS_LOCAL_WRITE);
	if (!mr_rx) {
		log_err("mlx5_init: Couldn't register mr");
		return -1;
	}

	for (i = 0; i < nr_rxq; i++) {
		ret = mlx5_create_rxq(i, &rxqs[i]);
		if (ret)
			return ret;

		rxq_out[i] = &rxqs[i].rxq;
		ind_tbl[i] = rxqs[i].rx_wq;
	}

	ret = verbs_create_rss_qps(ind_tbl, nr_bundles);
	if (ret) {
	  log_err("mlx5_init: verbs_create_rss_qps returned %d", ret);
	  return ret;
	}

	for (i = 0; i < nr_txq; i++) {
		ret = mlx5_init_txq(i, &txqs[i]);
		if (ret)
			return ret;

		txq_out[i] = &txqs[i].txq;
	}

	netcfg.ops = mlx5_ops;

	return 0;
}
