/**
 * @file audio_pulse.c  openai_rt audio layer on PulseAudio/PipeWire, for the standalone
 *
 * In baresip the module is a "sound" device: an ausrc that feeds the model's speech into the
 * call and an aufilt tap that takes the far end's decoded audio to the model. Here the two
 * directions are two PulseAudio simple streams against the agent's virtual devices (the same
 * ones pw-play/pw-record use):
 *
 *   capture  <source>  (default SfVirtualSpeakerSink.monitor, the far end)  -> uplink batch ->
 *            model, at the backend's uplink rate (OpenAI 24 kHz, Gemini 16 kHz), PCM16 mono
 *   playback <sink>    (default SfVirtualMicSink, what the far end hears)   <- injection buffer
 *            <- model audio deltas, 24 kHz PCM16 mono; silence while the model is quiet
 *
 * PipeWire resamples between the virtual devices' rates and these, nothing is resampled here.
 * The generic parts (uplink batching and commit, the injection ring buffer, the event queue the
 * WebSocket thread polls) are the module's audio.c, unchanged in behaviour.
 */
#include <string.h>
#include <pthread.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include "openai_rt.h"
#include "ai_model.h"
#include "compat.h"

struct audio_state g_audio = {0};

#define PLAYBACK_SRATE 24000
#define FRAME_MS 20

static struct {
	pa_simple *rec;
	pa_simple *play;
	unsigned uplink_srate;
	pthread_t rec_thread;
	pthread_t play_thread;
	bool rec_running;
	bool play_running;
	bool started;
	pthread_mutex_t batch_mutex;
	uint64_t captured;   /* samples taken from the far end */
	uint64_t played;     /* model samples played (not counting fill silence) */
} pa;

static void send_audio_commit(void);
static void force_audio_commit(void);
static void maybe_shrink_injection_buffer(void);

/* ------------------------------------------------------------------ init / close */

int audio_init(void)
{
	memset(&g_audio, 0, sizeof(g_audio));
	memset(&pa, 0, sizeof(pa));

	g_audio.buffer_size = 160;
	g_audio.commit_threshold = AUDIO_COMMIT_THRESHOLD;

	g_audio.injection_buffer_size = INJECTION_BUFFER_INITIAL_SIZE;
	g_audio.injection_buffer = mem_alloc(
		g_audio.injection_buffer_size * sizeof(int16_t), NULL);
	g_audio.uplink_batch_cap = UPLINK_BATCH_BYTES / sizeof(int16_t);
	g_audio.uplink_batch = mem_alloc(
		g_audio.uplink_batch_cap * sizeof(int16_t), NULL);
	if (!g_audio.injection_buffer || !g_audio.uplink_batch) {
		warning("openai_rt: failed to allocate audio buffers\n");
		return ENOMEM;
	}

	list_init(&g_audio.read_queue);
	list_init(&g_audio.write_queue);
	list_init(&g_audio.event_queue);
	mtx_init(&g_audio.read_queue_mutex, mtx_plain);
	mtx_init(&g_audio.write_queue_mutex, mtx_plain);
	mtx_init(&g_audio.event_queue_mutex, mtx_plain);
	mtx_init(&g_audio.injection_buffer_mutex, mtx_plain);
	cnd_init(&g_audio.read_queue_cond);
	cnd_init(&g_audio.write_queue_cond);
	cnd_init(&g_audio.event_queue_cond);
	pthread_mutex_init(&pa.batch_mutex, NULL);

	DEBUG_INFO("audio (pulse) initialized: injection buffer %zu samples\n",
		   g_audio.injection_buffer_size);
	return 0;
}

static void free_event_queue(void)
{
	struct le *le;
	mtx_lock(&g_audio.event_queue_mutex);
	while ((le = list_head(&g_audio.event_queue))) {
		struct audio_event *event = list_ledata(le);
		list_unlink(le);
		mem_deref(event);
	}
	mtx_unlock(&g_audio.event_queue_mutex);
}

void audio_close(void)
{
	audio_pulse_stop();
	free_event_queue();
	mtx_destroy(&g_audio.read_queue_mutex);
	mtx_destroy(&g_audio.write_queue_mutex);
	mtx_destroy(&g_audio.event_queue_mutex);
	mtx_destroy(&g_audio.injection_buffer_mutex);
	cnd_destroy(&g_audio.read_queue_cond);
	cnd_destroy(&g_audio.write_queue_cond);
	cnd_destroy(&g_audio.event_queue_cond);
	pthread_mutex_destroy(&pa.batch_mutex);
	g_audio.injection_buffer = mem_deref(g_audio.injection_buffer);
	g_audio.uplink_batch = mem_deref(g_audio.uplink_batch);
	DEBUG_INFO("audio (pulse) closed: captured %llu samples, played %llu model samples\n",
		   (unsigned long long)pa.captured, (unsigned long long)pa.played);
}

