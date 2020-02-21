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

#include "util.h"
#include "proto.h"

#define MAX_CLIENT_QDELAY_US	120
#define CRPC_CREDIT_LIFETIME_US	20

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

	/* construct the client header */
	chdr.magic = RPC_REQ_MAGIC;
	chdr.op = RPC_OP_WINUPDATE;
	chdr.len = 0;
	chdr.demand = s->head - s->tail;

	/* send the request */
	ret = tcp_write_full(s->c, &chdr, sizeof(chdr));
	if (unlikely(ret < 0))
		return ret;

	assert(ret == sizeof(chdr));
	s->last_demand = chdr.demand;

	return 0;
}

static ssize_t crpc_send_raw(struct crpc_session *s,
			     const void *buf, size_t len)
{
	struct iovec vec[2];
	struct crpc_hdr chdr;
	ssize_t ret;

	/* initialize the header */
	chdr.magic = RPC_REQ_MAGIC;
	chdr.op = RPC_OP_CALL;
	chdr.len = len;
	chdr.demand = s->head - s->tail;

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
	s->last_demand = chdr.demand;

	return len;
}

static void crpc_drain_queue(struct crpc_session *s)
{
	ssize_t ret;
	int pos;

	assert_mutex_held(&s->lock);

	if (s->waiting_winupdate) {
		return;
	}

	if (s->head == s->tail)
		return;

	/* initialize the window */
	if (s->win_timestamp == 0 || s->last_demand == 0) {
		crpc_send_winupdate(s);
		s->waiting_winupdate = true;
		return;
	}

	/* try to drain queued requests: FIFO */
	while (s->head != s->tail) {
		if (s->win_used >= s->win_avail)
			break;

		pos = s->tail++ % CRPC_QLEN;
		*s->cques[pos] = microtime() - *s->cques[pos];
		ret = crpc_send_raw(s, s->bufs[pos], s->lens[pos]);
		if (ret < 0)
			break;
		assert(ret == s->lens[pos]);
		s->win_used++;
	}
}

static bool crpc_enqueue_one(struct crpc_session *s,
			     const void *buf, size_t len, uint64_t *cque)
{
	int pos;

	assert_mutex_held(&s->lock);

	/* if the queue is full, drop tail */
	if (s->head - s->tail >= CRPC_QLEN)
		s->tail++;

	pos = s->head++ % CRPC_QLEN;
	*cque = microtime();
	memcpy(s->bufs[pos], buf, len);
	s->lens[pos] = len;
	s->cques[pos] = cque;

	if (unlikely(s->head - s->tail < 0))
		panic("overflow!\n");

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
	uint64_t now;

	/* implementation is currently limited to a maximum payload size */
	if (unlikely(len > SRPC_BUF_SIZE))
		return -E2BIG;

	mutex_lock(&s->lock);
	now = microtime();

	/* expire stale credits */
	if (s->win_timestamp > 0 &&
	    now - s->win_timestamp > CRPC_CREDIT_LIFETIME_US &&
	    s->win_used < s->win_avail) {
		s->win_avail = s->win_used;
		if (s->win_used == 0)
			s->win_timestamp = 0;
	}

	/* hot path, just send */
	if (s->win_used < s->win_avail && s->head == s->tail) {
		s->win_used++;
		*cque = 0;
		ret = crpc_send_raw(s, buf, len);
		mutex_unlock(&s->lock);
		return ret;
	}

	/* cold path, enqueue request and drain the queue */
	if (!crpc_enqueue_one(s, buf, len, cque)) {
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
		ret = tcp_read_full(s->c, buf, shdr.len);
		if (unlikely(ret <= 0))
			return ret;
		assert(ret == shdr.len);

		/* update the window */
		mutex_lock(&s->lock);
		assert(s->win_used > 0);
		s->win_used--;
		s->win_avail = shdr.win;
		s->win_timestamp = microtime();
		s->waiting_winupdate = false;

		if (s->win_avail > 0) {
			crpc_drain_queue(s);
		}
		mutex_unlock(&s->lock);

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
		s->win_timestamp = microtime();
		s->waiting_winupdate = false;

		if (s->win_avail > 0) {
			crpc_drain_queue(s);
		}
		mutex_unlock(&s->lock);

		goto again;
	default:
		log_warn("crpc: got invalid op %d", shdr.op);
		return -EINVAL;
	}

	return shdr.len;
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
int crpc_open(struct netaddr raddr, struct crpc_session **sout)
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
		s->bufs[i] = smalloc(SRPC_BUF_SIZE);
		if (!s->bufs[i])
			goto fail;
	}

	s->c = c;
	mutex_init(&s->lock);
	*sout = s;

	return 0;

fail:
	tcp_close(c);
	for (i = i - 1; i >= 0; i--)
		sfree(s->bufs[i]);
	sfree(s);
	return -ENOMEM;
}

uint32_t crpc_win_avail(struct crpc_session *s)
{
	return s->win_avail;
}

/**
 * crpc_close - closes an RPC session
 * @s: the session to close
 *
 * WARNING: This function could block.
 */
void crpc_close(struct crpc_session *s)
{
	tcp_close(s->c);
	sfree(s);
}
