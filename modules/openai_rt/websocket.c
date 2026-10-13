/**
 * @file websocket.c  OpenAI Realtime API - WebSocket communication
 */

 #include <re.h>
 #include <rem.h>
 #include <baresip.h>
 #include <libwebsockets.h>
 #include <json-c/json.h>
 #include <pthread.h>
 #include "openai_rt.h"
 #include "trace.h"
 #include "ai_model.h"

/* Forward declarations */
static int ws_callback(struct lws *wsi, enum lws_callback_reasons reason,
					   void *user, void *in, size_t len);
static int add_auth_headers(void *in, size_t len);
static void process_outgoing_messages(void);
static int ws_write_message(struct ws_message *msg, bool *stale);
static void send_function_call_output(const char *call_id,
	const char *name, const char *output);
static bool parse_transfer_destination(
	struct json_object *args_obj, char *destination,
	size_t destination_len);
static void send_response_create(void);
static struct ws_message *ws_message_alloc(const char *json_msg,
	size_t len, void (*callback)(void *arg, int err), void *arg);
static void ws_message_enqueue(struct ws_message *msg);
static void process_conversation_kick_pending(void);

/* Callbacks for AI model message parsing */
static void handle_audio_delta(const char *base64_audio, void *arg);
static void handle_session_updated_cb(void *arg);
static void handle_speech_started_cb(void *arg);
static void handle_function_call_cb(const char *call_id,
	const char *name, const char *arguments, void *arg);
static void handle_response_done_cb(const char *response_json, void *arg);
static int start_transfer_after_tts(const char *destination);
static void execute_tool_call(const char *call_id,
	const char *name, const char *arguments, bool allow_defer);

/* WebSocket protocols (local binding for callbacks; NOT sent as WS
 * subprotocol) */
 static const struct lws_protocols protocols[] = {
	 {
		/* Local name; not advertised unless ccinfo.protocol is set */
		 .name = "realtime",
		 .callback = ws_callback,
		 .per_session_data_size = 0,
		 .rx_buffer_size = 65536,
		 .id = 0,
		 .user = NULL,
		 .tx_packet_size = 0,
	 },
	 { NULL, NULL, 0, 0, 0, NULL, 0 }  /* Terminator */
 };

 /* Log an inbound server event. High-frequency streaming events (audio
 * chunks, transcript deltas) are skipped entirely: they are not readable and
 * would make up most of the log. The session echo is logged in full because
 * it is the only place the server's effective configuration is visible;
 * everything else is truncated. NB: libre's printf ignores string precision
 * (%.200s pads instead of truncating), so length-bounded %b is used. */
#define WS_LOG_BODY_MAX 1024
static void ws_log_received(const char *in, size_t len)
{
	static const char *const noisy[] = {
		/* OpenAI */
		"\"type\":\"response.output_audio.delta\"",
		"\"type\":\"response.audio.delta\"",
		"\"type\":\"response.output_audio_transcript.delta\"",
		"\"type\":\"response.audio_transcript.delta\"",
		"\"type\":\"conversation.item.input_audio_transcription"
			".delta\"",
		/* Gemini */
		"\"audioBytes\"",
		"\"serverContent\"",
	};
	size_t i;
	bool full;

	if (!in || !len)
		return;

	/* libwebsockets guarantees a NUL after the payload, but bound the
	 * scans to be safe with %b below anyway. */
	for (i = 0; i < sizeof(noisy) / sizeof(noisy[0]); i++) {
		if (strstr(in, noisy[i]))
			return;
	}

	full = strstr(in, "\"type\":\"session.created\"") ||
	       strstr(in, "\"type\":\"session.updated\"") ||
	       strstr(in, "\"type\":\"error\"") ||
	       strstr(in, "\"setupComplete\"");

	if (full || len <= WS_LOG_BODY_MAX) {
		info("openai_rt: rx event: %b\n", in, len);
	}
	else {
		info("openai_rt: rx event: %b... (%zu bytes)\n",
		     in, (size_t)WS_LOG_BODY_MAX, len);
	}
}

/* Message destructor */
 static void ws_message_destructor(void *arg)
 {
	 struct ws_message *msg = arg;
	 mem_deref(msg->data);
 }

#define WS_DRAIN_MAX_PER_WRITABLE 32

 /* Process outgoing messages (drain multiple per writable callback) */
 static void process_outgoing_messages(void)
 {
	 unsigned n;

	 for (n = 0; n < WS_DRAIN_MAX_PER_WRITABLE; n++) {
		 struct ws_message *msg = NULL;
		 struct le *le;
		 bool is_audio;
		 bool stale;
		 int written;

		 if (!g_oairt.ws_client)
			 break;

		 if (lws_send_pipe_choked(g_oairt.ws_client))
			 break;

		 pthread_mutex_lock(&g_oairt.ws_mutex);
		 le = list_head(&g_oairt.to_openai_queue);
		 if (le) {
			 msg = list_ledata(le);
			 list_unlink(le);
		 }
		 pthread_mutex_unlock(&g_oairt.ws_mutex);

		 if (!msg)
			 break;

		 is_audio = (msg->len > 0 &&
			 (strstr((const char *)(msg->data + LWS_PRE),
			  "\"audio\"") != NULL ||
			  strstr((const char *)(msg->data + LWS_PRE),
				"\"realtimeInput\"") != NULL ||
			  strstr((const char *)(msg->data + LWS_PRE),
				"\"audioBytes\"") != NULL));

		 written = ws_write_message(msg, &stale);
		 if (stale) {
			 DEBUG_INFO("process_outgoing_messages: stale tool"
				    " continuation of an ended call"
				    " discarded\n");
			 mem_deref(msg);
			 continue;
		 }
		 if (written < 0) {
			 warning("openai_rt: Failed to write WebSocket"
				 " message\n");
			 pthread_mutex_lock(&g_oairt.ws_mutex);
			 list_prepend(&g_oairt.to_openai_queue, &msg->le, msg);
			 pthread_mutex_unlock(&g_oairt.ws_mutex);
			 break;
		 }

		 if (written < (int)msg->len) {
			 warning("openai_rt: partial WS write (%d/%zu),"
				 " requeueing message\n",
					 written, msg->len);
			 pthread_mutex_lock(&g_oairt.ws_mutex);
			 list_prepend(&g_oairt.to_openai_queue, &msg->le, msg);
			 pthread_mutex_unlock(&g_oairt.ws_mutex);
			 break;
		 }

		 if (!is_audio)
			 DEBUG_INFO("process_outgoing_messages: Message sent"
				    " (%zu bytes)\n",
						msg->len);

		 if (msg->callback)
			 msg->callback(msg->arg, 0);
		 mem_deref(msg);
	 }

	 pthread_mutex_lock(&g_oairt.ws_mutex);
	 if (!list_isempty(&g_oairt.to_openai_queue) && g_oairt.ws_client)
		 lws_callback_on_writable(g_oairt.ws_client);
	 pthread_mutex_unlock(&g_oairt.ws_mutex);
 }

static inline void ws_wake_service(void)
{
	if (g_oairt.ws_context) {
		/* break poll and run lws_service() ASAP */
		lws_cancel_service(g_oairt.ws_context);
	}
}

/* Tool-call batch accounting for the OpenAI backend.
 *
 * The model pauses after a response that contains function calls until
 * the client has added a function_call_output for every call and then
 * sent exactly one response.create. One continuation per output would
 * start the next response while outputs are still missing, and the
 * extra response.create is rejected as an active response. So the
 * dispatcher announces how many outputs a response owes
 * (tool_batch_begin), every send_function_call_output, successful or
 * not, settles one of them (the model gets at most that one message per
 * call either way), and the last one sends the continuation. Deferred
 * tool threads settle theirs when they eventually run. A hangup_call in
 * the batch suppresses the continuation: the call is over. */
static struct {
	pthread_mutex_t mtx;
	size_t pending;
	bool hangup;
	/* Bumped at every call boundary (and WS reconnect). A tool
	 * execution belongs to the generation it was dispatched under;
	 * a deferred worker that outlives its call is stale and must
	 * neither run nor settle a slot of the next call's batch. */
	unsigned gen;
	/* Generation in which the response now being generated was
	 * created (response.created). A response.done whose response was
	 * created before the current call must not run tools against it:
	 * the WS stays up across calls and the model answers the previous
	 * conversation turn. */
	unsigned response_gen;
	/* A response is in progress (response.created seen, no
	 * response.done yet); cancelled at call end. */
	bool response_active;
} tool_batch = { PTHREAD_MUTEX_INITIALIZER, 0, false, 0, 0, false };

