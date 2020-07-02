/*
 * RPC client-side support
 */

#include <base/time.h>
#include <base/stddef.h>
#include <base/list.h>
#include <base/log.h>
#include <base/atomic.h>
#include <runtime/rpc.h>
#include <runtime/smalloc.h>
#include <runtime/sync.h>

#include "util.h"
#include "proto.h"

#define CRPC_MAX_CLIENT_DELAY_US	100

#define CRPC_TRACK_FLOW			false
#define CRPC_TRACK_FLOW_ID		1

/**
 * crpc_send_winupdate - send WINUPDATE message to update window size
 * @s: the RPC session to update the window
 *
 * On success, returns 0. On failure returns standard socket errors (< 0)
 */
ssize_t crpc_send_winupdate(struct crpc_session *s, struct crpc_conn *cc)
{
        struct crpc_hdr chdr;
        ssize_t ret;

	assert_mutex_held(&cc->lock);
	assert_mutex_held(&s->lock);

	/* construct the client header */
	chdr.magic = RPC_REQ_MAGIC;
	chdr.op = RPC_OP_WINUPDATE;
	chdr.id = 0;
	chdr.len = 0;
	chdr.demand = s->head - s->tail;
	chdr.sync = s->demand_sync;

	/* send the request */
	ret = tcp_write_full(cc->c, &chdr, sizeof(chdr));
	if (unlikely(ret < 0))
		return ret;

	assert(ret == sizeof(chdr));
	cc->winu_tx_++;

#if CRPC_TRACK_FLOW
	if (s->id == CRPC_TRACK_FLOW_ID) {
		printf("[%lu] <=== winupdate: demand = %lu, win = %u/%u\n",
		       microtime(), chdr.demand, cc->win_used, cc->win_avail);
	}
#endif
	return 0;
}

static ssize_t crpc_send_request_vector(struct crpc_session *s, struct crpc_conn *cc)
{
	struct crpc_hdr chdr[CRPC_QLEN];
	struct iovec v[CRPC_QLEN * 2];
	int nriov = 0;
	int nrhdr = 0;
	ssize_t ret;
	uint64_t now = microtime();

	assert_mutex_held(&cc->lock);
	assert_mutex_held(&s->lock);

	if (s->head == s->tail || cc->win_used >= cc->win_avail)
		return 0;

	while (s->head != s->tail && cc->win_used < cc->win_avail) {
		struct crpc_ctx *c = s->qreq[s->tail++ % CRPC_QLEN];

		chdr[nrhdr].magic = RPC_REQ_MAGIC;
		chdr[nrhdr].op = RPC_OP_CALL;
		chdr[nrhdr].id = c->id;
		chdr[nrhdr].len = c->len;
		chdr[nrhdr].demand = s->head - s->tail;
		chdr[nrhdr].sync = s->demand_sync;

		v[nriov].iov_base = &chdr[nrhdr];
		v[nriov].iov_len = sizeof(struct crpc_hdr);
		nrhdr++;
		nriov++;

		if (c->len > 0) {
			v[nriov].iov_base = c->buf;
			v[nriov++].iov_len = c->len;
		}
		*c->cque = now - *c->cque;

		cc->win_used++;
	}

	if (s->head == s->tail) {
		s->head = 0;
		s->tail = 0;
	}

	ret = tcp_writev_full(cc->c, v, nriov);

	cc->req_tx_ += nrhdr;

#if CRPC_TRACK_FLOW
	if (s->id == CRPC_TRACK_FLOW_ID) {
		printf("[%lu] <=== request (%d): qlen=%d win=%d/%d\n",
		       microtime(), nrhdr, s->head-s->tail, cc->win_used, cc->win_avail);
	}
#endif

	if (unlikely(ret < 0))
		return ret;
	return 0;
}

