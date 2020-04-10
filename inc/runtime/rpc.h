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
};

typedef void (*srpc_fn_t)(struct srpc_ctx *ctx);

extern int srpc_enable(srpc_fn_t handler);
extern uint64_t srpc_stat_winu_rx();
extern uint64_t srpc_stat_winu_tx();
extern uint64_t srpc_stat_win_tx();
extern uint64_t srpc_stat_req_rx();
extern uint64_t srpc_stat_resp_tx();

/*
 * Client API
 */

#define CRPC_QLEN		16

struct crpc_ctx {
	size_t			len;
	uint64_t		id;
	uint64_t		*cque;
	char			buf[SRPC_BUF_SIZE];
};

struct crpc_session {
	uint64_t		id;
	uint64_t		req_id;
	tcpconn_t		*c;
	mutex_t			lock;
	bool			waiting_winupdate;
	uint64_t		win_timestamp;
	uint32_t		win_avail;
	uint32_t		win_used;
	uint64_t		last_demand;
	int			num_timeout;
	uint64_t		next_resume_time;
	condvar_t		timer_cv;
	bool			running;

	/* a queue of pending RPC requests */
	uint32_t		head;
	uint32_t		tail;
	struct crpc_ctx		*qreq[CRPC_QLEN];

	/* client-side stats */
	uint64_t		winu_rx_;
	uint64_t		winu_tx_;
	uint64_t		resp_rx_;
	uint64_t		req_tx_;
	uint64_t		win_expired_;
	uint64_t		req_dropped_;
	uint64_t		wait_time_;
};

extern ssize_t crpc_send_one(struct crpc_session *s,
			     const void *buf, size_t len, uint64_t *cque);
extern ssize_t crpc_recv_one(struct crpc_session *s,
			     void *buf, size_t len);
extern int crpc_open(struct netaddr raddr, struct crpc_session **sout, int id);
extern void crpc_close(struct crpc_session *s);

static inline uint32_t crpc_win_avail(struct crpc_session *s)
{
	return s->win_avail;
}

/* client-side stats */
static inline uint64_t crpc_stat_winu_rx(struct crpc_session *s)
{
	return s->winu_rx_;
}

static inline uint64_t crpc_stat_winu_tx(struct crpc_session *s)
{
	return s->winu_tx_;
}

static inline uint64_t crpc_stat_resp_rx(struct crpc_session *s)
{
	return s->resp_rx_;
}

static inline uint64_t crpc_stat_req_tx(struct crpc_session *s)
{
	return s->req_tx_;
}

static inline uint64_t crpc_stat_win_expired(struct crpc_session *s)
{
	return s->win_expired_;
}

static inline uint64_t crpc_stat_req_dropped(struct crpc_session *s)
{
	return s->req_dropped_;
}

static inline uint64_t crpc_stat_wait_time(struct crpc_session *s)
{
	return s->wait_time_;
}

/**
 * crpc_is_busy - is the session busy (unable to accept requests right now)
 * @s: the session to check
 */
static inline bool crpc_is_busy(struct crpc_session *s)
{
	return s->win_avail <= s->win_used;
}
