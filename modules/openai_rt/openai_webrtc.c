/**
 * @file openai_webrtc.c  OpenAI Realtime WebRTC backend
 *
 * Owns a libdatachannel rtc_pc per SIP call. Inside one DTLS session this
 * carries:
 *   - a bidirectional Opus audio track (RTP / SRTP / UDP)
 *   - an SCTP data channel labelled "oai-events" carrying JSON events
 *
 * Audio flows:
 *   SIP RTP -> baresip auplay -> audio.c PCM queues -> oai_webrtc_send_audio
 *     -> Opus encode -> rtcSendMessage(audio_track) -> RTP/SRTP -> OpenAI
 *
 *   OpenAI -> RTP/SRTP -> libdatachannel -> track message cb (this file)
 *     -> Opus decode -> write_to_injection_buffer -> ausrc -> SIP RTP
 *
 * Events flow:
 *   builder (openai.c) -> oai_webrtc_dc_send -> rtcSendMessage(dc)
 *   rtcMessageCallback(dc) -> openai_parse_message -> existing callbacks
 *
 * The audio.c PCM pipeline (auplay/ausrc threads, ring buffer, injection
 * buffer, background-noise mixing point) is preserved — this backend changes
 * only the producer/consumer of those queues.
 *
 * Threading: libdatachannel calls our C callbacks from its internal worker
 * threads. Audio injection is safe (injection_buffer has its own mutex).
 * Event-emitting paths (function_call, response.done) are routed through the
 * existing mqueue-based handoff in calls.c so baresip events fire on the RE
 * main thread.
 *
 * NOTE: This file is built only when USE_OPENAI_WEBRTC=ON (and HAVE_OPENAI_WEBRTC
 * is defined). It links against libdatachannel and libopus.
 *
 * Copyright (C) 2026 Sipfront
 */

#include "openai_rt.h"
#include "ai_model.h"
#include "openai_webrtc.h"

#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <json-c/json.h>
#include <rtc/rtc.h>
#include <opus/opus.h>

#define OPENAI_WEBRTC_MODEL          "gpt-realtime"
#define OAI_EVENTS_DC_LABEL          "oai-events"
#define OAI_OPUS_PAYLOAD_TYPE        111
#define OAI_OPUS_CLOCK_RATE          48000
#define OAI_OPUS_FRAME_MS            20
#define OAI_OPUS_FRAME_SAMPLES       (OAI_OPUS_CLOCK_RATE * OAI_OPUS_FRAME_MS / 1000)
#define OAI_OPUS_MAX_PACKET_BYTES    1500
#define OAI_SETUP_TIMEOUT_MS         15000

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

/*
 * The data-channel side of openai_rt was originally implemented for the
 * WebSocket backend in websocket.c. The function-call routing into calls.c
 * (hangup_call / send_dtmf / api_call) and the response.done emission via
 * UA_EVENT_OPENAI_RESPONSE are implemented there as static handlers tied to
 * the WS message loop. We re-derive the same dispatch here, calling the
 * call-management functions directly. If you ever extract those handlers in
 * websocket.c into a shared file, point both backends at the shared copy.
 */

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
 * oai_webrtc_close_call. Single-instance because the module supports one
 * active SIP call at a time (mirrors g_oairt.current_call).
 */
struct oai_webrtc_state {
	int rtc_pc;                /* libdatachannel peer-connection id (>0 valid) */
	int rtc_audio_track;
	int rtc_dc_events;

	/* Codecs */
	OpusEncoder *enc;
	OpusDecoder *dec;

	/* Setup synchronization */
	pthread_t setup_thread;
	bool setup_thread_started;
	atomic_int gather_state;   /* rtcGatheringState (0=new, 1=gathering, 2=complete) */
	atomic_int conn_state;     /* rtcState */
	atomic_bool dc_open;
	pthread_mutex_t cv_mtx;
	pthread_cond_t cv_cond;

	/* SDP / auth strings (allocated with mem_alloc; free with mem_deref) */
	char *eph_key;
	char *local_sdp;
	char *answer_sdp;

	/* Lifecycle flag — set to true in close_call so async callbacks can
	 * see we're shutting down and avoid touching torn-down state. */
	atomic_bool closing;
};