/* the baresip aufilt registration: nothing to register here */
void audio_register_uplink_filter(void)
{
}

/* ------------------------------------------------------------------ uplink (far end -> model) */

static void queue_pcm_append(const int16_t *s16_data, size_t sampc)
{
	struct ai_model *model = get_ai_model();
	char *json_msg = NULL;
	char *b64;
	size_t byte_len;
	int err;

	if (!s16_data || sampc == 0)
		return;
	if (g_oairt.ws_state != WS_CONNECTED || !g_oairt.ws_client)
		return;
	if (!model || !model->build_audio_append) {
		warning("openai_rt: AI model not initialized\n");
		return;
	}

	byte_len = sampc * sizeof(int16_t);
	b64 = encode_audio_base64((const uint8_t *)s16_data, byte_len);
	if (!b64)
		return;
	err = model->build_audio_append(b64, &json_msg);
	mem_deref(b64);
	if (err || !json_msg)
		return;

	err = queue_message_to_openai(json_msg, str_len(json_msg), NULL, NULL);
	if (err) {
		warning("openai_rt: failed to queue PCM audio: %m\n", err);
	}
	else {
		g_audio.audio_accumulated += byte_len;
		if (g_audio.audio_accumulated >= g_audio.commit_threshold) {
			if (g_oairt.session_cfg_applied)
				send_audio_commit();
			g_audio.audio_accumulated = 0;
		}
	}
	mem_deref(json_msg);
}

/* caller holds pa.batch_mutex */
static void flush_uplink_batch_locked(bool force)
{
	if (!g_audio.uplink_batch || g_audio.uplink_batch_len == 0)
		return;
	if (!force && g_audio.uplink_batch_len * sizeof(int16_t) < UPLINK_BATCH_BYTES)
		return;
	queue_pcm_append(g_audio.uplink_batch, g_audio.uplink_batch_len);
	g_audio.uplink_batch_len = 0;
}

void audio_flush_uplink_batch(void)
{
	pthread_mutex_lock(&pa.batch_mutex);
	flush_uplink_batch_locked(true);
	pthread_mutex_unlock(&pa.batch_mutex);
}

void handle_incoming_audio(const int16_t *s16_data, size_t sampc)
{
	if (!s16_data || sampc == 0 || !g_oairt.call_active || !g_audio.uplink_batch)
		return;

	pthread_mutex_lock(&pa.batch_mutex);
	while (sampc > 0) {
		size_t space, copy;
		if (g_audio.uplink_batch_len >= g_audio.uplink_batch_cap)
			flush_uplink_batch_locked(true);
		space = g_audio.uplink_batch_cap - g_audio.uplink_batch_len;
		copy = sampc < space ? sampc : space;
		memcpy(g_audio.uplink_batch + g_audio.uplink_batch_len, s16_data,
		       copy * sizeof(int16_t));
		g_audio.uplink_batch_len += copy;
		s16_data += copy;
		sampc -= copy;
		flush_uplink_batch_locked(false);
	}
	pthread_mutex_unlock(&pa.batch_mutex);
}

void openai_rt_receive_audio(const int16_t *sampv, size_t sampc)
{
	handle_incoming_audio(sampv, sampc);
}

void handle_outgoing_audio(const uint8_t *g711u_data, size_t len)
{
	(void)g711u_data;
	(void)len;
}

void openai_rt_check_messages(void)
{
}

void openai_rt_provide_audio(void)
{
}

/* Send the first response.create once audio flows (unless the far end is to speak first) */
static void send_audio_commit(void)
{
	struct ai_model *model = get_ai_model();
	char *response_msg = NULL;
	int err;

	if (g_oairt.ws_state != WS_CONNECTED || !g_oairt.ws_client)
		return;
	if (g_oairt.wait_for_greeting)
		g_audio.response_created = true;

	if (!g_audio.response_created && g_oairt.call_active &&
	    model && model->build_response_create) {
		err = model->build_response_create(g_oairt.prompt, &response_msg);
		if (!err && response_msg) {
			err = queue_message_to_openai(response_msg, str_len(response_msg),
						      NULL, NULL);
			if (err)
				warning("openai_rt: failed to queue response.create: %m\n", err);
			else
				g_audio.response_created = true;
			mem_deref(response_msg);
		}
	}
	g_audio.audio_accumulated = 0;
}

