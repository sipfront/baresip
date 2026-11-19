/**
 * @file pipeline.c  Sipfront voice pipeline as an AI model backend
 *
 * The third backend beside OpenAI Realtime and Gemini Live: the simulated
 * party is not a speech-to-speech model here but Sipfront's voice pipeline
 * service, which hears the call through a speech-to-text vendor, answers
 * with a language model and speaks through a text-to-speech vendor -- each
 * chosen per session, which is also where a cloned voice comes from.
 *
 * The module is only the bridge. A session is made by the platform (prompt,
 * vendors, voice, tools); this side joins it with the session's token and
 * then:
 *   - sends the call's audio as binary frames (PCM s16le mono, the rate of
 *     auplay_srate) and plays the binary frames it gets (the rate of
 *     ausrc_srate) -- both rates are named in the hello,
 *   - "ready":  the session is up (as session.updated is for OpenAI),
 *   - "clear":  the far end spoke over the simulated party; drop what is
 *     buffered for playback,
 *   - "timing": how long each step of an answer took; emitted as the
 *     module event "openai_rt,timing,{...}" for whoever reads the call's
 *     events.
 *   - "transcript": what was just said, by either side; it goes into the
 *     conversation trace (openai_rt_transcribe) as the transcripts of the
 *     speech-to-speech models do,
 *   - "hangup" and "dtmf": the simulated party hangs up or presses keys;
 *     carried out by the module's tools (hangup_call, send_dtmf), which
 *     have to be enabled in openai_rt_tool_calls as for the other backends.
 *
 * Turn taking, interruptions and the conversation trace are the service's.
 *
 * Config:
 *   openai_rt_backend       sipfront_pipeline
 *   openai_rt_pipeline_url  wss://host[:port]/v1/ws   (ws:// for a local one)
 *   openai_rt_api_key       the session's token
 *
 * Copyright (C) 2026 Sipfront
 */

#include "openai_rt.h"
#include "ai_model.h"
#include "trace.h"
#include <json-c/json.h>
#include <libwebsockets.h>
#include <stdlib.h>

static int pipeline_init(struct openai_rt *ort);
static void pipeline_close(void);
static int pipeline_get_connection_info(char *address, size_t address_len,
		int *port, char *path, size_t path_len);
static bool pipeline_use_tls(void);
static int pipeline_add_auth_headers(void *in, size_t len);
static int pipeline_build_session_update(const char *prompt,
		char **json_msg);
static int pipeline_build_audio_append(const char *base64_audio,
		char **json_msg);
static int pipeline_build_response_create(const char *instructions,
		char **json_msg);
static int pipeline_build_function_call_output(const char *call_id,
		const char *name, const char *output, char **json_msg);
static int pipeline_parse_message(const char *json_str,
		void (*audio_delta_cb)(const char *base64_audio, void *arg),
		void (*session_updated_cb)(void *arg),
		void (*speech_started_cb)(void *arg),
		void (*function_call_cb)(const char *call_id,
			const char *name, const char *arguments, void *arg),
		void (*response_done_cb)(const char *response_json,
			void *arg),
		void *cb_arg);

struct ai_model pipeline_model = {
	.name = "sipfront_pipeline",
	.init = pipeline_init,
	.close = pipeline_close,
	.get_connection_info = pipeline_get_connection_info,
	.add_auth_headers = pipeline_add_auth_headers,
	.build_session_update = pipeline_build_session_update,
	.build_audio_append = pipeline_build_audio_append,
	.build_response_create = pipeline_build_response_create,
	.build_function_call_output = pipeline_build_function_call_output,
	.parse_message = pipeline_parse_message,
	.binary_audio = true,
	.use_tls = pipeline_use_tls,
};

/* The URL of the service, taken apart once per use: scheme, host, port and
 * path (with its query). */
struct pipeline_url {
	bool tls;
	char host[256];
	int port;
	char path[512];
};

