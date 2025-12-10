/**
 * @file sessiontimer.c Session Timer module (RFC 4082)
 *
 * Copyright (C) 2025
 */
#include <re.h>
#include <baresip.h>

/**
 * Session Timer module implementing RFC 4082
 *
 * This module implements the Session Timer mechanism for SIP sessions
 * to refresh sessions and detect session failures.
 */

#define MIN_SESSION_INTERVAL 90      /* RFC 4028 minimum: 90 seconds */
#define DEFAULT_SESSION_INTERVAL 1800 /* RFC 4028 recommended: 30 minutes */
#define REFRESH_FACTOR 2              /* Refresh at half interval */

struct sessiontimer {
	struct le le;
	struct call *call;
	struct tmr tmr;
	uint32_t session_interval;  /* Session interval in seconds */
	uint32_t min_se;             /* Minimum session interval */
	uint64_t session_expires;    /* Absolute expiration time */
	bool is_refresher;           /* true if we are the refresher */
	bool active;                 /* Timer is active */
	uint32_t retry_count;        /* Count of 422 retries */
};

static struct list sessiontimers;

/* Default minimum session interval */
static uint32_t default_min_se = MIN_SESSION_INTERVAL;
/* Default session interval */
static uint32_t default_session_interval = DEFAULT_SESSION_INTERVAL;

static int parse_session_expires(const struct pl *hdr_val,
				 uint32_t *delta_seconds, bool *refresher_uac)
{
	struct pl val, param;
	uint32_t delta = 0;
	bool is_uac = false;

	if (!hdr_val || !pl_isset(hdr_val))
		return EINVAL;

	/* Parse delta-seconds */
	if (re_regex(hdr_val->p, hdr_val->l, "[0-9]+[^;]*", &val)) {
		return EBADMSG;
	}

	delta = pl_u32(&val);
	if (delta < MIN_SESSION_INTERVAL)
		return EINVAL;

	/* Parse refresher parameter */
	if (!re_regex(hdr_val->p, hdr_val->l,
		      "[^;]*;[ \t]*refresher[ \t]*=[ \t]*uac", NULL)) {
		is_uac = true;
	}
	else if (!re_regex(hdr_val->p, hdr_val->l,
			   "[^;]*;[ \t]*refresher[ \t]*=[ \t]*uas", NULL)) {
		is_uac = false;
	}

	if (delta_seconds)
		*delta_seconds = delta;
	if (refresher_uac)
		*refresher_uac = is_uac;

	return 0;
}

static int parse_min_se(const struct pl *hdr_val, uint32_t *min_se)
{
	struct pl val;

	if (!hdr_val || !pl_isset(hdr_val))
		return EINVAL;

	if (re_regex(hdr_val->p, hdr_val->l, "[0-9]+", &val))
		return EBADMSG;

	*min_se = pl_u32(&val);
	if (*min_se < MIN_SESSION_INTERVAL)
		return EINVAL;

	return 0;
}

static struct sessiontimer *find_timer(const struct call *call)
{
	struct le *le;

	if (!call)
		return NULL;

	LIST_FOREACH(&sessiontimers, le) {
		struct sessiontimer *st = le->data;
		if (st->call == call)
			return st;
	}

	return NULL;
}

static void refresh_timer(struct sessiontimer *st)
{
	uint64_t now = tmr_jiffies();
	uint64_t refresh_time;

	if (!st->active || !st->is_refresher)
		return;

	/* Refresh at half the session interval */
	refresh_time = (st->session_interval * 1000) / REFRESH_FACTOR;

	/* Calculate when to refresh */
	st->session_expires = now + (st->session_interval * 1000);

	info("sessiontimer: scheduling refresh in %u seconds "
	     "(session expires in %u seconds)\n",
	     (uint32_t)(refresh_time / 1000),
	     st->session_interval);

	tmr_start(&st->tmr, refresh_time, tmr_handler, st);
}

