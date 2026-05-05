/**
 * @file openai_webrtc.c  OpenAI Realtime WebRTC backend
 *
 * Owns the libdatachannel rtc_pc, the Opus audio track, and the data channel
 * "oai-events" for events. Audio travels over the WebRTC RTP audio track (Opus
 * over SRTP/UDP). Events travel over the SCTP data channel. Both are
 * multiplexed inside a single DTLS session managed by libdatachannel.
 *
 * The audio.c PCM pipeline (auplay/ausrc threads, ring buffers, injection
 * buffer) is preserved: this backend just changes the producer/consumer of the
 * existing PCM queues from "base64 + WebSocket" to "Opus encode + RTP track".
 *
 * JSON event shapes are identical to the WebSocket Realtime API, so the JSON
 * builders (openai_build_session_update, _response_create,
 * _function_call_output) and parser (openai_parse_message) are reused
 * verbatim — they are exposed (non-static) from openai.c.
 *
 * Copyright (C) 2026 Sipfront
 */

#include "openai_rt.h"
#include "ai_model.h"
#include "openai_webrtc.h"

#define OPENAI_WEBRTC_MODEL "gpt-realtime"
#define OAI_EVENTS_DC_LABEL "oai-events"

/*
 * Reused from openai.c (these are transport-agnostic JSON helpers; the WS
 * variant uses them too).
 */
extern int openai_build_session_update(const char *prompt, char **json_msg);
extern int openai_build_response_create(const char *instructions,
					char **json_msg);
extern int openai_build_function_call_output(const char *call_id,
					     const char *output,
					     char **json_msg);
extern int openai_parse_message(const char *json_str,
		void (*audio_delta_cb)(const char *base64_audio, void *arg),
		void (*session_updated_cb)(void *arg),
		void (*speech_started_cb)(void *arg),
		void (*function_call_cb)(const char *call_id,
					 const char *name,
					 const char *arguments,
					 void *arg),
		void (*response_done_cb)(const char *response_json, void *arg),
		void *cb_arg);

/* ai_model interface implementation */
static int openai_webrtc_init(struct openai_rt *ort);
static void openai_webrtc_close(void);
static int openai_webrtc_get_connection_info(char *address, size_t address_len,
					     int *port, char *path,
					     size_t path_len);
static int openai_webrtc_add_auth_headers(void *in, size_t len);
static int openai_webrtc_build_audio_append(const char *base64_audio,
					    char **json_msg);

struct ai_model openai_webrtc_model = {
	.name = "openai_webrtc",
	.init = openai_webrtc_init,
	.close = openai_webrtc_close,
	.get_connection_info = openai_webrtc_get_connection_info,
	.add_auth_headers = openai_webrtc_add_auth_headers,
	.build_session_update = openai_build_session_update,
	.build_audio_append = openai_webrtc_build_audio_append,
	.build_response_create = openai_build_response_create,
	.build_function_call_output = openai_build_function_call_output,
	.parse_message = openai_parse_message,
};

/*
 * Per-call state. Allocated in oai_webrtc_init_call, freed in
 * oai_webrtc_close_call. Holds the libdatachannel peer-connection handle, the
 * audio track id, the data-channel id, the ephemeral key, and the negotiated
 * SDP — populated incrementally by the libdatachannel impl in task 5.
 */
struct oai_webrtc_state {
	int rtc_pc;             /* libdatachannel peer-connection id */
	int rtc_audio_track;    /* libdatachannel audio track id */
	int rtc_dc_events;      /* libdatachannel data channel id */
	char *eph_key;
	char *local_sdp;
	char *remote_sdp;
	bool dc_open;
};

/*
 * Module-init hook (not per-call). Today this is a no-op: the peer connection
 * and tracks are torn up and down per SIP call. We use it only to validate the
 * config and log that the WebRTC backend is selected.
 */
static int openai_webrtc_init(struct openai_rt *ort)
{
	(void)ort;

	if (!str_isset(g_oairt.api_key)) {
		warning("openai_rt: openai_webrtc backend requires "
			"openai_rt_api_key in config\n");
		return EINVAL;
	}

	info("openai_rt: OpenAI Realtime WebRTC backend selected (model=%s)\n",
	     OPENAI_WEBRTC_MODEL);
	return 0;
}

static void openai_webrtc_close(void)
{
	/* Per-call state is freed by oai_webrtc_close_call(). */
}

/*
 * The connection-info / auth-headers callbacks belong to the WebSocket abstract
 * interface — the WebRTC backend doesn't use them. Implemented as not-supported
 * so a misconfigured caller fails loudly rather than silently.
 */
static int openai_webrtc_get_connection_info(char *address, size_t address_len,
					     int *port, char *path,
					     size_t path_len)
{
	(void)address; (void)address_len;
	(void)port;
	(void)path; (void)path_len;
	return ENOSYS;
}

static int openai_webrtc_add_auth_headers(void *in, size_t len)
{
	(void)in; (void)len;
	return ENOSYS;
}

/*
 * On the WS path, audio is base64-encoded and pushed as
 * input_audio_buffer.append events. On WebRTC, audio flows over the audio
 * track, never over the data channel. So this builder is a no-op.
 */
static int openai_webrtc_build_audio_append(const char *base64_audio,
					    char **json_msg)
{
	(void)base64_audio;
	if (json_msg)
		*json_msg = NULL;
	return 0;
}

/*
 * Per-call lifecycle and audio I/O. Real implementation in task 5 (libdatachannel
 * peer-connection + Opus encode/decode + data-channel callbacks). Stubs return
 * ENOSYS so the framework is wired and the missing piece is obvious in logs.
 */
int oai_webrtc_init_call(void)
{
	warning("openai_rt: oai_webrtc_init_call: libdatachannel integration "
		"not yet implemented\n");
	return ENOSYS;
}

void oai_webrtc_close_call(void)
{
	/* Stub — see task 5. */
}

int oai_webrtc_send_audio(const int16_t *s16, size_t sampc)
{
	(void)s16; (void)sampc;
	return ENOSYS;
}

int oai_webrtc_dc_send(const char *json_msg)
{
	(void)json_msg;
	return ENOSYS;
}