static struct oai_webrtc_state *g_state;
static pthread_mutex_t g_state_mtx = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/* libdatachannel callbacks                                           */
/* ------------------------------------------------------------------ */

static void on_local_description(int pc, const char *sdp, const char *type,
				 void *ptr)
{
	(void)pc; (void)ptr;
	info("openai_rt: webrtc: local description ready (type=%s, %zu bytes)\n",
	     type ? type : "?", sdp ? strlen(sdp) : 0);
}

static void on_local_candidate(int pc, const char *cand, const char *mid,
			       void *ptr)
{
	(void)pc; (void)ptr;
	(void)cand; (void)mid;
	/* OpenAI's WebRTC API expects a single SDP POST containing all
	 * candidates (no trickle), so we ignore per-candidate notifications
	 * and read the consolidated SDP only after gathering completes. */
}

static void on_state_change(int pc, rtcState state, void *ptr)
{
	(void)pc;
	struct oai_webrtc_state *st = ptr;
	if (!st)
		return;

	atomic_store(&st->conn_state, (int)state);
	info("openai_rt: webrtc: peer-connection state -> %d\n", (int)state);

	pthread_mutex_lock(&st->cv_mtx);
	pthread_cond_broadcast(&st->cv_cond);
	pthread_mutex_unlock(&st->cv_mtx);
}

static void on_gathering_state_change(int pc, rtcGatheringState state,
				      void *ptr)
{
	(void)pc;
	struct oai_webrtc_state *st = ptr;
	if (!st)
		return;

	atomic_store(&st->gather_state, (int)state);
	info("openai_rt: webrtc: gathering state -> %d\n", (int)state);

	pthread_mutex_lock(&st->cv_mtx);
	pthread_cond_broadcast(&st->cv_cond);
	pthread_mutex_unlock(&st->cv_mtx);
}

/* Data-channel callbacks ------------------------------------------- */

static void dc_handle_function_call(const char *call_id, const char *name,
				    const char *arguments, void *arg);
static void dc_handle_response_done(const char *response_json, void *arg);
static void dc_handle_session_updated(void *arg);
static void dc_handle_speech_started(void *arg);

static void on_dc_open(int dc, void *ptr)
{
	(void)dc;
	struct oai_webrtc_state *st = ptr;
	if (!st)
		return;

	atomic_store(&st->dc_open, true);
	info("openai_rt: webrtc: data channel \"%s\" open\n", OAI_EVENTS_DC_LABEL);

	pthread_mutex_lock(&st->cv_mtx);
	pthread_cond_broadcast(&st->cv_cond);
	pthread_mutex_unlock(&st->cv_mtx);
}

static void on_dc_closed(int dc, void *ptr)
{
	(void)dc;
	struct oai_webrtc_state *st = ptr;
	if (!st)
		return;

	atomic_store(&st->dc_open, false);
	info("openai_rt: webrtc: data channel closed\n");
}

static void on_dc_message(int dc, const char *message, int size, void *ptr)
{
	(void)dc;
	struct oai_webrtc_state *st = ptr;

	if (!st || atomic_load(&st->closing) || !message)
		return;

	/* libdatachannel convention: size < 0 => null-terminated string,
	 * size >= 0 => binary payload of that length. OpenAI events are
	 * always JSON text. */
	const char *json_str = NULL;
	char *tmp = NULL;
	if (size < 0) {
		json_str = message;
	}
	else {
		tmp = mem_zalloc((size_t)size + 1, NULL);
		if (!tmp)
			return;
		memcpy(tmp, message, (size_t)size);
		tmp[size] = '\0';
		json_str = tmp;
	}

	openai_parse_message(json_str,
			     /* audio_delta_cb */ NULL, /* not used over DC */
			     dc_handle_session_updated,
			     dc_handle_speech_started,
			     dc_handle_function_call,
			     dc_handle_response_done,
			     st);

	mem_deref(tmp);
}

/* Audio-track receive callback ------------------------------------- */