static int parse_url(struct pipeline_url *u)
{
	const char *url = g_oairt.pipeline_url;
	const char *host, *end, *colon, *slash;
	size_t hlen;

	if (!str_isset(url)) {
		warning("openai_rt: openai_rt_pipeline_url is not set\n");
		return EINVAL;
	}

	if (!strncmp(url, "wss://", 6)) {
		u->tls = true;
		host = url + 6;
	}
	else if (!strncmp(url, "ws://", 5)) {
		u->tls = false;
		host = url + 5;
	}
	else {
		warning("openai_rt: openai_rt_pipeline_url must start with"
			" ws:// or wss://\n");
		return EINVAL;
	}

	slash = strchr(host, '/');
	end = slash ? slash : host + strlen(host);
	colon = memchr(host, ':', (size_t)(end - host));
	hlen = (size_t)((colon ? colon : end) - host);
	if (hlen == 0 || hlen >= sizeof(u->host)) {
		warning("openai_rt: openai_rt_pipeline_url has no usable"
			" host\n");
		return EINVAL;
	}
	memcpy(u->host, host, hlen);
	u->host[hlen] = '\0';

	u->port = u->tls ? 443 : 80;
	if (colon) {
		char *rest = NULL;
		long port = strtol(colon + 1, &rest, 10);

		if (rest != end || port < 1 || port > 65535) {
			warning("openai_rt: openai_rt_pipeline_url has a"
				" port that is none\n");
			return EINVAL;
		}
		u->port = (int)port;
	}

	if (slash && strlen(slash) >= sizeof(u->path)) {
		warning("openai_rt: openai_rt_pipeline_url has a path that"
			" is too long\n");
		return EINVAL;
	}
	str_ncpy(u->path, slash ? slash : "/v1/ws", sizeof(u->path));

	return 0;
}

static int pipeline_init(struct openai_rt *ort)
{
	struct pipeline_url u;
	(void)ort;

	/* A URL that cannot be used is said at startup, not at the first
	 * call. */
	return parse_url(&u);
}

static void pipeline_close(void)
{
}

static int pipeline_get_connection_info(char *address, size_t address_len,
		int *port, char *path, size_t path_len)
{
	struct pipeline_url u;
	int err;

	if (!address || !port || !path)
		return EINVAL;

	err = parse_url(&u);
	if (err)
		return err;

	if (address_len < strlen(u.host) + 1 || path_len < strlen(u.path) + 1)
		return EOVERFLOW;

	str_ncpy(address, u.host, address_len);
	str_ncpy(path, u.path, path_len);
	*port = u.port;

	return 0;
}

static bool pipeline_use_tls(void)
{
	struct pipeline_url u;

	/* Unreadable: the connection info fails right after; TLS is the
	 * answer that never sends a token in the clear. */
	return parse_url(&u) ? true : u.tls;
}

/* The session's token names the session and is good once: it goes in the
 * Authorization header, never in the URL, so it is in no access log. */
static int pipeline_add_auth_headers(void *in, size_t len)
{
	unsigned char **p = (unsigned char **)in;
	unsigned char *end = (*p) + len;
	int written;

	if (!str_isset(g_oairt.api_key)) {
		warning("openai_rt: no session token (openai_rt_api_key) for"
			" the voice pipeline\n");
		return -1;
	}

	written = lws_snprintf((char *)*p, end - *p,
			       "Authorization: Bearer %s\r\n",
			       g_oairt.api_key);
	if (written < 0 || written >= (int)(end - *p)) {
		warning("openai_rt: failed to add the voice pipeline's"
			" Authorization header\n");
		return -1;
	}
	*p += written;

	return 0;
}

/* The hello: the rates of the audio this side sends (what the call's audio
 * is resampled to for the model: auplay_srate) and of the audio it wants
 * (what the audio source runs at: ausrc_srate). The service resamples at
 * its edge. The prompt is not sent: the session was made with it. */
static int pipeline_build_session_update(const char *prompt, char **json_msg)
{
	uint32_t in_rate = 16000, out_rate = 24000;
	(void)prompt;

	if (!json_msg)
		return EINVAL;

	(void)conf_get_u32(conf_cur(), "auplay_srate", &in_rate);
	(void)conf_get_u32(conf_cur(), "ausrc_srate", &out_rate);

	return re_sdprintf(json_msg,
		"{\"type\":\"hello\",\"input_rate\":%u,\"output_rate\":%u}",
		in_rate, out_rate);
}

/* Audio goes out as binary frames (binary_audio); nothing is built here. */
static int pipeline_build_audio_append(const char *base64_audio,
		char **json_msg)
{
	(void)base64_audio;

	if (!json_msg)
		return EINVAL;

	*json_msg = NULL;
	return 0;
}

/* The service decides when the simulated party speaks; there is nothing to
 * ask for. */
static int pipeline_build_response_create(const char *instructions,
		char **json_msg)
{
	(void)instructions;

	if (!json_msg)
		return EINVAL;

	*json_msg = NULL;
	return 0;
}

/* What became of a hangup or dtmf event, for the session's trace. The
 * module's tools say "Error: ..." when they failed. */
