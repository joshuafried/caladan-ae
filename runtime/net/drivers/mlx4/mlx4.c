 /*
 * mlx4.c - MLX4 driver for Shenango's network statck
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
#include "mlx4.h"

#define PORT_NUM 1 // TODO: make this dynamic

#define RX_BUF_RESERVED \
 (align_up(sizeof(struct rx_net_hdr), CACHE_LINE_SIZE))

static struct ibv_context *context;
static struct ibv_pd *pd;
static struct ibv_mr *mr_tx;
static struct ibv_mr *mr_rx;

static struct mlx4_rxq rxqs[MAX_BUNDLES];
static struct mlx4_txq txqs[NCPU];

void mlx4_rx_completion(unsigned long completion_data)
{
	preempt_disable();
	tcache_free(&perthread_get(net_rx_buf_pt), (void *)completion_data);
	preempt_enable();
}

static inline unsigned char *mlx4_rx_alloc_buf(void)
{
	return tcache_alloc(&perthread_get(net_rx_buf_pt));
}


/*
 * mlx4_refill_rxqueue - replenish RX queue with nrdesc bufs
 * @vq: queue to refill
 * @nrdesc: number of buffers to fill
 *
 * WARNING: nrdesc must not exceed the number of free slots in the RXq
 * returns 0 on success, errno on error
 */

static inline int mlx4_refill_rxqueue(struct mlx4_rxq *vq, int nrdesc)
{
	unsigned int i;
	uint32_t index;
	unsigned char *buf;
	struct mlx4_wqe_data_seg *seg;

	struct mlx4dv_rwq *wq = &vq->rx_wq_dv;

	assert(nrdesc + vq->wq_head >= vq->rxq.consumer_idx + wq->rq.wqe_cnt);

	for (i = 0; i < nrdesc; i++) {
		buf = mlx4_rx_alloc_buf();
		if (unlikely(!buf))
			return -ENOMEM;

		index = vq->wq_head++ & (wq->rq.wqe_cnt - 1);
		seg = wq->buf.buf + wq->rq.offset + (index << wq->rq.wqe_shift);
		seg->addr = htobe64((unsigned long)buf + RX_BUF_RESERVED);
		vq->buffers[index] = buf;
	}

	udma_to_device_barrier();
	*wq->rdb = htobe32(vq->wq_head & 0xffff);

	return 0;
}

static void mlx4_init_tx_segment(struct mlx4_txq *v, unsigned int idx)
{
	struct mlx4dv_qp *qp = &v->tx_qp_dv;
	struct mlx4_wqe_ctrl_seg *ctrl;
	struct mlx4_wqe_data_seg *dpseg;
	void *segment;

	segment = qp->buf.buf + qp->sq.offset + (idx << qp->sq.wqe_shift);
	ctrl = segment;
	dpseg = segment + sizeof(*ctrl);

	ctrl->srcrb_flags = htobe32(MLX4_WQE_CTRL_CQ_UPDATE);
	/* For raw eth, the MLX4_WQE_CTRL_SOLICIT flag is used
	 * to indicate that no icrc should be calculated */
	ctrl->srcrb_flags |= htobe32(MLX4_WQE_CTRL_SOLICIT);

	ctrl->srcrb_flags |= htobe32(MLX4_WQE_CTRL_IP_HDR_CSUM |
							   MLX4_WQE_CTRL_TCP_UDP_CSUM);
	ctrl->imm = 0;
	dpseg->lkey = htobe32(mr_tx->lkey);
	ctrl->fence_size = (sizeof(*ctrl) + sizeof(*dpseg)) / 16;
}

/*
 * mlx4_gather_completions - collect up to budget received packets and completions
 */
static int mlx4_gather_completions(struct mbuf **mbufs, struct mlx4_txq *v, unsigned int budget)
{
	struct mlx4dv_cq *cq = &v->tx_cq_dv;
	struct mlx4_cqe *cqe, *cqes = cq->buf.buf;

	unsigned int compl_cnt;
	uint16_t wqe_idx;

	for (compl_cnt = 0; compl_cnt < budget; compl_cnt++, v->cq_head++) {
		cqe = &cqes[2 * (v->cq_head & (cq->cqe_cnt - 1))] + 1;

		if (!!(ACCESS_ONCE(cqe->owner_sr_opcode) & MLX4_CQE_OWNER_MASK) ^ !!(v->cq_head & cq->cqe_cnt))
			break;

		BUG_ON(mlx4dv_get_cqe_opcode(cqe) == MLX4_CQE_OPCODE_ERROR);

		wqe_idx = be16toh(cqe->wqe_index) & (v->tx_qp_dv.sq.wqe_cnt - 1);
		mbufs[compl_cnt] = v->buffers[wqe_idx];
	}

	*cq->set_ci_db = htobe32(v->cq_head  & 0xffffff);

	return compl_cnt;
}