/* Generation and call of the tool execution running on this thread. Set
 * at the dispatcher entry of execute_tool_call() on the I/O thread and
 * from the captured values in a deferred worker, so that the settlement
 * deep inside send_function_call_output() can tell stale from current,
 * and so that every call action (calls.c) is bound to the call the tool
 * was dispatched for rather than to whatever call is current when the
 * action is finally performed. */
static _Thread_local unsigned tool_exec_gen;
static _Thread_local struct call *tool_exec_call;

unsigned tool_exec_generation(void)
{
	return tool_exec_gen;
}

void tool_batch_note_response_created(void)
{
	pthread_mutex_lock(&tool_batch.mtx);
	tool_batch.response_gen = tool_batch.gen;
	tool_batch.response_active = true;
	pthread_mutex_unlock(&tool_batch.mtx);
}

static void tool_batch_note_response_done(void)
{
	pthread_mutex_lock(&tool_batch.mtx);
	tool_batch.response_active = false;
	pthread_mutex_unlock(&tool_batch.mtx);
}

/* Called from the RE thread at CALL_CLOSED. The WS stays up for trailing
 * transcripts, so the model could keep working on the ended call and
 * its events could straddle the boundary into the next call. Cancel the
 * response in progress (its function calls then come back without a
 * completed status and are skipped) and drop uncommitted input audio so
 * the server VAD cannot start a new response out of the old call's
 * tail. Both messages are plain client events; nothing is bound to a
 * generation here because they must go out regardless. */
void websocket_isolate_call_end(void)
{
	static const char cancel[] = "{\"type\":\"response.cancel\"}";
	static const char clear[] =
		"{\"type\":\"input_audio_buffer.clear\"}";
	bool active;

	if (g_oairt.backend_type != AI_BACKEND_OPENAI_REALTIME)
		return;
	if (g_oairt.ws_state != WS_CONNECTED)
		return;

	pthread_mutex_lock(&tool_batch.mtx);
	active = tool_batch.response_active;
	tool_batch.response_active = false;
	pthread_mutex_unlock(&tool_batch.mtx);

	if (active) {
		info("openai_rt: call ended with a response in progress,"
		     " cancelling it\n");
		queue_message_to_openai(cancel, sizeof(cancel) - 1, NULL,
					NULL);
	}
	queue_message_to_openai(clear, sizeof(clear) - 1, NULL, NULL);
}

bool tool_batch_begin(size_t n)
{
	bool accept;

	if (g_oairt.backend_type != AI_BACKEND_OPENAI_REALTIME || !n)
		return true;

	pthread_mutex_lock(&tool_batch.mtx);
	accept = g_oairt.call_active &&
		 tool_batch.response_gen == tool_batch.gen;
	if (accept)
		tool_batch.pending += n;
	pthread_mutex_unlock(&tool_batch.mtx);

	return accept;
}

/* Write one message to the socket. A generation-bound message (the
 * continuation after a tool batch) is written under tool_batch.mtx after
 * re-checking its generation, so tool_batch_reset() either purges it
 * from the queue before it is dequeued, or waits for this write to
 * finish; a reset can never slip in between the dequeue and the write.
 * Returns the lws_write() result; *stale is set when the message was
 * discarded instead of written. */
static int ws_write_message(struct ws_message *msg, bool *stale)
{
	int written;

	*stale = false;

	if (!msg->gen_bound) {
		return lws_write(g_oairt.ws_client, msg->data + LWS_PRE,
				 msg->len, LWS_WRITE_TEXT);
	}

	pthread_mutex_lock(&tool_batch.mtx);
	if (msg->gen != tool_batch.gen) {
		pthread_mutex_unlock(&tool_batch.mtx);
		*stale = true;
		return 0;
	}
	written = lws_write(g_oairt.ws_client, msg->data + LWS_PRE,
			    msg->len, LWS_WRITE_TEXT);
	pthread_mutex_unlock(&tool_batch.mtx);

	return written;
}

/* Called with tool_batch.mtx held. Drop every queued message that was
 * bound to an older generation: a continuation response.create of a
 * call that has ended must not start an unsolicited response in the
 * next call. Lock order is tool_batch.mtx, then ws_mutex, everywhere. */
static void tool_batch_purge_stale_locked(void)
{
	struct le *le;

	pthread_mutex_lock(&g_oairt.ws_mutex);
	le = list_head(&g_oairt.to_openai_queue);
	while (le) {
		struct le *next = le->next;
		struct ws_message *msg = list_ledata(le);

		if (msg->gen_bound && msg->gen != tool_batch.gen) {
			list_unlink(le);
			mem_deref(msg);
			DEBUG_INFO("stale tool continuation of an ended call"
				   " discarded\n");
		}
		le = next;
	}
	pthread_mutex_unlock(&g_oairt.ws_mutex);
}

void tool_batch_reset(void)
{
	pthread_mutex_lock(&tool_batch.mtx);
	tool_batch.pending = 0;
	tool_batch.hangup = false;
	tool_batch.gen++;
	tool_batch_purge_stale_locked();
	pthread_mutex_unlock(&tool_batch.mtx);
}

/* Called with tool_batch.mtx held: deciding that the batch is complete,
 * stamping the continuation with the generation and putting it on the
 * queue are one critical section with tool_batch_reset(), so a call
 * ending in between either sees the message on the queue and purges
 * it, or bumps the generation before it is stamped (and the settlement
 * that led here would not have passed its generation check). */
static void queue_continuation_locked(void)
{
	struct ai_model *model = get_ai_model();
	struct ws_message *msg;
	char *json_msg = NULL;
	int err;

	if (g_oairt.ws_state == WS_DISCONNECTED ||
	    g_oairt.ws_state == WS_DISCONNECTING)
		return;

	if (!model || !model->build_response_create) {
		warning("openai_rt: AI model not initialized for"
			" response.create\n");
		return;
	}

	err = model->build_response_create(NULL, &json_msg);
	if (err || !json_msg) {
		warning("openai_rt: Failed to build response.create: %m\n",
			err ? err : EINVAL);
		return;
	}

	msg = ws_message_alloc(json_msg, str_len(json_msg), NULL, NULL);
	mem_deref(json_msg);
	if (!msg) {
		warning("openai_rt: Failed to queue response.create: %m\n",
			ENOMEM);
		return;
	}

	msg->gen_bound = true;
	msg->gen = tool_batch.gen;
	ws_message_enqueue(msg);
	DEBUG_INFO("response.create queued after tool output\n");
}

unsigned tool_batch_current_gen(void)
{
	unsigned gen;

	pthread_mutex_lock(&tool_batch.mtx);
	gen = tool_batch.gen;
	pthread_mutex_unlock(&tool_batch.mtx);

	return gen;
}

/* Only the branch that actually hangs up marks the batch terminal; a
 * rejected hangup_call (tool disabled, defer failed) answers with an
 * error output and must still get its continuation. */
static void tool_batch_mark_hangup(void)
{
	pthread_mutex_lock(&tool_batch.mtx);
	if (tool_exec_gen == tool_batch.gen)
		tool_batch.hangup = true;
	pthread_mutex_unlock(&tool_batch.mtx);
}

/* Called with tool_batch.mtx held: one output of the current batch has
 * been handed to the queue (or could not be built; the model gets at
 * most that one message per call either way). The last one queues the
 * continuation under the same lock. */
static void tool_batch_output_settled_locked(void)
{
	bool cont = false;

	if (tool_batch.pending)
		tool_batch.pending--;
	if (tool_batch.pending == 0) {
		cont = !tool_batch.hangup;
		tool_batch.hangup = false;
	}
	if (cont)
		queue_continuation_locked();
}

/* Queue the output of one tool call. On the OpenAI backend the output
 * is bound to the generation of the execution that produced it and put
 * on the queue, settled in the batch and (if last) followed by the
 * continuation under one hold of tool_batch.mtx, so a call ending in
 * between purges output and continuation together and a stale
 * execution produces nothing at all. */
