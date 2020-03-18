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

#define CRPC_MAX_CLIENT_DELAY_US	100
#define CRPC_CREDIT_LIFETIME_US		-1
#define CRPC_MIN_DEMAND			0

#define CRPC_CLIENT_CLOSING		true
#define CRPC_MAX_TIMEOUT		3

#define CRPC_TRACK_FLOW			false
#define CRPC_TRACK_FLOW_ID		0

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
	chdr.demand = MAX(CRPC_MIN_DEMAND, s->head - s->tail);

	/* send the request */
	ret = tcp_write_full(s->c, &chdr, sizeof(chdr));
	if (unlikely(ret < 0))
		return ret;

	assert(ret == sizeof(chdr));
	s->last_demand = chdr.demand;
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
			     const void *buf, size_t len)
{
	struct iovec vec[2];
	struct crpc_hdr chdr;
	ssize_t ret;

	/* initialize the header */
	chdr.magic = RPC_REQ_MAGIC;
	chdr.op = RPC_OP_CALL;
	chdr.len = len;
	chdr.demand = MAX(CRPC_MIN_DEMAND, s->head - s->tail);

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
	s->req_tx_++;

#if CRPC_TRACK_FLOW
	if (s->id == CRPC_TRACK_FLOW_ID) {
		printf("[%lu] <=== request: demand = %lu, win = %u/%u\n",
		       microtime(), chdr.demand, s->win_used, s->win_avail);
	}
#endif
	return len;
}

static void crpc_drain_queue(struct crpc_session *s)
{
	ssize_t ret;
	int pos;
	uint64_t now;
	int num_drops = 0;

	assert_mutex_held(&s->lock);

	if (s->waiting_winupdate) {
		return;
	}

#if CRPC_MIN_DEMAND == 0
	if (s->head == s->tail)
		return;
#endif

	/* initialize the window */
	if (s->win_timestamp == 0 || s->last_demand == 0) {
		crpc_send_winupdate(s);
		s->waiting_winupdate = true;
		return;
	}

#if CRPC_MAX_CLIENT_DELAY_US > 0
       /* Remove old requests */
       now = microtime();
       while (s->head != s->tail) {
               pos = s->tail % CRPC_QLEN;
               if (now - *s->cques[pos] <= CRPC_MAX_CLIENT_DELAY_US)
                       break;
               s->tail++;
               s->req_dropped_++;
	       num_drops++;
#if CRPC_TRACK_FLOW
               if (s->id == CRPC_TRACK_FLOW_ID) {
                       printf("[%lu] Timeout Request dropped. qlen = %d, num_timeout = %d\n",
                               microtime(), s->head - s->tail, s->num_timeout);
               }
#endif
       }

       if (num_drops > 0 && s->head == s->tail) {
               s->waiting_winupdate = false;
               s->win_timestamp = 0;
               s->num_timeout++;
       }
#endif

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
	uint64_t now;
	int num_drops = 0;

	assert_mutex_held(&s->lock);

#if CRPC_MAX_CLIENT_DELAY_US > 0
	/* Remove old requests */
	now = microtime();
	while (s->head != s->tail) {
		pos = s->tail % CRPC_QLEN;
		if (now - *s->cques[pos] <= CRPC_MAX_CLIENT_DELAY_US)
			break;
		s->tail++;
		s->req_dropped_++;
		num_drops++;
#if CRPC_TRACK_FLOW
		if (s->id == CRPC_TRACK_FLOW_ID) {
			printf("[%lu] Timeout. Request dropped. qlen = %d, num_timeout = %d\n",
				microtime(), s->head - s->tail, s->num_timeout);
		}
#endif
	}

	if (num_drops > 0 && s->head == s->tail) {
		s->waiting_winupdate = false;
		s->win_timestamp = 0;
		s->num_timeout++;
	}

#if CRPC_CLIENT_CLOSING
	if (s->num_timeout > CRPC_MAX_TIMEOUT) {
#if CRPC_TRACK_FLOW
		if (s->id == CRPC_TRACK_FLOW_ID) {
			printf("[%lu] Timeout exceed 3. abort connection\n",
			       microtime());
		}
#endif
		s->req_dropped_++;
		return false;
	}
#endif
#endif

#if CRPC_MAX_CLIENT_DELAY_US == 0
	if (s->win_used >= s->win_avail) {
		s->req_dropped_++;
		return false;
	}
#endif

	/* if the queue is full, drop tail */
	if (s->head - s->tail >= CRPC_QLEN) {
		s->tail++;
		s->req_dropped_++;
#if CRPC_TRACK_FLOW
		if (s->id == CRPC_TRACK_FLOW_ID) {
			printf("[%lu] queue full. drop the request\n",
			       microtime());
		}
#endif
	}

	pos = s->head++ % CRPC_QLEN;
	*cque = microtime();
	memcpy(s->bufs[pos], buf, len);
	s->lens[pos] = len;
	s->cques[pos] = cque;

#if CRPC_TRACK_FLOW
		if (s->id == CRPC_TRACK_FLOW_ID) {
			printf("[%lu] request enqueued: qlen = %d\n",
			       microtime(), s->head - s->tail);
		}
#endif

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

#if CRPC_CLIENT_CLOSING
	if (s->num_timeout > CRPC_MAX_TIMEOUT) {
		s->req_dropped_++;
		return -ENOBUFS;
	}
#endif

	mutex_lock(&s->lock);

#if CRPC_CREDIT_LIFETIME_US > 0
	/* expire stale credits */
	now = microtime();
	if (s->win_timestamp > 0 &&
	    now - s->win_timestamp > CRPC_CREDIT_LIFETIME_US &&
	    s->win_used < s->win_avail) {
		s->win_expired_ += (s->win_avail - s->win_used);
		s->win_avail = s->win_used;
		if (s->win_used == 0)
			s->win_timestamp = 0;
	}
#endif

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

#if CRPC_CLIENT_CLOSING
		if (s->num_timeout > CRPC_MAX_TIMEOUT)
			s->win_avail = 0;
#endif

		if (s->win_avail > 0) {
			crpc_drain_queue(s);
		}
		mutex_unlock(&s->lock);
		s->resp_rx_++;

#if CRPC_MAX_CLIENT_DELAY_US > 0
		if (s->win_avail > 0)
			s->num_timeout = 0;
#endif

#if CRPC_TRACK_FLOW
		if (s->id == CRPC_TRACK_FLOW_ID) {
			printf("[%lu] ===> Response: shdr.win=%lu, win=%u/%u\n",
			       microtime(), shdr.win, s->win_used, s->win_avail);
		}
#endif

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

#if CRPC_CLIENT_CLOSING
		if (s->num_timeout > CRPC_MAX_TIMEOUT)
			s->win_avail = 0;
#endif

		if (s->win_avail > 0) {
			crpc_drain_queue(s);
		}
		mutex_unlock(&s->lock);
		s->winu_rx_++;

#if CRPC_MAX_CLIENT_DELAY_US > 0
		if (s->win_avail > 0)
			s->num_timeout = 0;
#endif

#if CRPC_TRACK_FLOW
		if (s->id == CRPC_TRACK_FLOW_ID) {
			printf("[%lu] ===> Winupdate: shdr.win=%lu, win=%u/%u\n",
			       microtime(), shdr.win, s->win_used, s->win_avail);
		}
#endif

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

bool crpc_closed(struct crpc_session *s)
{
	return (s->num_timeout > CRPC_MAX_TIMEOUT);
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
