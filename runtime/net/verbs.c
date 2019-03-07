/*
 * verbs.c - Verbs driver for Shenango's network statck
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
#include <base/random.h>
#include <base/slab.h>
#include <net/mbuf.h>

#include <util/mmio.h>
#include <util/udma_barrier.h>

#include "defs.h"
#include "verbs.h"

#define PORT_NUM 1 // TODO: make this dynamic

#define BUF_SZ MBUF_DEFAULT_LEN
#define RX_BUF_RESERVED \
 (align_up(sizeof(struct rx_net_hdr), CACHE_LINE_SIZE))

static struct ibv_context *context;
static struct ibv_pd *pd;
static struct ibv_mr *mr_tx;
static struct ibv_mr *mr_rx;

static struct mempool verbs_buf_mp;
static struct tcache *verbs_buf_tcache;
static DEFINE_PERTHREAD(struct tcache_perthread, verbs_buf_pt);

void verbs_rx_completion(unsigned long completion_data)
{
	char *buf = (char *)completion_data - RX_BUF_RESERVED;

	preempt_disable();
	tcache_free(&perthread_get(verbs_buf_pt), buf);
	preempt_enable();
}

static inline unsigned char *verbs_rx_alloc_buf(void)
{
	unsigned char *buf;

	assert_preempt_disabled();
	buf = tcache_alloc(&perthread_get(verbs_buf_pt));
	if (unlikely(!buf))
		return NULL;

	return buf + RX_BUF_RESERVED;
}


/*
 * verbs_refill_rxqueue - replenish RX queue with nrdesc bufs
 * @vq: queue to refill
 * @nrdesc: number of buffers to fill
 *
 * WARNING: nrdesc must not exceed the number of free slots in the RXq
 * returns 0 on success, errno on error
 */
static int verbs_refill_rxqueue(struct verbs_queue_rx *vq, int nrdesc)
{
	unsigned int i;
	uint32_t index;
	unsigned char *buf;
	struct mlx5_wqe_data_seg *seg;

	struct mlx5dv_rwq *wq = &vq->rx_wq_dv;

	assert(nrdesc + vq->wq_head > *vq->cq_head + wq->wqe_cnt);

	for (i = 0; i < nrdesc; i++) {
		buf = verbs_rx_alloc_buf();
		if (unlikely(!buf))
			return -ENOMEM;

		index = vq->wq_head++ & (wq->wqe_cnt - 1);
		seg = wq->buf + index * wq->stride;
		seg->addr = htobe64((unsigned long)buf);
		vq->buffers[index] = buf;
	}

	udma_to_device_barrier();
	wq->dbrec[0] = htobe32(vq->wq_head & 0xffff);

	return 0;

}

/*
 * verbs_transmit_one - send one mbuf
 * @v: queue to use
 * @m: mbuf to send
 *
 * returns 0 on success, errno on error
 */