static void on_audio_rx(int track, const char *data, int size, void *ptr)
{
	(void)track;
	struct oai_webrtc_state *st = ptr;
	int16_t pcm[OAI_OPUS_FRAME_SAMPLES * 2];
	int decoded;

	if (!st || atomic_load(&st->closing) || !st->dec || !data || size <= 0)
		return;

	/* When rtcSetOpusPacketizationHandler + rtcChainRtcpReceivingSession
	 * are wired, libdatachannel hands us the bare Opus payload here (RTP
	 * header already stripped). If that ever changes, depacketize the RTP
	 * header off the front before calling opus_decode. */
	decoded = opus_decode(st->dec,
			      (const unsigned char *)data, size,
			      pcm, OAI_OPUS_FRAME_SAMPLES, /*decode_fec*/ 0);
	if (decoded <= 0) {
		warning("openai_rt: webrtc: opus_decode failed: %d\n", decoded);
		return;
	}

	/* write_to_injection_buffer expects the call's negotiated sample rate;
	 * the existing WS path resamples 24k bot audio to 48k via auresamp at
	 * the SIP boundary. Here we deliver 48k mono PCM16 directly into the
	 * same injection buffer. Configure ausrc_srate/auplay_srate to match
	 * 48000 for the WebRTC backend (see README). */
	int err = write_to_injection_buffer(pcm, (size_t)decoded);
	if (err) {
		warning("openai_rt: webrtc: write_to_injection_buffer failed: %m\n",
			err);
	}
}

/* ------------------------------------------------------------------ */
/* Event dispatch (data channel -> baresip)                           */
/* ------------------------------------------------------------------ */

static void dc_handle_session_updated(void *arg)
{
	(void)arg;
	g_oairt.session_cfg_applied = true;
	g_oairt.session_ready = true;
	info("openai_rt: webrtc: session.updated received\n");
}

static void dc_handle_speech_started(void *arg)
{
	(void)arg;
	/* User started talking — clear pending bot audio in the injection
	 * buffer to support fast turntaking, mirroring the WS handler. */
	mtx_lock(&g_audio.injection_buffer_mutex);
	g_audio.injection_read_pos = g_audio.injection_write_pos;
	g_audio.injection_available = 0;
	mtx_unlock(&g_audio.injection_buffer_mutex);
	info("openai_rt: webrtc: speech_started — flushed injection buffer\n");
}

static void dc_handle_function_call(const char *call_id, const char *name,
				    const char *arguments, void *arg)
{
	(void)arg;
	if (!name || !call_id || !arguments)
		return;

	info("openai_rt: webrtc: function_call name=%s call_id=%s\n",
	     name, call_id);

	/* Match the dispatch in websocket.c's WS path. The tools live in
	 * calls.c and are transport-agnostic. */
	if (strcmp(name, "hangup_call") == 0) {
		calls_hangup();
	}
	else if (strcmp(name, "send_dtmf") == 0) {
		/* arguments is a JSON object {"digits":"..."} — keep parsing
		 * minimal here; reuse json-c rather than ad-hoc string find. */
		struct json_object *root = json_tokener_parse(arguments);
		if (root) {
			struct json_object *digits_obj = NULL;
			if (json_object_object_get_ex(root, "digits",
						      &digits_obj)) {
				const char *digits =
					json_object_get_string(digits_obj);
				if (digits)
					calls_send_dtmf(digits);
			}
			json_object_put(root);
		}
	}
	else if (strcmp(name, "api_call") == 0) {
		/* Mirrors the WS path: parse args, dispatch to calls_api_call,
		 * then post a function_call_output event back over the DC. */
		struct json_object *root = json_tokener_parse(arguments);
		const char *method = NULL, *uri = NULL;
		const char *content_type = NULL, *auth_type = NULL;
		const char *auth_username = NULL, *auth_password = NULL;
		const char *body = NULL;
		char *output = NULL;
		struct json_object *o = NULL;

		if (!root)
			return;

		if (json_object_object_get_ex(root, "method", &o))
			method = json_object_get_string(o);
		if (json_object_object_get_ex(root, "uri", &o))
			uri = json_object_get_string(o);
		if (json_object_object_get_ex(root, "content_type", &o))
			content_type = json_object_get_string(o);
		if (json_object_object_get_ex(root, "auth_type", &o))
			auth_type = json_object_get_string(o);
		if (json_object_object_get_ex(root, "auth_username", &o))
			auth_username = json_object_get_string(o);
		if (json_object_object_get_ex(root, "auth_password", &o))
			auth_password = json_object_get_string(o);
		if (json_object_object_get_ex(root, "body", &o))
			body = json_object_get_string(o);

		(void)calls_api_call(method, uri, content_type, auth_type,
				     auth_username, auth_password, body,
				     &output);

		char *out_json = NULL;
		if (openai_build_function_call_output(call_id,
						      output ? output : "",
						      &out_json) == 0
		    && out_json) {
			oai_webrtc_dc_send(out_json);
			mem_deref(out_json);
		}
		mem_deref(output);
		json_object_put(root);
	}
	else {
		warning("openai_rt: webrtc: unknown function_call name '%s'\n",
			name);
	}
}

