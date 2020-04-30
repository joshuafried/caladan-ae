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
ssize_t crpc_send_winupdate(struct crpc_session *s)
{
        struct crpc_hdr chdr;
        ssize_t ret;

	assert_mutex_held(&s->lock);

	/* construct the client header */
	chdr.magic = RPC_REQ_MAGIC;
	chdr.op = RPC_OP_WINUPDATE;
	chdr.id = 0;
	chdr.len = 0;
	chdr.demand = s->head - s->tail;
	chdr.sync = s->demand_sync;

	/* send the request */
	ret = tcp_write_full(s->c, &chdr, sizeof(chdr));
	if (unlikely(ret < 0))
		return ret;

	assert(ret == sizeof(chdr));
	s->winu_tx_++;

#if CRPC_TRACK_FLOW
	if (s->id == CRPC_TRACK_FLOW_ID) {
		printf("[%lu] <=== winupdate: demand = %lu, win = %u/%u\n",
		       microtime(), chdr.demand, s->win_used, s->win_avail);
	}
#endif
	return 0;
}

static ssize_t crpc_send_raw(struct crpc_session *s,
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
	ret = tcp_writev_full(s->c, vec, 2);
	if (unlikely(ret < 0))
		return ret;
	assert(ret == sizeof(chdr) + len);
	s->req_tx_++;

#if CRPC_TRACK_FLOW
	if (s->id == CRPC_TRACK_FLOW_ID) {
		printf("[%lu] <=== request: id=%lu, demand = %lu, win = %u/%u\n",
		       microtime(), chdr.id, chdr.demand, s->win_used, s->win_avail);
	}
#endif
	return len;
}

static void crpc_drain_queue(struct crpc_session *s)
{
	ssize_t ret;
	int pos;
	struct crpc_ctx *c;
	uint64_t now = microtime();

	assert_mutex_held(&s->lock);

	if (s->head == s->tail)
		return;

	if (s->waiting_winupdate) {
		return;
	}

	if (s->win_avail == 0 && s->demand_sync) {
		s->waiting_winupdate = true;
		crpc_send_winupdate(s);
		return;
	}

	/* try to drain queued requests: FIFO */
	while (s->head != s->tail) {
		if (s->win_used >= s->win_avail)
			break;

		pos = s->tail++ % CRPC_QLEN;
		c = s->qreq[pos];
		*c->cque = now - *c->cque;
		ret = crpc_send_raw(s, c->buf, c->len, c->id);
		if (ret < 0)
			break;
		assert(ret == c->len);
		s->win_used++;
	}
}

static bool crpc_enqueue_one(struct crpc_session *s,
			     const void *buf, size_t len, uint64_t *cque)
{
	int pos;
	struct crpc_ctx *c;
	uint64_t now = microtime();
	int num_drops = 0;

	assert_mutex_held(&s->lock);

	/* if the queue is full, drop tail */
	if (s->head - s->tail >= CRPC_QLEN) {
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
		printf("[%lu] request enqueued: id=%lu, qlen = %d, waiting_winupdate=%d\n",
		       now, c->id, s->head - s->tail, s->waiting_winupdate);
	}
#endif

	// very first message
	if (!s->init) {
		crpc_send_winupdate(s);
		s->waiting_winupdate = true;
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

	/* implementation is currently limited to a maximum payload size */
	if (unlikely(len > SRPC_BUF_SIZE))
		return -E2BIG;

	mutex_lock(&s->lock);

	/* hot path, just send */
	if (s->win_used < s->win_avail && s->head == s->tail) {
		s->win_used++;
		*cque = 0;
		ret = crpc_send_raw(s, buf, len, s->req_id++);
		mutex_unlock(&s->lock);
		return ret;
	}

	/* cold path, enqueue request and drain the queue */
	if (!crpc_enqueue_one(s, buf, len, cque)) {
		crpc_drain_queue(s);
		mutex_unlock(&s->lock);
		return -ENOBUFS;
	}
	crpc_drain_queue(s);
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
ssize_t crpc_recv_one(struct crpc_session *s, void *buf, size_t len)
{
	struct srpc_hdr shdr;
	ssize_t ret;
	uint64_t now = microtime();

again:
	/* read the server header */
	ret = tcp_read_full(s->c, &shdr, sizeof(shdr));
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
			ret = tcp_read_full(s->c, buf, shdr.len);
			if (unlikely(ret <= 0))
				return ret;
			assert(ret == shdr.len);
			s->resp_rx_++;
		}

		/* update the window */
		mutex_lock(&s->lock);
		assert(s->win_used > 0);
		s->win_used--;
		s->win_avail = shdr.win;
		s->waiting_winupdate = false;

#if CRPC_TRACK_FLOW
		if (s->id == CRPC_TRACK_FLOW_ID) {
			printf("[%lu] ===> response: id=%lu, shdr.win=%lu, win=%u/%u\n",
			       now, shdr.id, shdr.win, s->win_used, s->win_avail);
		}
#endif

		if (s->win_avail > 0) {
			crpc_drain_queue(s);
		}
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
		s->win_avail = shdr.win;
		s->waiting_winupdate = false;

#if CRPC_TRACK_FLOW
		if (s->id == CRPC_TRACK_FLOW_ID) {
			printf("[%lu] ===> Winupdate: shdr.win=%lu, win=%u/%u\n",
			       microtime(), shdr.win, s->win_used, s->win_avail);
		}
#endif

		if (s->win_avail > 0) {
			crpc_drain_queue(s);
		}
		mutex_unlock(&s->lock);
		s->winu_rx_++;

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
		while (s->head == s->tail) {
			condvar_wait(&s->timer_cv, &s->lock);
			if (!s->running) {
				mutex_unlock(&s->lock);
				return;
			}
		}

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
			if (num_drops > 0 && s->demand_sync) {
				s->waiting_winupdate = false;
				crpc_send_winupdate(s);
			}
			continue;
		}

		// caculate next wake up time
		pos = (s->head - 1) % CRPC_QLEN;
		c = s->qreq[pos];
		mutex_unlock(&s->lock);
		timer_sleep_until(*c->cque + CRPC_MAX_CLIENT_DELAY_US);
		mutex_lock(&s->lock);
	}
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
	tcpconn_t *c;
	int i, ret;

	/* set up ephemeral IP and port */
	laddr.ip = 0;
	laddr.port = 0;

	if (raddr.port != SRPC_PORT)
		return -EINVAL;

	ret = tcp_dial(laddr, raddr, &c);
	if (ret)
		return ret;

	s = smalloc(sizeof(*s));
	if (!s) {
		tcp_close(c);
		return -ENOMEM;
	}
	memset(s, 0, sizeof(*s));

	for (i = 0; i < CRPC_QLEN; ++i) {
		s->qreq[i] = smalloc(sizeof(struct crpc_ctx));
		if (!s->qreq[i])
			goto fail;
	}

	s->c = c;
	mutex_init(&s->lock);
	condvar_init(&s->timer_cv);
	s->running = true;
	s->demand_sync = true;
	if (id != -1)
		s->id = id;
	s->req_id = 1;
	*sout = s;

	ret = thread_spawn(crpc_timer, s);
	BUG_ON(ret);

	return 0;

fail:
	tcp_close(c);
	for (i = i - 1; i >= 0; i--)
		sfree(s->qreq[i]);
	sfree(s);
	return -ENOMEM;
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

	s->running = false;
	condvar_signal(&s->timer_cv);
	tcp_close(s->c);
	for(i = 0; i < CRPC_QLEN; ++i)
		sfree(s->qreq[i]);
	sfree(s);
}