static void send_function_call_output(const char *call_id,
	const char *name, const char *output)
{
	struct ai_model *model = get_ai_model();
	struct ws_message *msg = NULL;
	char *json_msg = NULL;
	int err;

	if (!model || !model->build_function_call_output) {
		warning("openai_rt: AI model not initialized\n");
	}
	else {
		err = model->build_function_call_output(call_id, name, output,
			&json_msg);
		if (err) {
			warning("openai_rt: Failed to build"
				" function_call_output: %m\n", err);
		}
		else if (json_msg && g_oairt.ws_state != WS_DISCONNECTED &&
			 g_oairt.ws_state != WS_DISCONNECTING) {
			msg = ws_message_alloc(json_msg, str_len(json_msg),
					       NULL, NULL);
			if (!msg) {
				warning("openai_rt: Failed to queue"
					" function_call_output: %m\n", ENOMEM);
			}
		}
		mem_deref(json_msg);
	}

	if (g_oairt.backend_type != AI_BACKEND_OPENAI_REALTIME) {
		if (msg)
			ws_message_enqueue(msg);
		return;
	}

	pthread_mutex_lock(&tool_batch.mtx);
	if (tool_exec_gen != tool_batch.gen) {
		pthread_mutex_unlock(&tool_batch.mtx);
		mem_deref(msg);
		DEBUG_INFO("tool output of an ended call discarded\n");
		return;
	}
	if (msg) {
		msg->gen_bound = true;
		msg->gen = tool_batch.gen;
		ws_message_enqueue(msg);
	}
	tool_batch_output_settled_locked();
	pthread_mutex_unlock(&tool_batch.mtx);
}

static void wait_for_tts_drain(void)
{
	while (g_oairt.call_active && audio_tts_playback_pending())
		sys_msleep(TRANSFER_DRAIN_POLL_MS);

	/* Give baresip a moment to actually play the last chunk */
	if (g_oairt.call_active)
		sys_msleep(TRANSFER_POST_DRAIN_MS);
}

struct deferred_tool_call {
	char *call_id;
	char *tool_name;
	char *arguments;
	unsigned gen;
	struct call *call;
};

static void *deferred_tool_call_thread(void *arg)
{
	struct deferred_tool_call *w = arg;

	wait_for_tts_drain();

	/* Run only for the call this was deferred in. If that call ended
	 * meanwhile the batch was reset and the model is no longer waiting
	 * for this output. This is a pre-filter: the binding that cannot
	 * race is the call carried by every queued action, checked on the
	 * RE thread (calls.c, mq_action_is_current). */
	tool_exec_gen = w->gen;
	tool_exec_call = w->call;
	if (g_oairt.call_active && tool_batch_current_gen() == w->gen)
		execute_tool_call(w->call_id, w->tool_name, w->arguments,
			false);
	else
		DEBUG_INFO("deferred tool '%s' dropped: call ended\n",
			   w->tool_name);

	mem_deref(w->call_id);
	mem_deref(w->tool_name);
	mem_deref(w->arguments);
	mem_deref(w);
	return NULL;
}

static int defer_tool_call_until_tts_drained(const char *call_id,
	const char *name, const char *arguments)
{
	struct deferred_tool_call *w;
	pthread_t tid;
	int err;

	w = mem_zalloc(sizeof(*w), NULL);
	if (!w)
		return ENOMEM;

	err = str_dup(&w->call_id, call_id);
	if (err)
		goto out;

	err = str_dup(&w->tool_name, name);
	if (err)
		goto out;

	/* Keep a safe JSON string for deferred parsing */
	err = str_dup(&w->arguments, arguments ? arguments : "{}");
	if (err)
		goto out;

	w->gen = tool_exec_gen;
	w->call = tool_exec_call;

	err = pthread_create(&tid, NULL, deferred_tool_call_thread, w);
	if (err) {
		warning("openai_rt: failed to start deferred tool thread\n");
		goto out;
	}

	pthread_detach(tid);
	return 0;

out:
	mem_deref(w->call_id);
	mem_deref(w->tool_name);
	mem_deref(w->arguments);
	mem_deref(w);
	return err;
}


static bool parse_transfer_destination(
	struct json_object *args_obj, char *destination,
	size_t destination_len)
{
	struct json_object *dest_obj = NULL;

	if (!args_obj || !destination || destination_len == 0)
		return false;

	if (!json_object_object_get_ex(args_obj, "destination", &dest_obj))
		return false;

	if (json_object_is_type(dest_obj, json_type_string)) {
		const char *s = json_object_get_string(dest_obj);
		if (!s || !*s)
			return false;
		str_ncpy(destination, s, destination_len);
		return true;
	}

	if (json_object_is_type(dest_obj, json_type_int)) {
		re_snprintf(destination, destination_len, "%d",
		    json_object_get_int(dest_obj));
		return destination[0] != '\0';
	}

	return false;
}


static bool parse_transfer_dest_from_prompt(char *dest, size_t dest_len)
{
	const char *p;
	size_t n;

	if (!dest || dest_len == 0)
		return false;

	p = strstr(g_oairt.prompt, "extension ");
	if (!p)
		return false;

	p += strlen("extension ");
	for (n = 0; p[n] >= '0' && p[n] <= '9' && n + 1 < dest_len; n++)
		dest[n] = p[n];
	dest[n] = '\0';

	return n > 0;
}


struct gemini_xfer_after_tts {
	char *destination;
	struct call *call;
	unsigned gen;
};

static void *gemini_xfer_after_tts_thread(void *arg)
{
	struct gemini_xfer_after_tts *w = arg;

	wait_for_tts_drain();

	tool_exec_gen = w->gen;
	if (g_oairt.call_active) {
		int err = calls_transfer(w->call, w->destination);
		if (err) {
			warning("openai_rt: Gemini deferred transfer to '%s'"
				" failed: %m\n",
			        w->destination, err);
		}
	}

	mem_deref(w->destination);
	mem_deref(w);
	return NULL;
}

static int start_transfer_after_tts(const char *destination)
{
	struct gemini_xfer_after_tts *w;
	pthread_t tid;
	int err;

	w = mem_zalloc(sizeof(*w), NULL);
	if (!w)
		return ENOMEM;

	err = str_dup(&w->destination, destination);
	if (err) {
		mem_deref(w);
		return err;
	}
	w->call = g_oairt.current_call;
	w->gen = tool_batch_current_gen();

	err = pthread_create(&tid, NULL, gemini_xfer_after_tts_thread, w);
	if (err) {
		warning("openai_rt: failed to start Gemini transfer-after-TTS"
			" thread\n");
		mem_deref(w->destination);
		mem_deref(w);
		return err;
	}

	pthread_detach(tid);
	return 0;
}


static int queue_response_create(void)
{
	struct ai_model *model = get_ai_model();
	char *json_msg = NULL;
	int err;

	if (!model || !model->build_response_create) {
		warning("openai_rt: AI model not initialized for"
			" response.create\n");
		return EINVAL;
	}

	err = model->build_response_create(NULL, &json_msg);
	if (err) {
		warning("openai_rt: Failed to build response.create: %m\n",
			err);
		return err;
	}

	if (!json_msg)
		return EINVAL;

	err = queue_message_to_openai(json_msg, str_len(json_msg), NULL, NULL);
	if (err) {
		warning("openai_rt: Failed to queue response.create: %m\n",
			err);
	}
	mem_deref(json_msg);
	return err;
}

/* Kick off the conversation (wait_for_greeting=no). Sent once per call:
 * response_created guards against a second kick from the audio commit
 * path. */
static void send_response_create(void)
{
	if (g_audio.response_created)
		return;

	if (!queue_response_create()) {
		info("openai_rt: Conversation start message queued\n");
		g_audio.response_created = true;
	}
}