static ssize_t crpc_send_raw(struct crpc_session *s, struct crpc_conn *cc,
			     const void *buf, size_t len,
			     uint64_t id)
{
	struct iovec vec[2];
	struct crpc_hdr chdr;
	ssize_t ret;

	/* initialize the header */
	chdr.magic = RPC_REQ_MAGIC;
	chdr.op = RPC_OP_CALL;
	chdr.id = id;
	chdr.len = len;
	chdr.demand = s->head - s->tail;
	chdr.sync = s->demand_sync;

	/* initialize the SG vector */
	vec[0].iov_base = &chdr;
	vec[0].iov_len = sizeof(chdr);
	vec[1].iov_base = (void *)buf;
	vec[1].iov_len = len;

	/* send the request */
	ret = tcp_writev_full(cc->c, vec, 2);
	if (unlikely(ret < 0))
		return ret;
	assert(ret == sizeof(chdr) + len);
	cc->req_tx_++;

#if CRPC_TRACK_FLOW
	if (s->id == CRPC_TRACK_FLOW_ID) {
		printf("[%lu] <=== request: id=%lu, demand = %lu, win = %u/%u\n",
		       microtime(), chdr.id, chdr.demand, cc->win_used, cc->win_avail);
	}
#endif
	return len;
}

static void crpc_drain_queue(struct crpc_session *s, struct crpc_conn *cc)
{
	ssize_t ret;
	int pos;
	struct crpc_ctx *c;
	uint64_t now = microtime();

	assert_mutex_held(&cc->lock);
	assert_mutex_held(&s->lock);

	if (s->head == s->tail || cc->waiting_winupdate)
		return;
/*
	if (s->win_avail == 0 && s->demand_sync) {
		s->waiting_winupdate = true;
		crpc_send_winupdate(s);
		return;
	}
*/
	s->req_dropped_++;
	while (s->head != s->tail) {
		pos = s->tail % CRPC_QLEN;
		c = s->qreq[pos];
		if (now - *c->cque <= CRPC_MAX_CLIENT_DELAY_US)
			break;

		s->tail++;

#if CRPC_TRACK_FLOW
		if (s->id == CRPC_TRACK_FLOW_ID) {
			printf("[%lu] request dropped: id=%lu, qlen = %d\n",
			       now, c->id, s->head - s->tail);
		}
#endif
	}

	crpc_send_request_vector(s, cc);
}

static bool crpc_enqueue_one(struct crpc_session *s,
			     const void *buf, size_t len, uint64_t *cque)
{
	int pos;
	struct crpc_ctx *c;
	uint64_t now = microtime();
	int num_drops = 0;
	int i;

	assert_mutex_held(&s->lock);

	/* if the queue is full, drop tail */
	if (s->head >= s->tail + CRPC_QLEN) {
		s->tail++;
		s->req_dropped_++;
#if CRPC_TRACK_FLOW
		if (s->id == CRPC_TRACK_FLOW_ID) {
			printf("[%lu] queue full. drop the request\n",
			       now);
		}
#endif
	}

	pos = s->head++ % CRPC_QLEN;
	c = s->qreq[pos];
	*cque = now;
	memcpy(c->buf, buf, len);
	c->id = s->req_id++;
	c->len = len;
	c->cque = cque;

#if CRPC_TRACK_FLOW
	if (s->id == CRPC_TRACK_FLOW_ID) {
		printf("[%lu] request enqueued: id=%lu, qlen = %d\n",
		       now, c->id, s->head - s->tail);
	}
#endif

	// very first message
	if (!s->init) {
		for(i = 0; i < s->num_conns; ++i) {
			struct crpc_conn *cc = s->c[i];
			mutex_lock(&cc->lock);
			crpc_send_winupdate(s, cc);
			cc->waiting_winupdate = true;
			mutex_unlock(&cc->lock);
		}
		s->init = true;
	}

	// if queue become non-empty, start expiration loop
	if (s->head - s->tail == 1)
		condvar_signal(&s->timer_cv);

	return true;
}

/**
 * crpc_send_one - sends one RPC request
 * @s: the RPC session to send to
 * @ident: the unique identifier associated with the request
 * @buf: the payload buffer to send
 * @len: the length of @buf (up to SRPC_BUF_SIZE)
 *
 * WARNING: This function could block.
 *
 * On success, returns the length sent in bytes (i.e. @len). On failure,
 * returns -ENOBUFS if the window is full. Otherwise, returns standard socket
 * errors (< 0).
 */
