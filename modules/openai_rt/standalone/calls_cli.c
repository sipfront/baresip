/**
 * @file calls_cli.c  openai_rt call layer for the standalone: tool calls become events
 *
 * In baresip the model's tool calls act on the SIP call (hang up, DTMF, transfer) and the
 * transcript turns are emitted as UA events. Here there is no call object: whoever started this
 * binary owns the call (the agent's modem or Bluetooth driver). So every tool call and every
 * transcript turn is written to stdout as one JSON line, and the caller acts on it (sends the
 * hangup to the modem daemon, publishes the transcript, ...). The api_call tool, which needs
 * nothing of the call, is executed here as in the module, with libre's HTTP client.
 *
 * Events (all carry "ts", seconds since the epoch):
 *   {"event":"tool","name":"hangup_call"}
 *   {"event":"tool","name":"send_dtmf","digits":"1234"}
 *   {"event":"tool","name":"transfer_call","destination":"..."}
 *   {"event":"content","side":"self"|"other","content":"..."}   (VOICEAI_CONTENT)
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include "openai_rt.h"
#include "trace.h"
#include "compat.h"

static struct {
	bool shutting_down;
	bool exit_on_hangup;
	struct dnsc *dnsc;
	pthread_mutex_t out_mutex;
} cs = { .out_mutex = PTHREAD_MUTEX_INITIALIZER };

static double now_ts(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* one JSON object per line on stdout; fmt is the object's body without the braces, the
 * timestamp is added */
void events_emit(const char *fmt, ...)
{
	va_list ap;
	pthread_mutex_lock(&cs.out_mutex);
	re_fprintf(stdout, "{\"ts\":%.3f,", now_ts());
	va_start(ap, fmt);
	re_vfprintf(stdout, fmt, ap);
	va_end(ap);
	re_fprintf(stdout, "}\n");
	fflush(stdout);
	pthread_mutex_unlock(&cs.out_mutex);
}

void calls_cli_set_exit_on_hangup(bool enable)
{
	cs.exit_on_hangup = enable;
}

int calls_init(void)
{
	int err;
	struct sa nsv[16];
	uint32_t nsn = RE_ARRAY_SIZE(nsv);

	cs.shutting_down = false;
	err = dns_srv_get(NULL, 0, nsv, &nsn);
	if (err) {
		warning("openai_rt: no DNS servers for api_call (%m), using 8.8.8.8\n", err);
		sa_set_str(&nsv[0], "8.8.8.8", 53);
		nsn = 1;
	}
	err = dnsc_alloc(&cs.dnsc, NULL, nsv, nsn);
	if (err) {
		warning("openai_rt: dnsc_alloc: %m\n", err);
		return err;
	}
	return 0;
}

void calls_close(void)
{
	cs.shutting_down = true;
	trace_flush();
	trace_write_file();
	cs.dnsc = mem_deref(cs.dnsc);
	g_oairt.call_active = false;
	g_oairt.current_call = NULL;
}

void openai_rt_call_started(struct call *call)
{
	(void)call;
}

void openai_rt_call_ended(void)
{
}

void calls_hangup(void)
{
	if (cs.shutting_down)
		return;
	info("openai_rt: model asked to hang up\n");
	events_emit("\"event\":\"tool\",\"name\":\"hangup_call\"");
	if (cs.exit_on_hangup) {
		g_oairt.call_active = false;
		re_cancel();
	}
}

void calls_send_digit(char key)
{
	(void)key;
}

int calls_send_dtmf(const char *digits)
{
	const char *p;
	char *escaped = NULL;
	int err;

	if (cs.shutting_down)
		return EINTR;
	if (!digits || !*digits)
		return EINVAL;
	for (p = digits; *p; p++) {
		char c = *p;
		bool valid = (c >= '0' && c <= '9') || c == '*' || c == '#' ||
			(c >= 'A' && c <= 'D') || (c >= 'a' && c <= 'd');
		if (!valid) {
			warning("openai_rt: invalid DTMF character '%c' in '%s'\n", c, digits);
			return EINVAL;
		}
	}
	err = json_escape(&escaped, digits);
	if (err)
		return err;
	info("openai_rt: model asked to send DTMF '%s'\n", digits);
	events_emit("\"event\":\"tool\",\"name\":\"send_dtmf\",\"digits\":\"%s\"", escaped);
	mem_deref(escaped);
	return 0;
}

