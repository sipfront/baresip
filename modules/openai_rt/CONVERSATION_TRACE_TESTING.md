# openai_rt conversation-trace — build & test instructions

Phase-2 addition to the `openai_rt` module for the tau3-style task-based voicebot
assessment. `openai_rt` is **our own** test bot: it dials the voice bot under test and
drives the conversation as a simulated caller. These changes make it capture a structured,
timestamped trace of that conversation so `function-voicebot-eval` can score task
completion and turn-taking.

> These changes could not be compiled/tested in the authoring environment. This doc is the
> checklist to build and validate them on a real agent host.

## What changed

| File | Change |
|---|---|
| `trace.c`, `trace.h` (new) | Self-contained, mutex-guarded trace store: turns, observable tool-calls, events; serializes to `conversation-trace.json`. |
| `openai.c` | `input_audio_transcription` added to the session update (gated by `openai_rt_transcribe`); parse branches for `conversation.item.input_audio_transcription.completed` → **OTHER** and `response.output_audio_transcript.done` / `response.audio_transcript.done` → **SELF**. New general-purpose observable-event tool def (`record_event`, with `label` + `value`). |
| `gemini.c` | `inputAudioTranscription` + `outputAudioTranscription` added to setup (gated), with `responseModalities` kept AUDIO-only. Parses `serverContent.inputTranscription.text` → **OTHER** and `outputTranscription.text` → **SELF**. Streamed fragments are coalesced by `trace.c`. (Note: `outputAudioTranscription` previously correlated with a turn-taking regression; re-enabled per request — re-test that the caller still yields.) |
| `websocket.c` | `handle_function_call_cb` records observable-action tool-calls into the trace and acks them; `handle_speech_started_cb` records a `speech_started` event. |
| `calls.c` | `trace_reset()` on `UA_EVENT_CALL_ESTABLISHED`; on `UA_EVENT_CALL_CLOSED` flush + start a 3s grace timer, then `trace_write_file()` (WS stays up; trailing transcripts are common). `content_call` keeps a call ref so final `VOICEAI_CONTENT` mqueue events still emit after `current_call` is cleared. |
| `openai_rt.c` / `utils.c` / `openai_rt.h` | Lifecycle (`trace_init`/`trace_close`), config (`openai_rt_transcribe`, `openai_rt_trace_dir`). |
| `ai_model.h` | `extern` decls for the observable-action tools. |
| `CMakeLists.txt` | `trace.c` added to `SRCS`. |