/*
 * mlx4_transmit_one - send one mbuf
 * @t: queue to use
 * @m: mbuf to send
 *
 * returns 0 on success, errno on error
 */
static int mlx4_transmit_one(struct tx_queue *t, struct mbuf *m)
{
	int i, compl = 0;
	struct mbuf *mbs[SQ_CLEAN_MAX];
	struct mlx4_txq *v = container_of(t, struct mlx4_txq, txq);
	struct mlx4dv_qp *qp = &v->tx_qp_dv;

	struct mlx4_wqe_ctrl_seg *ctrl;
	struct mlx4_wqe_data_seg *dpseg;

	uint32_t idx = v->sq_head & (v->tx_qp_dv.sq.wqe_cnt - 1);

	if (nr_inflight_tx(v) >= SQ_CLEAN_THRESH) {
		compl = mlx4_gather_completions(mbs, v, SQ_CLEAN_MAX);
		for (i = 0; i < compl; i++)
			mbuf_free(mbs[i]);
		if (unlikely(nr_inflight_tx(v) >= qp->sq.wqe_cnt)) {
			log_warn_ratelimited("txq full");
			return 1;
		}
	}

	ctrl = qp->buf.buf + qp->sq.offset + (idx << qp->sq.wqe_shift);
	dpseg = (void *)ctrl + sizeof(*ctrl);

	/* mac address goes into descriptor for loopback */
	ctrl->srcrb_flags16[0] = *(__be16 *)(uintptr_t)mbuf_data(m);
	ctrl->imm = *(__be32 *)((uintptr_t)(mbuf_data(m)) + 2);

	dpseg->addr = htobe64((uint64_t)mbuf_data(m));

	// assuming we dont straddle cache lines
	BUILD_ASSERT(sizeof(*ctrl) + sizeof(*dpseg) <= CACHE_LINE_SIZE);

	dpseg->byte_count = htobe32(mbuf_length(m));

	udma_to_device_barrier();

	ctrl->owner_opcode = htobe32(MLX4_OPCODE_SEND) |
			(v->sq_head & qp->sq.wqe_cnt ? htobe32(1 << 31) : 0);

	udma_to_device_barrier();

	mmio_write32_be(qp->sdb, qp->doorbell_qpn);
	v->buffers[v->sq_head++ & (v->tx_qp_dv.sq.wqe_cnt - 1)] = m;
	return 0;
}

static inline bool mlx4_csum_ok(struct mlx4_cqe *cqe)
{
	return (cqe->status & htobe32(MLX4_CQE_STATUS_IPV4_CSUM_OK)) ==
				 htobe32(MLX4_CQE_STATUS_IPV4_CSUM_OK);
}

static int mlx4_gather_rx(struct rx_queue *rxq, struct rx_net_hdr **hdrs, unsigned int budget)
{
	uint16_t wqe_idx;
	int rx_cnt;

	struct mlx4_rxq *v = container_of(rxq, struct mlx4_rxq, rxq);
	struct mlx4dv_rwq *wq = &v->rx_wq_dv;
	struct mlx4dv_cq *cq = &v->rx_cq_dv;

	struct mlx4_cqe *cqe, *cqes = cq->buf.buf;
	unsigned char *buf;
	struct rx_net_hdr *hdr;

	for (rx_cnt = 0; rx_cnt < budget; rx_cnt++, v->rxq.consumer_idx++) {
		cqe = &cqes[2 * (v->rxq.consumer_idx & (cq->cqe_cnt - 1))] + 1;

		if (!!(ACCESS_ONCE(cqe->owner_sr_opcode) & MLX4_CQE_OWNER_MASK) ^ !!(v->rxq.consumer_idx & cq->cqe_cnt))
			break;

		BUG_ON(mlx4dv_get_cqe_opcode(cqe) == MLX4_CQE_OPCODE_ERROR);

		wqe_idx = be16toh(cqe->wqe_index) & (wq->rq.wqe_cnt - 1);
		buf = v->buffers[wqe_idx];
		hdr = (struct rx_net_hdr *)(buf + RX_BUF_RESERVED - sizeof(*hdr));
		hdr->completion_data = (unsigned long)buf;
		hdr->len = be32toh(cqe->byte_cnt);
		hdr->csum_type = mlx4_csum_ok(cqe);
		hdr->rss_hash = cqe->immed_rss_invalid;
		hdrs[rx_cnt] = hdr;
	}

	if (unlikely(!rx_cnt))
		return rx_cnt;

	ACCESS_ONCE(*rxq->shadow_tail) = v->rxq.consumer_idx;

	*cq->set_ci_db = htobe32(v->rxq.consumer_idx  & 0xffffff);
	BUG_ON(mlx4_refill_rxqueue(v, rx_cnt));

	return rx_cnt;
}


