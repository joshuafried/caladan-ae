/*
 * rpc.h - an interface for remote procedure calls
 */

#pragma once

#include <base/types.h>
#include <base/atomic.h>
#include <runtime/net.h>
#include <runtime/tcp.h>
#include <runtime/sync.h>

/*
 * Server API
 */

struct srpc_session;

#define SRPC_PORT	8123
#define SRPC_BUF_SIZE	4096

struct srpc_ctx {
	struct srpc_session	*s;
	int			idx;
	uint64_t		id;
	size_t			req_len;
	size_t			resp_len;
	char			req_buf[SRPC_BUF_SIZE];
	char			resp_buf[SRPC_BUF_SIZE];
	bool			drop;
};

typedef void (*srpc_fn_t)(struct srpc_ctx *ctx);

extern int srpc_enable(srpc_fn_t handler);
extern uint64_t srpc_stat_winu_rx();
extern uint64_t srpc_stat_winu_tx();
extern uint64_t srpc_stat_win_tx();
extern uint64_t srpc_stat_req_rx();
extern uint64_t srpc_stat_req_dropped();
extern uint64_t srpc_stat_resp_tx();

/*
 * Client API
 */

#define CRPC_QLEN		16
#define CRPC_MAX_REPLICA	4

struct crpc_ctx {
	size_t			len;
	uint64_t		id;
	uint64_t		*cque;
	char			buf[SRPC_BUF_SIZE];
};

struct crpc_conn {
	tcpconn_t		*c;
	uint32_t		win_avail;
	uint32_t		win_used;
	bool			waiting_winupdate;
	mutex_t			lock;

	/* client-side stats */
	uint64_t		winu_rx_;
	uint64_t		winu_tx_;
	uint64_t		resp_rx_;
	uint64_t		req_tx_;
	uint64_t		win_expired_;
};

struct crpc_session {
	uint64_t		id;
	uint64_t		req_id;
	struct crpc_conn	*c[CRPC_MAX_REPLICA];
	int			num_conns;
	mutex_t			lock;
	waitgroup_t		timer_waiter;
//	bool			waiting_winupdate;
//	uint32_t		win_avail;
//	uint32_t		win_used;
	bool			running;
	bool			demand_sync;
	condvar_t		timer_cv;
	bool			init;

	/* a queue of pending RPC requests */
	uint64_t		head;
	uint64_t		tail;
	struct crpc_ctx		*qreq[CRPC_QLEN];

	/* client-side stats */
//	uint64_t		winu_rx_;
//	uint64_t		winu_tx_;
//	uint64_t		resp_rx_;
//	uint64_t		req_tx_;
//	uint64_t		win_expired_;
	uint64_t		req_dropped_;
};

extern ssize_t crpc_send_one(struct crpc_session *s,
			     const void *buf, size_t len, uint64_t *cque);
extern ssize_t crpc_recv_one(struct crpc_session *s, struct crpc_conn *cc,
			     void *buf, size_t len);

extern int crpc_open(struct netaddr raddr, struct crpc_session **sout, int id);
extern int crpc_add_conn(struct netaddr raddr, struct crpc_session *s);
extern void crpc_close(struct crpc_session *s);

/* client-side stats */
extern uint32_t crpc_win_avail(struct crpc_session *s);
extern uint64_t crpc_stat_winu_rx(struct crpc_session *s);
extern uint64_t crpc_stat_win_expired(struct crpc_session *s);
extern uint64_t crpc_stat_winu_tx(struct crpc_session *s);
extern uint64_t crpc_stat_resp_rx(struct crpc_session *s);
extern uint64_t crpc_stat_req_tx(struct crpc_session *s);
extern uint64_t crpc_stat_req_dropped(struct crpc_session *s);