/* Callbacks for AI model message parsing */
static void handle_audio_delta(const char *base64_audio, void *arg)
{
	(void)arg;

	if (!base64_audio || !*base64_audio) {
		/* DEBUG_INFO("[AUDIO RX] handle_audio_delta called but no
		 * audio data\n"); */
		return;
	}

	if (!g_oairt.call_active) {
		/* DEBUG_INFO("[AUDIO RX] handle_audio_delta called but call
		 * not active, dropping audio\n"); */
		return;
	}

	if (g_oairt.backend_type == AI_BACKEND_GEMINI_LIVE)
		g_oairt.gemini_turn_had_audio = true;

	/*size_t b64_len = str_len(base64_audio); */
	/* info("[AUDIO RX] Received base64 audio data (%zu chars),
	 * decoding...\n", b64_len); */

	/* Decode base64 -> PCM16LE */
	uint8_t *decoded = NULL;
	size_t nbytes = decode_audio_base64(base64_audio, &decoded);

	if (decoded && nbytes >= 2) {
		const int16_t *pcm = (const int16_t *)decoded;
		size_t nsamp = nbytes / 2;
		int16_t peak = 0;
		size_t i;

		for (i = 0; i < nsamp; i++) {
			int16_t a = pcm[i] < 0 ? (int16_t)-pcm[i] : pcm[i];
			if (a > peak)
				peak = a;
		}
		if (peak < OUTPUT_DELTA_PEAK_MIN) {
			mem_deref(decoded);
			return;
		}

		int err = write_to_injection_buffer(pcm, nsamp);
		if (err) {
			warning("openai_rt: write_to_injection_buffer failed:"
				" %m\n", err);
		}
		else {
			/* info("[AUDIO RX] Successfully wrote %zu samples to
			 * injection buffer\n", nsamp); */
		}
	}
	else {
		warning("[AUDIO RX] Failed to decode audio or insufficient"
			" data (nbytes=%zu)\n", nbytes);
	}

	if (decoded) mem_deref(decoded);
}

static void handle_session_updated_cb(void *arg)
{
	(void)arg;

	g_oairt.session_cfg_applied = true;
	g_oairt.session_ready = true;
	info("openai_rt: session.updated received; input_audio_format now"
	     " active\n");
	audio_flush_accumulated();
	if (!g_oairt.wait_for_greeting)
		g_oairt.conversation_kick_pending = true;
}

static void handle_speech_started_cb(void *arg)
{
	(void)arg;

	DEBUG_INFO("openai_rt: speech.started, interrupting outbound TTS\n");
	/* Record the barge-in as a turn-taking event for the evaluator. */
	trace_add_event("speech_started");
	audio_clear_injection_buffer();
}

static void execute_tool_call(const char *call_id,
	const char *name, const char *arguments,
	bool allow_defer)
{
	/* The dispatch path (I/O thread) binds this execution to the
	 * current batch generation and call; a deferred worker arrives
	 * with allow_defer=false and its own captured values. */
	if (allow_defer) {
		tool_exec_gen = tool_batch_current_gen();
		tool_exec_call = g_oairt.current_call;
	}

	/* Validate that the tool call is enabled in configuration */
	if (!ai_model_is_tool_enabled(name, g_oairt.enabled_tools)) {
		warning("openai_rt: Tool call '%s' is not enabled in"
			" configuration. Rejecting.\n", name);
		/* Send error response back to OpenAI */
		char error_msg[512];
		re_snprintf(error_msg, sizeof(error_msg),
		            "Error: Tool call '%s' is not enabled. Only these"
			    " tools are available: %s",
		            name, g_oairt.enabled_tools);
		send_function_call_output(call_id, name, error_msg);
		return;
	}

	/* Defer tool execution until the model stops talking */
	if (allow_defer && audio_tts_playback_pending()) {
		int err = defer_tool_call_until_tts_drained(call_id, name,
			arguments);
		if (err) {
			warning("openai_rt: Failed to defer tool '%s': %m\n",
				name, err);
			send_function_call_output(call_id, name,
			                          "Error: Failed to schedule"
						  " tool execution");
		}
		return;
	}

	/* Process enabled tool calls */
	if (strcmp(name, AI_TOOL_HANGUP_CALL.name) == 0) {
		DEBUG_INFO("openai_rt: Executing hangup_call function\n");
		if (calls_hangup(tool_exec_call) == 0) {
			/* The call is over: no continuation after this
			 * batch. Marked only once the hangup is actually
			 * queued; a failed one answers like any other
			 * failed tool and keeps the conversation going. */
			tool_batch_mark_hangup();
			send_function_call_output(call_id, name,
						  "Call hung up");
		}
		else {
			send_function_call_output(call_id, name,
						  "Error: Failed to hang up"
						  " the call");
		}
	}
	else if (strcmp(name, AI_TOOL_SEND_DTMF.name) == 0) {
		DEBUG_INFO("openai_rt: Executing send_dtmf function\n");

		/* Parse arguments JSON */
		struct json_object *args_obj = json_tokener_parse(arguments);
		if (!args_obj) {
			warning("openai_rt: Failed to parse send_dtmf"
				" arguments\n");
			send_function_call_output(call_id, name,
			                          "Error: Failed to parse"
						  " function arguments");
			return;
		}

		struct json_object *digits_obj = NULL;
		/* The schema says string, but a model may still hand a
		 * single digit over as a JSON number; json-c renders an
		 * int via json_object_get_string, so accept both. */
		if (json_object_object_get_ex(args_obj, "digits",
						     &digits_obj) &&
		    (json_object_is_type(digits_obj, json_type_string) ||
		     json_object_is_type(digits_obj, json_type_int))) {
			const char *digits =
				json_object_get_string(digits_obj);
			if (digits && *digits) {
				int err = calls_send_dtmf(tool_exec_call,
							  digits);
				if (!err) {
					char output[256];
					re_snprintf(output, sizeof(output),
						"DTMF tones sent: %s", digits);
					send_function_call_output(call_id,
						name, output);
				}
				else {
					char error_msg[256];
					re_snprintf(error_msg,
						sizeof(error_msg),
					            "Error: Failed to send"
						    " DTMF string '%s'"
						    , digits);
					send_function_call_output(call_id,
						name, error_msg);
					warning("openai_rt: Failed to send"
						" DTMF string '%s': %m\n"
						, digits, err);
				}
			}
			else {
				send_function_call_output(call_id, name,
				                          "Error: Missing or"
							  " empty 'digits'"
							  " parameter");
			}
		}
		else {
			send_function_call_output(call_id, name,
			                          "Error: Missing or invalid"
						  " 'digits' parameter");
		}
		json_object_put(args_obj);
	}
	else if (strcmp(name, AI_TOOL_API_CALL.name) == 0) {
		DEBUG_INFO("openai_rt: Executing api_call function\n");

		/* Parse arguments JSON */
		struct json_object *args_obj = json_tokener_parse(arguments);
		if (!args_obj) {
			warning("openai_rt: Failed to parse api_call"
				" arguments\n");
			send_function_call_output(call_id, name,
			                          "Error: Failed to parse"
						  " function arguments");
			return;
		}

		const char *method = NULL;
		const char *uri = NULL;
		const char *content_type = NULL;
		const char *auth_type = NULL;
		const char *auth_username = NULL;
		const char *auth_password = NULL;
		const char *body = NULL;

		struct json_object *obj = NULL;
		if (json_object_object_get_ex(args_obj, "method", &obj))
			method = json_object_get_string(obj);
		if (json_object_object_get_ex(args_obj, "uri", &obj))
			uri = json_object_get_string(obj);
		if (json_object_object_get_ex(args_obj, "content_type", &obj))
			content_type = json_object_get_string(obj);
		if (json_object_object_get_ex(args_obj, "auth_type", &obj))
			auth_type = json_object_get_string(obj);
		if (json_object_object_get_ex(args_obj, "auth_username", &obj))
			auth_username = json_object_get_string(obj);
		if (json_object_object_get_ex(args_obj, "auth_password", &obj))
			auth_password = json_object_get_string(obj);
		if (json_object_object_get_ex(args_obj, "body", &obj))
			body = json_object_get_string(obj);

		if (method && uri) {
			char *api_output = NULL;
			int err = calls_api_call(method, uri,
				content_type, auth_type, auth_username,
				auth_password, body, &api_output);

			if (err == 0) {
				send_function_call_output(call_id, name,
						api_output ? api_output
						: "Success");
			}
			else {
				char error_msg[256];
				re_snprintf(error_msg, sizeof(error_msg),
				            "Error: API call failed with code"
					    " %d", err);
				send_function_call_output(call_id, name,
					error_msg);
			}
			mem_deref(api_output);
		}
		else {
			send_function_call_output(call_id, name,
			                          "Error: Missing 'method' or"
						  " 'uri' parameter");
		}
		json_object_put(args_obj);
	}
	else if (strcmp(name, AI_TOOL_TRANSFER_CALL.name) == 0) {
		struct json_object *args_obj = json_tokener_parse(arguments);
		char destination[256];

		if (!args_obj) {
			warning("openai_rt: Failed to parse transfer_call"
				" arguments\n");
			send_function_call_output(call_id, name,
			                          "Error: Failed to parse"
						  " function arguments");
			return;
		}

		if (parse_transfer_destination(args_obj,
						  destination,
						  sizeof(destination))) {
			int err;

			if (g_oairt.backend_type == AI_BACKEND_GEMINI_LIVE)
				g_oairt.gemini_xfer_scheduled = true;

			err = calls_transfer(tool_exec_call, destination);
			if (!err) {
				char msg[256];
				re_snprintf(msg, sizeof(msg), "Call transfer"
							      " initiated to"
							      " %s",
				            destination);
				send_function_call_output(call_id, name, msg);
			}
			else {
				char error_msg[256];
				re_snprintf(error_msg, sizeof(error_msg),
				            "Error: Failed to transfer call to"
					    " '%s'", destination);
				send_function_call_output(call_id, name,
					error_msg);
				warning("openai_rt: Failed to transfer to"
					" '%s': %m\n",
				        destination, err);
			}
		}
		else {
			send_function_call_output(call_id, name,
			                          "Error: Missing or invalid"
						  " 'destination' parameter");
		}
		json_object_put(args_obj);
	}
	else if (strcmp(name, AI_TOOL_RECORD_EVENT.name) == 0) {
		/* General-purpose observable-event tool: capture a
		 * specific fact the simulated caller heard into the
		 * conversation trace for the downstream task evaluator.
		 * No side effect on the call -- record under the
		 * caller-supplied label (with the full arguments as
		 * payload) and acknowledge. */
		struct json_object *args_obj = json_tokener_parse(arguments);
		struct json_object *label_obj = NULL, *value_obj = NULL;
		const char *label = NULL, *value = NULL;

		if (args_obj) {
			if (json_object_object_get_ex(args_obj, "label",
						      &label_obj))
				label = json_object_get_string(label_obj);
			if (json_object_object_get_ex(args_obj, "value",
						      &value_obj))
				value = json_object_get_string(value_obj);
		}

		if (label && *label && value && *value) {
			DEBUG_INFO("openai_rt: Recording event '%s'\n", label);
			trace_add_toolcall(label, arguments);
			send_function_call_output(call_id, name, "Recorded");
		}
		else {
			warning("openai_rt: record_event missing 'label' or"
				" 'value'\n");
			send_function_call_output(call_id, name,
			                          "Error: Missing 'label' or"
						  " 'value' parameter");
		}

		if (args_obj)
			json_object_put(args_obj);
	}
	else {
		/* This shouldn't happen if validation above worked, but handle
		 * it anyway */
		warning("openai_rt: Unknown function call: %s (but was enabled"
			" in config?)\n", name);
		char error_msg[256];
		re_snprintf(error_msg, sizeof(error_msg),
		            "Error: Unknown or unsupported"
		            " tool call: %s", name);
		send_function_call_output(call_id, name, error_msg);
	}
}