static void force_audio_commit(void)
{
	if (g_audio.audio_accumulated > 0)
		send_audio_commit();
}

void audio_flush_accumulated(void)
{
	audio_flush_uplink_batch();
	if (g_audio.audio_accumulated > 0) {
		info("openai_rt: flushing %u bytes of accumulated audio\n",
		     (unsigned)g_audio.audio_accumulated);
		send_audio_commit();
	}
}

/* ------------------------------------------------------------------ injection ring buffer */

void audio_clear_injection_buffer(void)
{
	mtx_lock(&g_audio.injection_buffer_mutex);
	g_audio.injection_read_pos = 0;
	g_audio.injection_write_pos = 0;
	g_audio.injection_available = 0;
	mtx_unlock(&g_audio.injection_buffer_mutex);
}

int resize_injection_buffer(size_t new_size_samples)
{
	int16_t *new_buffer;

	if (new_size_samples < INJECTION_BUFFER_MIN_SIZE)
		new_size_samples = INJECTION_BUFFER_MIN_SIZE;
	if (new_size_samples > INJECTION_BUFFER_MAX_SIZE)
		new_size_samples = INJECTION_BUFFER_MAX_SIZE;

	mtx_lock(&g_audio.injection_buffer_mutex);
	if (new_size_samples <= g_audio.injection_buffer_size) {
		mtx_unlock(&g_audio.injection_buffer_mutex);
		return 0;
	}
	new_buffer = mem_alloc(new_size_samples * sizeof(int16_t), NULL);
	if (!new_buffer) {
		mtx_unlock(&g_audio.injection_buffer_mutex);
		return ENOMEM;
	}
	if (g_audio.injection_available > 0) {
		size_t i, pos = g_audio.injection_read_pos;
		for (i = 0; i < g_audio.injection_available; i++) {
			new_buffer[i] = g_audio.injection_buffer[pos];
			pos = (pos + 1) % g_audio.injection_buffer_size;
		}
		g_audio.injection_read_pos = 0;
		g_audio.injection_write_pos = g_audio.injection_available;
	}
	mem_deref(g_audio.injection_buffer);
	g_audio.injection_buffer = new_buffer;
	g_audio.injection_buffer_size = new_size_samples;
	mtx_unlock(&g_audio.injection_buffer_mutex);
	info("openai_rt: injection buffer resized to %zu samples (%.1f s @ 24 kHz)\n",
	     new_size_samples, (double)new_size_samples / PLAYBACK_SRATE);
	return 0;
}

static void maybe_shrink_injection_buffer(void)
{
	size_t new_size;
	mtx_lock(&g_audio.injection_buffer_mutex);
	if (!(g_audio.injection_available < g_audio.injection_buffer_size / 4 &&
	      g_audio.injection_buffer_size > INJECTION_BUFFER_INITIAL_SIZE * 2)) {
		mtx_unlock(&g_audio.injection_buffer_mutex);
		return;
	}
	new_size = g_audio.injection_buffer_size / INJECTION_BUFFER_GROWTH_FACTOR;
	mtx_unlock(&g_audio.injection_buffer_mutex);
	if (new_size >= INJECTION_BUFFER_INITIAL_SIZE)
		(void)resize_injection_buffer(new_size);
}

int write_to_injection_buffer(const int16_t *samples, size_t sample_count)
{
	size_t i, space;

	if (!samples || sample_count == 0)
		return EINVAL;

	mtx_lock(&g_audio.injection_buffer_mutex);
	space = g_audio.injection_buffer_size - g_audio.injection_available;
	if (sample_count > space) {
		size_t new_size = (g_audio.injection_buffer_size + sample_count - space)
			* INJECTION_BUFFER_GROWTH_FACTOR;
		int err;
		mtx_unlock(&g_audio.injection_buffer_mutex);
		err = resize_injection_buffer(new_size);
		if (err) {
			warning("openai_rt: injection buffer full, dropping %zu samples\n",
				sample_count);
			return err;
		}
		mtx_lock(&g_audio.injection_buffer_mutex);
	}
	for (i = 0; i < sample_count; i++) {
		g_audio.injection_buffer[g_audio.injection_write_pos] = samples[i];
		g_audio.injection_write_pos =
			(g_audio.injection_write_pos + 1) % g_audio.injection_buffer_size;
	}
	g_audio.injection_available += sample_count;
	mtx_unlock(&g_audio.injection_buffer_mutex);
	return 0;
}