/*
 * simple_alloc - simple memory allocator for internal MLX4 structures
 */
static void *simple_alloc(size_t size, void *priv_data)
{
	void *out;
	iok_shm_alloc(size, PGSIZE_4KB, &out);
	return out;
}

static void simple_free(void *ptr, void *priv_data) {}

static struct mlx4dv_ctx_allocators dv_allocators = {
	.alloc = simple_alloc,
	.free = simple_free,
};

static int mlx4_create_rxq(int index, int rwq_wqn_alignment)
{
	int i, ret, rwq_discard = 0;
	unsigned char *buf;
	struct mlx4_rxq *v = &rxqs[index];
	struct mlx4dv_rwq *wq;
	struct mlx4_wqe_data_seg *seg;
	struct ibv_wq *to_free[rwq_wqn_alignment];
	struct ibv_wq_attr wq_attr;

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
	v->rx_cq = ibv_create_cq_ex(context, &cq_attr);
	if (!v->rx_cq)
		return -errno;

	/* Create the work queue for RX */
	struct ibv_wq_init_attr wq_init_attr = {
		.wq_type = IBV_WQT_RQ,
		.max_wr = RQ_NUM_DESC,
		.max_sge = 1,
		.pd = pd,
		.cq = ibv_cq_ex_to_cq(v->rx_cq),
		.comp_mask = 0,
	};
	v->rx_wq = ibv_create_wq(context, &wq_init_attr);

	/* mlx4 wants the wqn to be aligned with the size of the RSS table */
	/* keep creating WQs until the alignment matches */
	if (rwq_wqn_alignment) {
		while (v->rx_wq && v->rx_wq->wq_num % rwq_wqn_alignment != 0) {
			to_free[rwq_discard++] = v->rx_wq;
			v->rx_wq = ibv_create_wq(context, &wq_init_attr);
		}

		for (i = 0; i < rwq_discard; i++)
			ibv_destroy_wq(to_free[i]);
	}

	if (!v->rx_wq)
		return -errno;

	if (wq_init_attr.max_wr != RQ_NUM_DESC)
		log_warn("Ring size is larger than anticipated");

	/* Set the WQ state to ready */
	memset(&wq_attr, 0, sizeof(wq_attr));
	wq_attr.attr_mask = IBV_WQ_ATTR_STATE;
	wq_attr.wq_state = IBV_WQS_RDY;
	ret = ibv_modify_wq(v->rx_wq, &wq_attr);
	if (ret)
		return -ret;

	/* expose direct verbs objects */
	struct mlx4dv_obj obj = {
		.cq = {
			.in = ibv_cq_ex_to_cq(v->rx_cq),
			.out = &v->rx_cq_dv,
		},
		.rwq = {
			.in = v->rx_wq,
			.out = &v->rx_wq_dv,
		},
	};
	ret = mlx4dv_init_obj(&obj, MLX4DV_OBJ_CQ | MLX4DV_OBJ_RWQ);
	if (ret)
		return -ret;

	BUG_ON(v->rx_cq_dv.cqe_size != 64);

	/* allocate list of posted buffers */
	v->buffers = aligned_alloc(CACHE_LINE_SIZE, v->rx_wq_dv.rq.wqe_cnt * sizeof(void *));
	if (!v->buffers)
		return -ENOMEM;

	v->rxq.descriptor_table = v->rx_cq_dv.buf.buf;
	v->rxq.nr_descriptors = v->rx_cq_dv.cqe_cnt;
	v->rxq.descriptor_log_size = __builtin_ctz(v->rx_cq_dv.cqe_size);
	v->rxq.parity_byte_offset = sizeof(struct mlx4_cqe) + offsetof(struct mlx4_cqe, owner_sr_opcode);
	v->rxq.parity_bit_mask = MLX4_CQE_OWNER_MASK;
	v->rxq.hwq_type = HWQ_MLX4;

	/* set byte_count and lkey for all descriptors once */
	wq = &v->rx_wq_dv;
	for (i = 0; i < wq->rq.wqe_cnt; i++) {
		seg = wq->buf.buf + wq->rq.offset + (i << wq->rq.wqe_shift);
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

	udma_to_device_barrier();
	*wq->rdb = htobe32(v->wq_head & 0xffff);

	return 0;
}

static int mlx4_init_txq(int index, struct mlx4_txq *v)
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
	v->tx_cq = ibv_create_cq_ex(context, &cq_attr);
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
	struct mlx4dv_qp_init_attr dv_qp_attr = {
		.comp_mask = 0,
	};
	v->tx_qp = mlx4dv_create_qp(context, &qp_init_attr, &dv_qp_attr);
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

	struct mlx4dv_obj obj = {
		.cq = {
			.in = ibv_cq_ex_to_cq(v->tx_cq),
			.out = &v->tx_cq_dv,
		},
		.qp = {
			.in = v->tx_qp,
			.out = &v->tx_qp_dv,
		},
	};
	ret = mlx4dv_init_obj(&obj, MLX4DV_OBJ_CQ | MLX4DV_OBJ_QP);
	if (ret)
		return -ret;

	BUG_ON(v->tx_cq_dv.cqe_size != 64);

	/* allocate list of posted buffers */
	v->buffers = aligned_alloc(CACHE_LINE_SIZE, v->tx_qp_dv.sq.wqe_cnt * sizeof(*v->buffers));
	if (!v->buffers)
		return -ENOMEM;

	for (i = 0; i < v->tx_qp_dv.sq.wqe_cnt; i++)
		mlx4_init_tx_segment(v, i);

	return 0;
}