static int pipeline_build_function_call_output(const char *call_id,
		const char *name, const char *output, char **json_msg)
{
	char *escaped_id = NULL, *escaped_out = NULL;
	bool ok;
	int err;
	(void)name;

	if (!call_id || !json_msg)
		return EINVAL;

	ok = !(output && !strncmp(output, "Error", 5));

	err = json_escape(&escaped_id, call_id);
	if (err)
		return err;
	err = json_escape(&escaped_out, output ? output : "");
	if (err) {
		mem_deref(escaped_id);
		return err;
	}

	err = re_sdprintf(json_msg,
		"{\"type\":\"tool_result\",\"id\":\"%s\",\"ok\":%s,"
		"\"message\":\"%s\"}",
		escaped_id, ok ? "true" : "false", escaped_out);

	mem_deref(escaped_id);
	mem_deref(escaped_out);
	return err;
}

static const char *str_field(struct json_object *obj, const char *key)
{
	struct json_object *v = NULL;

	if (!json_object_object_get_ex(obj, key, &v) ||
	    !json_object_is_type(v, json_type_string))
		return NULL;

	return json_object_get_string(v);
}

/* The service's events. Audio does not come this way (binary frames go
 * straight to the playback buffer), so audio_delta_cb is not used. */
static int pipeline_parse_message(const char *json_str,
		void (*audio_delta_cb)(const char *base64_audio, void *arg),
		void (*session_updated_cb)(void *arg),
		void (*speech_started_cb)(void *arg),
		void (*function_call_cb)(const char *call_id,
			const char *name, const char *arguments, void *arg),
		void (*response_done_cb)(const char *response_json,
			void *arg),
		void *cb_arg)
{
	struct json_object *root;
	const char *type, *id;
	(void)audio_delta_cb;
	(void)response_done_cb;

	if (!json_str)
		return EINVAL;

	root = json_tokener_parse(json_str);
	if (!root || !json_object_is_type(root, json_type_object)) {
		if (root)
			json_object_put(root);
		return EBADMSG;
	}

	type = str_field(root, "type");
	id = str_field(root, "id");
	if (!type) {
		json_object_put(root);
		return EBADMSG;
	}

	if (!strcmp(type, "ready")) {
		info("openai_rt: voice pipeline session ready\n");
		if (session_updated_cb)
			session_updated_cb(cb_arg);
	}
	else if (!strcmp(type, "clear")) {
		if (speech_started_cb)
			speech_started_cb(cb_arg);
	}
	else if (!strcmp(type, "hangup")) {
		const char *reason = str_field(root, "reason");

		info("openai_rt: voice pipeline hangs up (%s)\n",
		     reason ? reason : "no reason given");
		if (function_call_cb)
			function_call_cb(id ? id : "hangup", "hangup_call",
					 "{}", cb_arg);
	}
	else if (!strcmp(type, "dtmf")) {
		const char *digits = str_field(root, "digits");
		struct json_object *args = json_object_new_object();

		/* Built as JSON, not printed: the digits are the service's
		 * words and go through the tool's own validation as any
		 * model's would. */
		if (args) {
			json_object_object_add(args, "digits",
				json_object_new_string(digits ? digits : ""));
			if (function_call_cb)
				function_call_cb(id ? id : "dtmf",
					"send_dtmf",
					json_object_to_json_string(args),
					cb_arg);
			json_object_put(args);
		}
	}
	else if (!strcmp(type, "transcript")) {
		/* What was just said, for the conversation trace the evaluation
		 * reads: the far end's turn as the service understood it, and
		 * the simulated party's words as they begin to play. */
		const char *role = str_field(root, "role");
		const char *text = str_field(root, "text");

		if (role && text && *text) {
			if (!strcmp(role, "assistant"))
				trace_add_turn(TRACE_ROLE_SELF, text);
			else if (!strcmp(role, "user"))
				trace_add_turn(TRACE_ROLE_OTHER, text);
		}
	}
	else if (!strcmp(type, "timing")) {
		/* Where the time of the answer that begins to play went:
		 * handed on as it is, for whoever reads the call's events. */
		struct json_object *timing = NULL;

		if (json_object_object_get_ex(root, "timing", &timing) &&
		    json_object_is_type(timing, json_type_object))
			(void)calls_queue_voiceai_timing(
				json_object_to_json_string_ext(timing,
					JSON_C_TO_STRING_PLAIN));
	}
	else if (!strcmp(type, "error")) {
		const char *code = str_field(root, "code");
		const char *message = str_field(root, "message");

		warning("openai_rt: voice pipeline error (%s): %s\n",
			code ? code : "?", message ? message : "");
	}
	else {
		DEBUG_INFO("openai_rt: voice pipeline event '%s' ignored\n",
			   type);
	}

	json_object_put(root);
	return 0;
}
