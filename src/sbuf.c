/*
 * PgBouncer - Lightweight connection pooler for PostgreSQL.
 * 
 * Copyright (c) 2007 Marko Kreen, Skype Technologies OÜ
 * 
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Stream buffer
 *
 * The task is to copy data from one socket to another
 * efficiently, while allowing callbacks to look
 * at packet headers.
 */

#include "bouncer.h"

/* sbuf_main_loop() skip_recv values */
#define DO_RECV		false
#define SKIP_RECV	true

#define ACT_UNSET 0
#define ACT_SEND 1
#define ACT_SKIP 2
#define ACT_CALL 3


#define AssertSanity(sbuf) do { \
	Assert((sbuf)->send_pos >= 0); \
	Assert((sbuf)->send_pos <= (sbuf)->pkt_pos); \
	Assert((sbuf)->pkt_pos <= (sbuf)->recv_pos); \
	Assert((sbuf)->recv_pos <= cf_sbuf_len + SBUF_MAX_REWRITE); \
	Assert((sbuf)->pkt_remain >= 0); \
	Assert((sbuf)->send_remain >= 0); \
} while (0)

#define AssertActive(sbuf) do { \
	Assert((sbuf)->sock > 0); \
	AssertSanity(sbuf); \
} while (0)

/* declare static stuff */
static bool sbuf_queue_send(SBuf *sbuf) _MUSTCHECK;
static bool sbuf_send_pending(SBuf *sbuf) _MUSTCHECK;
static bool sbuf_process_pending(SBuf *sbuf) _MUSTCHECK;
static void sbuf_connect_cb(int sock, short flags, void *arg);
static void sbuf_recv_cb(int sock, short flags, void *arg);
static void sbuf_send_cb(int sock, short flags, void *arg);
static void sbuf_try_resync(SBuf *sbuf);
static bool sbuf_wait_for_data(SBuf *sbuf) _MUSTCHECK;
static void sbuf_main_loop(SBuf *sbuf, bool skip_recv);
static bool sbuf_call_proto(SBuf *sbuf, int event) /* _MUSTCHECK */;
static bool sbuf_actual_recv(SBuf *sbuf, int len)  _MUSTCHECK;
static bool sbuf_after_connect_check(SBuf *sbuf)  _MUSTCHECK;

/*********************************
 * Public functions
 *********************************/

/* initialize SBuf with proto handler */
void sbuf_init(SBuf *sbuf, sbuf_cb_t proto_fn, void *arg)
{
	memset(sbuf, 0, RAW_SBUF_SIZE);
	sbuf->proto_cb_arg = arg;
	sbuf->proto_cb = proto_fn;
}

/* got new socket from accept() */
bool sbuf_accept(SBuf *sbuf, int sock, bool is_unix)
{
	bool res;

	Assert(sbuf->recv_pos == 0 && sbuf->sock == 0);
	AssertSanity(sbuf);

	tune_socket(sock, is_unix);
	sbuf->sock = sock;
	sbuf->is_unix = is_unix;

	if (!cf_reboot) {
		res = sbuf_wait_for_data(sbuf);
		if (!res) {
			sbuf_call_proto(sbuf, SBUF_EV_RECV_FAILED);
			return false;
		}
		/* socket should already have some data (linux only) */
		if (cf_tcp_defer_accept && !is_unix) {
			sbuf_main_loop(sbuf, DO_RECV);
			if (!sbuf->sock)
				return false;
		}
	}
	return true;
}