struct net_driver_ops mlx4_ops = {
	.rx_batch = mlx4_gather_rx,
	.tx_single = mlx4_transmit_one,
	.rx_completion = mlx4_rx_completion,
};

/*
 * mlx4_init - intialize all TX/RX queues
 */
int mlx4_init(struct rx_queue **rxq_out, struct tx_queue **txq_out,
	             unsigned int nr_rxq, unsigned int nr_txq)
{
	int i, ret;

	struct ibv_device **dev_list;
	struct ibv_device *ib_dev;
	struct ibv_qp *qp;
	struct ibv_qp_attr qp_attr;
	struct ibv_rwq_ind_table *rwq_ind_table;
	struct ibv_wq *ind_tbl[MAX_BUNDLES];
	struct ibv_flow *eth_flow;

	if (!is_power_of_two(nr_rxq) || nr_rxq > MAX_BUNDLES)
		return -EINVAL;

	dev_list = ibv_get_device_list(NULL);
	if (!dev_list) {
		perror("Failed to get IB devices list");
		return -1;
	}

	i = 0;
	while ((ib_dev = dev_list[i])) {
		if (strncmp(ibv_get_device_name(ib_dev), "mlx4", 4) == 0)
			break;
		i++;
	}

	if (!ib_dev) {
		log_err("mlx4_init: IB device not found");
		return -1;
	}

	context = ibv_open_device(ib_dev);

	if (!context) {
		log_err("mlx4_init: Couldn't get context for %s: errno = %d",
			ibv_get_device_name(ib_dev), errno);
		return -1;
	}

	ibv_free_device_list(dev_list);

	ret = mlx4dv_set_context_attr(context,
		  MLX4DV_SET_CTX_ATTR_BUF_ALLOCATORS, &dv_allocators);
	if (ret) {
		log_err("mlx4_init: error setting memory allocator");
		return -1;
	}

	pd = ibv_alloc_pd(context);
	if (!pd) {
		log_err("mlx4_init: Couldn't allocate PD");
		return -1;
	}

	/* Register memory for TX buffers */
	mr_tx = ibv_reg_mr(pd, net_tx_buf_mp.buf, net_tx_buf_mp.len, IBV_ACCESS_LOCAL_WRITE);
	if (!mr_tx) {
		log_err("mlx4_init: Couldn't register mr");
		return -1;
	}

	mr_rx = ibv_reg_mr(pd, net_rx_buf_mp.buf, net_rx_buf_mp.len, IBV_ACCESS_LOCAL_WRITE);
	if (!mr_rx) {
		log_err("mlx4_init: Couldn't register mr");
		return -1;
	}

	for (i = 0; i < nr_rxq; i++) {
		ret = mlx4_create_rxq(i, i ? 0 : nr_rxq);
		if (ret) {
			log_err("mlx4_init: failed to create rxq");
			return ret;
		}

		rxq_out[i] = &rxqs[i].rxq;
		ind_tbl[i] = rxqs[i].rx_wq;
	}

	/* Create Receive Work Queue Indirection Table */
	struct ibv_rwq_ind_table_init_attr rwq_attr = {
		.log_ind_tbl_size = __builtin_ctz(nr_bundles),
		.ind_tbl = ind_tbl,
		.comp_mask = 0,
	};
	rwq_ind_table = ibv_create_rwq_ind_table(context, &rwq_attr);
	if (!rwq_ind_table)
		return -errno;

	/* Create the main RX QP using the indirection table */
	struct ibv_rx_hash_conf rss_cnf = {
		.rx_hash_function = IBV_RX_HASH_FUNC_TOEPLITZ,
		.rx_hash_key_len = ARRAY_SIZE(rss_key),
		.rx_hash_key = rss_key,
#ifdef MLX4_TCP_RSS
		.rx_hash_fields_mask = IBV_RX_HASH_SRC_IPV4 | IBV_RX_HASH_DST_IPV4 | IBV_RX_HASH_SRC_PORT_TCP | IBV_RX_HASH_DST_PORT_TCP,
#else
		.rx_hash_fields_mask = IBV_RX_HASH_SRC_IPV4 | IBV_RX_HASH_DST_IPV4 | IBV_RX_HASH_SRC_PORT_UDP | IBV_RX_HASH_DST_PORT_UDP,
#endif
	};

	struct ibv_qp_init_attr_ex qp_ex_attr = {
		.qp_type = IBV_QPT_RAW_PACKET,
		.comp_mask = IBV_QP_INIT_ATTR_IND_TABLE | IBV_QP_INIT_ATTR_RX_HASH | IBV_QP_INIT_ATTR_PD,
		.pd = pd,
		.rwq_ind_tbl = rwq_ind_table,
		.rx_hash_conf = rss_cnf,
	};
	struct mlx4dv_qp_init_attr dv_qp_attr = {
		.comp_mask = 0,
	};

	qp = mlx4dv_create_qp(context, &qp_ex_attr, &dv_qp_attr);
	if (!qp)
		return -errno;

	/* Turn on QP in 2 steps */
	memset(&qp_attr, 0, sizeof(qp_attr));
	qp_attr.qp_state = IBV_QPS_INIT;
	qp_attr.port_num = 1;
	ret = ibv_modify_qp(qp, &qp_attr, IBV_QP_STATE | IBV_QP_PORT);
	if (ret)
		return -ret;

	memset(&qp_attr, 0, sizeof(qp_attr));
	qp_attr.qp_state = IBV_QPS_RTR;
	ret = ibv_modify_qp(qp, &qp_attr, IBV_QP_STATE);
	if (ret)
		return -ret;

	/* Route packets for our MAC address to our set of RX work queues */
	struct raw_eth_flow_attr {
		struct ibv_flow_attr attr;
		struct ibv_flow_spec_eth spec_eth;
	} __attribute__((packed)) flow_attr = {
		.attr = {
			.comp_mask = 0,
			.type = IBV_FLOW_ATTR_NORMAL,
			.size = sizeof(flow_attr),
			.priority = 0,
			.num_of_specs = 1,
			.port = PORT_NUM,
			.flags = 0,
		},
		.spec_eth = {
			.type = IBV_FLOW_SPEC_ETH,
			.size = sizeof(struct ibv_flow_spec_eth),
			.val = {
				.src_mac = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
				.ether_type = 0,
				.vlan_tag = 0,
			},
			.mask = {
				.dst_mac = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
				.src_mac = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
				.ether_type = 0,
				.vlan_tag = 0,
			}
		}
	};
	memcpy(&flow_attr.spec_eth.val.dst_mac, netcfg.mac.addr, 6);
	eth_flow = ibv_create_flow(qp, &flow_attr.attr);
	if (!eth_flow)
		return -errno;

	/* Route broadcst packets to our set of RX work queues */
	memcpy(&flow_attr.spec_eth.val.dst_mac, &flow_attr.spec_eth.mask.dst_mac, 6);
	eth_flow = ibv_create_flow(qp, &flow_attr.attr);
	if (!eth_flow)
		return -errno;

	for (i = 0; i < nr_txq; i++) {
		ret = mlx4_init_txq(i, &txqs[i]);
		if (ret)
			return ret;

		txq_out[i] = &txqs[i].txq;
	}

	netcfg.ops = mlx4_ops;

	return 0;
}
