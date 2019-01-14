/*
 * Copyright (c) 2014-2018 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * If we're being asked to sleep a short amount of time, ignore it.
 * A non-zero value means there may be a temporary violation of the
 * capacity limitation, but one that would even out. That is, possibly
 * fewer sleeps with the risk of more choppy behavior as this number
 * is larger.
 */
#define	WT_CAPACITY_PCT			10
#define	WT_CAPACITY_SLEEP_CUTOFF_US	100

/*
 * When given a total capacity, divide it up for each subsystem. We allow
 * and expect the sum of the subsystems to exceed 100.
 * We aim for:
 *    checkpoint: 10% of total
 *    eviction: 50% of total
 *    log:      25% of total
 *    reads:    50% of total
 */
#define	WT_CAPACITY(total, pct)	((total) * (pct) / 100)

#define	WT_CAP_CKPT		10
#define	WT_CAP_EVICT		60
#define	WT_CAP_LOG		20
#define	WT_CAP_READ		60

#define	WT_CAPACITY_CHK(v, str)	do {				\
	if ((v) != 0 && (v) < WT_THROTTLE_MIN)			\
		WT_RET_MSG(session, EINVAL,			\
		    "%s I/O capacity value %" PRId64		\
		    " below minimum %d",			\
		    str, v, WT_THROTTLE_MIN);			\
} while (0)

/*
 * Compute the time in nanoseconds that must be reserved to represent
 * a number of bytes in a subsystem with a particular capacity per second.
 */
#define	WT_RESERVATION_NS(bytes, capacity)			\
	(((bytes) * WT_BILLION) / (capacity))

/*
 * __capacity_config --
 *	Set I/O capacity configuration.
 */
static int
__capacity_config(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;
	uint64_t total;

	conn = S2C(session);

	WT_RET(__wt_config_gets(session, cfg, "io_capacity.total", &cval));
	WT_CAPACITY_CHK(cval.val, "total");
	conn->capacity_total = total = (uint64_t)cval.val;

	if (total != 0) {
		/*
		 * We've been given a total capacity, set the
		 * capacity of all the subsystems.
		 */
		conn->capacity_ckpt = WT_CAPACITY(total, WT_CAP_CKPT);
		conn->capacity_evict = WT_CAPACITY(total, WT_CAP_EVICT);
		conn->capacity_log = WT_CAPACITY(total, WT_CAP_LOG);
		conn->capacity_read = WT_CAPACITY(total, WT_CAP_READ);
	}

	/*
	 * Set the threshold to the percent of our capacity to periodically
	 * asynchronously flush what we've written.
	 */
	conn->capacity_threshold = (conn->capacity_ckpt +
	    conn->capacity_evict + conn->capacity_log) / 100 *
	    WT_CAPACITY_PCT;
	WT_STAT_CONN_SET(session, capacity_threshold, conn->capacity_threshold);

	return (0);
}

/*
 * __capacity_server_run_chk --
 *	Check to decide if the capacity server should continue running.
 */
static bool
__capacity_server_run_chk(WT_SESSION_IMPL *session)
{
	return (F_ISSET(S2C(session), WT_CONN_SERVER_CAPACITY));
}

/*
 * __capacity_server --
 *	The capacity server thread.
 */
static WT_THREAD_RET
__capacity_server(void *arg)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	bool signalled;

	session = arg;
	conn = S2C(session);
	for (;;) {
		/*
		 * Wait until signalled but check once per second in case
		 * the signal was missed.
		 */
		signalled = false;
		__wt_cond_wait_signal(session, conn->capacity_cond,
		    WT_MILLION/10, __capacity_server_run_chk, &signalled);

		if (signalled == false)
			WT_STAT_CONN_INCR(session, capacity_timeout);
		else
			WT_STAT_CONN_INCR(session, capacity_signalled);

		/* Check if we're quitting or being reconfigured. */
		if (!__capacity_server_run_chk(session))
			break;

		WT_PUBLISH(conn->capacity_signalled, false);
		if (conn->capacity_written > conn->capacity_threshold) {
			WT_ERR(__wt_fsync_all_background(session));
			__wt_atomic_storev64(&conn->capacity_written, 0);
		} else
			WT_STAT_CONN_INCR(session, fsync_notyet);
	}

	if (0) {
err:		WT_PANIC_MSG(session, ret, "capacity server error");
	}
	return (WT_THREAD_RET_VALUE);
}

