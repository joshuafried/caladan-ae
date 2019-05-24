#pragma once

/* ibverbs defs */
extern struct ibv_context *context;
extern struct ibv_pd *pd;
extern struct ibv_mr *mr_tx;
extern struct ibv_mr *mr_rx;

struct ibv_wq;
extern int verbs_create_rss_qps(struct ibv_wq **ind_tbl, unsigned int rss_tbl_sz);
