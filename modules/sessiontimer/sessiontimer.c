/**
 * @file sessiontimer.c Session Timer module (RFC 4028)
 *
 * Copyright (C) 2025
 */
#include <string.h>
#include <re.h>
#include <baresip.h>

/**
 * Session Timer module implementing RFC 4028
 *
 * Observes SIP via UA events, call_offer_post_register(), and
 * call_answer_prep_register(). Session-Expires on outgoing INVITE uses call
 * custom headers; in-dialog messages use call_set_sess_hdrs(). Refresh 200 OK
 * responses restart the timer via sip_resp_handler.
 */

#define MIN_SESSION_INTERVAL 90
#define DEFAULT_SESSION_INTERVAL 1800
#define REFRESH_FACTOR 2

enum st_refresher {
	ST_REF_NONE = 0,
	ST_REF_UAC,
	ST_REF_UAS,
};

struct sessiontimer {
	struct le le;
	struct call *call;
	struct tmr tmr;
	struct tmr defer_tmr;
	uint32_t session_interval;
	uint32_t min_se;
	uint64_t session_expires;
	enum st_refresher refresher;
	uint32_t pending_interval;
	enum st_refresher pending_refresher;
	bool is_refresher;
	bool active;
	bool pending_restart;
	uint32_t retry_count;
};

static struct list sessiontimers;
static struct sip_lsnr *lsnr_resp;

static uint32_t default_min_se = MIN_SESSION_INTERVAL;
static uint32_t default_session_interval = DEFAULT_SESSION_INTERVAL;
static bool module_enabled = true;


static void reload_sessiontimer_config(void)
{
	uint32_t interval = DEFAULT_SESSION_INTERVAL;
	uint32_t min_se = MIN_SESSION_INTERVAL;
	int err;

	err = conf_get_u32(conf_cur(), "sessiontimer_interval", &interval);
	if (err) {
		warning("sessiontimer: sessiontimer_interval missing in config, "
			"using %u seconds\n", DEFAULT_SESSION_INTERVAL);
		interval = DEFAULT_SESSION_INTERVAL;
	}
	else if (interval < MIN_SESSION_INTERVAL) {
		warning("sessiontimer: sessiontimer_interval %u too low, "
			"using %u seconds\n", interval, MIN_SESSION_INTERVAL);
		interval = MIN_SESSION_INTERVAL;
	}

	err = conf_get_u32(conf_cur(), "sessiontimer_min_se", &min_se);
	if (err)
		min_se = MIN_SESSION_INTERVAL;
	else if (min_se < MIN_SESSION_INTERVAL)
		min_se = MIN_SESSION_INTERVAL;

	default_session_interval = interval;
	default_min_se = min_se;
}


/* RFC 4028 §9: UAS MAY reduce Session-Expires in 2xx but MUST NOT go
 * below Min-SE from the request (or 90 seconds). */
static uint32_t uas_answer_interval(uint32_t invite_interval,
				    uint32_t invite_min_se)
{
	uint32_t interval = invite_interval;

	if (!interval)
		return 0;

	if (interval > default_session_interval) {
		info("sessiontimer: shortening interval %u to %u "
		     "(local config)\n",
		     interval, default_session_interval);
		interval = default_session_interval;
	}

	if (invite_min_se && interval < invite_min_se)
		interval = invite_min_se;

	if (interval < MIN_SESSION_INTERVAL)
		interval = MIN_SESSION_INTERVAL;

	return interval;
}


static void timer_ext_enable(struct ua *ua)
{
	if (!ua)
		return;

	ua_add_extension(ua, "timer");
}


static void timer_ext_disable(struct ua *ua)
{
	if (!ua)
		return;

	ua_remove_extension(ua, "timer");
}


static void timer_ext_enable_all(void)
{
	struct le *le;

	for (le = list_head(uag_list()); le; le = le->next)
		timer_ext_enable(le->data);
}


static void timer_ext_disable_all(void)
{
	struct le *le;

	for (le = list_head(uag_list()); le; le = le->next)
		timer_ext_disable(le->data);
}


static void parse_msg_session_params(const struct sip_msg *msg,
				     uint32_t *interval,
				     uint32_t *min_se,
				     enum st_refresher *refresher);