static size_t read_from_injection_buffer(int16_t *samples, size_t max_samples)
{
	size_t n, i;

	mtx_lock(&g_audio.injection_buffer_mutex);
	n = g_audio.injection_available < max_samples ? g_audio.injection_available : max_samples;
	for (i = 0; i < n; i++) {
		samples[i] = g_audio.injection_buffer[g_audio.injection_read_pos];
		g_audio.injection_read_pos =
			(g_audio.injection_read_pos + 1) % g_audio.injection_buffer_size;
	}
	g_audio.injection_available -= n;
	mtx_unlock(&g_audio.injection_buffer_mutex);

	if (n > 0 && g_audio.injection_available < g_audio.injection_buffer_size / 8)
		maybe_shrink_injection_buffer();
	return n;
}

bool audio_tts_playback_pending(void)
{
	bool pending;
	mtx_lock(&g_audio.injection_buffer_mutex);
	pending = g_audio.injection_available > 0;
	mtx_unlock(&g_audio.injection_buffer_mutex);
	return pending;
}

bool audio_source_ready_for_injection(void)
{
	return pa.play != NULL;
}

/* ------------------------------------------------------------------ pulse streams */

static void *rec_thread_func(void *arg)
{
	size_t sampc = pa.uplink_srate * FRAME_MS / 1000;
	int16_t *buf = mem_alloc(sampc * sizeof(int16_t), NULL);
	int pa_err;
	(void)arg;

	if (!buf)
		return NULL;
	info("openai_rt: capture thread started (%u Hz, %zu samples per read)\n",
	     pa.uplink_srate, sampc);
	while (pa.rec_running && g_oairt.call_active) {
		if (pa_simple_read(pa.rec, buf, sampc * sizeof(int16_t), &pa_err) < 0) {
			warning("openai_rt: pa_simple_read: %s\n", pa_strerror(pa_err));
			break;
		}
		pa.captured += sampc;
		handle_incoming_audio(buf, sampc);
	}
	mem_deref(buf);
	info("openai_rt: capture thread ended\n");
	return NULL;
}

static void *play_thread_func(void *arg)
{
	size_t sampc = PLAYBACK_SRATE * FRAME_MS / 1000;
	int16_t *buf = mem_alloc(sampc * sizeof(int16_t), NULL);
	int pa_err;
	(void)arg;

	if (!buf)
		return NULL;
	info("openai_rt: playback thread started (%d Hz, %zu samples per write)\n",
	     PLAYBACK_SRATE, sampc);
	while (pa.play_running && g_oairt.call_active) {
		size_t have = read_from_injection_buffer(buf, sampc);
		if (have < sampc)
			memset(buf + have, 0, (sampc - have) * sizeof(int16_t));
		pa.played += have;
		/* blocks until the server has room: this is the pacing */
		if (pa_simple_write(pa.play, buf, sampc * sizeof(int16_t), &pa_err) < 0) {
			warning("openai_rt: pa_simple_write: %s\n", pa_strerror(pa_err));
			break;
		}
	}
	if (pa.play)
		pa_simple_drain(pa.play, NULL);
	mem_deref(buf);
	info("openai_rt: playback thread ended\n");
	return NULL;
}