/* need to connect() to get a socket */
bool sbuf_connect(SBuf *sbuf, const PgAddr *addr, const char *unix_dir, int timeout_sec)
{
	int res, sock, domain;
	struct sockaddr_in sa_in;
	struct sockaddr_un sa_un;
	struct sockaddr *sa;
	socklen_t len;
	struct timeval timeout;

	Assert(sbuf->recv_pos == 0 && sbuf->sock == 0);
	AssertSanity(sbuf);

	/* prepare sockaddr */
	if (addr->is_unix) {
		sa = (void*)&sa_un;
		len = sizeof(sa_un);
		memset(sa, 0, len);
		sa_un.sun_family = AF_UNIX;
		snprintf(sa_un.sun_path, sizeof(sa_un.sun_path),
			 "%s/.s.PGSQL.%d", unix_dir, addr->port);
		domain = AF_UNIX;
	} else {
		sa = (void*)&sa_in;
		len = sizeof(sa_in);
		memset(sa, 0, len);
		sa_in.sin_family = AF_INET;
		sa_in.sin_addr = addr->ip_addr;
		sa_in.sin_port = htons(addr->port);
		domain = AF_INET;
	}

	/*
	 * common stuff
	 */
	sock = socket(domain, SOCK_STREAM, 0);
	if (sock < 0)
		/* probably fd limit */
		goto failed;

	tune_socket(sock, addr->is_unix);

	sbuf->is_unix = addr->is_unix;
	sbuf->sock = sock;

	timeout.tv_sec = timeout_sec;
	timeout.tv_usec = 0;

	/* launch connection */
	res = safe_connect(sock, sa, len);
	if (res == 0) {
		/* unix socket gives connection immidiately */
		sbuf_connect_cb(sock, EV_WRITE, sbuf);
		return true;
	} else if (errno == EINPROGRESS) {
		/* tcp socket needs waiting */
		event_set(&sbuf->ev, sock, EV_WRITE, sbuf_connect_cb, sbuf);
		res = event_add(&sbuf->ev, &timeout);
		if (res >= 0)
			return true;
	}

failed:
	log_warning("sbuf_connect failed: %s", strerror(errno));

	if (sock >= 0)
		safe_close(sock);
	sbuf->sock = 0;
	sbuf_call_proto(sbuf, SBUF_EV_CONNECT_FAILED);
	return false;
}

/* don't wait for data on this socket */
void sbuf_pause(SBuf *sbuf)
{
	AssertActive(sbuf);
	Assert(sbuf->wait_send == 0);

	if (event_del(&sbuf->ev) < 0)
		/* fixme */
		fatal_perror("event_del");
}

/* resume from pause, start waiting for data */
void sbuf_continue(SBuf *sbuf)
{
	bool do_recv = DO_RECV;
	bool res;
	AssertActive(sbuf);

	res = sbuf_wait_for_data(sbuf);
	if (!res) {
		/* drop if problems */
		sbuf_call_proto(sbuf, SBUF_EV_RECV_FAILED);
		return;
	}

	/*
	 * It's tempting to try to avoid the recv() but that would
	 * only work if no code wants to see full packet.
	 *
	 * This is not true in ServerParameter case.
	 */
	/*
	 * if (sbuf->recv_pos - sbuf->pkt_pos >= SBUF_SMALL_PKT)
	 *	do_recv = false;
	 */

	sbuf_main_loop(sbuf, do_recv);
}

/*
 * Resume from pause and give socket over to external
 * callback function.
 *
 * The callback will be called with arg given to sbuf_init.
 */
bool sbuf_continue_with_callback(SBuf *sbuf, sbuf_libevent_cb user_cb)
{
	int err;

	AssertActive(sbuf);

	event_set(&sbuf->ev, sbuf->sock, EV_READ | EV_PERSIST,
		  user_cb, sbuf->proto_cb_arg);

	err = event_add(&sbuf->ev, NULL);
	if (err < 0) {
		log_warning("sbuf_continue_with_callback: %s", strerror(errno));
		return false;
	}
	return true;
}

/* socket cleanup & close */
void sbuf_close(SBuf *sbuf)
{
	/* keep handler & arg values */
	if (sbuf->sock > 0) {
		if (event_del(&sbuf->ev) < 0)
			fatal_perror("event_del");
		safe_close(sbuf->sock);
	}
	sbuf->dst = NULL;
	sbuf->sock = 0;
	sbuf->pkt_pos = sbuf->pkt_remain = sbuf->recv_pos = 0;
	sbuf->pkt_action = sbuf->wait_send = 0;
	sbuf->send_pos = sbuf->send_remain = 0;
}

/* proto_fn tells to send some bytes to socket */
void sbuf_prepare_send(SBuf *sbuf, SBuf *dst, int amount)
{
	AssertActive(sbuf);
	Assert(sbuf->pkt_remain == 0);
	Assert(sbuf->pkt_action == ACT_UNSET || sbuf->pkt_action == ACT_SEND || sbuf->send_remain == 0);
	Assert(amount > 0);

	sbuf->pkt_action = ACT_SEND;
	sbuf->pkt_remain = amount;
	sbuf->dst = dst;
}

/* proto_fn tells to skip some amount of bytes */
void sbuf_prepare_skip(SBuf *sbuf, int amount)
{
	AssertActive(sbuf);
	Assert(sbuf->pkt_remain == 0);
	Assert(sbuf->pkt_action == ACT_UNSET || sbuf->send_remain == 0);
	Assert(amount > 0);

	sbuf->pkt_action = ACT_SKIP;
	sbuf->pkt_remain = amount;
	/* sbuf->dst = NULL; // fixme ?? */
}