static void tmr_handler(void *arg)
{
	struct sessiontimer *st = arg;
	int err;
	uint64_t now;

	if (!st || !st->call)
		return;

	now = tmr_jiffies();

	/* Check if session has expired */
	if (now >= st->session_expires) {
		warning("sessiontimer: session expired, sending BYE\n");
		call_hangup(st->call, 408, "Session Timer Expired");
		mem_deref(st);
		return;
	}

	if (st->is_refresher) {
		/* We are the refresher, send refresh */
		info("sessiontimer: timer expired, sending refresh\n");

		/* Send re-INVITE to refresh session */
		err = call_modify(st->call);
		if (err) {
			warning("sessiontimer: failed to send refresh: %m\n", err);
			/* Retry after a short delay */
			tmr_start(&st->tmr, 5000, tmr_handler, st);
			return;
		}

		/* Timer will be restarted when we get 2xx response */
	}
	else {
		/* We are not the refresher, but timer expired.
		 * This means we didn't receive a refresh in time. */
		warning("sessiontimer: no refresh received, session expired\n");
		call_hangup(st->call, 408, "Session Timer Expired");
		mem_deref(st);
	}
}

static void destructor(void *arg)
{
	struct sessiontimer *st = arg;

	tmr_cancel(&st->tmr);
	list_unlink(&st->le);
}

static void update_session_timer(struct sessiontimer *st,
				 uint32_t session_interval,
				 bool is_refresher)
{
	uint64_t now;

	if (!st)
		return;

	now = tmr_jiffies();
	st->session_interval = session_interval;
	st->is_refresher = is_refresher;
	st->active = true;

	/* Calculate session expiration time */
	st->session_expires = now + (session_interval * 1000);

	info("sessiontimer: session interval=%u, refresher=%s, "
	     "expires in %u seconds\n",
	     session_interval, is_refresher ? "uac" : "uas",
	     session_interval);

	if (is_refresher) {
		/* We are the refresher, schedule refresh */
		refresh_timer(st);
	}
	else {
		/* We are not the refresher, but we still need to track
		 * expiration to send BYE if no refresh arrives */
		info("sessiontimer: waiting for peer to refresh, "
		     "will expire in %u seconds\n", session_interval);
		/* Set timer to expire when session expires */
		tmr_start(&st->tmr, session_interval * 1000, tmr_handler, st);
	}
}

static void add_session_headers(struct call *call, uint32_t session_interval,
				uint32_t min_se, bool is_refresher)
{
	struct list hdrs;
	char hdr_val[128];
	int err;

	if (!call)
		return;

	list_init(&hdrs);

	/* Add Session-Expires header */
	if (session_interval > 0) {
		re_snprintf(hdr_val, sizeof(hdr_val), "%u;refresher=%s",
			    session_interval, is_refresher ? "uac" : "uas");
		err = custom_hdrs_add(&hdrs, "Session-Expires", "%s", hdr_val);
		if (err) {
			warning("sessiontimer: failed to add Session-Expires: "
				"%m\n", err);
		}
	}

	/* Add Min-SE header if we have a minimum */
	if (min_se > 0 && min_se >= MIN_SESSION_INTERVAL) {
		re_snprintf(hdr_val, sizeof(hdr_val), "%u", min_se);
		err = custom_hdrs_add(&hdrs, "Min-SE", "%s", hdr_val);
		if (err) {
			warning("sessiontimer: failed to add Min-SE: %m\n",
				err);
		}
	}

	/* Add Supported: timer header */
	err = custom_hdrs_add(&hdrs, "Supported", "timer");
	if (err) {
		warning("sessiontimer: failed to add Supported: %m\n", err);
	}

	call_set_custom_hdrs(call, &hdrs);
	list_flush(&hdrs);
}

static int find_header(const struct sip_msg *msg, const char *name,
		       const struct sip_hdr **hdrp)
{
	const struct sip_hdr *hdr;
	struct pl hdr_name;

	if (!msg || !name || !hdrp)
		return EINVAL;

	pl_set_str(&hdr_name, name);

	/* Iterate through all headers */
	for (hdr = sip_msg_hdr(msg, SIP_HDR_OTHER); hdr;
	     hdr = sip_msg_hdr_next(msg, hdr)) {
		if (pl_strcasecmp(&hdr->name, name) == 0) {
			*hdrp = hdr;
			return 0;
		}
	}

	return ENOENT;
}