/*
 * __capacity_server_start --
 *	Start the capacity server thread.
 */
static int
__capacity_server_start(WT_CONNECTION_IMPL *conn)
{
	WT_SESSION_IMPL *session;

	/* Nothing to do if the server is already running. */
	if (conn->capacity_session != NULL)
		return (0);

	F_SET(conn, WT_CONN_SERVER_CAPACITY);

	/*
	 * The capacity server gets its own session.
	 */
	WT_RET(__wt_open_internal_session(conn,
	    "capacity-server", false, 0, &conn->capacity_session));
	session = conn->capacity_session;

	WT_RET(__wt_cond_alloc(session,
	    "capacity server", &conn->capacity_cond));

	/*
	 * Start the thread.
	 */
	WT_RET(__wt_thread_create(
	    session, &conn->capacity_tid, __capacity_server, session));
	conn->capacity_tid_set = true;

	return (0);
}

/*
 * __wt_capacity_server_create --
 *	Configure and start the capacity server.
 */
int
__wt_capacity_server_create(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);

	/*
	 * If it is a read only connection there is nothing to do.
	 */
	if (F_ISSET(conn, WT_CONN_READONLY))
		return (0);

	/*
	 * Stop any server that is already running. This means that each time
	 * reconfigure is called we'll bounce the server even if there are no
	 * configuration changes. This makes our life easier as the underlying
	 * configuration routine doesn't have to worry about freeing objects
	 * in the connection structure (it's guaranteed to always start with a
	 * blank slate), and we don't have to worry about races where a running
	 * server is reading configuration information that we're updating, and
	 * it's not expected that reconfiguration will happen a lot.
	 */
	if (conn->capacity_session != NULL)
		WT_RET(__wt_capacity_server_destroy(session));

	WT_RET(__capacity_config(session, cfg));
	if (conn->capacity_threshold != 0)
		WT_RET(__capacity_server_start(conn));

	return (0);
}

/*
 * __wt_capacity_server_destroy --
 *	Destroy the capacity server thread.
 */
int
__wt_capacity_server_destroy(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION *wt_session;

	conn = S2C(session);

	F_CLR(conn, WT_CONN_SERVER_CAPACITY);
	if (conn->capacity_tid_set) {
		__wt_cond_signal(session, conn->capacity_cond);
		WT_TRET(__wt_thread_join(session, &conn->capacity_tid));
		conn->capacity_tid_set = false;
	}
	__wt_cond_destroy(session, &conn->capacity_cond);

	/* Close the server thread's session. */
	if (conn->capacity_session != NULL) {
		wt_session = &conn->capacity_session->iface;
		WT_TRET(wt_session->close(wt_session, NULL));
	}

	/*
	 * Ensure capacity settings are cleared - so that reconfigure doesn't
	 * get confused.
	 */
	conn->capacity_session = NULL;
	conn->capacity_tid_set = false;
	conn->capacity_cond = NULL;
	conn->capacity_usecs = 0;

	return (ret);
}

/*
 * __wt_capacity_signal --
 *	Signal the capacity thread if sufficient data has been written.
 */
void
__wt_capacity_signal(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);
	WT_STAT_CONN_INCR(session, capacity_signal_calls);
	if (conn->capacity_written >= conn->capacity_threshold &&
	    !conn->capacity_signalled) {
		__wt_cond_signal(session, conn->capacity_cond);
		WT_PUBLISH(conn->capacity_signalled, true);
		WT_STAT_CONN_INCR(session, capacity_signals);
	}
}

/*
 * __capacity_reserve --
 *	Make a reservation for the given number of bytes against
 * the capacity of the subsystem.
 */