/* proto_fn tells to skip some amount of bytes */
void sbuf_prepare_fetch(SBuf *sbuf, int amount)
{
	AssertActive(sbuf);
	Assert(sbuf->pkt_remain == 0);
	Assert(sbuf->pkt_action == ACT_UNSET || sbuf->send_remain == 0);
	Assert(amount > 0);

	sbuf->pkt_action = ACT_CALL;
	sbuf->pkt_remain = amount;
	/* sbuf->dst = NULL; // fixme ?? */
}

/*************************
 * Internal functions
 *************************/

/*
 * Call proto callback with proper MBuf.
 *
 * If callback returns true it used one of sbuf_prepare_* on sbuf,
 * and processing can continue.
 *
 * If it returned false it used sbuf_pause(), sbuf_close() or simply
 * wants to wait for next event loop (e.g. too few data available).
 * Callee should not touch sbuf in that case and just return to libevent.
 */
static bool sbuf_call_proto(SBuf *sbuf, int event)
{
	MBuf mbuf;
	uint8_t *pos = sbuf->buf + sbuf->pkt_pos;
	int avail = sbuf->recv_pos - sbuf->pkt_pos;
	bool res;

	AssertSanity(sbuf);
	Assert(event != SBUF_EV_READ || avail > 0);

	/* if pkt callback, limit only with current packet */
	if (event == SBUF_EV_PKT_CALLBACK) {
		if (avail > sbuf->pkt_remain)
			avail = sbuf->pkt_remain;
	}

	mbuf_init(&mbuf, pos, avail);
	res = sbuf->proto_cb(sbuf, event, &mbuf, sbuf->proto_cb_arg);

	AssertSanity(sbuf);
	Assert(event != SBUF_EV_READ || !res || sbuf->sock > 0);

	return res;
}

/* let's wait for new data */
static bool sbuf_wait_for_data(SBuf *sbuf)
{
	int err;

	event_set(&sbuf->ev, sbuf->sock, EV_READ | EV_PERSIST, sbuf_recv_cb, sbuf);
	err = event_add(&sbuf->ev, NULL);
	if (err < 0) {
		log_warning("sbuf_wait_for_data: event_add: %s", strerror(errno));
		return false;
	}
	return true;
}

/* libevent EV_WRITE: called when dest socket is writable again */
static void sbuf_send_cb(int sock, short flags, void *arg)
{
	SBuf *sbuf = arg;
	bool res;

	/* sbuf was closed before in this loop */
	if (!sbuf->sock)
		return;

	AssertSanity(sbuf);
	Assert(sbuf->wait_send);

	/* prepare normal situation for sbuf_main_loop */
	sbuf->wait_send = 0;
	res = sbuf_wait_for_data(sbuf);
	if (res) {
		/* here we should certainly skip recv() */
		sbuf_main_loop(sbuf, SKIP_RECV);
	} else
		/* drop if problems */
		sbuf_call_proto(sbuf, SBUF_EV_SEND_FAILED);
}

/* socket is full, wait until it's writable again */
static bool sbuf_queue_send(SBuf *sbuf)
{
	int err;
	AssertActive(sbuf);

	sbuf->wait_send = 1;
	err = event_del(&sbuf->ev);
	if (err < 0) {
		log_warning("sbuf_queue_send: event_del failed: %s", strerror(errno));
		return false;
	}
	event_set(&sbuf->ev, sbuf->dst->sock, EV_WRITE, sbuf_send_cb, sbuf);
	err = event_add(&sbuf->ev, NULL);
	if (err < 0) {
		log_warning("sbuf_queue_send: event_add failed: %s", strerror(errno));
		return false;
	}
	return true;
}

/*
 * There's data in buffer to be sent. Returns bool if processing can continue.
 *
 * Does not look at pkt_pos/remain fields, expects them to be merged to send_*
 */
