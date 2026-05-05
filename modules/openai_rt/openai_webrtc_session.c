/**
 * @file openai_webrtc_session.c  REST helpers for OpenAI Realtime WebRTC
 *
 * Two synchronous HTTP helpers used by the openai_webrtc backend:
 *
 *   oai_webrtc_session_create_ephemeral()
 *     POST /v1/realtime/sessions  (Authorization: Bearer <api_key>)
 *     Returns the short-lived ephemeral key (client_secret.value).
 *
 *   oai_webrtc_sdp_exchange()
 *     POST /v1/realtime?model=<model>  (Authorization: Bearer <eph_key>,
 *                                       Content-Type: application/sdp)
 *     Sends the SDP offer in the body; returns the SDP answer in the body.
 *
 * Pattern mirrors calls.c calls_api_call: re_thread_enter/leave, http_client +
 * http_reqconn, sync_obj for blocking the caller until the response arrives,
 * 10s timeout.
 *
 * Copyright (C) 2026 Sipfront
 */

#include "openai_rt.h"
#include "openai_webrtc.h"

#include <json-c/json.h>

#define OAI_WEBRTC_HTTP_TIMEOUT_MS 10000

#define OAI_REALTIME_HOST "api.openai.com"
#define OAI_REALTIME_SESSIONS_URL "https://api.openai.com/v1/realtime/sessions"
#define OAI_REALTIME_SDP_URL_FMT "https://api.openai.com/v1/realtime?model=%s"

struct webrtc_sync {
	mtx_t mtx;
	cnd_t cnd;
	bool done;
};

struct webrtc_resp {
	char **body_out;
	size_t *body_len_out;
	struct webrtc_sync *sync;
	int err;
};

static void webrtc_resp_handler(int err, const struct http_msg *msg, void *arg)
{
	struct webrtc_resp *r = arg;
	r->err = err;

	if (!err && msg && msg->mb && r->body_out) {
		size_t len = mbuf_get_left(msg->mb);
		char *buf = mem_zalloc(len + 1, NULL);
		if (buf) {
			memcpy(buf, mbuf_buf(msg->mb), len);
			buf[len] = '\0';
			*r->body_out = buf;
			if (r->body_len_out)
				*r->body_len_out = len;
		}
		else {
			r->err = ENOMEM;
		}
	}

	mtx_lock(&r->sync->mtx);
	r->sync->done = true;
	cnd_signal(&r->sync->cnd);
	mtx_unlock(&r->sync->mtx);
}

/*
 * One-shot blocking HTTPS POST.
 *  - bearer: required Authorization: Bearer token
 *  - content_type: e.g. "application/sdp" or "application/json"
 *  - body: request body (NUL-terminated; length taken with strlen)
 *  - response_body: out, allocated; freed with mem_deref by caller
 *  - response_len: optional, out
 */
static int webrtc_https_post(const char *url, const char *bearer,
			     const char *content_type, const char *body,
			     char **response_body, size_t *response_len)
{
	struct http_cli *cli = NULL;
	struct http_reqconn *conn = NULL;
	struct webrtc_sync sync;
	struct webrtc_resp resp;
	struct pl pl_met, pl_uri, pl_ct, pl_bearer;
	struct mbuf *mb_body = NULL;
	int err;

	if (!url || !bearer || !response_body)
		return EINVAL;

	*response_body = NULL;
	if (response_len)
		*response_len = 0;

	re_thread_enter();

	memset(&resp, 0, sizeof(resp));
	memset(&sync, 0, sizeof(sync));

	err = mtx_init(&sync.mtx, mtx_plain);
	if (err)
		goto out;
	err = cnd_init(&sync.cnd);
	if (err) {
		mtx_destroy(&sync.mtx);
		goto out;
	}
	sync.done = false;
	resp.sync = &sync;
	resp.body_out = response_body;
	resp.body_len_out = response_len;

	err = http_client_alloc(&cli, net_dnsc(baresip_network()));
	if (err) {
		warning("openai_rt: webrtc_https_post: http_client_alloc failed: %m\n",
			err);
		goto cleanup_sync;
	}

#ifdef USE_TLS
	http_client_disable_verify_server(cli);
#endif

	err = http_reqconn_alloc(&conn, cli, webrtc_resp_handler, NULL, &resp);
	if (err) {
		warning("openai_rt: webrtc_https_post: http_reqconn_alloc failed: %m\n",
			err);
		goto cleanup_sync;
	}

	pl_set_str(&pl_met, "POST");
	http_reqconn_set_method(conn, &pl_met);

	if (content_type) {
		pl_set_str(&pl_ct, content_type);
		http_reqconn_set_ctype(conn, &pl_ct);
	}

	pl_set_str(&pl_bearer, bearer);
	http_reqconn_set_bearer(conn, &pl_bearer);

	if (body && *body) {
		mb_body = mbuf_alloc(strlen(body));
		if (!mb_body) {
			err = ENOMEM;
			goto cleanup_sync;
		}
		mbuf_write_str(mb_body, body);
		mbuf_set_pos(mb_body, 0);
		http_reqconn_set_body(conn, mb_body);
	}

	pl_set_str(&pl_uri, url);

	err = http_reqconn_send(conn, &pl_uri);
	if (err) {
		warning("openai_rt: webrtc_https_post: http_reqconn_send failed: %m\n",
			err);
		goto cleanup_sync;
	}

	re_thread_leave();

	mtx_lock(&sync.mtx);
	if (!sync.done) {
		uint64_t start = tmr_jiffies();
		while (!sync.done) {
			cnd_wait(&sync.cnd, &sync.mtx);
			if (tmr_jiffies() - start > OAI_WEBRTC_HTTP_TIMEOUT_MS) {
				warning("openai_rt: webrtc_https_post: timeout after %d ms\n",
					OAI_WEBRTC_HTTP_TIMEOUT_MS);
				resp.err = ETIMEDOUT;
				break;
			}
		}
	}
	mtx_unlock(&sync.mtx);

	re_thread_enter();

	err = resp.err;

cleanup_sync:
	mem_deref(conn);
	mem_deref(cli);
	mem_deref(mb_body);
	mtx_destroy(&sync.mtx);
	cnd_destroy(&sync.cnd);

out:
	re_thread_leave();

	if (err && *response_body) {
		mem_deref(*response_body);
		*response_body = NULL;
	}

	return err;
}