static void
__capacity_reserve(WT_SESSION_IMPL *session, uint64_t *reservation,
    uint64_t bytes, uint64_t capacity, uint64_t now_ns, bool is_total,
    uint64_t *result)
{
	uint64_t res_len, res_value;

	if (capacity != 0) {
		res_len = WT_RESERVATION_NS(bytes, capacity);
		res_value = __wt_atomic_add64(reservation, res_len);
		__wt_verbose(session, WT_VERB_TEMPORARY,
		    "THROTTLE:%s len %" PRIu64 " reservation %" PRIu64
		    " now %" PRIu64, is_total ? " TOTAL:" : "",
		    res_len, res_value, now_ns);
		if (now_ns > res_value && now_ns - res_value > WT_BILLION) {
			/*
			 * If the total reservation clock is out of date,
			 * bring it to within a second of a current time.
			 */
			__wt_verbose(session, WT_VERB_TEMPORARY,
			    "THROTTLE:%s ADJ available %" PRIu64
			    " capacity %" PRIu64 " adjustment %" PRIu64,
			    is_total ? " TOTAL:" : "",
			    now_ns - res_value, capacity,
			    now_ns - WT_BILLION + res_len);
			__wt_atomic_store64(reservation,
			    now_ns - WT_BILLION + res_len);
		}
	} else
		res_value = now_ns;

	*result = res_value;
}

/*
 * __wt_capacity_throttle --
 *	Reserve a time to perform a write operation for the subsystem,
 * and wait until that time.
 *
 * The concept is that each write to a subsystem reserves a time slot
 * to do its write, and atomically adjusts the reservation marker to
 * point past the reserved slot. The size of the adjustment (i.e. the
 * length of time represented by the slot in nanoseconds) is chosen to
 * be proportional to the number of bytes to be written, and the
 * proportion is a simple calculation so that we can fit reservations for
 * exactly the configured capacity in a second. Reservation times are
 * in nanoseconds since the epoch.
 */