static bool sbuf_send_pending(SBuf *sbuf)
{
	int res, avail;
	uint8_t *pos;

	AssertActive(sbuf);
	Assert(sbuf->dst || !sbuf->send_remain);

try_more:
	/* how much data is available for sending */
	avail = sbuf->recv_pos - sbuf->send_pos;
	if (avail > sbuf->send_remain)
		avail = sbuf->send_remain;
	if (avail == 0)
		goto all_sent;

	if (sbuf->dst->sock == 0) {
		log_error("sbuf_send_pending: no dst sock?");
		return false;
	}

	/* actually send it */
	pos = sbuf->buf + sbuf->send_pos;
	res = safe_send(sbuf->dst->sock, pos, avail, 0);
	if (res < 0) {
		if (errno == EAGAIN) {
			if (!sbuf_queue_send(sbuf))
				/* drop if queue failed */
				sbuf_call_proto(sbuf, SBUF_EV_SEND_FAILED);
		} else
			sbuf_call_proto(sbuf, SBUF_EV_SEND_FAILED);
		return false;
	}

	sbuf->send_remain -= res;
	sbuf->send_pos += res;

	AssertActive(sbuf);

	/*
	 * Should do sbuf_queue_send() immediately?
	 *
	 * To be sure, let's run into EAGAIN.
	 */
	if (res < avail)
		goto try_more;

all_sent:

	/* send_pos may lag pkt_pos in case of skip packets, move it here */
	if (sbuf->send_remain == 0 && sbuf->send_pos < sbuf->pkt_pos)
		sbuf->send_pos = sbuf->pkt_pos;

	return true;
}

/* process as much data as possible */
static bool sbuf_process_pending(SBuf *sbuf)
{
	int avail;
	bool full = sbuf->recv_pos >= cf_sbuf_len;
	bool res;

	while (1) {
		AssertActive(sbuf);

		/*
		 * Enough for now?
		 *
		 * The (avail <= SBUF_SMALL_PKT) check is to avoid partial pkts.
		 * As SBuf should not assume knowledge about packets,
		 * the check is not done in !full case.  Packet handler can
		 * then still notify about partial packet by returning false.
		 */
		avail = sbuf->recv_pos - sbuf->pkt_pos;
		if (avail == 0 || (full && avail <= SBUF_SMALL_PKT))
			break;

		/*
		 * If start of packet, process packet header.
		 */
		if (sbuf->pkt_remain == 0) {
			res = sbuf_call_proto(sbuf, SBUF_EV_READ);
			if (!res)
				return false;
			Assert(sbuf->pkt_remain > 0);
		}

		/* walk pkt, merge sends */
		if (avail > sbuf->pkt_remain)
			avail = sbuf->pkt_remain;

		switch (sbuf->pkt_action) {
		case ACT_SEND:
			if (sbuf->send_remain == 0)
				sbuf->send_pos = sbuf->pkt_pos;
			sbuf->send_remain += avail;
			break;
		case ACT_CALL:
			res = sbuf_call_proto(sbuf, SBUF_EV_PKT_CALLBACK);
			if (!res)
				return false;
			/* after callback, skip pkt */
		case ACT_SKIP:
			res = sbuf_send_pending(sbuf);
			if (!res)
				return res;
			break;
		}
		sbuf->pkt_remain -= avail;
		sbuf->pkt_pos += avail;
	}

	return sbuf_send_pending(sbuf);
}

/* reposition at buffer start again */
static void sbuf_try_resync(SBuf *sbuf)
{
	int avail;

	AssertActive(sbuf);

	if (sbuf->send_pos == 0)
		return;

	avail = sbuf->recv_pos - sbuf->send_pos;

	if (avail == 0) {
		sbuf->recv_pos = sbuf->pkt_pos = sbuf->send_pos = 0;
	} else if (avail <= SBUF_SMALL_PKT) {
		memmove(sbuf->buf, sbuf->buf + sbuf->send_pos, avail);
		sbuf->pkt_pos -= sbuf->send_pos;
		sbuf->send_pos = 0;
		sbuf->recv_pos = avail;
	}
}

/* actually ask kernel for more data */
static bool sbuf_actual_recv(SBuf *sbuf, int len)
{
	int got;
	uint8_t *pos;

	AssertActive(sbuf);
	Assert(len > 0);
	Assert(sbuf->recv_pos + len <= cf_sbuf_len);

	pos = sbuf->buf + sbuf->recv_pos;
	got = safe_recv(sbuf->sock, pos, len, 0);

	if (got == 0) {
		/* eof from socket */
		sbuf_call_proto(sbuf, SBUF_EV_RECV_FAILED);
		return false;
	} else if (got < 0) {
		if (errno == EAGAIN) {
			/* we tried too much, socket is empty.
			   act as zero bytes was read */
			got = 0;
		} else {
			/* some error occured */
			sbuf_call_proto(sbuf, SBUF_EV_RECV_FAILED);
			return false;
		}
	}
	sbuf->recv_pos += got;
	return true;
}

/* callback for libevent EV_READ */
static void sbuf_recv_cb(int sock, short flags, void *arg)
{
	SBuf *sbuf = arg;
	sbuf_main_loop(sbuf, DO_RECV);
}