static void handle_422_response(struct sessiontimer *st,
				const struct sip_msg *msg)
{
	const struct sip_hdr *hdr;
	uint32_t min_se = 0;
	int err;

	if (!st || !msg)
		return;

	/* Extract Min-SE from 422 response */
	err = find_header(msg, "Min-SE", &hdr);
	if (!err) {
		err = parse_min_se(&hdr->val, &min_se);
		if (!err && min_se > st->min_se) {
			st->min_se = min_se;
			info("sessiontimer: 422 response, new Min-SE=%u\n",
			     min_se);
		}
	}

	/* Retry with adjusted session interval */
	if (st->min_se > 0) {
		uint32_t new_interval = st->min_se;
		if (st->session_interval > new_interval)
			new_interval = st->session_interval;

		st->retry_count++;
		if (st->retry_count > 5) {
			warning("sessiontimer: too many 422 retries, giving up\n");
			call_hangup(st->call, 422, "Session Interval Too Small");
			mem_deref(st);
			return;
		}

		info("sessiontimer: retrying with session interval=%u\n",
		     new_interval);
		add_session_headers(st->call, new_interval, st->min_se,
				    st->is_refresher);
		/* The call_modify will be triggered by the caller */
	}
}

static void process_2xx_response(struct sessiontimer *st,
				 const struct sip_msg *msg)
{
	const struct sip_hdr *hdr;
	uint32_t session_interval = 0;
	bool refresher_uac = false;
	int err;
	bool is_outgoing;

	if (!st || !msg || !st->call)
		return;

	/* Determine if this is an outgoing call */
	is_outgoing = call_is_outgoing(st->call);

	/* Look for Session-Expires header in 2xx response */
	err = find_header(msg, "Session-Expires", &hdr);
	if (!err) {
		err = parse_session_expires(&hdr->val, &session_interval,
					   &refresher_uac);
		if (!err && session_interval >= MIN_SESSION_INTERVAL) {
			/* Determine if we are the refresher */
			/* If refresher=uac and we're UAC, we refresh */
			/* If refresher=uas and we're UAS, we refresh */
			bool is_refresher;
			if (is_outgoing) {
				is_refresher = refresher_uac;
			}
			else {
				is_refresher = !refresher_uac;
			}

			update_session_timer(st, session_interval,
					     is_refresher);
			return;
		}
	}

	/* If no Session-Expires in response but we requested it,
	 * we become the refresher (UAC case) */
	if (st->session_interval > 0 && is_outgoing) {
		info("sessiontimer: no Session-Expires in response, "
		     "we become refresher\n");
		update_session_timer(st, st->session_interval, true);
	}
}

static void process_incoming_request(struct sessiontimer *st,
				    const struct sip_msg *msg)
{
	const struct sip_hdr *hdr;
	uint32_t session_interval = 0;
	uint32_t min_se = 0;
	bool refresher_uac = false;
	int err;
	struct list hdrs;
	char hdr_val[128];

	if (!st || !msg)
		return;

	/* Parse Session-Expires from incoming request */
	err = find_header(msg, "Session-Expires", &hdr);
	if (!err) {
		err = parse_session_expires(&hdr->val, &session_interval,
					   &refresher_uac);
		if (!err) {
			st->session_interval = session_interval;
			info("sessiontimer: incoming request, "
			     "interval=%u, refresher=%s\n",
			     session_interval,
			     refresher_uac ? "uac" : "uas");
		}
	}

	/* Parse Min-SE from incoming request */
	err = find_header(msg, "Min-SE", &hdr);
	if (!err) {
		err = parse_min_se(&hdr->val, &min_se);
		if (!err && min_se > st->min_se) {
			st->min_se = min_se;
		}
	}

	/* If we received Session-Expires, we need to echo it back in response */
	if (session_interval > 0) {
		/* Adjust session interval if needed */
		if (st->min_se > 0 && session_interval < st->min_se) {
			session_interval = st->min_se;
		}

		/* Add Session-Expires to response */
		list_init(&hdrs);
		re_snprintf(hdr_val, sizeof(hdr_val), "%u;refresher=%s",
			    session_interval,
			    refresher_uac ? "uac" : "uas");
		err = custom_hdrs_add(&hdrs, "Session-Expires", "%s", hdr_val);
		if (!err) {
			call_set_custom_hdrs(st->call, &hdrs);
		}
		list_flush(&hdrs);

		/* Update our timer state - we're UAS, so if refresher=uac,
		 * we're not the refresher */
		update_session_timer(st, session_interval, !refresher_uac);
	}
}