int calls_transfer(const char *destination)
{
	char *escaped = NULL;
	int err;

	if (cs.shutting_down)
		return EINTR;
	if (!destination || !*destination)
		return EINVAL;
	err = json_escape(&escaped, destination);
	if (err)
		return err;
	info("openai_rt: model asked to transfer the call to %s\n", destination);
	events_emit("\"event\":\"tool\",\"name\":\"transfer_call\",\"destination\":\"%s\"",
		    escaped);
	mem_deref(escaped);
	return 0;
}

int calls_queue_voiceai_content(const char *side, const char *content)
{
	char *escaped = NULL;
	int err;

	if (cs.shutting_down)
		return EINTR;
	if (!side || !content)
		return EINVAL;
	if (strcmp(side, "self") != 0 && strcmp(side, "other") != 0)
		return EINVAL;
	err = json_escape(&escaped, content);
	if (err)
		return err;
	events_emit("\"event\":\"content\",\"side\":\"%s\",\"content\":\"%s\"", side, escaped);
	mem_deref(escaped);
	return 0;
}

/* ------------------------------------------------------------------ api_call tool
 * Same shape as the module's: libre HTTP client, a 10 s deadline, and an orphaned request
 * (timeout) frees its state from the late callback instead of from here. */

struct api_call_data {
	char **output;
	struct {
		mtx_t mtx;
		cnd_t cnd;
		bool done;
		bool cancelled;
		bool free_on_complete;
		bool mtx_ok;
		bool cnd_ok;
	} sync;
	int err;
};

static void api_call_data_destructor(void *arg)
{
	struct api_call_data *ad = arg;
	if (ad->sync.cnd_ok)
		cnd_destroy(&ad->sync.cnd);
	if (ad->sync.mtx_ok)
		mtx_destroy(&ad->sync.mtx);
}

static void api_call_resph(int err, const struct http_msg *msg, void *arg)
{
	struct api_call_data *ad = arg;
	bool free_on_complete;

	mtx_lock(&ad->sync.mtx);
	if (!ad->sync.cancelled) {
		ad->err = err;
		if (!err && msg && msg->mb && ad->output) {
			size_t len = mbuf_get_left(msg->mb);
			*ad->output = mem_zalloc(len + 1, NULL);
			if (*ad->output)
				memcpy(*ad->output, mbuf_buf(msg->mb), len);
		}
	}
	ad->sync.done = true;
	free_on_complete = ad->sync.free_on_complete;
	cnd_signal(&ad->sync.cnd);
	mtx_unlock(&ad->sync.mtx);
	if (free_on_complete)
		mem_deref(ad);
}

