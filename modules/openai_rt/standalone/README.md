# sf-voiceai: openai_rt as a standalone sound device

The `openai_rt` baresip module talks to the OpenAI Realtime or Gemini Live API and acts as the
sound card of a SIP call. `sf-voiceai` is the same implementation without baresip: it takes the
far end's audio from a PulseAudio/PipeWire source and plays the model's speech into a sink, so
any call whose audio runs through PipeWire can be driven by the voice AI: the Bluetooth phone
driver and the 5G modem driver of the Sipfront agent, whose calls use the virtual devices
`SfVirtualSpeakerSink` (far end) and `SfVirtualMicSink` (what the far end hears).

The module's sources are compiled unchanged (`websocket.c`, `openai.c`, `gemini.c`,
`ai_model.c`, `utils.c`, `trace.c`); only the baresip-bound layers are swapped:

| module | standalone | what it does |
|---|---|---|
| `audio.c` (ausrc/auplay/aufilt) | `audio_pulse.c` | capture source → uplink batches to the model; model audio → injection buffer → playback sink. OpenAI listens at 24 kHz, Gemini at 16 kHz, both speak at 24 kHz; PipeWire resamples. |
| `calls.c` (baresip call ops, UA events) | `calls_cli.c` | tool calls and transcript turns are JSON events on stdout; `api_call` runs here (libre HTTP) |
| `openai_rt.c` (module init) | `main.c` | config, lifecycle, signals, stdin commands |
| baresip `info`/`warning`/`conf_cur`/`conf_path_get` | `compat.c` | log to stderr, config from libre's conf module |

## Usage

```
sf-voiceai --config voiceai.conf [--source SfVirtualSpeakerSink.monitor] [--sink SfVirtualMicSink]
           [--trace-dir DIR] [--exit-on-hangup] [--no-stdin]
```

`voiceai.conf` uses the module's keys in baresip config syntax, the same lines the agent renders
into the baresip config for a voicebot call:

```
openai_rt_backend       openai_realtime        # or gemini_live
openai_rt_api_key       ek_...                 # ephemeral token (sk-... works too)
openai_rt_openai_model  gpt-realtime-2.1
openai_rt_prompt        "You are ..."
openai_rt_wait_for_greeting yes
openai_rt_voice         marin
openai_rt_temperature   0.5
openai_rt_tool_calls    "hangup_call,send_dtmf"
openai_rt_transcribe    yes
openai_rt_trace_dir     /usr/share/sipfront-agent/workspace/artifacts
```

The session starts at once (the caller starts the binary when the call is up, exactly where it
started `pw-play` before). It ends on SIGTERM/SIGINT, on `stop` on stdin, on stdin EOF, or with
`--exit-on-hangup` when the model calls `hangup_call`. The conversation trace
(`conversation-trace.json`, when `openai_rt_transcribe` is on) is written at the end.

stdout, one JSON object per line, each with `ts`:

```
{"ts":...,"event":"started","backend":"openai_realtime","source":"...","sink":"..."}
{"ts":...,"event":"content","side":"self","content":"Hello, I would like to ..."}   # our model
{"ts":...,"event":"content","side":"other","content":"..."}                         # the far end
{"ts":...,"event":"tool","name":"hangup_call"}
{"ts":...,"event":"tool","name":"send_dtmf","digits":"1234"}
{"ts":...,"event":"tool","name":"transfer_call","destination":"..."}
{"ts":...,"event":"error","error":"cannot open the audio devices"}
{"ts":...,"event":"stopped"}
```

Tool calls are not executed here (there is no call object): the caller owns the call and acts
on the event, e.g. the modem driver sends the hangup to `mgw agentd`. The log goes to stderr.

## Building

Needs libre 3.x (which includes rem), libwebsockets, json-c, libpulse-simple and this
checkout's baresip headers (types only; nothing of libbaresip is linked).

```
cmake -S modules/openai_rt/standalone -B build-voiceai && cmake --build build-voiceai
```

For the Sipfront agent image (Debian 12, libre 3.23.0): `modules/openai_rt/standalone/build.sh`
builds in a container and leaves `build/sf-voiceai`; the agent-base image builds it after
baresip and ships it as `/usr/local/bin/sf-voiceai`.

## Quick test against the virtual devices

In the agent container (PulseAudio on PipeWire, the Sf virtual devices present):

```
sf-voiceai --config /tmp/voiceai.conf --no-stdin 2>voiceai.log | tee events.jsonl
pactl list short clients | grep sf-voiceai          # its two streams
```

With a Bluetooth or modem call active the far end hears the model; without a call, play a file
into `SfVirtualSpeakerSink` (`pw-play --target SfVirtualSpeakerSink file.wav`) and the model
answers into `SfVirtualMicSink`.