static void dc_handle_response_done(const char *response_json, void *arg)
{
	(void)arg;
	if (!response_json)
		return;
	/* Routes to the RE main thread via mqueue and emits
	 * UA_EVENT_OPENAI_RESPONSE — same as the WS path. */
	calls_queue_openai_response(response_json);
}

/* ------------------------------------------------------------------ */
/* Setup thread                                                       */
/* ------------------------------------------------------------------ */

static int read_local_sdp(int pc, char **sdp_out)
{
	int needed, n;
	char *buf;

	needed = rtcGetLocalDescription(pc, NULL, 0);
	if (needed <= 0)
		return EPROTO;

	buf = mem_alloc((size_t)needed + 1, NULL);
	if (!buf)
		return ENOMEM;

	n = rtcGetLocalDescription(pc, buf, needed + 1);
	if (n <= 0) {
		mem_deref(buf);
		return EPROTO;
	}

	buf[n] = '\0';
	*sdp_out = buf;
	return 0;
}

static int wait_until(struct oai_webrtc_state *st,
		      bool (*pred)(struct oai_webrtc_state *), int timeout_ms)
{
	int rc = 0;
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += timeout_ms / 1000;
	deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec += 1;
		deadline.tv_nsec -= 1000000000L;
	}

	pthread_mutex_lock(&st->cv_mtx);
	while (!pred(st) && !atomic_load(&st->closing)) {
		rc = pthread_cond_timedwait(&st->cv_cond, &st->cv_mtx,
					    &deadline);
		if (rc == ETIMEDOUT)
			break;
	}
	bool ok = pred(st);
	pthread_mutex_unlock(&st->cv_mtx);

	return ok ? 0 : ETIMEDOUT;
}

static bool pred_gather_complete(struct oai_webrtc_state *st)
{
	return atomic_load(&st->gather_state) == RTC_GATHERING_COMPLETE;
}

static bool pred_dc_open(struct oai_webrtc_state *st)
{
	return atomic_load(&st->dc_open);
}