static void handle_function_call_cb(const char *call_id,
	const char *name, const char *arguments, void *arg)
{
	(void)arg;

	execute_tool_call(call_id, name, arguments, true);
}

static void handle_response_done_cb(const char *response_json, void *arg)
{
	char dest[128];

	(void)arg;

	DEBUG_INFO("openai_rt: response.done received\n");
	tool_batch_note_response_done();

	/* Gemini native audio often skips tool calls; transfer after speech */
	if (g_oairt.backend_type == AI_BACKEND_GEMINI_LIVE &&
	    g_oairt.call_active && !g_oairt.gemini_xfer_scheduled &&
	    g_oairt.gemini_turn_had_audio &&
	    ai_model_is_tool_enabled(AI_TOOL_TRANSFER_CALL.name,
	                              g_oairt.enabled_tools) &&
	    parse_transfer_dest_from_prompt(dest, sizeof(dest))) {
		g_oairt.gemini_xfer_scheduled = true;
		g_oairt.gemini_turn_had_audio = false;
		info("openai_rt: Gemini turn complete; scheduling transfer to"
		     " %s "
		    "after TTS drain\n", dest);
		start_transfer_after_tts(dest);
	}

	(void)response_json;
	/* Transcript content is emitted per turn as UA_EVENT_VOICEAI_CONTENT
	 * from the transcription parse branches (tagged self/other);
	 * response.done is only an end-of-turn marker now. */
}

 /* WebSocket callback */
 static int ws_callback(struct lws *wsi, enum lws_callback_reasons reason,
						void *user, void *in,
						size_t len)
 {
	 (void)user; /* Unused parameter */

	 /*info("openai_rt: WebSocket callback: reason=%d\n", reason); */

	 switch (reason) {
	 case LWS_CALLBACK_WSI_CREATE:
		 info("WSI created\n");
		 break;

	 case LWS_CALLBACK_CLIENT_ESTABLISHED:
		 {
			 info("openai_rt: WebSocket connection established\n");
			 g_oairt.ws_state = WS_CONNECTED;
			 g_oairt.ws_client = wsi;

			/* not ready yet: wait for server to ack session.update
			 */
			 g_oairt.session_ready = false;
			 g_oairt.session_cfg_applied = false;
			 g_oairt.conversation_kick_pending = false;
			 tool_batch_reset();

			 send_session_update();

			/* Request writable callback to send session update and
			 * any queued messages */
			 pthread_mutex_lock(&g_oairt.ws_mutex);
			 size_t established_queue_size =
				list_count(&g_oairt.to_openai_queue);
			 pthread_mutex_unlock(&g_oairt.ws_mutex);
			 DEBUG_INFO("WebSocket connected, queue size=%zu,"
				    " requesting writable callback\n"
				    , established_queue_size);
			 lws_callback_on_writable(wsi);
		 }
		 break;

	case LWS_CALLBACK_CLIENT_WRITEABLE:
		{
			pthread_mutex_lock(&g_oairt.ws_mutex);
			/* size_t queue_size =
			 * list_count(&g_oairt.to_openai_queue); */
			pthread_mutex_unlock(&g_oairt.ws_mutex);
			/* DEBUG_INFO("openai_rt: WebSocket writable callback,
			 * queue size=%zu\n", queue_size); */
			/* Process outgoing messages */
			process_outgoing_messages();
		}
		break;

	case LWS_CALLBACK_CLIENT_RECEIVE:
		{
			ws_log_received((const char *)in, len);

			if (len > 0 && g_oairt.ws_state == WS_CONNECTED) {
			   struct ai_model *model = get_ai_model();
			   if (model && model->parse_message) {
				   int parse_err = model->parse_message(
					   (const char *)in,
					   handle_audio_delta,
					   handle_session_updated_cb,
					   handle_speech_started_cb,
					   handle_function_call_cb,
					   handle_response_done_cb,
					   NULL);
				   if (parse_err) {
					   DEBUG_INFO("openai_rt:"
						      " parse_message returned"
						      " error %d\n"
						      , parse_err);
				   }
			   }
			}
		}
		break;

	 case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
		 warning("openai_rt: WebSocket connection error\n");
		 g_oairt.ws_state = WS_DISCONNECTED;
		 g_oairt.ws_client = NULL;
		 g_oairt.session_ready = false;
		 g_oairt.session_cfg_applied = false;
		 g_oairt.setup_sent = false;
		 g_oairt.conversation_kick_pending = false;
		 websocket_clear_message_queue();
		 tool_batch_reset();
		 break;

	case LWS_CALLBACK_CLIENT_CLOSED:
		info("openai_rt: WebSocket disconnected\n");
		if (len > 0 && in) {
			info("openai_rt: Close reason/data: %.200s\n",
				(const char *)in);
		}
		g_oairt.ws_state = WS_DISCONNECTED;
		g_oairt.ws_client = NULL;
		g_oairt.session_ready = false;
		g_oairt.session_cfg_applied = false;
		g_oairt.setup_sent = false;
		g_oairt.conversation_kick_pending = false;
		websocket_clear_message_queue();
	tool_batch_reset();
		break;

	 case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER: {
		 info("openai_rt: Adding authentication headers\n");
		 info("openai_rt: API key length: %zu\n",
			str_len(g_oairt.api_key));
		 int ret = add_auth_headers(in, len);
		 info("openai_rt: add_auth_headers returned: %d\n", ret);
		 return ret;
	 }

	 case LWS_CALLBACK_CLIENT_FILTER_PRE_ESTABLISH:
		 info("openai_rt: Client filter pre-establish\n");
		 return 0;

	 case LWS_CALLBACK_ESTABLISHED_CLIENT_HTTP:
		 info("openai_rt: HTTP connection established\n");
		 break;

	 case LWS_CALLBACK_CLIENT_HTTP_WRITEABLE:
		 info("openai_rt: HTTP writeable\n");
		 break;

	 case LWS_CALLBACK_CLIENT_HTTP_REDIRECT:
		 info("openai_rt: HTTP redirect\n");
		 break;

	 case LWS_CALLBACK_CLIENT_HTTP_DROP_PROTOCOL:
		 info("openai_rt: HTTP drop protocol\n");
		 break;

	 case LWS_CALLBACK_CLIENT_HTTP_BIND_PROTOCOL:
		 info("openai_rt: HTTP bind protocol\n");
		/* Called when the HTTP connection is established but before WS
		 * upgrade */
		 break;

	 case LWS_CALLBACK_GET_THREAD_ID:
		 info("openai_rt: Get thread ID requested\n");
		/* Avoid returning pthread_t as int; most apps can just return
		 * 0 */
		 return 0;

	 case LWS_CALLBACK_PROTOCOL_INIT:
		 info("openai_rt: Protocol init\n");
		 break;

	 case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
		 /*info("openai_rt: Event wait cancelled\n"); */
		 break;

	 case LWS_CALLBACK_HTTP:
		 info("openai_rt: HTTP request received\n");
		 break;

	 case LWS_CALLBACK_HTTP_CONFIRM_UPGRADE:
		 info("openai_rt: HTTP upgrade confirmed\n");
		 /* Called when the HTTP upgrade to WebSocket is confirmed */
		 break;

	 case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION:
		 info("openai_rt: Filter protocol connection\n");
		 break;

	 case LWS_CALLBACK_RECEIVE_CLIENT_HTTP:
		 info("openai_rt: Received HTTP response from server\n");
		 break;

	 case LWS_CALLBACK_COMPLETED_CLIENT_HTTP:
		 info("openai_rt: HTTP client transaction completed\n");
		 break;

	/* LWS_CALLBACK_CLIENT_CONFIRM_EXTENSION_SUPPORTED or similar */
	case 38:
		info("openai_rt: WebSocket callback: reason=38 (likely client"
		     " confirm extension)\n");
		break;

	default:
		info("openai_rt: WebSocket callback: reason=%d (unhandled)\n",
			reason);
		if (len > 0 && in) {
			info("openai_rt: Callback data: %.200s%s\n",
				 (const char *)in, len > 200 ? "..." : "");
		}
		break;
	 }

	 return 0;
 }

 static int add_auth_headers(void *in, size_t len)
 {
	 struct ai_model *model = get_ai_model();
	 if (!model || !model->add_auth_headers) {
		 warning("openai_rt: AI model not initialized\n");
		 return -1;
	 }
	 return model->add_auth_headers(in, len);
 }

 /* Connect to AI model WebSocket */
 int connect_openai_ws(void)
 {
	 struct lws_client_connect_info ccinfo;
	 struct ai_model *model = get_ai_model();
	 char address[256];
	 char path[512];
	 int port;

	 if (g_oairt.ws_state != WS_DISCONNECTED) {
		 warning("openai_rt: WebSocket already connected or"
			 " connecting\n");
		 return EALREADY;
	 }
	 if (!g_oairt.ws_context) {
		 warning("openai_rt: WebSocket context not initialized\n");
		 return ENOMEM;
	 }
	 if (!model || !model->get_connection_info) {
		 warning("openai_rt: AI model not initialized\n");
		 return EINVAL;
	 }

	 int err = model->get_connection_info(address, sizeof(address), &port,
		path, sizeof(path));
	 if (err) {
		 warning("openai_rt: Failed to get connection info: %m\n",
			err);
		 return err;
	 }

	 info("openai_rt: Initiating WebSocket connection to AI model...\n");
	 g_oairt.ws_state = WS_CONNECTING;

	 memset(&ccinfo, 0, sizeof(ccinfo));
	 ccinfo.context = g_oairt.ws_context;
	 ccinfo.address = address;
	 ccinfo.port = port;
	 ccinfo.path = path;
	/* SNI / Host header */
	 ccinfo.host = address;
	 ccinfo.origin = NULL;                            /* Not required */
	/* DO NOT send Sec-WebSocket-Protocol */
	 ccinfo.protocol = NULL;
	/* Auto-select WS version */
	 ccinfo.ietf_version_or_minus_one = -1;
	/* Verify cert + hostname in prod */
	 ccinfo.ssl_connection = LCCSCF_USE_SSL;
	/* Single authoritative pointer */
	 ccinfo.pwsi = &g_oairt.ws_client;

	 info("openai_rt: Connecting to wss://%s:%d%s\n",
		  ccinfo.address, ccinfo.port, ccinfo.path);
	 info("openai_rt: Calling lws_client_connect_via_info...\n");

	 g_oairt.ws_client = lws_client_connect_via_info(&ccinfo);
	 if (!g_oairt.ws_client) {
		 warning("openai_rt: Failed to connect to AI"
			 " model WebSocket\n");
		 g_oairt.ws_state = WS_DISCONNECTED;
		 return ENOMEM;
	 }

	 info("openai_rt: lws_client_connect_via_info returned: %p\n",
		(void*)g_oairt.ws_client);
	 info("openai_rt: WebSocket connection initiated, waiting for"
	      " callback...\n");
	 return 0;
 }

 /* Disconnect from AI model */
 void disconnect_openai_ws(void)
 {
	 if (g_oairt.ws_state == WS_CONNECTED && g_oairt.ws_client) {
		 g_oairt.ws_state = WS_DISCONNECTING;
		 lws_callback_on_writable(g_oairt.ws_client);
	 }

	 g_oairt.ws_state = WS_DISCONNECTED;
	 g_oairt.ws_client = NULL;
	 g_oairt.session_ready = false;
	 g_oairt.session_cfg_applied = false;
	 g_oairt.setup_sent = false;
	 g_oairt.conversation_kick_pending = false;
	 websocket_clear_message_queue();
	 tool_batch_reset();

	 info("openai_rt: WebSocket disconnected from AI model\n");
 }

 /* Check if WebSocket is ready for use */
 bool websocket_is_ready(void)
 {
	 return (g_oairt.ws_state == WS_CONNECTED && g_oairt.session_ready);
 }

 /* Wait for WebSocket to be ready (with timeout) */
 int websocket_wait_ready(int timeout_ms)
 {
	 uint64_t start_time = lws_now_usecs();
	 uint64_t timeout_us = timeout_ms * 1000ULL;

	 while (!websocket_is_ready()) {
		 uint64_t now = lws_now_usecs();
		 if (now - start_time > timeout_us) {
			 warning("openai_rt: WebSocket ready timeout after %d"
				 " ms\n", timeout_ms);
			 return ETIMEDOUT;
		 }
		 /* Small sleep to avoid busy waiting */
		 sys_msleep(10);
	 }

	 info("openai_rt: WebSocket is ready for use\n");
	 return 0;
 }

 /* Get WebSocket status string for debugging */
 const char *websocket_status_string(void)
 {
	 switch (g_oairt.ws_state) {
	 case WS_DISCONNECTED:
		 return "DISCONNECTED";
	 case WS_CONNECTING:
		 return "CONNECTING";
	 case WS_CONNECTED:
		 return g_oairt.session_ready
			 ? "CONNECTED_READY"
			 : "CONNECTED_NOT_READY";
	 case WS_DISCONNECTING:
		 return "DISCONNECTING";
	 default:
		 return "UNKNOWN";
	 }
 }

