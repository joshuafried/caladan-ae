/*
 * verbs.c - common verbs routines for mlx4 and mlx5
 */

#include <base/log.h>
#include <base/stddef.h>

#include <infiniband/verbs.h>

#include "../../defs.h"
#include "../defs.h"
#include "common.h"

#include "verbs.h"
#define PORT_NUM 1

struct ibv_context *context;
struct ibv_pd *pd;
struct ibv_mr *mr_tx;
struct ibv_mr *mr_rx;

/*
 * verbs_create_rss_qps - use ibverbs interface to setup RSS for given set of work queues
 */
int verbs_create_rss_qps(struct ibv_wq **ind_tbl, unsigned int rss_tbl_sz)
{
	struct ibv_flow *eth_flow;
	struct ibv_qp *tcp_qp, *other_qp;
	struct ibv_rwq_ind_table *rwq_ind_table;

	int ret;

	if (!is_power_of_two(rss_tbl_sz))
		return -EINVAL;

	/* Create Receive Work Queue Indirection Table */
	struct ibv_rwq_ind_table_init_attr rwq_attr = {
		.log_ind_tbl_size = __builtin_ctz(rss_tbl_sz),
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
		.rx_hash_fields_mask = IBV_RX_HASH_SRC_IPV4 | IBV_RX_HASH_DST_IPV4 | IBV_RX_HASH_SRC_PORT_TCP | IBV_RX_HASH_DST_PORT_TCP,
	};

	struct ibv_qp_init_attr_ex qp_ex_attr = {
		.qp_type = IBV_QPT_RAW_PACKET,
		.comp_mask =  IBV_QP_INIT_ATTR_IND_TABLE | IBV_QP_INIT_ATTR_RX_HASH | IBV_QP_INIT_ATTR_PD,
		.pd = pd,
		.rwq_ind_tbl = rwq_ind_table,
		.rx_hash_conf = rss_cnf,
	};

	tcp_qp = ibv_create_qp_ex(context, &qp_ex_attr);
	if (!tcp_qp)
		return -errno;

	/* Turn on QP in 2 steps */
	// this only matters for mlx4. mlx5 returns ENOSYS
	struct ibv_qp_attr qp_attr = {0};
	qp_attr.qp_state = IBV_QPS_INIT;
	qp_attr.port_num = 1;
	ret = ibv_modify_qp(tcp_qp, &qp_attr, IBV_QP_STATE | IBV_QP_PORT);
	if (ret && ret != ENOSYS)
		return -ret;

	memset(&qp_attr, 0, sizeof(qp_attr));
	qp_attr.qp_state = IBV_QPS_RTR;
	ret = ibv_modify_qp(tcp_qp, &qp_attr, IBV_QP_STATE);
	if (ret && ret != ENOSYS)
		return -ret;


	rss_cnf.rx_hash_fields_mask = IBV_RX_HASH_SRC_IPV4 | IBV_RX_HASH_DST_IPV4 | IBV_RX_HASH_SRC_PORT_UDP | IBV_RX_HASH_DST_PORT_UDP,
	qp_ex_attr.rx_hash_conf = rss_cnf;
	other_qp = ibv_create_qp_ex(context, &qp_ex_attr);
	if (!other_qp)
		return -errno;

	/* Route TCP packets for our MAC address to the QP with TCP RSS configuration */
	struct raw_eth_flow_attr {
		struct ibv_flow_attr attr;
		struct ibv_flow_spec_eth spec_eth;
		struct ibv_flow_spec_tcp_udp spec_tcp;
	} __attribute__((packed)) flow_attr = {
		.attr = {
			.comp_mask = 0,
			.type = IBV_FLOW_ATTR_NORMAL,
			.size = sizeof(flow_attr),
			.priority = 0,
			.num_of_specs = 2,
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
		},
		.spec_tcp = {
			.type = IBV_FLOW_SPEC_TCP,
			.size = sizeof(struct ibv_flow_spec_tcp_udp),
			.val = {0},
			.mask = {0},
		},
	};
	memcpy(&flow_attr.spec_eth.val.dst_mac, netcfg.mac.addr, 6);
	eth_flow = ibv_create_flow(tcp_qp, &flow_attr.attr);
	if (!eth_flow)
		return -errno;

	/* Route other unicast packets to the QP with the UDP RSS configuration */
	flow_attr.attr.num_of_specs = 1;
	eth_flow = ibv_create_flow(other_qp, &flow_attr.attr);
	if (!eth_flow)
		return -errno;

	/* Route broadcst packets to our set of RX work queues */
	memset(&flow_attr.spec_eth.val.dst_mac, 0xff, 6);
	eth_flow = ibv_create_flow(other_qp, &flow_attr.attr);
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
	eth_flow = ibv_create_flow(other_qp, &mc_attr);
	if (!eth_flow)
		return -errno;

	return 0;
}