ssize_t crpc_send_one(struct crpc_session *s,
		      const void *buf, size_t len, uint64_t *cque)
{
	ssize_t ret;
	uint64_t now = microtime();
	int i;
	struct crpc_conn *cc;

	/* implementation is currently limited to a maximum payload size */
	if (unlikely(len > SRPC_BUF_SIZE))
		return -E2BIG;

	mutex_lock(&s->lock);
	/* hot path, just send */
	if (s->head == s->tail) {
		for(i = 0; i < s->num_conns; ++i) {
			cc = s->c[i];
			mutex_lock(&cc->lock);
			if (cc->win_used < cc->win_avail) {
				cc->win_used++;
				*cque = 0;
				ret = crpc_send_raw(s, cc, buf, len, s->req_id++);
				mutex_unlock(&cc->lock);
				mutex_unlock(&s->lock);
				return ret;
			}
			mutex_unlock(&cc->lock);
		}
	}

	/* cold path, enqueue request and drain the queue */
	crpc_enqueue_one(s, buf, len, cque);
	for(i = 0; i < s->num_conns; ++i) {
		cc = s->c[i];
		mutex_lock(&cc->lock);
		crpc_drain_queue(s, cc);
		mutex_unlock(&cc->lock);
	}
	mutex_unlock(&s->lock);

	return len;
}

/**
 * crpc_recv_one - receive one RPC request
 * @s: the RPC session to receive from
 * @buf: a buffer to store the received payload
 * @len: the length of @buf (up to SRPC_BUF_SIZE)
 *
 * WARNING: This function could block.
 *
 * On success, returns the length received in bytes. On failure returns standard
 * socket errors (<= 0).
 */
ssize_t crpc_recv_one(struct crpc_session *s, struct crpc_conn *cc, void *buf, size_t len)
{
	struct srpc_hdr shdr;
	ssize_t ret;
	uint64_t now = microtime();

again:
	/* read the server header */
	ret = tcp_read_full(cc->c, &shdr, sizeof(shdr));
	if (unlikely(ret <= 0))
		return ret;
	assert(ret == sizeof(shdr));

	/* parse the server header */
	if (unlikely(shdr.magic != RPC_RESP_MAGIC)) {
		log_warn("crpc: got invalid magic %x", shdr.magic);
		return -EINVAL;
	}
	if (unlikely(shdr.len > MIN(SRPC_BUF_SIZE, len))) {
		log_warn("crpc: request len %ld too large (limit %ld)",
			 shdr.len, MIN(SRPC_BUF_SIZE, len));
		return -EINVAL;
	}

	switch (shdr.op) {
	case RPC_OP_CALL:
		/* read the payload */
		if (shdr.len > 0) {
			ret = tcp_read_full(cc->c, buf, shdr.len);
			if (unlikely(ret <= 0))
				return ret;
			assert(ret == shdr.len);
			cc->resp_rx_++;
		}

		/* update the window */
		mutex_lock(&s->lock);
		mutex_lock(&cc->lock);
		assert(cc->win_used > 0);
		cc->win_used--;
		cc->win_avail = shdr.win;
		cc->waiting_winupdate = false;

#if CRPC_TRACK_FLOW
		if (s->id == CRPC_TRACK_FLOW_ID) {
			printf("[%lu] ===> response: id=%lu, shdr.win=%lu, win=%u/%u\n",
			       now, shdr.id, shdr.win, cc->win_used, cc->win_avail);
		}
#endif

		if (cc->win_avail > 0) {
			crpc_drain_queue(s, cc);
		}
		mutex_unlock(&cc->lock);
		mutex_unlock(&s->lock);

		if (shdr.len == 0)
			goto again;

		break;
	case RPC_OP_WINUPDATE:
		if (unlikely(shdr.len != 0)) {
			log_warn("crpc: winupdate has nonzero len");
			return -EINVAL;
		}
		assert(shdr.len == 0);

		/* update the window */
		mutex_lock(&s->lock);
		mutex_lock(&cc->lock);
		cc->win_avail = shdr.win;
		cc->waiting_winupdate = false;

#if CRPC_TRACK_FLOW
		if (s->id == CRPC_TRACK_FLOW_ID) {
			printf("[%lu] ===> Winupdate: shdr.win=%lu, win=%u/%u\n",
			       microtime(), shdr.win, cc->win_used, cc->win_avail);
		}
#endif

		if (cc->win_avail > 0) {
			crpc_drain_queue(s, cc);
		}
		cc->winu_rx_++;
		mutex_unlock(&cc->lock);
		mutex_unlock(&s->lock);

		goto again;
	default:
		log_warn("crpc: got invalid op %d", shdr.op);
		return -EINVAL;
	}

	return shdr.len;
}