void websocket_kick_session_setup(void)
{
	if (g_oairt.ws_state == WS_CONNECTED && !g_oairt.session_ready &&
	    !g_oairt.setup_sent)
		send_session_update();

	ws_wake_service();

	if (g_oairt.ws_state == WS_CONNECTED && g_oairt.ws_client)
		lws_callback_on_writable(g_oairt.ws_client);
}


static void process_conversation_kick_pending(void)
{
	if (!g_oairt.conversation_kick_pending)
		return;
	if (g_oairt.ws_state != WS_CONNECTED || !g_oairt.ws_client)
		return;
	if (!g_oairt.session_cfg_applied || g_oairt.wait_for_greeting)
		return;
	if (!g_oairt.call_active)
		return;
	if (g_audio.response_created)
		return;

	g_oairt.conversation_kick_pending = false;
	info("openai_rt: Starting AI conversation (wait_for_greeting=no)\n");
	send_response_create();
	if (g_oairt.ws_client)
		lws_callback_on_writable(g_oairt.ws_client);
}

 void send_session_update(void)
{
	struct ai_model *model = get_ai_model();
	char *json_msg = NULL;
	int err;

	if (g_oairt.ws_state != WS_CONNECTED) {
		warning("openai_rt: Cannot send session update - WebSocket not"
			" connected\n");
		return;
	}

	if (g_oairt.setup_sent) {
		DEBUG_INFO("openai_rt: Setup already sent on this connection,"
			   " skipping\n");
		return;
	}

	if (!model || !model->build_session_update) {
		warning("openai_rt: AI model not initialized\n");
		return;
	}

	err = model->build_session_update(g_oairt.prompt, &json_msg);
	if (err) {
		warning("openai_rt: Failed to build session update: %m\n",
			err);
		return;
	}

	info("openai_rt: Session update: %s\n", json_msg);

	if (json_msg) {
		err = queue_message_to_openai(json_msg, str_len(json_msg),
			NULL, NULL);
		if (err) {
			warning("openai_rt: Failed to queue session update:"
				" %m\n", err);
		}
		else {
			g_oairt.setup_sent = true;
			info("openai_rt: Session update queued (PCM16"
			     " format)\n");
		}
		mem_deref(json_msg);
	}
}

