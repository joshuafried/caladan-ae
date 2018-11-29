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
#include <net/mbuf.h>
#include <base/slab.h>

#include "defs.h"
#include "verbs.h"

#define RQ_NUM_DESC (max(8, 128 / VERB_QUEUES_PER_CORE))
#define SQ_NUM_DESC 128
#define PORT_NUM 1

#define RX_BUF_BOOL_SZ(nrqs) \
 (align_up(nrqs * RQ_NUM_DESC * 8 * MBUF_DEFAULT_LEN, PGSIZE_2MB))
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
	unsigned char *buf;

	preempt_disable();
	buf = (unsigned char *)completion_data - RX_BUF_RESERVED;
	tcache_free(&perthread_get(verbs_buf_pt), buf);
	preempt_enable();
}

static inline unsigned char *verbs_rx_alloc_buf(void)
{
	unsigned char *buf;

	assert(!preempt_enabled());
	buf = tcache_alloc(&perthread_get(verbs_buf_pt));
	if (unlikely(!buf))
		return NULL;

	return buf + RX_BUF_RESERVED;
}

/*
 * verbs_queue_is_empty - check if there is a pending completion event
 */
bool verbs_queue_is_empty(struct verbs_custom_cq *cq)
{
	struct mlx5_cqe64 *cqes = cq->dvcq.buf;
	struct mlx5_cqe64 *cqe = &cqes[cq->cq_idx & (cq->dvcq.cqe_cnt - 1)];
	uint16_t parity = cq->cq_idx & cq->dvcq.cqe_cnt;
	uint8_t op_own = ACCESS_ONCE(cqe->op_own);
	uint8_t op_owner = op_own & MLX5_CQE_OWNER_MASK;
	uint8_t op_code = (op_own & 0xf0) >> 4;

	return op_owner != !!parity || op_code == MLX5_CQE_INVALID;
}

/*
 * verbs_refill_rxqueue - replenish RX queue with nrdesc bufs
 * @rx_wq: queue to refill
 * @nrdesc: number of buffers to fill
 *
 * returns 0 on success, errno on error
 */
static int verbs_refill_rxqueue(struct ibv_wq *rx_wq, int nrdesc)
{
	unsigned int i;
	unsigned char *buf;
	struct ibv_sge sg_entry[nrdesc];
	struct ibv_recv_wr *bad_wr, wr[nrdesc];

	for (i = 0; i < nrdesc; i++) {
		buf = verbs_rx_alloc_buf();
		BUG_ON(!buf);
		sg_entry[i].addr = (uint64_t)buf;
		sg_entry[i].length = MBUF_DEFAULT_LEN - RX_BUF_RESERVED;
		sg_entry[i].lkey = mr_rx->lkey;

		wr[i].num_sge = 1;
		wr[i].sg_list = &sg_entry[i];
		wr[i].next = (i < nrdesc - 1) ? &wr[i+1] : NULL;
		wr[i].wr_id = (uint64_t)buf;
	}

	return ibv_post_wq_recv(rx_wq, wr, &bad_wr);
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
	struct ibv_sge sg_entry = {
		.addr = (uint64_t)mbuf_data(m),
		.length = mbuf_length(m),
		.lkey = mr_tx->lkey,
	};

	struct ibv_send_wr *bad_wr, wr = {
		.num_sge = 1,
		.sg_list = &sg_entry,
		.next = NULL,
		.wr_id = (uint64_t)m,
		.send_flags = 0,
		.opcode = IBV_WR_SEND,
	};

	if (m->txflags & OLFLAG_IP_CHKSUM)
		wr.send_flags |= IBV_SEND_IP_CSUM;

	return ibv_post_send(v->tx_qp, &wr, &bad_wr);
}