static void crpc_timer(void *arg)
{
	struct crpc_session *s = (struct crpc_session *)arg;
	uint64_t now;
	int pos;
	struct crpc_ctx *c;
	int num_drops;

	mutex_lock(&s->lock);
	while(true) {
		while (s->running && s->head == s->tail)
			condvar_wait(&s->timer_cv, &s->lock);

		if (!s->running)
			goto done;

		num_drops = 0;
		now = microtime();

		// Drop requests if expired
		while (s->head != s->tail) {
			pos = s->tail % CRPC_QLEN;
			c = s->qreq[pos];
			if (now - *c->cque <= CRPC_MAX_CLIENT_DELAY_US)
				break;

			s->tail++;
			s->req_dropped_++;
			num_drops++;
#if CRPC_TRACK_FLOW
			if (s->id == CRPC_TRACK_FLOW_ID) {
				printf("[%lu] request dropped: id=%lu, qlen = %d\n",
				       now, c->id, s->head - s->tail);
			}
#endif
		}

		// If queue becomes empty
		if (s->head == s->tail) {
/*
			if (num_drops > 0 && s->demand_sync) {
				s->waiting_winupdate = false;
				crpc_send_winupdate(s);
			}
*/
			continue;
		}

		// caculate next wake up time
		pos = (s->head - 1) % CRPC_QLEN;
		c = s->qreq[pos];
		mutex_unlock(&s->lock);
		timer_sleep_until(*c->cque + CRPC_MAX_CLIENT_DELAY_US);
		mutex_lock(&s->lock);
	}
done:
	mutex_unlock(&s->lock);
	waitgroup_done(&s->timer_waiter);
}

/**
 * crpc_open - creates an RPC session
 * @raddr: the remote address to connect to (port must be SRPC_PORT)
 * @sout: the connection session that was created
 *
 * WARNING: This function could block.
 *
 * Returns 0 if successful.
 */
int crpc_open(struct netaddr raddr, struct crpc_session **sout, int id)
{
	struct netaddr laddr;
	struct crpc_session *s;
	struct crpc_conn *cc;
	tcpconn_t *c;
	int i, ret;

	/* set up ephemeral IP and port */
	laddr.ip = 0;
	laddr.port = 0;

	if (raddr.port != SRPC_PORT)
		return -EINVAL;

	/* establish connection */
	ret = tcp_dial(laddr, raddr, &c);
	if (ret)
		return ret;

	s = smalloc(sizeof(*s));
	if (!s) {
		tcp_close(c);
		return -ENOMEM;
	}
	memset(s, 0, sizeof(*s));

	cc = smalloc(sizeof(*cc));
	if (!cc) {
		tcp_close(c);
		sfree(s);
		return -ENOMEM;
	}
	memset(cc, 0, sizeof(*cc));

	/* initialize connection */
	mutex_init(&cc->lock);
	cc->c = c;

	/* initialize queue */
	for (i = 0; i < CRPC_QLEN; ++i) {
		s->qreq[i] = smalloc(sizeof(struct crpc_ctx));
		if (!s->qreq[i])
			goto fail;
	}

	/* initialize session */
	s->c[0] = cc;
	mutex_init(&s->lock);
	condvar_init(&s->timer_cv);
	waitgroup_init(&s->timer_waiter);
	waitgroup_add(&s->timer_waiter, 1);
	s->running = true;
	s->demand_sync = false;
	if (id != -1)
		s->id = id;
	s->req_id = 1;
	s->num_conns = 1;
	*sout = s;

	ret = thread_spawn(crpc_timer, s);
	BUG_ON(ret);

	return 0;

fail:
	tcp_close(c);
	for (i = i - 1; i >= 0; i--)
		sfree(s->qreq[i]);
	sfree(cc);
	sfree(s);
	return -ENOMEM;
}

