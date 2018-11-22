/*
 * verbs.h - Verbs driver for Shenango's network statck
 */


#pragma once

#include <infiniband/verbs.h>

struct mempool;

struct verbs_queue {
	struct ibv_wq *rx_wq; /* single RX work queue */
	struct ibv_qp *tx_qp; /* QP with a single TX queue */
	struct ibv_cq_ex *cq; /* Completion Queue shared by rx_wq and tx_qp */
};

struct verbs_work {
	unsigned int *rx_cnt, *compl_cnt;
	struct rx_net_hdr **rx_bufs;
	struct mbuf **compl_bufs;
};


void verbs_rx_completion(unsigned long completion_data);
int verbs_transmit_one(struct verbs_queue *v, struct mbuf *m);
int verbs_gather_work(struct verbs_work *w, struct verbs_queue *v, unsigned int budget);

/* Initialization functions */
int verbs_init(struct mempool *mp, struct verbs_queue **qs, int nrqs);
int verbs_init_thread(void);
int verbs_init_queue(struct verbs_queue *v);