static void tmr_handler(void *arg);
static void negotiate_from_msg(struct sessiontimer *st,
			       const struct sip_msg *msg, bool request,
			       bool restart_timer);
static void update_session_timer(struct sessiontimer *st,
				 uint32_t session_interval,
				 enum st_refresher refresher);
static void schedule_timer_restart(struct sessiontimer *st,
				   uint32_t interval,
				   enum st_refresher refresher);
static enum st_refresher default_refresher_msg(const struct call *call,
					       bool request);
static void start_session_timer(struct sessiontimer *st);
static size_t format_session_headers(char *hdrs, size_t sz,
				     uint32_t session_interval,
				     uint32_t min_se, enum st_refresher refresher,
				     bool require_timer);
static void add_session_headers(struct call *call, uint32_t session_interval,
				uint32_t min_se, enum st_refresher refresher,
				bool require_timer);
static void process_incoming_request(struct sessiontimer *st,
				     const struct sip_msg *msg);
static void prepare_answer_headers(struct sessiontimer *st);


static int hdr_text_copy(const struct sip_msg *msg, const char *name,
			 char *buf, size_t bufsz)
{
	const struct sip_hdr *hdr;

	if (!msg || !name || !buf || !bufsz)
		return EINVAL;

	buf[0] = '\0';

	if (!msg->hdrht)
		return ENOENT;

	hdr = sip_msg_xhdr(msg, name);
	if (!hdr)
		return ENOENT;

	if (!hdr->val.p || !hdr->val.l)
		return ENOENT;

	if (hdr->val.l >= bufsz)
		return EOVERFLOW;

	memcpy(buf, hdr->val.p, hdr->val.l);
	buf[hdr->val.l] = '\0';

	return 0;
}


static int parse_session_expires_str(const char *s, uint32_t *delta_seconds,
				     enum st_refresher *refresher)
{
	uint32_t delta = 0;
	enum st_refresher ref = ST_REF_NONE;
	const char *p;

	if (!s || !*s)
		return EINVAL;

	p = s;
	while (*p == ' ' || *p == '\t')
		++p;

	while (*p >= '0' && *p <= '9') {
		delta = delta * 10u + (uint32_t)(*p - '0');
		++p;
	}

	if (delta < MIN_SESSION_INTERVAL)
		return EINVAL;

	if (strstr(s, "refresher=uac"))
		ref = ST_REF_UAC;
	else if (strstr(s, "refresher=uas"))
		ref = ST_REF_UAS;

	if (delta_seconds)
		*delta_seconds = delta;
	if (refresher)
		*refresher = ref;

	return 0;
}


static int parse_min_se_str(const char *s, uint32_t *min_se)
{
	uint32_t mse = 0;
	const char *p;

	if (!s || !*s || !min_se)
		return EINVAL;

	p = s;
	while (*p == ' ' || *p == '\t')
		++p;

	while (*p >= '0' && *p <= '9') {
		mse = mse * 10u + (uint32_t)(*p - '0');
		++p;
	}

	if (mse < MIN_SESSION_INTERVAL)
		return EINVAL;

	*min_se = mse;
	return 0;
}


static int parse_session_expires(const struct pl *hdr_val,
				 uint32_t *delta_seconds,
				 enum st_refresher *refresher)
{
	char buf[128];

	if (!hdr_val || !hdr_val->p || !hdr_val->l)
		return EINVAL;

	if (hdr_val->l >= sizeof(buf))
		return EOVERFLOW;

	memcpy(buf, hdr_val->p, hdr_val->l);
	buf[hdr_val->l] = '\0';

	return parse_session_expires_str(buf, delta_seconds, refresher);
}