static void *setup_thread_main(void *arg)
{
	struct oai_webrtc_state *st = arg;
	int err = 0;
	int opus_err;

	info("openai_rt: webrtc: setup thread starting\n");

	/* 1. Allocate Opus codec ----------------------------------------- */
	st->enc = opus_encoder_create(OAI_OPUS_CLOCK_RATE, /*channels*/ 1,
				      OPUS_APPLICATION_VOIP, &opus_err);
	if (!st->enc || opus_err != OPUS_OK) {
		warning("openai_rt: webrtc: opus_encoder_create failed: %d\n",
			opus_err);
		err = ENOSYS;
		goto fail;
	}
	opus_encoder_ctl(st->enc, OPUS_SET_BITRATE(32000));
	opus_encoder_ctl(st->enc, OPUS_SET_INBAND_FEC(1));
	opus_encoder_ctl(st->enc, OPUS_SET_PACKET_LOSS_PERC(10));

	st->dec = opus_decoder_create(OAI_OPUS_CLOCK_RATE, /*channels*/ 1,
				      &opus_err);
	if (!st->dec || opus_err != OPUS_OK) {
		warning("openai_rt: webrtc: opus_decoder_create failed: %d\n",
			opus_err);
		err = ENOSYS;
		goto fail;
	}

	/* 2. Create peer connection -------------------------------------- */
	rtcConfiguration cfg = {0};
	const char *ice_servers[] = { "stun:stun.l.google.com:19302" };
	cfg.iceServers = ice_servers;
	cfg.iceServersCount = 1;
	cfg.disableAutoNegotiation = false;

	int pc = rtcCreatePeerConnection(&cfg);
	if (pc < 0) {
		warning("openai_rt: webrtc: rtcCreatePeerConnection failed: %d\n",
			pc);
		err = ENOSYS;
		goto fail;
	}
	st->rtc_pc = pc;
	rtcSetUserPointer(pc, st);
	rtcSetLocalDescriptionCallback(pc, on_local_description);
	rtcSetLocalCandidateCallback(pc, on_local_candidate);
	rtcSetStateChangeCallback(pc, on_state_change);
	rtcSetGatheringStateChangeCallback(pc, on_gathering_state_change);

	/* 3. Add audio track (sendrecv Opus mono) ------------------------ */
	rtcTrackInit ti = {0};
	ti.direction = RTC_DIRECTION_SENDRECV;
	ti.codec = RTC_CODEC_OPUS;
	ti.payloadType = OAI_OPUS_PAYLOAD_TYPE;
	ti.ssrc = (uint32_t)rand_u32();
	ti.mid = "audio";
	ti.name = "audio";
	ti.msid = "openai-rt";
	ti.trackId = "openai-rt-audio";

	int track = rtcAddTrackEx(pc, &ti);
	if (track < 0) {
		warning("openai_rt: webrtc: rtcAddTrackEx failed: %d\n", track);
		err = ENOSYS;
		goto fail;
	}
	st->rtc_audio_track = track;
	rtcSetUserPointer(track, st);

	/* Configure Opus packetization on the track. With this set,
	 * rtcSendMessage(track, opus_payload, len) wraps in RTP automatically,
	 * and the receive callback is invoked with depacketized Opus payloads
	 * once rtcChainRtcpReceivingSession is called. */
	rtcPacketizationHandlerInit pi = {0};
	pi.ssrc = ti.ssrc;
	pi.payloadType = OAI_OPUS_PAYLOAD_TYPE;
	pi.clockRate = OAI_OPUS_CLOCK_RATE;
	pi.sequenceNumber = (uint16_t)(rand_u32() & 0xFFFF);
	pi.timestamp = rand_u32();
	pi.cname = "openai-rt";
	pi.nalSeparator = RTC_NAL_SEPARATOR_DEFAULT;
	pi.maxFragmentSize = 0;

	if (rtcSetOpusPacketizationHandler(track, &pi) != RTC_ERR_SUCCESS) {
		warning("openai_rt: webrtc: rtcSetOpusPacketizationHandler "
			"failed (continuing without auto-packetization)\n");
	}
	rtcChainRtcpReceivingSession(track);

	rtcSetMessageCallback(track, on_audio_rx);

	/* 4. Create data channel "oai-events" ---------------------------- */
	int dc = rtcCreateDataChannel(pc, OAI_EVENTS_DC_LABEL);
	if (dc < 0) {
		warning("openai_rt: webrtc: rtcCreateDataChannel failed: %d\n",
			dc);
		err = ENOSYS;
		goto fail;
	}
	st->rtc_dc_events = dc;
	rtcSetUserPointer(dc, st);
	rtcSetOpenCallback(dc, on_dc_open);
	rtcSetClosedCallback(dc, on_dc_closed);
	rtcSetMessageCallback(dc, on_dc_message);

	/* 5. Generate offer & wait for ICE gathering --------------------- */
	if (rtcSetLocalDescription(pc, "offer") != RTC_ERR_SUCCESS) {
		warning("openai_rt: webrtc: rtcSetLocalDescription failed\n");
		err = EPROTO;
		goto fail;
	}

	err = wait_until(st, pred_gather_complete, OAI_SETUP_TIMEOUT_MS);
	if (err) {
		warning("openai_rt: webrtc: ICE gathering timed out\n");
		goto fail;
	}

	err = read_local_sdp(pc, &st->local_sdp);
	if (err) {
		warning("openai_rt: webrtc: failed to read local SDP: %m\n",
			err);
		goto fail;
	}

	/* 6. Fetch ephemeral key & POST SDP offer ----------------------- */
	err = oai_webrtc_session_create_ephemeral(g_oairt.api_key,
						  OPENAI_WEBRTC_MODEL,
						  &st->eph_key);
	if (err) {
		warning("openai_rt: webrtc: ephemeral key request failed: %m\n",
			err);
		goto fail;
	}

	err = oai_webrtc_sdp_exchange(st->eph_key, OPENAI_WEBRTC_MODEL,
				      st->local_sdp, &st->answer_sdp);
	if (err) {
		warning("openai_rt: webrtc: SDP exchange failed: %m\n", err);
		goto fail;
	}

	/* 7. Apply remote answer ----------------------------------------- */
	if (rtcSetRemoteDescription(st->rtc_pc, st->answer_sdp, "answer")
	    != RTC_ERR_SUCCESS) {
		warning("openai_rt: webrtc: rtcSetRemoteDescription failed\n");
		err = EPROTO;
		goto fail;
	}

	/* 8. Wait for the data channel to open --------------------------- */
	err = wait_until(st, pred_dc_open, OAI_SETUP_TIMEOUT_MS);
	if (err) {
		warning("openai_rt: webrtc: data channel did not open within "
			"%d ms\n", OAI_SETUP_TIMEOUT_MS);
		goto fail;
	}

	/* 9. Send session.update -- same builder as the WS path ---------- */
	char *session_update_json = NULL;
	if (openai_build_session_update(g_oairt.prompt, &session_update_json)
	    == 0 && session_update_json) {
		oai_webrtc_dc_send(session_update_json);
		mem_deref(session_update_json);
	}

	/* If the bot should greet first, kick off a response.create now. */
	if (!g_oairt.wait_for_greeting) {
		char *resp_json = NULL;
		if (openai_build_response_create(NULL, &resp_json) == 0
		    && resp_json) {
			oai_webrtc_dc_send(resp_json);
			mem_deref(resp_json);
		}
	}

	info("openai_rt: webrtc: setup complete\n");
	return NULL;

fail:
	warning("openai_rt: webrtc: setup failed (err=%d) — closing call\n",
		err);
	/* Caller (calls.c) detects via session_ready timeout and hangs up. */
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Public per-call API                                                */
/* ------------------------------------------------------------------ */

static void state_destructor(void *arg)
{
	struct oai_webrtc_state *st = arg;
	if (!st)
		return;

	if (st->rtc_dc_events > 0)
		rtcDeleteDataChannel(st->rtc_dc_events);
	if (st->rtc_audio_track > 0)
		rtcDeleteTrack(st->rtc_audio_track);
	if (st->rtc_pc > 0)
		rtcClosePeerConnection(st->rtc_pc);
	if (st->rtc_pc > 0)
		rtcDeletePeerConnection(st->rtc_pc);

	if (st->enc)
		opus_encoder_destroy(st->enc);
	if (st->dec)
		opus_decoder_destroy(st->dec);

	mem_deref(st->eph_key);
	mem_deref(st->local_sdp);
	mem_deref(st->answer_sdp);

	pthread_mutex_destroy(&st->cv_mtx);
	pthread_cond_destroy(&st->cv_cond);
}

int oai_webrtc_init_call(void)
{
	int err;

	pthread_mutex_lock(&g_state_mtx);
	if (g_state) {
		warning("openai_rt: webrtc: init_call called while a state "
			"already exists; closing previous one\n");
		pthread_mutex_unlock(&g_state_mtx);
		oai_webrtc_close_call();
		pthread_mutex_lock(&g_state_mtx);
	}

	g_state = mem_zalloc(sizeof(*g_state), state_destructor);
	if (!g_state) {
		pthread_mutex_unlock(&g_state_mtx);
		return ENOMEM;
	}

	atomic_init(&g_state->gather_state, RTC_GATHERING_NEW);
	atomic_init(&g_state->conn_state, RTC_NEW);
	atomic_init(&g_state->dc_open, false);
	atomic_init(&g_state->closing, false);

	pthread_mutex_init(&g_state->cv_mtx, NULL);
	pthread_cond_init(&g_state->cv_cond, NULL);

	err = pthread_create(&g_state->setup_thread, NULL, setup_thread_main,
			     g_state);
	if (err) {
		warning("openai_rt: webrtc: pthread_create(setup) failed: %d\n",
			err);
		mem_deref(g_state);
		g_state = NULL;
		pthread_mutex_unlock(&g_state_mtx);
		return err;
	}
	g_state->setup_thread_started = true;
	pthread_detach(g_state->setup_thread);

	pthread_mutex_unlock(&g_state_mtx);
	return 0;
}

void oai_webrtc_close_call(void)
{
	struct oai_webrtc_state *st;

	pthread_mutex_lock(&g_state_mtx);
	st = g_state;
	g_state = NULL;
	pthread_mutex_unlock(&g_state_mtx);

	if (!st)
		return;

	atomic_store(&st->closing, true);

	/* Wake the setup thread if it's still in cond_timedwait */
	pthread_mutex_lock(&st->cv_mtx);
	pthread_cond_broadcast(&st->cv_cond);
	pthread_mutex_unlock(&st->cv_mtx);

	g_oairt.session_ready = false;
	g_oairt.session_cfg_applied = false;

	mem_deref(st);
	info("openai_rt: webrtc: call closed\n");
}

int oai_webrtc_send_audio(const int16_t *s16, size_t sampc)
{
	struct oai_webrtc_state *st;
	unsigned char opus_buf[OAI_OPUS_MAX_PACKET_BYTES];
	int opus_len;
	int rc;

	pthread_mutex_lock(&g_state_mtx);
	st = g_state;
	pthread_mutex_unlock(&g_state_mtx);

	if (!st || atomic_load(&st->closing) || !st->enc
	    || st->rtc_audio_track <= 0)
		return ENOTCONN;

	/* Opus expects exactly 2.5/5/10/20/40/60 ms frames. The auplay thread
	 * delivers 20 ms frames at the configured rate. We require 48 kHz mono,
	 * i.e. 960 samples per call. If the frame size doesn't match, drop the
	 * frame loudly (config error). */
	if (sampc != OAI_OPUS_FRAME_SAMPLES) {
		static int once = 0;
		if (!once) {
			once = 1;
			warning("openai_rt: webrtc: unexpected frame size %zu "
				"(expected %d for 20ms at %d Hz mono); set "
				"ausrc_srate=auplay_srate=48000 in baresip "
				"config\n",
				sampc, OAI_OPUS_FRAME_SAMPLES,
				OAI_OPUS_CLOCK_RATE);
		}
		return EINVAL;
	}

	opus_len = opus_encode(st->enc, s16, (int)sampc,
			       opus_buf, sizeof(opus_buf));
	if (opus_len <= 0) {
		warning("openai_rt: webrtc: opus_encode failed: %d\n", opus_len);
		return EPROTO;
	}

	rc = rtcSendMessage(st->rtc_audio_track, (const char *)opus_buf,
			    opus_len);
	if (rc != RTC_ERR_SUCCESS) {
		/* Common during ICE/DTLS setup; suppress spam at info level. */
		return EAGAIN;
	}
	return 0;
}

int oai_webrtc_dc_send(const char *json_msg)
{
	struct oai_webrtc_state *st;
	int rc;

	if (!json_msg)
		return EINVAL;

	pthread_mutex_lock(&g_state_mtx);
	st = g_state;
	pthread_mutex_unlock(&g_state_mtx);

	if (!st || atomic_load(&st->closing) || !atomic_load(&st->dc_open)
	    || st->rtc_dc_events <= 0)
		return ENOTCONN;

	/* size = -1 tells libdatachannel this is a null-terminated string
	 * (sent as an SCTP text message). */
	rc = rtcSendMessage(st->rtc_dc_events, json_msg, -1);
	if (rc != RTC_ERR_SUCCESS) {
		warning("openai_rt: webrtc: dc rtcSendMessage failed: %d\n",
			rc);
		return EPROTO;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* ai_model interface stubs                                           */
/* ------------------------------------------------------------------ */

static int openai_webrtc_init(struct openai_rt *ort)
{
	(void)ort;

	if (!str_isset(g_oairt.api_key)) {
		warning("openai_rt: openai_webrtc backend requires "
			"openai_rt_api_key in config\n");
		return EINVAL;
	}

	rtcInitLogger(RTC_LOG_WARNING, NULL);

	info("openai_rt: OpenAI Realtime WebRTC backend selected (model=%s)\n",
	     OPENAI_WEBRTC_MODEL);
	return 0;
}

static void openai_webrtc_close(void)
{
	oai_webrtc_close_call();
	rtcCleanup();
}

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

static int openai_webrtc_build_audio_append(const char *base64_audio,
					    char **json_msg)
{
	(void)base64_audio;
	if (json_msg)
		*json_msg = NULL;
	return 0;
}