static struct ws_message *ws_message_alloc(const char *json_msg,
	size_t len, void (*callback)(void *arg, int err), void *arg)
{
	struct ws_message *msg = mem_zalloc(sizeof(*msg),
		ws_message_destructor);
	if (!msg)
		return NULL;

	msg->type = WS_MSG_TO_OPENAI;
	msg->data = mem_alloc(LWS_PRE + len + 1, NULL);
	if (!msg->data) {
		mem_deref(msg);
		return NULL;
	}

	memcpy(msg->data + LWS_PRE, json_msg, len);
	msg->data[LWS_PRE + len] = '\0';
	msg->len = len;
	msg->callback = callback;
	msg->arg = arg;

	return msg;
}

/* Append to the outgoing queue and wake the I/O thread. */
static void ws_message_enqueue(struct ws_message *msg)
{
	pthread_mutex_lock(&g_oairt.ws_mutex);
	list_append(&g_oairt.to_openai_queue, &msg->le, msg);
	pthread_mutex_unlock(&g_oairt.ws_mutex);

	/* Wake the I/O thread right away */
	ws_wake_service();

	/* If already connected, also request a writable callback */
	if (g_oairt.ws_state == WS_CONNECTED && g_oairt.ws_client) {
		lws_callback_on_writable(g_oairt.ws_client);
	}
	else {
		DEBUG_INFO("ws_message_enqueue: Not requesting writable"
			   " (state=%d, client=%p)\n",
			   g_oairt.ws_state, g_oairt.ws_client);
	}
}