int verbs_gather_rx(struct rx_net_hdr **hdrs, struct verbs_queue_rx *v, unsigned int budget)
{
	struct rx_net_hdr *hdr;
	int rx_cnt = 0;

	if (verbs_queue_is_empty(v->raw_cq))
		return 0;

#if 0
	int ret;
	struct ibv_poll_cq_attr attr = { 0 };

	ret = ibv_start_poll(v->rx_cq, &attr);
	if (ret == ENOENT)
		return 0;
	BUG_ON(ret);

	while (budget--) {
		v->raw_cq->cq_idx++;
		BUG_ON(v->rx_cq->status != IBV_WC_SUCCESS);
		BUG_ON(!(ibv_wc_read_opcode(v->rx_cq) & IBV_WC_RECV));
		hdr = (struct rx_net_hdr *)((char *)v->rx_cq->wr_id - sizeof(*hdr));
		hdr->completion_data = v->rx_cq->wr_id;
		hdr->len = ibv_wc_read_byte_len(v->rx_cq);
		hdr->csum_type = (ibv_wc_read_wc_flags(v->rx_cq) & IBV_WC_IP_CSUM_OK) ? CHECKSUM_TYPE_UNNECESSARY : CHECKSUM_TYPE_NEEDED;
		hdrs[rx_cnt++] = hdr;
		if (budget) {
			ret = ibv_next_poll(v->rx_cq);
			if (ret == ENOENT)
				break;
			BUG_ON(ret);
		}
	}

	ibv_end_poll(v->rx_cq);

#else
	struct ibv_wc wc[budget];
	int msgs;

	msgs = ibv_poll_cq(ibv_cq_ex_to_cq(v->rx_cq), budget, wc);
	BUG_ON(msgs < 0);
	for (int i = 0; i < msgs; i++) {
		v->raw_cq->cq_idx++;
		BUG_ON(wc[i].status != IBV_WC_SUCCESS);
		BUG_ON(!(wc[i].opcode & IBV_WC_RECV));
		hdr = (struct rx_net_hdr *)((char *)wc[i].wr_id - sizeof(*hdr));
		hdr->completion_data = wc[i].wr_id;
		hdr->len = wc[i].byte_len;
		hdr->csum_type = (wc[i].wc_flags & IBV_WC_IP_CSUM_OK) ? CHECKSUM_TYPE_UNNECESSARY : CHECKSUM_TYPE_NEEDED;
		hdrs[rx_cnt++] = hdr;
	}
#endif

	if (rx_cnt)
		BUG_ON(verbs_refill_rxqueue(v->rx_wq, rx_cnt));

	return rx_cnt;

}

/*
 * verbs_gather_work - collect up to budget received packets and completions
 */
int verbs_gather_completions(struct mbuf **mbufs, struct verbs_queue_tx *v, unsigned int budget)
{
	int compl_cnt = 0;

	if (verbs_queue_is_empty(v->raw_cq))
		return 0;

#if 0
	int ret;
	struct ibv_poll_cq_attr attr = { 0 };

	ret = ibv_start_poll(v->tx_cq, &attr);
	if (ret == ENOENT)
		return 0;
	BUG_ON(ret);

	while (budget--) {
		v->raw_cq->cq_idx++;
		BUG_ON(v->tx_cq->status != IBV_WC_SUCCESS);
		BUG_ON(ibv_wc_read_opcode(v->tx_cq) & IBV_WC_RECV);
		mbufs[compl_cnt++] = (struct mbuf *)v->tx_cq->wr_id;
		if (budget) {
			ret = ibv_next_poll(v->tx_cq);
			if (ret == ENOENT)
				break;
			BUG_ON(ret);
		}
	}

	ibv_end_poll(v->tx_cq);
#else
	struct ibv_wc wc[budget];
	int msgs;

	msgs = ibv_poll_cq(ibv_cq_ex_to_cq(v->tx_cq), budget, wc);
	BUG_ON(msgs < 0);
	for (int i = 0; i < msgs; i++) {
		v->raw_cq->cq_idx++;
		BUG_ON(wc[i].status != IBV_WC_SUCCESS);
		BUG_ON(wc[i].opcode & IBV_WC_RECV);
		mbufs[compl_cnt++] = (struct mbuf *)wc[i].wr_id;
	}

#endif

	return compl_cnt;
}

/*
 * simple_alloc - simple memory allocator for internal MLX5 structures
 * TODO: replace me with SHM implementation.
 */
static void *simple_alloc(size_t size, void *priv_data)
{
	static unsigned char *mem;
	static size_t nxt;

	if (!mem) {
		mem = mem_map_anom(NULL, 4 * PGSIZE_2MB, PGSIZE_2MB, 0);
		BUG_ON(mem == MAP_FAILED);
	}

	BUG_ON(nxt + size > 4 * PGSIZE_2MB);
	void *p = mem + nxt;
	nxt += size;
	return p;
}

static void simple_free(void *ptr, void *priv_data) {}

static struct mlx5dv_ctx_allocators dv_allocators = {
	.alloc = simple_alloc,
	.free = simple_free,
};

/*
 * verbs_init - intialize all TX/RX queues
 */
