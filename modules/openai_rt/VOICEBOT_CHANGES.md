# baresip / openai_rt — enhanced voice-AI test changes

## What changed
- New `trace.c` / `trace.h`: a self-contained, mutex-guarded **conversation-trace** store
  (dialogue turns + observable tool-calls + interruption events) that serializes to
  `conversation-trace.json` on call close.
- OpenAI + Gemini backends now enable **both-direction transcription** in the session/setup
  (gated by the new `openai_rt_transcribe` flag) and parse the transcript events into the
  trace. Role mapping: **CALLER** = our simulated user (model output), **AGENT** = the voice
  bot under test (model input transcription).
- New **observable-action tools** (`record_confirmation_number`, `record_quoted_price`) that
  let the simulated caller record what it heard; captured into the trace and acknowledged.
- New config keys: `openai_rt_transcribe` (bool, default off), `openai_rt_trace_dir` (where
  the trace file is written).
- Build: `trace.c` added to `CMakeLists.txt`. Testing guide in
  `CONVERSATION_TRACE_TESTING.md`.

## Why
`openai_rt` is Sipfront's own test bot — it dials the bot under test and drives the
conversation. Previously only the assistant side and coarse audio metrics were available, so
we could not tell **what the bot actually said** or **whether it did the task**. Capturing a
structured, speaker-labelled transcript (plus simulator-observed facts) is the raw material
the task scorer needs.

## Role in the overall flow
Produces the `conversation-trace.json` artifact for each call. The agent's existing
artifact-upload loop ships it to S3 (no driver change), where `function-voicebot-eval`
consumes it for the LLM-judge transcript, observable-action checks, and turn-taking events.
Everything is **off by default**, so non-task voice calls are unaffected.