int queue_message_to_openai(const char *json_msg, size_t len,
	void (*callback)(void *arg, int err), void *arg)
{
	struct ws_message *msg;

	if (g_oairt.ws_state == WS_DISCONNECTED ||
		g_oairt.ws_state == WS_DISCONNECTING) {
		return ENOTCONN;
	}

	msg = ws_message_alloc(json_msg, len, callback, arg);
	if (!msg)
		return ENOMEM;

	ws_message_enqueue(msg);

	return 0;
}

 /* Queue message received from OpenAI */
 int queue_message_from_openai(const uint8_t *data, size_t len)
 {
	 struct ws_message *msg;

	 /*DEBUG_INFO("Queueing message from OpenAI: %zu bytes\n", len); */

	 msg = mem_zalloc(sizeof(*msg), ws_message_destructor);
	 if (!msg)
		 return ENOMEM;

	 msg->type = WS_MSG_FROM_OPENAI;
	 msg->data = mem_alloc(len, NULL);
	 if (!msg->data) {
		 mem_deref(msg);
		 return ENOMEM;
	 }

	 memcpy(msg->data, data, len);
	 msg->len = len;
	 msg->callback = NULL;
	 msg->arg = NULL;

	 pthread_mutex_lock(&g_oairt.ws_mutex);
	 list_append(&g_oairt.from_openai_queue, &msg->le, msg);
	 pthread_mutex_unlock(&g_oairt.ws_mutex);

	/* DEBUG_INFO("Message queued from OpenAI, queue size: %u\n",
	 * list_count(&g_oairt.from_openai_queue)); */

	 return 0;
 }

 /* WebSocket thread - processes events and audio queues */
 static void *websocket_thread(void *arg)
 {
	 (void)arg;

	 info("openai_rt: WebSocket thread started\n");

	 /* Create WebSocket context in this thread */
	 struct lws_context_creation_info ctx_info;
	 memset(&ctx_info, 0, sizeof(ctx_info));
	 ctx_info.port = CONTEXT_PORT_NO_LISTEN;
	 ctx_info.protocols = protocols;
	 ctx_info.gid = -1;
	 ctx_info.uid = -1;
	/* pure client; no explicit vhost */
	 ctx_info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

	 g_oairt.ws_context = lws_create_context(&ctx_info);
	 if (!g_oairt.ws_context) {
		 warning("openai_rt: Failed to create WebSocket context in"
			 " thread\n");
		 return NULL;
	 }

	 /* Enable libwebsockets debugging after context creation */
	 lws_set_log_level(LLL_ERR | LLL_WARN | LLL_NOTICE, NULL);

	 info("openai_rt: WebSocket context created in thread\n");

	 /* Connect immediately when thread starts */
	 if (g_oairt.ws_state == WS_DISCONNECTED && g_oairt.ws_context) {
		 info("openai_rt: WebSocket thread starting - connecting to"
		      " OpenAI immediately\n");
		 int err = connect_openai_ws();
		 if (err) {
			 warning("openai_rt: Failed to connect to OpenAI on"
				 " thread start: %m\n", err);
		 }
	 }

	 uint64_t last_connecting_log = 0;
	 uint64_t connection_start_time = 0;

	while (g_oairt.ws_thread_running) {
		/* Service the context to advance the handshake / IO state
		 * machine */
		int ret = lws_service(g_oairt.ws_context, 10);
		if (ret < 0) {
			warning("openai_rt: lws_service returned error: %d\n",
				ret);
		}

		if (!g_oairt.ws_thread_running) {
			break;
		}

		uint64_t now = lws_now_usecs();

		 /* Log when actively connecting; manage a simple timeout */
		 if (g_oairt.ws_state == WS_CONNECTING) {
			 if (!connection_start_time) {
				 connection_start_time = now;
			 }
			 if (now - last_connecting_log > 1000000) { /* 1s */
				 info("openai_rt: WebSocket thread actively"
				      " servicing connection, state: %d\n"
				      , g_oairt.ws_state);
				 last_connecting_log = now;
			 }
			/* 30s */
			 if (now - connection_start_time > 30000000) {
				 warning("openai_rt: WebSocket connection"
					 " timeout, aborting\n");
				 g_oairt.ws_state = WS_DISCONNECTED;
				 g_oairt.ws_client = NULL;
				 g_oairt.setup_sent = false;
				 connection_start_time = 0;
				 last_connecting_log = 0;
			 }
		 }

		/* Auto-reconnect if disconnected and not actively connecting
		 */
		 if (g_oairt.ws_state == WS_DISCONNECTED&&
			!g_oairt.ws_client&&
			g_oairt.ws_context) {
			 static uint64_t last_reconnect_attempt = 0;
			 uint64_t now3 = now;
			/* 10s */
			 if (now3 - last_reconnect_attempt > 10000000) {
				 info("openai_rt: Attempting to reconnect to"
				      " OpenAI...\n");
				 int err = connect_openai_ws();
				 if (err) {
					 warning("openai_rt: Reconnection"
						 " attempt failed: %m\n", err);
				 }
				 last_reconnect_attempt = now3;
			 }
		 }

		 /* Process events from event queue */
		 struct audio_event *event = audio_get_next_event();
		 if (event) {
			 switch (event->type) {
			 case EVENT_CALL_START:
				 info("openai_rt: Processing call start"
				      " event\n");
				 if (g_oairt.ws_state == WS_DISCONNECTED&&
					g_oairt.ws_context) {
					 info("openai_rt: Call start -"
					      " ensuring OpenAI"
					      " connection...\n");
					 int err = connect_openai_ws();
					 if (err) {
						 warning("openai_rt: Failed to"
							 " connect to OpenAI:"
							 " %m\n", err);
					 }
				}
				else if (g_oairt.ws_state == WS_CONNECTED&&
					!g_oairt.session_ready &&
						   !g_oairt.setup_sent) {
					info("openai_rt: Call start -"
					     " WebSocket connected but session"
					     " not ready, sending"
					     " session.update\n");
					send_session_update();
					if (g_oairt.ws_client) {
						lws_callback_on_writable(
							g_oairt.ws_client);
					}
				}
				else {
					DEBUG_INFO("Call start event ignored"
						   " (ws_state=%d,"
						   " session_ready=%d)\n",
						   g_oairt.ws_state,
						   g_oairt.session_ready);
				}
				 break;

			 case EVENT_CALL_END:
				 info("openai_rt: Processing call end"
				      " event\n");
				 /* Keep connection alive */
				 info("openai_rt: Call ended, keeping OpenAI"
				      " connection alive\n");
				 break;
			 }
			mem_deref(event);
		}

		process_conversation_kick_pending();

		/* Gentle pacing */
		sys_msleep(g_oairt.ws_state == WS_CONNECTING ? 1 : 10);
	}

	 info("openai_rt: WebSocket thread ended\n");
	 return NULL;
 }

 /* Initialize WebSocket */
 int websocket_init(void)
 {

	 g_oairt.ws_state = WS_DISCONNECTED;
	 g_oairt.ws_context = NULL; /* Will be created in thread */
	 g_oairt.ws_client = NULL;

	 /* Initialize message queues */
	 list_init(&g_oairt.to_openai_queue);
	 list_init(&g_oairt.from_openai_queue);

	 /* Initialize mutex and condition variable */
	 pthread_mutex_init(&g_oairt.ws_mutex, NULL);
	 pthread_cond_init(&g_oairt.ws_cond, NULL);

	 /* Start WebSocket thread */
	 g_oairt.ws_thread_running = true;
	 int ret = pthread_create(&g_oairt.ws_thread, NULL, websocket_thread,
		NULL);
	 if (ret != 0) {
		 warning("openai_rt: Failed to create WebSocket thread: %d\n",
			ret);
		 pthread_mutex_destroy(&g_oairt.ws_mutex);
		 pthread_cond_destroy(&g_oairt.ws_cond);
		 return ENOMEM;
	 }

	 info("openai_rt: WebSocket subsystem initialized - thread will create"
	      " context and connect immediately\n");
	 return 0;
 }

 /* Close WebSocket */
 void websocket_close(void)
 {

	 /* Stop WebSocket thread */
	 g_oairt.ws_thread_running = false;

	 /* Cancel any ongoing WebSocket operations to speed up shutdown */
	 if (g_oairt.ws_context) {
		 lws_cancel_service(g_oairt.ws_context);
	 }

	 /* Signal thread to wake up and exit */
	 pthread_mutex_lock(&g_oairt.ws_mutex);
	 pthread_cond_signal(&g_oairt.ws_cond);
	 pthread_mutex_unlock(&g_oairt.ws_mutex);

	 /* Wait for thread to exit with timeout */
	 struct timespec timeout;
	 clock_gettime(CLOCK_REALTIME, &timeout);
	 timeout.tv_sec += 2; /* 2 second timeout */

	 int join_result = pthread_timedjoin_np(g_oairt.ws_thread, NULL,
		&timeout);
	 if (join_result == ETIMEDOUT) {
		 warning("openai_rt: WebSocket thread shutdown timeout,"
			 " forcing termination\n");
		/* Force thread termination - this is not ideal but prevents
		 * hanging */
		 pthread_cancel(g_oairt.ws_thread);
		 pthread_join(g_oairt.ws_thread, NULL);
	 }
	 else if (join_result != 0) {
		 warning("openai_rt: WebSocket thread join failed: %d\n",
			join_result);
	 }

	 /* Disconnect if connected */
	 if (g_oairt.ws_state != WS_DISCONNECTED) {
		 disconnect_openai_ws();
	 }

	 /* Destroy context */
	 if (g_oairt.ws_context) {
		 lws_context_destroy(g_oairt.ws_context);
		 g_oairt.ws_context = NULL;
	 }

	 g_oairt.ws_client = NULL;
	 g_oairt.ws_state = WS_DISCONNECTED;
	 g_oairt.session_ready = false;
	 g_oairt.session_cfg_applied = false;
	 g_oairt.setup_sent = false;

		 /* Clear message queues properly */
	websocket_clear_message_queue();
	tool_batch_reset();

	 /* Clean up mutex and condition variable */
	 pthread_mutex_destroy(&g_oairt.ws_mutex);
	 pthread_cond_destroy(&g_oairt.ws_cond);

	 DEBUG_INFO("WebSocket subsystem closed\n");
 }

 /* Request immediate WebSocket shutdown */
 void websocket_request_shutdown(void)
 {

	 /* Set shutdown flag */
	 g_oairt.ws_thread_running = false;

	 /* Cancel any ongoing WebSocket operations */
	 if (g_oairt.ws_context) {
		 lws_cancel_service(g_oairt.ws_context);
	 }

	 /* Signal thread to wake up immediately */
	 pthread_mutex_lock(&g_oairt.ws_mutex);
	 pthread_cond_signal(&g_oairt.ws_cond);
	 pthread_mutex_unlock(&g_oairt.ws_mutex);

	 DEBUG_INFO("WebSocket shutdown requested\n");
 }

 /* Force WebSocket shutdown with timeout */
 void websocket_force_shutdown(int timeout_ms)
 {

	 /* Request shutdown first */
	 websocket_request_shutdown();

	 /* Wait for thread to exit with timeout */
	 struct timespec timeout;
	 clock_gettime(CLOCK_REALTIME, &timeout);
	 timeout.tv_nsec += timeout_ms * 1000000; /* Convert ms to ns */
	 if (timeout.tv_nsec >= 1000000000) {
		 timeout.tv_sec += timeout.tv_nsec / 1000000000;
		 timeout.tv_nsec %= 1000000000;
	 }

	 int join_result = pthread_timedjoin_np(g_oairt.ws_thread, NULL,
		&timeout);
	 if (join_result == ETIMEDOUT) {
		 warning("openai_rt: WebSocket thread force shutdown timeout,"
			 " cancelling thread\n");
		 pthread_cancel(g_oairt.ws_thread);
		 pthread_join(g_oairt.ws_thread, NULL);
	 }

	 DEBUG_INFO("WebSocket force shutdown completed\n");
 }

 /* Clear all pending messages in the WebSocket queue */
 void websocket_clear_message_queue(void)
 {
	 struct ws_message *msg = NULL;
	 struct le *le;

	 DEBUG_INFO("Clearing WebSocket message queue\n");

	 pthread_mutex_lock(&g_oairt.ws_mutex);

	 /* Clear messages to OpenAI */
	 le = list_head(&g_oairt.to_openai_queue);
	 while (le) {
		 struct le *next = le->next;
		 msg = list_ledata(le);
		 list_unlink(le);
		 mem_deref(msg);
		 le = next;
	 }

	 /* Clear messages from OpenAI */
	 le = list_head(&g_oairt.from_openai_queue);
	 while (le) {
		 struct le *next = le->next;
		 msg = list_ledata(le);
		 list_unlink(le);
		 mem_deref(msg);
		 le = next;
	 }

	 pthread_mutex_unlock(&g_oairt.ws_mutex);

	 DEBUG_INFO("WebSocket message queue cleared\n");
 }