/*
 * POST /v1/realtime/sessions to obtain a short-lived ephemeral key.
 *
 * Body shape (minimal): {"model":"<model>"}
 *
 * Response (parsed): client_secret.value
 */
int oai_webrtc_session_create_ephemeral(const char *api_key, const char *model,
					char **eph_key_out)
{
	char *body = NULL;
	char *resp_body = NULL;
	struct json_object *root = NULL;
	struct json_object *cs_obj = NULL;
	struct json_object *val_obj = NULL;
	const char *eph_str = NULL;
	int err;

	if (!api_key || !model || !eph_key_out)
		return EINVAL;

	*eph_key_out = NULL;

	err = re_sdprintf(&body, "{\"model\":\"%s\"}", model);
	if (err)
		return err;

	err = webrtc_https_post(OAI_REALTIME_SESSIONS_URL, api_key,
				"application/json", body, &resp_body, NULL);
	mem_deref(body);
	if (err) {
		warning("openai_rt: ephemeral key request failed: %m\n", err);
		goto out;
	}

	if (!resp_body) {
		warning("openai_rt: ephemeral key response was empty\n");
		err = EPROTO;
		goto out;
	}

	root = json_tokener_parse(resp_body);
	if (!root) {
		warning("openai_rt: ephemeral key response not valid JSON: %s\n",
			resp_body);
		err = EPROTO;
		goto out;
	}

	if (!json_object_object_get_ex(root, "client_secret", &cs_obj) ||
	    !json_object_object_get_ex(cs_obj, "value", &val_obj) ||
	    !json_object_is_type(val_obj, json_type_string)) {
		warning("openai_rt: ephemeral key response missing "
			"client_secret.value: %s\n", resp_body);
		err = EPROTO;
		goto out;
	}

	eph_str = json_object_get_string(val_obj);
	err = str_dup(eph_key_out, eph_str);

out:
	if (root)
		json_object_put(root);
	mem_deref(resp_body);
	return err;
}

/*
 * POST /v1/realtime?model=<model> with the SDP offer in the body.
 * Returns the SDP answer body verbatim.
 */
int oai_webrtc_sdp_exchange(const char *eph_key, const char *model,
			    const char *offer_sdp, char **answer_sdp_out)
{
	char *url = NULL;
	int err;

	if (!eph_key || !model || !offer_sdp || !answer_sdp_out)
		return EINVAL;

	*answer_sdp_out = NULL;

	err = re_sdprintf(&url, OAI_REALTIME_SDP_URL_FMT, model);
	if (err)
		return err;

	err = webrtc_https_post(url, eph_key, "application/sdp", offer_sdp,
				answer_sdp_out, NULL);
	mem_deref(url);

	if (err) {
		warning("openai_rt: SDP exchange failed: %m\n", err);
		return err;
	}

	if (!*answer_sdp_out || !**answer_sdp_out) {
		warning("openai_rt: SDP exchange returned empty body\n");
		if (*answer_sdp_out) {
			mem_deref(*answer_sdp_out);
			*answer_sdp_out = NULL;
		}
		return EPROTO;
	}

	return 0;
}