int calls_api_call(const char *method, const char *uri, const char *content_type,
		   const char *auth_type, const char *auth_username, const char *auth_password,
		   const char *body, char **output)
{
	struct http_cli *cli = NULL;
	struct http_reqconn *conn = NULL;
	struct api_call_data *ad = NULL;
	struct pl pl_met, pl_uri;
	struct mbuf *mb_body = NULL;
	bool left_re = false;
	bool orphaned = false;
	int err;

	if (!method || !uri || !output)
		return EINVAL;

	DEBUG_INFO("calls_api_call: %s %s\n", method, uri);
	re_thread_enter();

	ad = mem_zalloc(sizeof(*ad), api_call_data_destructor);
	if (!ad) {
		err = ENOMEM;
		goto out;
	}
	ad->output = output;
	if (mtx_init(&ad->sync.mtx, mtx_plain) != thrd_success) {
		err = ENOMEM;
		goto out;
	}
	ad->sync.mtx_ok = true;
	if (cnd_init(&ad->sync.cnd) != thrd_success) {
		err = ENOMEM;
		goto out;
	}
	ad->sync.cnd_ok = true;

	err = http_client_alloc(&cli, cs.dnsc);
	if (err) {
		warning("openai_rt: http_client_alloc failed: %m\n", err);
		goto out;
	}
	err = http_reqconn_alloc(&conn, cli, api_call_resph, NULL, ad);
	if (err) {
		warning("openai_rt: http_reqconn_alloc failed: %m\n", err);
		goto out;
	}

	pl_set_str(&pl_met, method);
	http_reqconn_set_method(conn, &pl_met);
	if (content_type) {
		struct pl pl_ct;
		pl_set_str(&pl_ct, content_type);
		http_reqconn_set_ctype(conn, &pl_ct);
	}
	if (auth_type && auth_username) {
		if (strcmp(auth_type, "bearer") == 0) {
			struct pl pl_b;
			pl_set_str(&pl_b, auth_username);
			http_reqconn_set_bearer(conn, &pl_b);
		}
		else if (strcmp(auth_type, "basic") == 0) {
			char *auth_buf = NULL, *b64_buf = NULL, *hdr_buf = NULL;
			size_t b64_len;
			re_sdprintf(&auth_buf, "%s:%s", auth_username,
				    auth_password ? auth_password : "");
			if (auth_buf) {
				b64_len = (strlen(auth_buf) * 4 / 3) + 4;
				b64_buf = mem_zalloc(b64_len, NULL);
				if (b64_buf && !base64_encode((uint8_t *)auth_buf, strlen(auth_buf),
							     b64_buf, &b64_len)) {
					re_sdprintf(&hdr_buf, "Authorization: Basic %s", b64_buf);
					if (hdr_buf) {
						struct pl pl_hdr;
						pl_set_str(&pl_hdr, hdr_buf);
						http_reqconn_add_header(conn, &pl_hdr);
					}
				}
			}
			mem_deref(hdr_buf);
			mem_deref(b64_buf);
			mem_deref(auth_buf);
		}
	}
	if (body) {
		mb_body = mbuf_alloc(strlen(body));
		if (!mb_body) {
			err = ENOMEM;
			goto out;
		}
		mbuf_write_str(mb_body, body);
		mbuf_set_pos(mb_body, 0);
		http_reqconn_set_body(conn, mb_body);
		if (!content_type) {
			struct pl pl_ct = PL("text/plain");
			http_reqconn_set_ctype(conn, &pl_ct);
		}
	}

	pl_set_str(&pl_uri, uri);
	err = http_reqconn_send(conn, &pl_uri);
	if (err) {
		warning("openai_rt: http_reqconn_send failed: %m\n", err);
		goto out;
	}

	re_thread_leave();
	left_re = true;

	mtx_lock(&ad->sync.mtx);
	if (!ad->sync.done) {
		struct timespec ts;
		timespec_get(&ts, TIME_UTC);
		ts.tv_sec += 10;
		while (!ad->sync.done) {
			int rc = cnd_timedwait(&ad->sync.cnd, &ad->sync.mtx, &ts);
			if (rc == thrd_timedout || rc != thrd_success) {
				if (!ad->sync.done) {
					warning("openai_rt: calls_api_call %s\n",
						rc == thrd_timedout ? "timed out after 10s"
								    : "wait failed");
					ad->err = rc == thrd_timedout ? ETIMEDOUT : EPIPE;
					ad->sync.cancelled = true;
					ad->output = NULL;
					ad->sync.free_on_complete = true;
					orphaned = true;
				}
				break;
			}
		}
	}
	err = ad->err;
	mtx_unlock(&ad->sync.mtx);

	re_thread_enter();
	left_re = false;

out:
	mem_deref(conn);
	mem_deref(cli);
	mem_deref(mb_body);
	if (!orphaned)
		mem_deref(ad);
	if (!left_re)
		re_thread_leave();
	return err;
}
