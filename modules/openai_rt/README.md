# Module documentation

# OpenAI Realtime (openai_rt) Module

The `openai_rt` module provides real-time integration between Baresip and the OpenAI API using WebSockets. By implementing a virtual sound card, it passes audio data from calls to the OpenAI realtime API and vice versa, allowing to implement simple AI Voice Bots.

## Overall Architecture

The module starts 3 threads on startup:

* websocket thread
* audio reader thread
* audio writer thread

The two audio threads communicate with the websocket thread via mutexed lists to pass both control data and audio data back and forth in a non-blocking way.

## Websocket Communication 

On startup, when the openai_rt module is loaded, it establishes a websocket connection with the OpenAI realtime API. While in theory, you can use a `sk_xxx` key for authentication, you really want to create externally an ephemeral key first, because the module does NOT take care of defining much of the overall session params such as voice, instructions etc. Instead, use `https://api.openai.com/v1/realtime/client_secrets` to create an ephemeral token `ek_xxx` along with passing your session parameters (voice, instructions etc). Use the returned `value` param in the baresip config `openai_rt_api_key`, so the websocket connection can authenticate.

Once a call is established, the websocket module will start passing audio data from the reader thread towards OpenAI, and will receive audio data from OpenAI back to the writer thread.

## Audio Pipeline

The reader thread continuously captures audio data from the SIP call. Since audio data towards and from OpenAI must be pcm16 mono sampled at 24kHz, make sure to load the `auresamp` module in your baresip config and set the `ausrc_srate` and `auplay_srate` to `24000` and the `ausrc_channels` and `auplay_channels` to `1`, with `ausrc_format` and `auplay_format` to `s16`. The module does NOT do any resampling on its own!

### SIP to OpenAI

Audio is read in the audio.c `ausrc_read_thread()` in a loop, then:

1. The reader thread captures audio from the SIP call via the ausrc interface.
2. Audio data (PCM16 24kHz mono) is encoded to base64.
3. A JSON message with type `input_audio_buffer.append` is created containing the base64 audio.
4. The message is queued to the websocket thread via the `to_openai_queue`.
5. Audio data is NOT accumulated and explicitly committed with an `input_audio_buffer.commit`, the module rather relies on server side VAD to handle turns.
6. If no `response.create` has been sent yet for the session, one is automatically triggered to start the conversation, but only if the `openai_rt_wait_for_greeting` is set to `no`. This is to control whether OpenAI should start talking first without waiting for the other end to talk, or whether the other end is supposed to talk first.

### OpenAI to SIP

Every time the websocket connection receives a `response.output_audio.delta` message from OpenAI:

1. The websocket thread parses the JSON message and extracts the base64 audio payload
2. The base64 audio is decoded back to its raw PCM16 24kHz mono format 
4. The raw audio is written to a circular injection buffer
5. The `auplay_write_thread()` continuously reads from this injection buffer and provides audio samples to the SIP call via the auplay interface

## Configuration

The module requires the following configuration parameters in your baresip config file:

- `openai_rt_api_key` - OpenAI API key (preferably an ephemeral token from `/v1/realtime/client_secrets`)
- `openai_rt_prompt` - The instructions for OpenAI used in `session.update` and `response.create`
- `openai_rt_wait_for_greeting` - Whether OpenAI is instructed to actively start the conversation bu sending a `response.create` message to OpenAI 

### Audio Settings

For proper operation, configure the following audio parameters:

```
ausrc_srate         24000
auplay_srate        24000  
ausrc_channels      1
auplay_channels     1
ausrc_format        s16
auplay_format       s16
```

Also ensure the `auresamp` module is loaded to handle sample rate conversion:

```
module_path         /path/to/modules
module              auresamp.so
module              openai_rt.so
```

## Session Setup

For best results, create an ephemeral token instead of using your main API key:

```bash
curl -X POST https://api.openai.com/v1/realtime/client_secrets \
  -H "Authorization: Bearer YOUR_API_KEY" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "gpt-realtimew",
    "voice": "alloy",
    "instructions": "You are a helpful assistant..."
  }'
```

Use the returned `value` as your `openai_rt_api_key` in the baresip configuration.

## OpenAI Realtime WebRTC backend (optional)

In addition to the WebSocket-based path described above, the module can connect
to the OpenAI Realtime API over **WebRTC**. Audio travels as Opus over RTP/SRTP/UDP
on a media track; events (session.update, response.done, function_call, transcripts,
speech_started, etc.) travel over an SCTP data channel labelled `oai-events`.
Same JSON event shapes as the WebSocket variant.

Why use it: eliminates ~33% base64 inflation and TCP head-of-line blocking that
the WebSocket-over-TCP transport suffers from when carrying real-time audio.

### Build

The WebRTC backend depends on libdatachannel (full WebRTC stack: ICE + DTLS +
SRTP + SCTP) and libopus. It is gated behind a CMake option, **off** by default
so existing builds are unaffected.

```bash
cmake -B build -DUSE_OPENAI_WEBRTC=ON
cmake --build build -j
```

In the Debian docker build, install `libdatachannel-dev` (and `libopus-dev`,
already present) before configuring. If `libdatachannel-dev` is not in your
distro, build from source:

```bash
git clone --recursive https://github.com/paullouisageneau/libdatachannel.git
cd libdatachannel && cmake -B build -DUSE_GNUTLS=0 -DUSE_NICE=0
cmake --build build -j && cmake --install build
```

### Configure

Set the backend in your baresip config:

```
openai_rt_backend   openai_webrtc
openai_rt_api_key   sk-...                # standing API key (used to mint
                                          # ephemeral keys per call)
openai_rt_prompt    You are a helpful voice assistant.
```

The standing API key is used to POST `https://api.openai.com/v1/realtime/sessions`
once per call to obtain an ephemeral `client_secret.value`, which is then used
as the bearer token in the SDP exchange against
`https://api.openai.com/v1/realtime?model=gpt-realtime`.

### What's preserved, what's replaced

The PCM audio pipeline in `audio.c` (auplay/ausrc threads, ring buffers,
injection buffer, background-noise mixing point) is **unchanged**. Only the
producer and consumer of those queues change: instead of base64-encoding into
a JSON message and sending over a WebSocket, the WebRTC backend Opus-encodes
PCM16 frames and pushes them as RTP into the libdatachannel audio track.
Reverse path: incoming RTP → Opus decode → existing `injection_buffer`.