/*
 * Main recv-parse-send-repeat loop.
 *
 * Reason for skip_recv is to avoid extra recv().  The problem with it
 * is EOF from socket.  Currently that means that the pending data is
 * dropped.  Fortunately server sockets are not paused and dropping
 * data from client is no problem.  So only place where skip_recv is
 * important is sbuf_send_cb().
 */
static void sbuf_main_loop(SBuf *sbuf, bool skip_recv)
{
	int free, ok;

	/* sbuf was closed before in this event loop */
	if (!sbuf->sock)
		return;

	/* reading should be disabled when waiting */
	Assert(sbuf->wait_send == 0);
	AssertSanity(sbuf);

	/* avoid recv() if asked */
	if (skip_recv)
		goto skip_recv;

try_more:
	/* make room in buffer */
	sbuf_try_resync(sbuf);

	/*
	 * here used to be if (free > SBUF_SMALL_PKT) check
	 * but with skip_recv switch its should not be needed anymore.
	 */
	free = cf_sbuf_len - sbuf->recv_pos;
	if (free > 0) {
		/*
		 * When suspending, try to hit packet boundary ASAP.
		 */
		if (cf_pause_mode == P_SUSPEND
		    && sbuf->pkt_remain > 0
		    && sbuf->pkt_remain < free)
		{
			free = sbuf->pkt_remain;
		}

		/* now fetch the data */
		ok = sbuf_actual_recv(sbuf, free);
		if (!ok)
			return;
	}

skip_recv:
	/* now handle it */
	ok = sbuf_process_pending(sbuf);
	if (!ok)
		return;

	/* if the buffer is full, there can be more data available */
	if (sbuf->recv_pos >= cf_sbuf_len)
		goto try_more;

	/* clean buffer */
	sbuf_try_resync(sbuf);

	/* notify proto that all is sent */
	if (sbuf_is_empty(sbuf))
		sbuf_call_proto(sbuf, SBUF_EV_FLUSH);
}

/* check if there is any error pending on socket */
static bool sbuf_after_connect_check(SBuf *sbuf)
{
	int optval = 0, err;
	socklen_t optlen = sizeof(optval);

	err = getsockopt(sbuf->sock, SOL_SOCKET, SO_ERROR, (void*)&optval, &optlen);
	if (err < 0) {
		log_error("sbuf_after_connect_check: getsockopt: %s",
			  strerror(errno));
		return false;
	}
	if (optval != 0) {
		log_error("sbuf_after_connect_check: pending error: %s",
			  strerror(optval));
		return false;
	}
	return true;
}

/* callback for libevent EV_WRITE when connecting */
static void sbuf_connect_cb(int sock, short flags, void *arg)
{
	SBuf *sbuf = arg;

	if (flags & EV_WRITE) {
		if (!sbuf_after_connect_check(sbuf))
			goto failed;
		if (!sbuf_call_proto(sbuf, SBUF_EV_CONNECT_OK))
			return;
		if (!sbuf_wait_for_data(sbuf))
			goto failed;
		return;
	}
failed:
	sbuf_call_proto(sbuf, SBUF_EV_CONNECT_FAILED);
}

/* send some data to listening socket */
bool sbuf_answer(SBuf *sbuf, const void *buf, int len)
{
	int res;
	if (sbuf->sock <= 0)
		return false;
	res = safe_send(sbuf->sock, buf, len, 0);
	if (res < 0)
		log_debug("sbuf_answer: error sending: %s", strerror(errno));
	else if (res != len)
		log_debug("sbuf_answer: partial send: len=%d sent=%d", len, res);
	return res == len;
}

bool sbuf_rewrite_header(SBuf *sbuf, int old_len,
			 const uint8_t *new_hdr, int new_len)
{
	int avail = sbuf->recv_pos - sbuf->pkt_pos;
	int diff = new_len - old_len;
	uint8_t *pkt_pos = sbuf->buf + sbuf->pkt_pos;
	uint8_t *old_pos = pkt_pos + old_len;
	uint8_t *new_pos = pkt_pos + new_len;

	AssertActive(sbuf);
	Assert(old_len >= 0 && new_len >= 0);
	Assert(diff <= SBUF_MAX_REWRITE);

	/* overflow can be triggered by user by sending multiple Parse pkts */
	if (sbuf->recv_pos + diff > cf_sbuf_len + SBUF_MAX_REWRITE)
		return false;

	memmove(new_pos, old_pos, avail - old_len);
	memcpy(pkt_pos, new_hdr, new_len);
	sbuf->recv_pos += diff;
	return true;
}