### Role mapping (important)
The trace uses **generic** roles (the module doesn't assume SIP direction); the eval
lambda maps them to the concrete role it already knows from the DB:
- **SELF** = this endpoint = our simulated user = the Realtime model's *own output* transcript.
- **OTHER** = the far end (voice bot under test) = the Realtime model's *input-audio* transcription.

The eval lambda `SELECT`s the artifact's `role` column, so SELF resolves to that fetched role
and OTHER to the session's other party — no role is hardcoded in the lambda.

## Config keys (new)

```
openai_rt_transcribe   yes            # enable input transcription + trace capture (default: no)
openai_rt_trace_dir    /path/to/dir   # where conversation-trace.json is written
```

The agent's `config.tt2` now emits these automatically inside the `openai_rt == 'yes'` block
when the test sets `openai_rt_transcribe = yes`; `openai_rt_trace_dir` is set to the agent's
`artifacts_path` so the file lands in the directory the agent already scans and uploads
(`Worker.pm` `scan_directory($cb, $syscfg->{artifacts})`). If `openai_rt_trace_dir` is unset,
the module falls back to baresip's config dir (`conf_path_get()`), which is **not** uploaded —
so for end-to-end runs the dir must be the artifacts dir.

Everything is **off by default**: with `openai_rt_transcribe` unset, the session update, the
extra tools, and the trace file are all absent, so plain voice / fixed-media calls behave
exactly as before.

## Build

```sh
# in the baresip build tree (same flow as any module change)
cmake --build build --target openai_rt        # or the project's usual `make`
# confirm the module links (trace.c compiled in) and openai_rt.so is produced
```

Watch for: missing `trace.c` in the build (CMake cache stale → reconfigure), and json-c /
libre symbol availability (`json_object_to_json_string_ext`, `fs_fopen`, `fs_isdir`,
`tmr_jiffies`, `conf_path_get` — all already used elsewhere in the module/baresip).

## Manual test (single call)

1. Configure a baresip instance with `openai_rt == yes`, a valid `openai_rt_api_key`, and add:
   ```
   openai_rt_transcribe   yes
   openai_rt_trace_dir    /tmp/sf-trace
   openai_rt_tool_calls   "hangup_call,record_event"
   ```
   (`mkdir -p /tmp/sf-trace` first.)
2. Give it a task-style prompt that instructs the simulated caller to, e.g., confirm a price
   and call `record_event` with `label: "quoted_price"` when the agent states one.
3. Place a call to a real voice bot and let a short conversation happen; hang up.
4. Verify `/tmp/sf-trace/conversation-trace.json` was written and looks like:
   ```json
   {
     "schema": "sipfront.voicebot-trace/1",
     "duration_ms": 42350,
     "turns": [
       { "role": "SELF",  "speaker": "SELF",  "text": "Hi, I'd like ...", "ts_ms": 1200 },
       { "role": "OTHER", "speaker": "OTHER", "text": "Sure, that is ...", "ts_ms": 3800 }
     ],
     "observable_actions": [
       { "name": "quoted_price", "arguments": { "label": "quoted_price", "value": "$40" }, "ts_ms": 15200 }
     ],
     "events": [ { "kind": "speech_started", "ts_ms": 8100 } ]
   }
   ```
5. Check both roles appear: **SELF** turns (our sim) and **OTHER** turns (bot under test).
   If only SELF turns appear, input transcription isn't being returned — see caveats.
   (For Gemini, only **OTHER** turns are captured in-session — see the Gemini caveat.)

## Log checkpoints

- `openai_rt: conversation-trace capture ENABLED (dir: ...)` at module init.
- `openai_rt: trace: wrote <path> (N turns, M actions, K events)` ~3s after call
  close (post-hangup grace for trailing transcripts).
- `openai_rt: Recording observable action '<name>'` when a `record_*` tool fires.

## Caveats to verify against the live APIs (version-sensitive)

The exact request keys and event names differ across Realtime/Live API versions and could
not be verified here:

- **OpenAI**: input transcription is requested as
  `session.audio.input.transcription.model` (GA `type:realtime` session). If the deployed
  API expects the beta top-level `input_audio_transcription`, adjust `audio_block` in
  `openai_build_session_update` (`openai.c`). Assistant-transcript event name is handled for
  both `response.output_audio_transcript.done` and `response.audio_transcript.done`; the
  agent-side event is `conversation.item.input_audio_transcription.completed`. Confirm these
  against the model in use (`OPENAI_TRANSCRIBE_MODEL` defaults to `whisper-1`).
- **Gemini Live**: setup enables `inputAudioTranscription` (OTHER) and `outputAudioTranscription`
  (SELF); transcripts arrive as `serverContent.inputTranscription.text` /
  `outputTranscription.text`. `responseModalities` is kept **AUDIO-only** (adding a TEXT modality
  can make the model loop). Streamed fragments are coalesced into whole turns by `trace.c`
  (TRACE_COALESCE_MS). NOTE: `outputAudioTranscription` previously correlated with a turn-taking
  regression (the caller not yielding); it is re-enabled per request with AUDIO-only modality —
  re-test that the caller still yields and only reconsider disabling it if the loop returns.

Both backends also emit each transcript turn as a `VOICEAI_CONTENT` baresip event
(`{"side":"self|other","content":"..."}`), tagged by side.

If a name is off, transcripts simply won't be captured (no crash) — the eval lambda then
falls back to the Call-Analytics / transcribe metrics, and only observable-action checks and
finer turn-taking degrade.

## End-to-end (with the pipeline)

1. A task-based voicebot test (test_config carries `voicebot_task_json`, tags
   `task:<id>`/`condition:<name>`, `postproc_voicebot_eval`, and `openai_rt_transcribe=yes`).
2. Call runs → `conversation-trace.json` written to the artifacts dir → agent uploads it to
   `sipfront-session-artifacts-<env>` and registers it in `session_artifact_buckets`.
3. `function-voicebot-eval` downloads the trace (`key LIKE '%conversation-trace%'`), builds the
   transcript for the LLM judge, checks `observable_actions`, and writes `task_eval` +
   `turn_taking` metrics.
4. The app report (`/projects/:id/ai-voice-bot-report`) shows Task completion / reliability /
   Voice interaction, per-scenario drill-down, and the clean-vs-realistic retention table.