static void call_event_handler(struct call *call, enum call_event ev,
			       const char *str, void *arg)
{
	struct sessiontimer *st = arg;
	(void)str;

	if (!st || !call)
		return;

	switch (ev) {

	case CALL_EVENT_CLOSED:
		debug("sessiontimer: call closed\n");
		mem_deref(st);
		break;

	default:
		break;
	}
}

static void event_handler(enum ua_event ev, struct bevent *event, void *arg)
{
	struct ua *ua = bevent_get_ua(event);
	struct call *call = bevent_get_call(event);
	const struct sip_msg *msg = bevent_get_msg(event);
	struct sessiontimer *st;
	(void)arg;

	if (!call)
		return;

	st = find_timer(call);

	switch (ev) {

	case UA_EVENT_CALL_OUTGOING:
		/* Create session timer for outgoing call */
		if (!st) {
			st = mem_zalloc(sizeof(*st), destructor);
			if (!st)
				return;

			st->call = call;
			st->session_interval = default_session_interval;
			st->min_se = default_min_se;
			st->is_refresher = true; /* We initiate, so we refresh */

			list_append(&sessiontimers, &st->le, st);
			call_set_handlers(call, call_event_handler, NULL, st);

			/* Add Session-Expires header to initial INVITE */
			add_session_headers(call, st->session_interval,
					    st->min_se, true);
		}
		break;

	case UA_EVENT_CALL_INCOMING:
		/* Create session timer for incoming call */
		if (!st) {
			st = mem_zalloc(sizeof(*st), destructor);
			if (!st)
				return;

			st->call = call;
			st->session_interval = 0;
			st->min_se = default_min_se;
			st->is_refresher = false; /* UAS, wait to see who refreshes */

			list_append(&sessiontimers, &st->le, st);
			call_set_handlers(call, call_event_handler, NULL, st);

			/* Process incoming request if available */
			if (msg) {
				process_incoming_request(st, msg);
			}
		}
		break;

	case UA_EVENT_CALL_ESTABLISHED:
		/* Call is established, ensure timer is active */
		if (st && st->active && st->is_refresher) {
			refresh_timer(st);
		}
		/* Also check if we received a re-INVITE with Session-Expires */
		if (st && msg && !call_is_outgoing(call)) {
			/* We're UAS, check for Session-Expires in incoming re-INVITE */
			process_incoming_request(st, msg);
		}
		break;


	case UA_EVENT_CALL_REMOTE_SDP:
		/* Response received, check for Session-Expires */
		if (st && msg) {
			if (msg->scode >= 200 && msg->scode < 300) {
				process_2xx_response(st, msg);
			}
			else if (msg->scode == 422) {
				handle_422_response(st, msg);
				/* Retry the request with adjusted headers */
				if (st->min_se > 0) {
					call_modify(st->call);
				}
			}
		}
		/* Also check for incoming re-INVITE with Session-Expires */
		if (st && msg && !call_is_outgoing(call) &&
		    pl_strcmp(&msg->met, "INVITE") == 0) {
			/* This is a re-INVITE, process Session-Expires */
			process_incoming_request(st, msg);
		}
		break;

	case UA_EVENT_CALL_LOCAL_SDP:
		/* Re-INVITE being sent, add session headers if active */
		if (st) {
			if (st->active) {
				/* This is a refresh, use current values */
				add_session_headers(call, st->session_interval,
						    st->min_se, st->is_refresher);
			}
			else if (st->session_interval > 0) {
				/* Initial request, add headers */
				add_session_headers(call, st->session_interval,
						    st->min_se, st->is_refresher);
			}
		}
		break;

	default:
		break;
	}
}

static int module_init(void)
{
	list_init(&sessiontimers);

	bevent_register(event_handler, NULL);

	info("sessiontimer: module loaded (default interval=%u, min=%u)\n",
	     default_session_interval, default_min_se);

	return 0;
}

static int module_close(void)
{
	debug("sessiontimer: module closing..\n");

	if (!list_isempty(&sessiontimers)) {
		info("sessiontimer: flushing %u timers\n",
		     list_count(&sessiontimers));
		list_flush(&sessiontimers);
	}

	bevent_unregister(event_handler);

	return 0;
}

const struct mod_export DECL_EXPORTS(sessiontimer) = {
	"sessiontimer",
	"application",
	module_init,
	module_close
};