int verbs_init(struct mempool *tx_mp, struct verbs_queue_rx **qs, int nrqs)
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

	setenv("MLX5_SINGLE_THREADED", "1", true);
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
			     PGSIZE_2MB, MBUF_DEFAULT_LEN);
	if (ret)
		return ret;

	verbs_buf_tcache = mempool_create_tcache(&verbs_buf_mp,
		"verbs_rx_bufs", TCACHE_DEFAULT_MAG_SIZE);
	if (!verbs_buf_tcache)
		return -ENOMEM;

	mr_rx = ibv_reg_mr(pd, rx_buf, RX_BUF_BOOL_SZ(nrqs), IBV_ACCESS_LOCAL_WRITE);
	if (!mr_rx) {
		log_err("verbs_init: Couldn't register mr");
		return -1;
	}

	for (i = 0; i < nrqs; i++) {
		struct verbs_queue_rx *v = qs[i];

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
		BUG_ON(!v->rx_cq);

		/* Create the work queue for RX */
		struct ibv_wq_init_attr wq_init_attr = {
			.wq_type = IBV_WQT_RQ,
			.max_wr = RQ_NUM_DESC,
			.max_sge = 1,
			.pd = pd,
			.cq = ibv_cq_ex_to_cq(v->rx_cq),
			.comp_mask = 0,
			.create_flags = 0,
		};
		struct mlx5dv_wq_init_attr dv_wq_attr = {
			.comp_mask = 0,
		};
		v->rx_wq = mlx5dv_create_wq(context, &wq_init_attr, &dv_wq_attr);
		BUG_ON(!v->rx_wq);
		ind_tbl[i] = v->rx_wq;

		if (wq_init_attr.max_wr != RQ_NUM_DESC)
			log_warn("Ring size is larger than anticipated");

		/* Set the WQ state to ready */
		struct ibv_wq_attr wq_attr;
		memset(&wq_attr, 0, sizeof(wq_attr));
		wq_attr.attr_mask = IBV_WQ_ATTR_STATE;
		wq_attr.wq_state = IBV_WQS_RDY;
		BUG_ON(ibv_modify_wq(v->rx_wq, &wq_attr));

		/* Directly expose completion queue */
		v->raw_cq = simple_alloc(align_up(sizeof(*v->raw_cq), PGSIZE_4KB), 0);
		struct mlx5dv_obj obj = {
			.cq = {
				.in = ibv_cq_ex_to_cq(v->rx_cq),
				.out = &v->raw_cq->dvcq,
			},
		};
		BUG_ON(mlx5dv_init_obj(&obj, MLX5DV_OBJ_CQ));
		v->raw_cq->cq_idx = 0;
	}

	/* Create Receive Work Queue Indirection Table */
	struct ibv_rwq_ind_table_init_attr rwq_attr = {
		.log_ind_tbl_size = __builtin_ctz(nrqs),
		.ind_tbl = ind_tbl,
		.comp_mask = 0,
	};
	rwq_ind_table = ibv_create_rwq_ind_table(context, &rwq_attr);
	BUG_ON(!rwq_ind_table);

	/* Populate random hash key, todo - decide what to do here */
	static char key[40];
	fd = open("/dev/urandom", O_RDONLY);
	BUG_ON(fd < 0);
	BUG_ON(read(fd, key, 40) != 40);
	close(fd);

	/* Create the main RX QP using the indirection table */
	struct ibv_rx_hash_conf rss_cnf = {
		.rx_hash_function = IBV_RX_HASH_FUNC_TOEPLITZ,
		.rx_hash_key_len = 40,
		.rx_hash_key = (void *)&key[0],
		.rx_hash_fields_mask = IBV_RX_HASH_SRC_IPV4 | IBV_RX_HASH_DST_IPV4 | IBV_RX_HASH_SRC_PORT_UDP | IBV_RX_HASH_DST_PORT_UDP,
	};
	struct ibv_qp_init_attr_ex qp_ex_attr = {
		.qp_type = IBV_QPT_RAW_PACKET,
		.comp_mask =  IBV_QP_INIT_ATTR_IND_TABLE | IBV_QP_INIT_ATTR_RX_HASH | IBV_QP_INIT_ATTR_PD,
		.pd = pd,
		.rwq_ind_tbl = rwq_ind_table,
		.rx_hash_conf = rss_cnf,
	};

	qp = ibv_create_qp_ex(context, &qp_ex_attr);
	BUG_ON(!qp);

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
	BUG_ON(!eth_flow);

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
	BUG_ON(!eth_flow);

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
	BUG_ON(!v->tx_cq);

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
	BUG_ON(!v->tx_qp);

	/* Turn on TX QP in 3 steps */
	struct ibv_qp_attr qp_attr;
	memset(&qp_attr, 0, sizeof(qp_attr));
	qp_attr.qp_state = IBV_QPS_INIT;
	qp_attr.port_num = 1;
	BUG_ON(ibv_modify_qp(v->tx_qp, &qp_attr, IBV_QP_STATE | IBV_QP_PORT));

	memset(&qp_attr, 0, sizeof(qp_attr));
	qp_attr.qp_state = IBV_QPS_RTR;
	BUG_ON(ibv_modify_qp(v->tx_qp, &qp_attr, IBV_QP_STATE));

	memset(&qp_attr, 0, sizeof(qp_attr));
	qp_attr.qp_state = IBV_QPS_RTS;
	BUG_ON(ibv_modify_qp(v->tx_qp, &qp_attr, IBV_QP_STATE));

	v->raw_cq = simple_alloc(align_up(sizeof(*v->raw_cq), PGSIZE_4KB), 0);
	struct mlx5dv_obj obj = {
		.cq = {
			.in = ibv_cq_ex_to_cq(v->tx_cq),
			.out = &v->raw_cq->dvcq,
		},
	};
	BUG_ON(mlx5dv_init_obj(&obj, MLX5DV_OBJ_CQ));
	v->raw_cq->cq_idx = 0;

	return 0;
}

int verbs_init_rx_queue(struct verbs_queue_rx *v)
{
	return verbs_refill_rxqueue(v->rx_wq, RQ_NUM_DESC);
}
