/**
 * @file openai_webrtc.h  Internal API for the OpenAI Realtime WebRTC backend
 *
 * Only included when the module is built with USE_OPENAI_WEBRTC=ON.
 * Functions here are private to the openai_rt module — not exported beyond it.
 *
 * Copyright (C) 2026 Sipfront
 */

#ifndef OPENAI_WEBRTC_H
#define OPENAI_WEBRTC_H

#include <stddef.h>

/* Forward declaration of the model state owned by openai_webrtc.c */
struct oai_webrtc_state;

/*
 * REST helpers (openai_webrtc_session.c)
 */

/* POST /v1/realtime/sessions and return client_secret.value (allocated; free
 * with mem_deref). The standing API key authenticates this request.
 */
int oai_webrtc_session_create_ephemeral(const char *api_key, const char *model,
					char **eph_key_out);

/* POST /v1/realtime?model=<model> with offer_sdp as Content-Type:application/sdp
 * body, authenticated with the ephemeral key. The response body is returned
 * verbatim as the SDP answer (allocated; free with mem_deref).
 */
int oai_webrtc_sdp_exchange(const char *eph_key, const char *model,
			    const char *offer_sdp, char **answer_sdp_out);

/*
 * Per-call lifecycle (openai_webrtc.c)
 *
 * Wired from calls.c when g_oairt.backend_type == AI_BACKEND_OPENAI_WEBRTC.
 */
int oai_webrtc_init_call(void);
void oai_webrtc_close_call(void);

/*
 * Audio path (openai_webrtc.c)
 *
 * Encode PCM16 from the SIP-side capture and push as RTP/Opus into the
 * libdatachannel audio track. Counterpart to handle_incoming_audio's WS path.
 */
int oai_webrtc_send_audio(const int16_t *s16, size_t sampc);

/*
 * Data channel send (openai_webrtc.c)
 *
 * Send a JSON event over the "oai-events" data channel. JSON shape is identical
 * to the WebSocket Realtime API events; reuses openai_build_*() builders.
 */
int oai_webrtc_dc_send(const char *json_msg);

#endif /* OPENAI_WEBRTC_H */
