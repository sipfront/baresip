/**
 * @file trace.h  openai_rt conversation-trace capture
 *
 * openai_rt is Sipfront's OWN test bot: it dials the voice bot under test
 * and drives the conversation as a simulated caller. This module accumulates
 * a structured, timestamped trace of that conversation for later scoring by
 * the function-voicebot-eval lambda.
 *
 * Roles are kept generic (this endpoint vs the far end); the lambda maps them
 * to the concrete SIP role (caller/callee) it already knows from the DB when
 * it fetches the artifact:
 *   - TRACE_ROLE_SELF  = this endpoint = the Realtime model's OWN audio
 *                        transcript output (what WE say into the call).
 *   - TRACE_ROLE_OTHER = the far end = the Realtime model's INPUT audio
 *                        transcription (what the other party / bot under
 *                        test says back to us).
 *
 * Copyright (C) 2025 Sipfront
 */
#ifndef OPENAI_RT_TRACE_H
#define OPENAI_RT_TRACE_H

#include <stdbool.h>

/* this endpoint = our own model (output transcript) */
#define TRACE_ROLE_SELF  "SELF"
/* the far end (input transcription) */
#define TRACE_ROLE_OTHER "OTHER"

/* Module lifecycle (call once at module init/close). */
void trace_init(void);
void trace_close(void);

/* Enable/disable capture. When disabled every add_* / write is a no-op, so
 * the default (non-task) voice-AI call behaves exactly as before. */
void trace_set_enabled(bool enabled);
bool trace_enabled(void);

/* Per-call lifecycle. */
/* clear accumulated trace and stamp the call start time */
void trace_reset(void);

/* Capture (safe to call from the WebSocket thread). trace_add_turn coalesces
 * streamed fragments into one turn and, when a turn completes, logs it and
 * emits it as a VOICEAI_CONTENT event (combined text, tagged by side). */
void trace_add_turn(const char *role, const char *text);
void trace_add_toolcall(const char *name, const char *arguments);
void trace_add_event(const char *kind);

/* Emit the final pending turn (call at end of call, before writing the trace
 * file). */
void trace_flush(void);

/* Serialize the accumulated trace to <trace_dir>/conversation-trace.json (or
 * the baresip config dir when no trace_dir is configured). Returns 0 on
 * success and is a no-op returning 0 when capture is disabled. Call from the
 * RE main thread on call close. */
int trace_write_file(void);

#endif /* OPENAI_RT_TRACE_H */