void
__wt_capacity_throttle(WT_SESSION_IMPL *session, uint64_t bytes,
    WT_THROTTLE_TYPE type)
{
	struct timespec now;
	WT_CONNECTION_IMPL *conn;
	uint64_t best_res, capacity, new_res, now_ns, sleep_us, res_total_value;
	uint64_t res_value, steal_capacity, stolen_bytes, this_res;
	uint64_t *reservation, *steal;
	uint64_t total_capacity;

	capacity = steal_capacity = 0;
	reservation = steal = NULL;
	conn = S2C(session);

	switch (type) {
	case WT_THROTTLE_CKPT:
		capacity = conn->capacity_ckpt;
		reservation = &conn->reservation_ckpt;
		WT_STAT_CONN_INCR(session, capacity_ckpt_calls);
		break;
	case WT_THROTTLE_EVICT:
		capacity = conn->capacity_evict;
		reservation = &conn->reservation_evict;
		WT_STAT_CONN_INCR(session, capacity_evict_calls);
		break;
	case WT_THROTTLE_LOG:
		capacity = conn->capacity_log;
		reservation = &conn->reservation_log;
		WT_STAT_CONN_INCR(session, capacity_log_calls);
		break;
	case WT_THROTTLE_READ:
		capacity = conn->capacity_read;
		reservation = &conn->reservation_read;
		WT_STAT_CONN_INCR(session, capacity_read_calls);
		break;
	}
	total_capacity = conn->capacity_total;

	__wt_verbose(session, WT_VERB_TEMPORARY,
	    "THROTTLE: type %d bytes %" PRIu64 " capacity %" PRIu64
	    "  reservation %" PRIu64,
	    (int)type, bytes, capacity, *reservation);
	if ((capacity == 0 && total_capacity == 0) ||
	    F_ISSET(conn, WT_CONN_RECOVERING))
		return;

	/*
	 * There may in fact be some reads done under the umbrella of log
	 * I/O, but they are mostly done under recovery. And if we are
	 * recovering, we don't reach this code.
	 */
	if (type != WT_THROTTLE_READ) {
		/* Sizes larger than this may overflow */
		(void)__wt_atomic_addv64(&conn->capacity_written, bytes);
		WT_STAT_CONN_INCRV(session, capacity_bytes_written, bytes);
		__wt_capacity_signal(session);
	} else
		WT_STAT_CONN_INCRV(session, capacity_bytes_read, bytes);
	WT_ASSERT(session, bytes < 16 * (uint64_t)WT_GIGABYTE);
	WT_ASSERT(session, capacity != 0);

	/* Get he current time in nanoseconds since the epoch. */
	__wt_epoch(session, &now);
	now_ns = (uint64_t)now.tv_sec * WT_BILLION + (uint64_t)now.tv_nsec;

again:
	/* Take a reservation for the subsystem, and for the total */
	__capacity_reserve(session, reservation, bytes, capacity, now_ns,
	    false, &res_value);
	__capacity_reserve(session, &conn->reservation_total, bytes,
	    total_capacity, now_ns, true, &res_total_value);

	/*
	 * If we ended up with a future reservation, and we aren't constricted
	 * by the total capacity, then we may be able to reallocate some
	 * unused reservation time from another subsystem.
	 */
	if (res_value > now_ns && res_total_value < now_ns && steal == NULL &&
	    total_capacity != 0) {
		best_res = now_ns - WT_BILLION / 2;
		if (type != WT_THROTTLE_CKPT &&
		    (this_res = conn->reservation_ckpt) < best_res) {
			steal = &conn->reservation_ckpt;
			steal_capacity = conn->capacity_ckpt;
			best_res = this_res;
		}
		if (type != WT_THROTTLE_EVICT &&
		    (this_res = conn->reservation_evict) < best_res) {
			steal = &conn->reservation_evict;
			steal_capacity = conn->capacity_evict;
			best_res = this_res;
		}
		if (type != WT_THROTTLE_LOG &&
		    (this_res = conn->reservation_log) < best_res) {
			steal = &conn->reservation_log;
			steal_capacity = conn->capacity_log;
			best_res = this_res;
		}
		if (type != WT_THROTTLE_READ &&
		    (this_res = conn->reservation_read) < best_res) {
			steal = &conn->reservation_read;
			steal_capacity = conn->capacity_read;
			best_res = this_res;
		}

		/*
		 * We have a subsystem that has enough spare capacity to
		 * steal.  We'll take a small slice and add it to our
		 * own subsystem.
		 */
		if (steal != NULL) {
			if (best_res < now_ns - WT_BILLION &&
			    now_ns > WT_BILLION)
				new_res = now_ns - WT_BILLION;
			else
				new_res = best_res;
			WT_ASSERT(session, steal_capacity != 0);
			new_res += WT_BILLION / 16 +
			    WT_RESERVATION_NS(bytes, steal_capacity);
			if (!__wt_atomic_casv64(steal, best_res, new_res)) {
				/*
				 * Give up our reservations and try again.
				 * We won't try to steal the next time.
				 */
				(void)__wt_atomic_sub64(reservation,
				    WT_RESERVATION_NS(bytes, capacity));
				(void)__wt_atomic_sub64(&
				    conn->reservation_total,
				    WT_RESERVATION_NS(bytes, total_capacity));
				goto again;
			}

			/*
			 * We've actually stolen capacity in terms of bytes,
			 * not nanoseconds, so we need to convert it.
			 */

			stolen_bytes = steal_capacity / 16;
			res_value = __wt_atomic_sub64(reservation,
			    WT_RESERVATION_NS(stolen_bytes, capacity));
		}
	}
	if (res_value < res_total_value)
		res_value = res_total_value;

	if (res_value > now_ns) {
		sleep_us = (res_value - now_ns) / WT_THOUSAND;
		__wt_verbose(session, WT_VERB_TEMPORARY,
		    "THROTTLE: SLEEP sleep us %" PRIu64,
		    sleep_us);
		if (res_value == res_total_value) {
			WT_STAT_CONN_INCR(session, capacity_total_throttles);
			WT_STAT_CONN_INCRV(session,
			    capacity_total_time, sleep_us);
		} else if (type == WT_THROTTLE_CKPT) {
			WT_STAT_CONN_INCR(session, capacity_ckpt_throttles);
			WT_STAT_CONN_INCRV(session,
			    capacity_ckpt_time, sleep_us);
		} else if (type == WT_THROTTLE_EVICT) {
			WT_STAT_CONN_INCR(session, capacity_evict_throttles);
			WT_STAT_CONN_INCRV(session,
			    capacity_evict_time, sleep_us);
		} else if (type == WT_THROTTLE_LOG) {
			WT_STAT_CONN_INCR(session, capacity_log_throttles);
			WT_STAT_CONN_INCRV(session,
			    capacity_log_time, sleep_us);
		} else if (type == WT_THROTTLE_READ) {
			WT_STAT_CONN_INCR(session, capacity_read_throttles);
			WT_STAT_CONN_INCRV(session,
			    capacity_read_time, sleep_us);
		}
		if (sleep_us > WT_CAPACITY_SLEEP_CUTOFF_US)
			/* Sleep handles large usec values. */
			__wt_sleep(0, sleep_us);
	}

	__wt_verbose(session, WT_VERB_TEMPORARY,
	    "THROTTLE: DONE reservation %" PRIu64, *reservation);
}