/**
 *
 * crpc_add_conn - add a connection to a replica to a session
 * @raddr: the remote address to connect to
 * @s: the session to add replica
 *
 * Returns 0 if successful.
 */
int crpc_add_conn(struct netaddr raddr, struct crpc_session *s)
{
	struct netaddr laddr;
	tcpconn_t *c;
	struct crpc_conn *cc;
	int i, ret;

	/* set up ephemeral IP and port */
	laddr.ip = 0;
	laddr.port = 0;

	if (s->num_conns >= CRPC_MAX_REPLICA)
		return -ENOMEM;

	if (raddr.port != SRPC_PORT)
		return -EINVAL;

	/* establish connection */
	ret = tcp_dial(laddr, raddr, &c);
	if (ret)
		return ret;

	cc = smalloc(sizeof(*cc));
	if (!cc) {
		tcp_close(c);
		return -ENOMEM;
	}
	memset(cc, 0, sizeof(*cc));

	/* initialize connection */
	mutex_init(&cc->lock);
	cc->c = c;

	/* add connection to session*/
	s->c[s->num_conns++] = cc;

	return 0;
}

/**
 * crpc_close - closes an RPC session
 * @s: the session to close
 *
 * WARNING: This function could block.
 */
void crpc_close(struct crpc_session *s)
{
	int i;
	struct crpc_conn *cc;

	mutex_lock(&s->lock);
	s->running = false;
	condvar_signal(&s->timer_cv);
	mutex_unlock(&s->lock);

	waitgroup_wait(&s->timer_waiter);

	for(i = 0; i < s->num_conns; ++i) {
		cc = s->c[i];
		tcp_close(cc->c);
		sfree(cc);
	}

	for(i = 0; i < CRPC_QLEN; ++i)
		sfree(s->qreq[i]);
	sfree(s);
}

/* client-side stats */
uint32_t crpc_win_avail(struct crpc_session *s)
{
	uint32_t sum = 0;
	int i;

	for(i = 0; i < s->num_conns; ++i)
		sum += s->c[i]->win_avail;

	return sum;
}

uint64_t crpc_stat_win_expired(struct crpc_session *s)
{
	uint64_t sum = 0;
	int i;

	for(i = 0; i < s->num_conns; ++i)
		sum += s->c[i]->win_expired_;

	return sum;
}

uint64_t crpc_stat_winu_rx(struct crpc_session *s)
{
	uint64_t sum = 0;
	int i;

	for(i = 0; i < s->num_conns; ++i)
		sum += s->c[i]->winu_rx_;

	return sum;
}

uint64_t crpc_stat_winu_tx(struct crpc_session *s)
{
	uint64_t sum = 0;
	int i;

	for(i = 0; i < s->num_conns; ++i)
		sum += s->c[i]->winu_tx_;

	return sum;
}

uint64_t crpc_stat_resp_rx(struct crpc_session *s)
{
	uint64_t sum = 0;
	int i;

	for(i = 0; i < s->num_conns; ++i)
		sum += s->c[i]->resp_rx_;

	return sum;
}

uint64_t crpc_stat_req_tx(struct crpc_session *s)
{
	uint64_t sum = 0;
	int i;

	for(i = 0; i < s->num_conns; ++i)
		sum += s->c[i]->req_tx_;

	return sum;
}

uint64_t crpc_stat_req_dropped(struct crpc_session *s)
{
	return s->req_dropped_;
}