int audio_pulse_start(const char *source, const char *sink, unsigned uplink_srate,
		      const char *app_name)
{
	pa_sample_spec rec_ss = { .format = PA_SAMPLE_S16LE, .rate = uplink_srate, .channels = 1 };
	pa_sample_spec play_ss = { .format = PA_SAMPLE_S16LE, .rate = PLAYBACK_SRATE, .channels = 1 };
	/* small server-side buffers: the playback thread paces on them, and a barge-in
	 * (injection buffer cleared) should cut the model's speech within a frame or two */
	pa_buffer_attr play_attr = { .maxlength = (uint32_t)-1, .tlength = PLAYBACK_SRATE * 2 * 4 * FRAME_MS / 1000,
				     .prebuf = 0, .minreq = (uint32_t)-1, .fragsize = (uint32_t)-1 };
	pa_buffer_attr rec_attr = { .maxlength = (uint32_t)-1, .tlength = (uint32_t)-1, .prebuf = (uint32_t)-1,
				    .minreq = (uint32_t)-1, .fragsize = uplink_srate * 2 * FRAME_MS / 1000 };
	int pa_err;

	if (pa.started)
		return EALREADY;
	pa.uplink_srate = uplink_srate;

	pa.rec = pa_simple_new(NULL, app_name, PA_STREAM_RECORD, source, "far end",
			       &rec_ss, NULL, &rec_attr, &pa_err);
	if (!pa.rec) {
		warning("openai_rt: cannot open capture stream on '%s': %s\n",
			source ? source : "(default)", pa_strerror(pa_err));
		return EIO;
	}
	pa.play = pa_simple_new(NULL, app_name, PA_STREAM_PLAYBACK, sink, "model",
				&play_ss, NULL, &play_attr, &pa_err);
	if (!pa.play) {
		warning("openai_rt: cannot open playback stream on '%s': %s\n",
			sink ? sink : "(default)", pa_strerror(pa_err));
		pa_simple_free(pa.rec);
		pa.rec = NULL;
		return EIO;
	}
	info("openai_rt: audio: capture '%s' @ %u Hz -> model, model -> playback '%s' @ %d Hz\n",
	     source ? source : "(default source)", uplink_srate,
	     sink ? sink : "(default sink)", PLAYBACK_SRATE);

	pa.rec_running = true;
	pa.play_running = true;
	if (pthread_create(&pa.rec_thread, NULL, rec_thread_func, NULL) != 0 ||
	    pthread_create(&pa.play_thread, NULL, play_thread_func, NULL) != 0) {
		warning("openai_rt: cannot start audio threads\n");
		pa.rec_running = pa.play_running = false;
		return ENOMEM;
	}
	pa.started = true;
	return 0;
}

void audio_pulse_stop(void)
{
	if (!pa.started)
		return;
	pa.rec_running = false;
	pa.play_running = false;
	/* the capture read blocks for one frame at most; the playback write for one frame */
	pthread_join(pa.rec_thread, NULL);
	pthread_join(pa.play_thread, NULL);
	if (pa.rec)
		pa_simple_free(pa.rec);
	if (pa.play)
		pa_simple_free(pa.play);
	pa.rec = NULL;
	pa.play = NULL;
	pa.started = false;
	force_audio_commit();
}

/* ------------------------------------------------------------------ lifecycle hooks the
 * shared code and main.c use (the baresip module starts/stops its driver threads here; the
 * pulse streams are started by main.c through audio_pulse_start) */

void audio_stop_threads(void)
{
	audio_flush_uplink_batch();
	free_event_queue();
	force_audio_commit();
}

void audio_restart_threads(void)
{
}

bool audio_threads_running(void)
{
	return pa.started;
}

bool audio_ready_for_call(void)
{
	return true;
}

void audio_reset_for_new_call(void)
{
	audio_flush_uplink_batch();
	g_audio.audio_accumulated = 0;
	g_audio.response_created = false;
	pthread_mutex_lock(&pa.batch_mutex);
	g_audio.uplink_batch_len = 0;
	pthread_mutex_unlock(&pa.batch_mutex);
	audio_clear_injection_buffer();
}

/* ------------------------------------------------------------------ event queue (WS thread) */

int audio_queue_event(enum event_type type, void *data)
{
	struct audio_event *event = mem_zalloc(sizeof(*event), NULL);
	if (!event)
		return ENOMEM;
	event->type = type;
	event->data = data;
	mtx_lock(&g_audio.event_queue_mutex);
	list_append(&g_audio.event_queue, &event->le, event);
	cnd_signal(&g_audio.event_queue_cond);
	mtx_unlock(&g_audio.event_queue_mutex);
	if (type == EVENT_CALL_START)
		websocket_kick_session_setup();
	return 0;
}

struct audio_event *audio_get_next_event(void)
{
	struct audio_event *event = NULL;
	mtx_lock(&g_audio.event_queue_mutex);
	if (!list_isempty(&g_audio.event_queue)) {
		event = list_ledata(list_head(&g_audio.event_queue));
		list_unlink(&event->le);
	}
	mtx_unlock(&g_audio.event_queue_mutex);
	return event;
}

int audio_queue_read_frame(const int16_t *sampv, size_t sampc, uint32_t srate, uint8_t ch)
{
	(void)srate;
	(void)ch;
	return write_to_injection_buffer(sampv, sampc);
}

struct audio_frame *audio_get_next_write_frame(void)
{
	return NULL;
}

void audio_free_frame(struct audio_frame *frame)
{
	if (frame) {
		mem_deref(frame->sampv);
		mem_deref(frame);
	}
}