static int parse_min_se(const struct pl *hdr_val, uint32_t *min_se)
{
	char buf[64];

	if (!hdr_val || !hdr_val->p || !hdr_val->l)
		return EINVAL;

	if (hdr_val->l >= sizeof(buf))
		return EOVERFLOW;

	memcpy(buf, hdr_val->p, hdr_val->l);
	buf[hdr_val->l] = '\0';

	return parse_min_se_str(buf, min_se);
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


static bool match_call_dialog(const struct call *call,
			      const struct sip_msg *msg)
{
	return call_dialog_cmp(call, &msg->callid);
}


struct call_match_ctx {
	struct call *call;
	const struct sip_msg *msg;
};

static void pick_call_handler(struct call *call, void *arg)
{
	struct call_match_ctx *ctx = arg;

	if (ctx->call)
		return;

	if (match_call_dialog(call, ctx->msg))
		ctx->call = call;
}

static struct call *find_call_by_msg(const struct sip_msg *msg)
{
	struct call_match_ctx ctx = {NULL, msg};

	if (!msg)
		return NULL;

	uag_filter_calls(pick_call_handler, NULL, &ctx);

	return ctx.call;
}


static bool refresher_is_local(const struct sessiontimer *st)
{
	if (!st)
		return false;

	if (call_is_outgoing(st->call))
		return st->refresher == ST_REF_UAC;

	return st->refresher == ST_REF_UAS;
}


static const char *refresher_param(enum st_refresher ref)
{
	switch (ref) {

	case ST_REF_UAC:  return "uac";
	case ST_REF_UAS:  return "uas";
	default:          return "uac";
	}
}


static enum st_refresher local_refresher(const struct sessiontimer *st)
{
	if (call_is_outgoing(st->call))
		return ST_REF_UAC;

	return ST_REF_UAS;
}


static void refresh_timer(struct sessiontimer *st);


static void refresh_timer(struct sessiontimer *st)
{
	uint64_t refresh_time;

	if (!st->active || !st->is_refresher)
		return;

	refresh_time = (st->session_interval * 1000) / REFRESH_FACTOR;
	st->session_expires = tmr_jiffies() + (st->session_interval * 1000);

	info("sessiontimer: scheduling refresh in %u seconds "
	     "(session expires in %u seconds)\n",
	     (uint32_t)(refresh_time / 1000), st->session_interval);

	tmr_start(&st->tmr, refresh_time, tmr_handler, st);
}


static void destructor(void *arg)
{
	struct sessiontimer *st = arg;

	tmr_cancel(&st->defer_tmr);
	tmr_cancel(&st->tmr);
	list_unlink(&st->le);
}


static void start_session_timer(struct sessiontimer *st);


static void defer_work_handler(void *arg)
{
	struct sessiontimer *st = arg;
	uint32_t interval;
	enum st_refresher refresher;

	if (!st)
		return;

	if (!st->pending_restart)
		return;

	interval = st->pending_interval;
	refresher = st->pending_refresher;
	st->pending_restart = false;

	info("sessiontimer: defer restart timer interval=%u refresher=%s\n",
	     interval, refresher_param(refresher));

	update_session_timer(st, interval, refresher);
	st->retry_count = 0;
	start_session_timer(st);
	info("sessiontimer: session timer restarted (interval=%u, "
	     "refresher=%s)\n",
	     interval, refresher_param(refresher));
}


static void schedule_defer_work(struct sessiontimer *st)
{
	if (!st)
		return;

	tmr_start(&st->defer_tmr, 1, defer_work_handler, st);
}


static void schedule_timer_restart(struct sessiontimer *st,
				   uint32_t interval,
				   enum st_refresher refresher)
{
	if (!st)
		return;

	st->pending_interval = interval;
	st->pending_refresher = refresher;
	st->pending_restart = true;
	schedule_defer_work(st);
}


static void handle_refresh_2xx_response(struct sessiontimer *st,
					const struct sip_msg *msg)
{
	uint32_t interval = 0;
	uint32_t min_se = 0;
	enum st_refresher refresher = ST_REF_NONE;
	uint32_t restart_iv;
	enum st_refresher restart_ref;

	if (!st || !msg || call_state(st->call) != CALL_STATE_ESTABLISHED)
		return;

	info("sessiontimer: refresh 2xx %u %r\n", msg->scode, &msg->cseq.met);

	parse_msg_session_params(msg, &interval, &min_se, &refresher);

	if (interval) {
		if (min_se > st->min_se)
			st->min_se = min_se;
		if (st->min_se && interval < st->min_se)
			interval = st->min_se;
		if (refresher == ST_REF_NONE)
			refresher = default_refresher_msg(st->call, false);
		restart_iv = interval;
		restart_ref = refresher;
	}
	else if (st->session_interval) {
		info("sessiontimer: no Session-Expires in 2xx, keep "
		     "interval=%u\n", st->session_interval);
		restart_iv = st->session_interval;
		restart_ref = st->refresher != ST_REF_NONE ?
			      st->refresher : local_refresher(st);
	}
	else {
		return;
	}

	schedule_timer_restart(st, restart_iv, restart_ref);
}


static void refresh_answer_handler(struct call *call,
				   const struct sip_msg *msg)
{
	struct sessiontimer *st;

	if (!call || !msg)
		return;

	st = find_timer(call);
	if (!st)
		return;

	handle_refresh_2xx_response(st, msg);
}


static void update_session_timer(struct sessiontimer *st,
				 uint32_t session_interval,
				 enum st_refresher refresher)
{
	if (!st)
		return;

	st->session_interval = session_interval;
	st->refresher = refresher;
	st->is_refresher = refresher_is_local(st);
	st->active = true;
	st->session_expires = tmr_jiffies() + (session_interval * 1000);

	info("sessiontimer: session interval=%u, refresher=%s, "
	     "local_refresher=%s, expires in %u seconds\n",
	     session_interval, refresher_param(refresher),
	     st->is_refresher ? "yes" : "no", session_interval);
}


static void start_session_timer(struct sessiontimer *st)
{
	uint32_t margin;

	if (!st || !st->active)
		return;

	if (st->is_refresher) {
		refresh_timer(st);
		return;
	}

	margin = st->session_interval / 3;

	if (margin > 32)
		margin = 32;

	if (margin >= st->session_interval)
		margin = st->session_interval / 2;

	info("sessiontimer: waiting for peer refresh, "
	     "expire margin %u seconds\n", margin);
	tmr_start(&st->tmr,
		  (st->session_interval - margin) * 1000,
		  tmr_handler, st);
}


static enum st_refresher default_refresher_msg(const struct call *call,
					       bool request)
{
	/* RFC 4028: without refresher param, the UA that generated the
	 * message is the refresher.  Peer requests come from the remote
	 * role; peer responses echo the answering role. */
	if (request)
		return call_is_outgoing(call) ? ST_REF_UAS : ST_REF_UAC;

	return call_is_outgoing(call) ? ST_REF_UAS : ST_REF_UAC;
}


static void negotiate_from_msg(struct sessiontimer *st,
			       const struct sip_msg *msg, bool request,
			       bool restart_timer)
{
	uint32_t session_interval = 0;
	uint32_t min_se = 0;
	enum st_refresher refresher = ST_REF_NONE;

	if (!st || !msg)
		return;

	parse_msg_session_params(msg, &session_interval, &min_se, &refresher);

	if (!session_interval)
		return;

	if (min_se > st->min_se)
		st->min_se = min_se;

	if (st->min_se && session_interval < st->min_se)
		session_interval = st->min_se;

	if (refresher == ST_REF_NONE)
		refresher = default_refresher_msg(st->call, request);

	if (!restart_timer && st->session_interval &&
	    st->session_interval != session_interval) {
		info("sessiontimer: peer negotiated interval=%u "
		     "(proposed %u)\n",
		     session_interval, st->session_interval);
	}

	update_session_timer(st, session_interval, refresher);

	if (restart_timer) {
		info("sessiontimer: scheduling session timer restart "
		     "(interval=%u, refresher=%s)\n",
		     session_interval, refresher_param(refresher));
		schedule_timer_restart(st, session_interval, refresher);
	}
	else {
		info("sessiontimer: negotiated interval=%u, refresher=%s\n",
		     session_interval, refresher_param(refresher));
	}
}


static void handle_peer_refresh_request(struct sessiontimer *st,
					const struct sip_msg *msg)
{
	uint32_t invite_interval = 0;
	uint32_t invite_min_se = 0;
	uint32_t session_interval;
	enum st_refresher refresher = ST_REF_NONE;
	enum st_refresher reply_ref;
	char hdrs[384];
	size_t n;

	if (!st || !msg)
		return;

	info("sessiontimer: peer refresh [1] %r from peer\n", &msg->met);

	parse_msg_session_params(msg, &invite_interval, &invite_min_se,
				 &refresher);

	if (!invite_interval) {
		info("sessiontimer: peer refresh [2] no Session-Expires, skip\n");
		return;
	}

	info("sessiontimer: peer refresh [2] parsed interval=%u refresher=%s "
	     "min_se=%u\n", invite_interval, refresher_param(refresher),
	     invite_min_se);

	if (invite_min_se > st->min_se)
		st->min_se = invite_min_se;

	session_interval = uas_answer_interval(invite_interval, invite_min_se);
	if (!session_interval) {
		info("sessiontimer: peer refresh [3] interval rejected\n");
		return;
	}

	info("sessiontimer: peer refresh [3] answer interval=%u\n",
	     session_interval);

	if (refresher == ST_REF_NONE)
		refresher = default_refresher_msg(st->call, true);

	reply_ref = refresher;
	n = format_session_headers(hdrs, sizeof(hdrs), session_interval,
				   st->min_se, reply_ref, false);
	if (!n) {
		warning("sessiontimer: peer refresh [4] format headers failed\n");
		return;
	}

	info("sessiontimer: peer refresh [4] headers ready (%zu bytes)\n", n);
	info("sessiontimer: peer refresh [5] staging headers on call\n");

	call_stage_sess_hdrs(st->call, hdrs);

	info("sessiontimer: peer refresh [6] staged, restart timer\n");
	schedule_timer_restart(st, session_interval, refresher);
	info("sessiontimer: peer refresh [7] done\n");
}


static void offer_post_handler(struct call *call, const struct sip_msg *msg)
{
	struct sessiontimer *st;

	if (!call || !msg)
		return;

	info("sessiontimer: offer_post enter %r\n", &msg->met);

	st = find_timer(call);
	if (!st) {
		warning("sessiontimer: offer_post no timer for call\n");
		return;
	}

	handle_peer_refresh_request(st, msg);
	info("sessiontimer: offer_post leave %r\n", &msg->met);
}


static size_t format_session_headers(char *hdrs, size_t sz,
				     uint32_t session_interval,
				     uint32_t min_se, enum st_refresher refresher,
				     bool require_timer)
{
	size_t n = 0;

	if (!hdrs || !sz)
		return 0;

	if (session_interval > 0) {
		n += re_snprintf(hdrs + n, sz - n,
				 "Session-Expires: %u;refresher=%s\r\n",
				 session_interval, refresher_param(refresher));
	}

	if (min_se > 0 && min_se >= MIN_SESSION_INTERVAL) {
		n += re_snprintf(hdrs + n, sz - n,
				 "Min-SE: %u\r\n", min_se);
	}

	if (require_timer) {
		n += re_snprintf(hdrs + n, sz - n,
				 "Require: timer\r\n");
	}

	return n;
}


static void add_session_headers(struct call *call, uint32_t session_interval,
				uint32_t min_se, enum st_refresher refresher,
				bool require_timer)
{
	char hdrs[384];
	size_t n;
	int err;

	if (!call)
		return;

	n = format_session_headers(hdrs, sizeof(hdrs), session_interval, min_se,
				   refresher, require_timer);
	if (!n)
		return;

	if (!call_set_sess_hdrs(call, hdrs)) {
		info("sessiontimer: set in-dialog headers via sess (%s)\n",
		     hdrs);
		return;
	}

	info("sessiontimer: set in-dialog headers via custom_hdr fallback\n");
	call_custom_hdr_remove(call, "Session-Expires");
	call_custom_hdr_remove(call, "Min-SE");
	call_custom_hdr_remove(call, "Require");

	if (session_interval > 0) {
		err = call_custom_hdr_add(call, "Session-Expires",
					  "%u;refresher=%s", session_interval,
					  refresher_param(refresher));
		if (err)
			warning("sessiontimer: Session-Expires: %m\n", err);
	}

	if (min_se > 0 && min_se >= MIN_SESSION_INTERVAL) {
		err = call_custom_hdr_add(call, "Min-SE", "%u", min_se);
		if (err)
			warning("sessiontimer: Min-SE: %m\n", err);
	}

	if (require_timer) {
		err = call_custom_hdr_add(call, "Require", "timer");
		if (err)
			warning("sessiontimer: Require: %m\n", err);
	}
}


static void parse_msg_session_params(const struct sip_msg *msg,
				     uint32_t *interval,
				     uint32_t *min_se,
				     enum st_refresher *refresher)
{
	char sebuf[128];
	char msebuf[64];
	int err;

	if (interval)
		*interval = 0;
	if (min_se)
		*min_se = 0;
	if (refresher)
		*refresher = ST_REF_NONE;

	warning("sessiontimer: parse [a] Session-Expires lookup\n");

	err = hdr_text_copy(msg, "Session-Expires", sebuf, sizeof(sebuf));
	warning("sessiontimer: parse [b] Session-Expires err=%d\n", err);

	if (!err) {
		uint32_t se = 0;
		enum st_refresher ref = ST_REF_NONE;
		int perr;

		perr = parse_session_expires_str(sebuf, &se, &ref);
		warning("sessiontimer: parse [c] Session-Expires parse err=%d "
			"val=%u\n", perr, se);

		if (!perr) {
			if (interval)
				*interval = se;
			if (refresher)
				*refresher = ref;
		}
	}

	warning("sessiontimer: parse [d] Min-SE lookup\n");

	err = hdr_text_copy(msg, "Min-SE", msebuf, sizeof(msebuf));
	if (!err && min_se) {
		uint32_t mse = 0;

		if (!parse_min_se_str(msebuf, &mse))
			*min_se = mse;
	}

	warning("sessiontimer: parse [e] done interval=%u\n",
		interval ? *interval : 0);
}


static struct sessiontimer *alloc_timer(struct call *call)
{
	struct sessiontimer *st;

	st = mem_zalloc(sizeof(*st), destructor);
	if (!st)
		return NULL;

	st->call = call;
	st->min_se = default_min_se;
	tmr_init(&st->tmr);
	tmr_init(&st->defer_tmr);
	list_append(&sessiontimers, &st->le, st);

	return st;
}


static void handle_422_response(struct sessiontimer *st,
				const struct sip_msg *msg)
{
	char msebuf[64];
	uint32_t min_se = 0;
	int err;

	if (!st || !msg)
		return;

	err = hdr_text_copy(msg, "Min-SE", msebuf, sizeof(msebuf));
	if (!err) {
		err = parse_min_se_str(msebuf, &min_se);
		if (!err && min_se > st->min_se) {
			st->min_se = min_se;
			info("sessiontimer: 422 response, new Min-SE=%u\n",
			     min_se);
		}
	}

	if (!st->min_se)
		return;

	st->retry_count++;
	if (st->retry_count > 5) {
		warning("sessiontimer: too many 422 retries, giving up\n");
		call_hangup(st->call, 422, "Session Interval Too Small");
		mem_deref(st);
		return;
	}

	if (st->session_interval < st->min_se)
		st->session_interval = st->min_se;

	info("sessiontimer: retrying with session interval=%u\n",
	     st->session_interval);
	add_session_headers(st->call, st->session_interval, st->min_se,
			    local_refresher(st), false);

	if (call_is_outgoing(st->call) &&
	    call_state(st->call) == CALL_STATE_ESTABLISHED)
		(void)call_modify(st->call);
}


static void activate_on_established(struct sessiontimer *st)
{
	enum st_refresher ref;
	const struct sip_msg *msg;

	if (!st || !st->call)
		return;

	info("sessiontimer: call established, activate timer\n");

	/* UAC: parse Session-Expires from 200 OK after media is up.
	 * Must not run from sipsess_answer_handler (reentrancy crash). */
	if (call_is_outgoing(st->call)) {
		msg = call_msg(st->call);
		if (msg)
			negotiate_from_msg(st, msg, false, false);
	}

	if (st->active) {
		start_session_timer(st);
		return;
	}

	if (st->session_interval < MIN_SESSION_INTERVAL) {
		if (call_is_outgoing(st->call))
			st->session_interval = default_session_interval;
		else
			return;
	}

	ref = st->refresher;
	if (ref == ST_REF_NONE)
		ref = call_is_outgoing(st->call) ? ST_REF_UAC : ST_REF_UAS;

	update_session_timer(st, st->session_interval, ref);
	start_session_timer(st);
}


static void prepare_answer_headers(struct sessiontimer *st)
{
	enum st_refresher reply_ref;
	const struct sip_msg *msg;

	if (!st || !st->call)
		return;

	reload_sessiontimer_config();

	msg = call_msg(st->call);
	if (msg)
		process_incoming_request(st, msg);

	if (!st->session_interval)
		return;

	reply_ref = st->refresher;
	if (reply_ref == ST_REF_NONE)
		reply_ref = ST_REF_UAS;

	info("sessiontimer: applying answer headers interval=%u "
	     "refresher=%s\n",
	     st->session_interval, refresher_param(reply_ref));

	add_session_headers(st->call, st->session_interval, st->min_se,
			    reply_ref, reply_ref == ST_REF_UAC);
}


static void answer_prep_handler(struct call *call)
{
	struct sessiontimer *st;

	if (!call || call_is_outgoing(call))
		return;

	st = find_timer(call);
	if (!st) {
		st = alloc_timer(call);
		if (!st)
			return;

		st->refresher = ST_REF_UAS;
		st->is_refresher = false;
	}

	prepare_answer_headers(st);
}


static void process_incoming_request(struct sessiontimer *st,
				     const struct sip_msg *msg)
{
	uint32_t invite_interval = 0;
	uint32_t invite_min_se = 0;
	uint32_t session_interval;
	enum st_refresher refresher = ST_REF_NONE;
	enum st_refresher reply_ref;

	if (!st || !msg)
		return;

	parse_msg_session_params(msg, &invite_interval, &invite_min_se,
				 &refresher);

	if (invite_min_se > st->min_se)
		st->min_se = invite_min_se;

	if (!invite_interval)
		return;

	session_interval = uas_answer_interval(invite_interval, invite_min_se);
	if (!session_interval)
		return;

	if (refresher == ST_REF_NONE)
		refresher = default_refresher_msg(st->call, true);

	reply_ref = refresher;
	add_session_headers(st->call, session_interval, st->min_se, reply_ref,
			    reply_ref == ST_REF_UAC);
	update_session_timer(st, session_interval, refresher);
}


static void tmr_handler(void *arg)
{
	struct sessiontimer *st = arg;
	int err;

	if (!st || !st->call)
		return;

	if (tmr_jiffies() >= st->session_expires) {
		warning("sessiontimer: session expired, sending BYE\n");
		call_hangup(st->call, 408, "Session Timer Expired");
		mem_deref(st);
		return;
	}

	if (!st->is_refresher) {
		uint32_t remain;

		if (tmr_jiffies() >= st->session_expires) {
			warning("sessiontimer: no refresh received, session "
				"expired\n");
			call_hangup(st->call, 408, "Session Timer Expired");
			mem_deref(st);
			return;
		}

		remain = (uint32_t)(st->session_expires - tmr_jiffies());
		if (remain > 5000)
			remain = 5000;

		tmr_start(&st->tmr, remain, tmr_handler, st);
		return;
	}

	if (!call_refresh_allowed(st->call)) {
		info("sessiontimer: refresh blocked (pending re-INVITE?), "
		     "retry in 5s\n");
		tmr_start(&st->tmr, 5000, tmr_handler, st);
		return;
	}

	info("sessiontimer: sending session refresh (interval=%u)\n",
	     st->session_interval);
	add_session_headers(st->call, st->session_interval, st->min_se,
			    local_refresher(st), false);
	err = call_modify(st->call);
	if (err) {
		warning("sessiontimer: refresh failed: %m\n", err);
		tmr_start(&st->tmr, 5000, tmr_handler, st);
	}
	else {
		st->retry_count = 0;
		info("sessiontimer: refresh sent, awaiting 2xx\n");
	}
}


static bool sip_resp_handler(const struct sip_msg *msg, void *arg)
{
	struct call *call;
	struct sessiontimer *st;
	(void)arg;

	if (!msg || msg->req)
		return false;

	if (pl_strcmp(&msg->cseq.met, "INVITE") &&
	    pl_strcmp(&msg->cseq.met, "UPDATE"))
		return false;

	call = find_call_by_msg(msg);
	if (!call)
		return false;

	st = find_timer(call);
	if (!st)
		return false;

	if (msg->scode == 422) {
		handle_422_response(st, msg);
		return false;
	}

	if (msg->scode >= 200 && msg->scode < 300 &&
	    call_state(call) == CALL_STATE_ESTABLISHED) {
		info("sessiontimer: sip_resp %u %r\n", msg->scode,
		     &msg->cseq.met);
		handle_refresh_2xx_response(st, msg);
	}

	return false;
}


static void event_handler(enum ua_event ev, struct bevent *event, void *arg)
{
	struct call *call = bevent_get_call(event);
	struct sessiontimer *st;
	(void)arg;
	(void)event;

	switch (ev) {

	case UA_EVENT_CREATE:
		if (!module_enabled)
			break;

		timer_ext_enable(bevent_get_ua(event));
		break;

	case UA_EVENT_CALL_OUTGOING:
		if (!call)
			break;

		if (find_timer(call))
			break;

		reload_sessiontimer_config();

		st = alloc_timer(call);
		if (!st)
			break;

		st->session_interval = default_session_interval;
		st->refresher = ST_REF_UAC;
		st->is_refresher = true;
		info("sessiontimer: proposing interval=%u on INVITE\n",
		     st->session_interval);
		add_session_headers(call, st->session_interval, st->min_se,
				    ST_REF_UAC, false);
		break;

	case UA_EVENT_CALL_INCOMING:
		if (!call)
			break;

		if (find_timer(call))
			break;

		reload_sessiontimer_config();

		st = alloc_timer(call);
		if (!st)
			break;

		st->session_interval = 0;
		st->refresher = ST_REF_UAS;
		st->is_refresher = false;
		break;

	case UA_EVENT_CALL_ESTABLISHED:
		st = find_timer(call);
		if (st)
			activate_on_established(st);
		break;

	case UA_EVENT_CALL_REMOTE_SDP:
		break;

	case UA_EVENT_CALL_CLOSED:
		st = find_timer(call);
		if (st) {
			debug("sessiontimer: call closed\n");
			mem_deref(st);
		}
		break;

	default:
		break;
	}
}


static int module_init(void)
{
	int err;

	list_init(&sessiontimers);

	conf_get_bool(conf_cur(), "sessiontimer_enable", &module_enabled);
	if (!module_enabled) {
		info("sessiontimer: disabled by config\n");
		return 0;
	}

	reload_sessiontimer_config();

	timer_ext_enable_all();

	err = sip_listen(&lsnr_resp, uag_sip(), false, sip_resp_handler, NULL);
	if (err)
		goto out;

	err = bevent_register(event_handler, NULL);
	if (err)
		goto out;

	call_answer_prep_register(answer_prep_handler);
	call_offer_post_register(offer_post_handler);
	call_refresh_answer_register(refresh_answer_handler);

	info("sessiontimer: module loaded (interval=%u, min=%u)\n",
	     default_session_interval, default_min_se);

	return 0;

 out:
	timer_ext_disable_all();
	lsnr_resp = mem_deref(lsnr_resp);
	return err;
}


static int module_close(void)
{
	debug("sessiontimer: module closing..\n");

	lsnr_resp = mem_deref(lsnr_resp);
	bevent_unregister(event_handler);
	call_answer_prep_unregister(answer_prep_handler);
	call_offer_post_unregister(offer_post_handler);
	call_refresh_answer_unregister(refresh_answer_handler);
	timer_ext_disable_all();

	if (!list_isempty(&sessiontimers)) {
		info("sessiontimer: flushing %u timers\n",
		     list_count(&sessiontimers));
		list_flush(&sessiontimers);
	}

	return 0;
}


const struct mod_export DECL_EXPORTS(sessiontimer) = {
	"sessiontimer",
	"application",
	module_init,
	module_close
};