int verbs_transmit_one(struct verbs_queue_tx *v, struct mbuf *m)
{
	uint32_t idx = v->sq_head & (v->tx_qp_dv.sq.wqe_cnt - 1);

	if (unlikely(nr_inflight_tx(v) >= v->tx_qp_dv.sq.wqe_cnt)) {
		log_warn_ratelimited("txq full");
		return 1;
	}

	void *segment = v->tx_qp_dv.sq.buf + idx * v->tx_qp_dv.sq.stride;
	struct mlx5_wqe_ctrl_seg *ctrl = segment;
	struct mlx5_wqe_eth_seg *eseg = segment + sizeof(*ctrl);
	struct mlx5_wqe_data_seg *dpseg = (void *)eseg + (offsetof(struct mlx5_wqe_eth_seg, inline_hdr) & ~0xf);

	int size = (sizeof(*ctrl) / 16) +
	           (offsetof(struct mlx5_wqe_eth_seg, inline_hdr)) / 16 +
	           sizeof(struct mlx5_wqe_data_seg) / 16;

	/* set ctrl segment */
	*(uint32_t *)(segment + 8) = 0;
	ctrl->imm = 0;
	ctrl->fm_ce_se = MLX5_WQE_CTRL_CQ_UPDATE;
	ctrl->opmod_idx_opcode = htobe32(((v->sq_head & 0xffff) << 8) |
					       MLX5_OPCODE_SEND);
	ctrl->qpn_ds = htobe32(size | (v->tx_qp->qp_num << 8));

	/* set eseg */
	memset(eseg, 0, sizeof(struct mlx5_wqe_eth_seg));
	eseg->cs_flags |= MLX5_ETH_WQE_L3_CSUM | MLX5_ETH_WQE_L4_CSUM;

	/* set dpseg */
	dpseg->byte_count = htobe32(mbuf_length(m));
	dpseg->lkey = htobe32(mr_tx->lkey);
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

static inline int mlx5_csum_ok(struct mlx5_cqe64 *cqe)
{
	return ((cqe->hds_ip_ext & (MLX5_CQE_L4_OK | MLX5_CQE_L3_OK)) ==
		 (MLX5_CQE_L4_OK | MLX5_CQE_L3_OK)) &
		(((cqe->l4_hdr_type_etc >> 2) & 0x3) == MLX5_CQE_L3_HDR_TYPE_IPV4);
}

static inline int mlx5_get_cqe_opcode(struct mlx5_cqe64 *cqe)
{
	return (cqe->op_own & 0xf0) >> 4;
}

static inline int mlx5_get_cqe_format(struct mlx5_cqe64 *cqe)
{
	return (cqe->op_own & 0xc) >> 2;
}

int verbs_gather_rx(struct rx_net_hdr **hdrs, struct verbs_queue_rx *v, unsigned int budget)
{
	char *buf;
	uint8_t opcode;
	uint16_t wqe_idx;
	uint32_t head = *v->cq_head;
	int rx_cnt;

	struct mlx5dv_rwq *wq = &v->rx_wq_dv;
	struct mlx5dv_cq *cq = &v->rx_cq_dv;

	struct mlx5_cqe64 *cqe, *cqes = cq->buf;
	struct rx_net_hdr *hdr;

	for (rx_cnt = 0; rx_cnt < budget; rx_cnt++, head++) {
		cqe = &cqes[head & (cq->cqe_cnt - 1)];
		opcode = cqe_status(cqe, cq->cqe_cnt, head);

		if (opcode == MLX5_CQE_INVALID)
			break;

		BUG_ON(opcode != MLX5_CQE_RESP_SEND);

		assert(mlx5_get_cqe_format(cqe) != 0x3); // not compressed

		wqe_idx = be16toh(cqe->wqe_counter) & (wq->wqe_cnt - 1);
		buf = v->buffers[wqe_idx];
		hdr = (struct rx_net_hdr *)(buf - sizeof(*hdr));
		hdr->completion_data = (unsigned long)buf;
		hdr->len = be32toh(cqe->byte_cnt);
		hdr->csum_type = mlx5_csum_ok(cqe);
		hdrs[rx_cnt] = hdr;
	}

	*v->cq_head = head;

	if (likely(rx_cnt)) {
		udma_to_device_barrier();
		cq->dbrec[0] = htobe32(head & 0xffffff);
		BUG_ON(verbs_refill_rxqueue(v, rx_cnt));
	}

	return rx_cnt;

}

/*
 * verbs_gather_work - collect up to budget received packets and completions
 */
int verbs_gather_completions(struct mbuf **mbufs, struct verbs_queue_tx *v, unsigned int budget)
{
	struct mlx5dv_cq *cq = &v->tx_cq_dv;
	struct mlx5_cqe64 *cqe, *cqes = cq->buf;

	unsigned int compl_cnt;
	uint8_t opcode;
	uint32_t head = *v->cq_head;
	uint16_t wqe_idx;

	for (compl_cnt = 0; compl_cnt < budget; compl_cnt++, head++) {
		cqe = &cqes[head & (cq->cqe_cnt - 1)];
		opcode = cqe_status(cqe, cq->cqe_cnt, head);

		if (opcode == MLX5_CQE_INVALID)
			break;

		BUG_ON(opcode != MLX5_CQE_REQ);

		assert(mlx5_get_cqe_format(cqe) != 0x3);

		wqe_idx = be16toh(cqe->wqe_counter) & (v->tx_qp_dv.sq.wqe_cnt - 1);
		mbufs[compl_cnt] = load_acquire(&v->buffers[wqe_idx]);
	}

	if (likely(compl_cnt)) {
		udma_to_device_barrier();
		cq->dbrec[0] = htobe32(head & 0xffffff);
	}

	*v->cq_head = head;

	return compl_cnt;
}

size_t verbs_shm_space_needed(size_t rx_qs, size_t tx_qs)
{
	// TODO: precisely calculate
	return 12 * PGSIZE_2MB;
}

/*
 * simple_alloc - simple memory allocator for internal MLX5 structures
 */
static void *simple_alloc(size_t size, void *priv_data)
{
	static size_t nxt;
	static DEFINE_SPINLOCK(alloc_lock);

	void *p = NULL;

	spin_lock(&alloc_lock);

	if (nxt + size > iok.verbs_mem_len)
		goto out;

	p = (unsigned char *)iok.verbs_mem + nxt;
	nxt += align_up(size, PGSIZE_4KB);

out:
	spin_unlock(&alloc_lock);
	return p;
}

static void simple_free(void *ptr, void *priv_data) {}

static struct mlx5dv_ctx_allocators dv_allocators = {
	.alloc = simple_alloc,
	.free = simple_free,
};

static int verbs_create_rx_queue(struct verbs_queue_rx *v)
{
	int i, ret;

	memset(v, 0, sizeof(*v));

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

	/* allocate list of posted buffers */
	v->buffers = aligned_alloc(CACHE_LINE_SIZE, v->rx_wq_dv.wqe_cnt * sizeof(void *));
	if (!v->buffers)
		return -ENOMEM;

	/* allocate a shared memory head pointer */
	v->cq_head = simple_alloc(CACHE_LINE_SIZE, 0);
	if (!v->cq_head)
		return -ENOMEM;
	*v->cq_head = 0;

	/* send queue spec to iokernel */
	struct mlxq_spec *qs = &iok.qspec[iok.mlxq_count++];
	BUG_ON(iok.mlxq_count > NCPU);
	qs->cq_buf = ptr_to_shmptr(&iok.shared_region, v->rx_cq_dv.buf,  v->rx_cq_dv.cqe_cnt * sizeof(struct mlx5_cqe64));
	qs->cq_idx = ptr_to_shmptr(&iok.shared_region, v->cq_head, sizeof(*v->cq_head));
	qs->cqe_cnt = v->rx_cq_dv.cqe_cnt;

	/* set byte_count and lkey for all descriptors once */
	struct mlx5dv_rwq *wq = &v->rx_wq_dv;
	for (i = 0; i < wq->wqe_cnt; i++) {
		struct mlx5_wqe_data_seg *seg = wq->buf + i * wq->stride;
		seg->byte_count =  htobe32(BUF_SZ - RX_BUF_RESERVED);
		seg->lkey = htobe32(mr_rx->lkey);
	}

	return 0;
}

/*
 * verbs_init - intialize all TX/RX queues
 */
int verbs_init(struct mempool *tx_mp, struct verbs_queue_rx *qs, int nrqs)
{
	int i, fd, ret;
	void *rx_buf;

	struct ibv_device **dev_list;
	struct ibv_device *ib_dev;
	struct ibv_qp *qp;
	struct ibv_rwq_ind_table *rwq_ind_table;
	struct ibv_wq *ind_tbl[nrqs];
	struct ibv_flow *eth_flow;

	if (!is_power_of_two(nrqs))
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
		log_err("verbs_init: IB device not found");
		return -1;
	}

	struct mlx5dv_context_attr attr;
	memset(&attr, 0, sizeof(attr));
	context = mlx5dv_open_device(ib_dev, &attr);
	if (!context) {
		log_err("verbs_init: Couldn't get context for %s",
			ibv_get_device_name(ib_dev));
		return -1;
	}

	ibv_free_device_list(dev_list);

	ret = mlx5dv_set_context_attr(context,
		  MLX5DV_CTX_ATTR_BUF_ALLOCATORS, &dv_allocators);
	if (ret) {
		log_err("verbs_init: error setting memory allocator");
		return -1;
	}

	pd = ibv_alloc_pd(context);
	if (!pd) {
		log_err("verbs_init: Couldn't allocate PD");
		return -1;
	}

	/* Register memory for TX buffers */
	mr_tx = ibv_reg_mr(pd, tx_mp->buf, tx_mp->len, IBV_ACCESS_LOCAL_WRITE);
	if (!mr_tx) {
		log_err("verbs_init: Couldn't register mr");
		return -1;
	}

	rx_buf = mem_map_anom(NULL, RX_BUF_BOOL_SZ(nrqs), PGSIZE_2MB, 0);
	if (rx_buf == MAP_FAILED)
		return -ENOMEM;

	ret = mempool_create(&verbs_buf_mp, rx_buf, RX_BUF_BOOL_SZ(nrqs),
			     PGSIZE_2MB, BUF_SZ);
	if (ret)
		return ret;

	verbs_buf_tcache = mempool_create_tcache(&verbs_buf_mp,
		"verbs_rx_bufs", VERBS_RX_BUF_TC_MAG);
	if (!verbs_buf_tcache)
		return -ENOMEM;

	mr_rx = ibv_reg_mr(pd, rx_buf, RX_BUF_BOOL_SZ(nrqs), IBV_ACCESS_LOCAL_WRITE);
	if (!mr_rx) {
		log_err("verbs_init: Couldn't register mr");
		return -1;
	}

	for (i = 0; i < nrqs; i++) {
		struct verbs_queue_rx *v = &qs[i];
		ret = verbs_create_rx_queue(v);
		if (ret)
			return ret;

		ind_tbl[i] = v->rx_wq;
	}

	/* Create Receive Work Queue Indirection Table */
	struct ibv_rwq_ind_table_init_attr rwq_attr = {
		.log_ind_tbl_size = __builtin_ctz(nrqs),
		.ind_tbl = ind_tbl,
		.comp_mask = 0,
	};
	rwq_ind_table = ibv_create_rwq_ind_table(context, &rwq_attr);
	if (!rwq_ind_table)
		return -errno;

	/* Populate random hash key, todo - decide what to do here */
	static char key[40];
	ret = fill_random_bytes(key, 40);
	if (ret)
		return ret;

	/* Create the main RX QP using the indirection table */
	struct ibv_rx_hash_conf rss_cnf = {
		.rx_hash_function = IBV_RX_HASH_FUNC_TOEPLITZ,
		.rx_hash_key_len = 40,
		.rx_hash_key = (void *)&key[0],
#ifdef MLX5_TCP_RSS
		.rx_hash_fields_mask = IBV_RX_HASH_SRC_IPV4 | IBV_RX_HASH_DST_IPV4 | IBV_RX_HASH_SRC_PORT_TCP | IBV_RX_HASH_DST_PORT_TCP,
#else
		.rx_hash_fields_mask = IBV_RX_HASH_SRC_IPV4 | IBV_RX_HASH_DST_IPV4 | IBV_RX_HASH_SRC_PORT_UDP | IBV_RX_HASH_DST_PORT_UDP,
#endif
	};
	struct ibv_qp_init_attr_ex qp_ex_attr = {
		.qp_type = IBV_QPT_RAW_PACKET,
		.comp_mask =  IBV_QP_INIT_ATTR_IND_TABLE | IBV_QP_INIT_ATTR_RX_HASH | IBV_QP_INIT_ATTR_PD,
		.pd = pd,
		.rwq_ind_tbl = rwq_ind_table,
		.rx_hash_conf = rss_cnf,
	};

	qp = ibv_create_qp_ex(context, &qp_ex_attr);
	if (!qp)
		return -errno;

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

	/* Route multicast traffic to our RX queues */
	struct ibv_flow_attr mc_attr = {
		.comp_mask = 0,
		.type = IBV_FLOW_ATTR_MC_DEFAULT,
		.size = sizeof(mc_attr),
		.priority = 0,
		.num_of_specs = 0,
		.port = PORT_NUM,
		.flags = 0,
	};
	eth_flow = ibv_create_flow(qp, &mc_attr);
	if (!eth_flow)
		return -errno;

	return 0;
}

/*
 * verbs_init_thread - intializes per-thread data structures
 * */
int verbs_init_thread(void)
{
	tcache_init_perthread(verbs_buf_tcache, &perthread_get(verbs_buf_pt));
	return 0;
}

int verbs_init_tx_queue(struct verbs_queue_tx *v)
{
	int ret;

	memset(v, 0, sizeof(*v));

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

	/* allocate list of posted buffers */
	v->buffers = aligned_alloc(CACHE_LINE_SIZE, v->tx_qp_dv.sq.wqe_cnt * sizeof(*v->buffers));
	if (!v->buffers)
		return -ENOMEM;

	/* allocate a shared memory head pointer */
	v->cq_head = simple_alloc(CACHE_LINE_SIZE, 0);
	if (!v->cq_head)
		return -ENOMEM;
	*v->cq_head = 0;

	return 0;
}

int verbs_init_rx_queue(struct verbs_queue_rx *v)
{
	return verbs_refill_rxqueue(v, RQ_NUM_DESC);
}
