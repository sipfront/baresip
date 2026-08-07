/**
 * @file trace.c  openai_rt conversation-trace capture
 *
 * Accumulates a structured, timestamped trace of the conversation openai_rt drives
 * against the voice bot under test -- both sides of the dialogue plus the simulator's
 * "observable action" tool-calls and barge-in/interruption events. On call close the
 * trace is written as conversation-trace.json into the artifacts directory so the
 * agent's existing artifact-upload loop ships it to S3 (no agent driver change), where
 * function-voicebot-eval consumes it (transcript for the LLM judge; tool-calls as the
 * observable_actions checks; events + structured_response_times as turn-taking input).
 *
 * Threading: trace_add_* run on the WebSocket thread (from the message parsers);
 * trace_write_file runs on the RE main thread (UA_EVENT_CALL_CLOSED). A single mutex
 * guards the shared lists. Everything is a no-op unless capture is enabled
 * (openai_rt_transcribe=yes), so plain voice / fixed-media calls are unaffected.
 *
 * Copyright (C) 2025 Sipfront
 */
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <re.h>
#include <baresip.h>
#include <json-c/json.h>
#include "openai_rt.h"
#include "trace.h"

#define TRACE_FILENAME "conversation-trace.json"
#define TRACE_SCHEMA   "sipfront.voicebot-trace/1"

/* Some backends (Gemini) stream transcription in many tiny fragments ("Wel", "com",
 * "e"). Consecutive fragments of the SAME role that arrive within this window are
 * merged into one turn; a gap larger than this (e.g. the other party speaking) starts
 * a new turn. OpenAI already delivers whole utterances, so this is effectively a no-op
 * there. */
#define TRACE_COALESCE_MS 3000

struct trace_turn {
	struct le le;
	char *role;
	char *text;
	uint64_t ts_ms;    /* start of the (possibly coalesced) turn */
	uint64_t end_ms;   /* time of the most recent fragment merged in */
	bool emitted;      /* whether this (combined) turn was already logged + evented */
};

struct trace_toolcall {
	struct le le;
	char *name;
	char *arguments;   /* raw JSON arguments string, may be NULL */
	uint64_t ts_ms;
};

struct trace_event {
	struct le le;
	char *kind;
	uint64_t ts_ms;
};

static struct {
	bool inited;
	bool enabled;
	pthread_mutex_t mtx;
	uint64_t call_start_ms;
	struct list turns;       /* struct trace_turn */
	struct list toolcalls;   /* struct trace_toolcall */
	struct list events;      /* struct trace_event */
} g_trace;

static void turn_destructor(void *arg)
{
	struct trace_turn *t = arg;
	mem_deref(t->role);
	mem_deref(t->text);
}

static void toolcall_destructor(void *arg)
{
	struct trace_toolcall *t = arg;
	mem_deref(t->name);
	mem_deref(t->arguments);
}

static void event_destructor(void *arg)
{
	struct trace_event *e = arg;
	mem_deref(e->kind);
}

void trace_init(void)
{
	if (g_trace.inited)
		return;
	memset(&g_trace, 0, sizeof(g_trace));
	pthread_mutex_init(&g_trace.mtx, NULL);
	list_init(&g_trace.turns);
	list_init(&g_trace.toolcalls);
	list_init(&g_trace.events);
	g_trace.inited = true;
}

/* Free all accumulated items. Caller must hold the mutex. */
static void clear_locked(void)
{
	struct le *le;
	while ((le = list_head(&g_trace.turns))) {
		struct trace_turn *t = list_ledata(le);
		list_unlink(le);
		mem_deref(t);
	}
	while ((le = list_head(&g_trace.toolcalls))) {
		struct trace_toolcall *t = list_ledata(le);
		list_unlink(le);
		mem_deref(t);
	}
	while ((le = list_head(&g_trace.events))) {
		struct trace_event *e = list_ledata(le);
		list_unlink(le);
		mem_deref(e);
	}
}

void trace_close(void)
{
	if (!g_trace.inited)
		return;
	pthread_mutex_lock(&g_trace.mtx);
	clear_locked();
	pthread_mutex_unlock(&g_trace.mtx);
	pthread_mutex_destroy(&g_trace.mtx);
	g_trace.inited = false;
}

void trace_set_enabled(bool enabled)
{
	g_trace.enabled = enabled;
}

bool trace_enabled(void)
{
	return g_trace.enabled;
}

void trace_reset(void)
{
	if (!g_trace.inited || !g_trace.enabled)
		return;
	pthread_mutex_lock(&g_trace.mtx);
	clear_locked();
	g_trace.call_start_ms = tmr_jiffies();
	pthread_mutex_unlock(&g_trace.mtx);
}

/* Relative ms since call start. Caller must hold the mutex. */
static uint64_t rel_ms_locked(void)
{
	uint64_t now = tmr_jiffies();
	if (!g_trace.call_start_ms || now < g_trace.call_start_ms)
		return 0;
	return now - g_trace.call_start_ms;
}

/* Log the finished (coalesced) turn and emit it as a VOICEAI_CONTENT event, tagged by
 * side. Called with no lock held (mqueue is independently thread-safe). */
static void emit_turn(const char *role, const char *text)
{
	const char *side = (role && strcmp(role, TRACE_ROLE_SELF) == 0) ? "self" : "other";
	DEBUG_INFO("trace: turn %s: %.500s\n", role, text);
	calls_queue_voiceai_content(side, text);
}

/* Snapshot the last turn (role+text) for emission if it has not been emitted yet, and
 * mark it emitted. Caller must hold the mutex; returns duplicated strings (or NULLs)
 * that the caller emits + frees after unlocking. */
static void take_pending_turn_locked(char **role, char **text)
{
	struct le *tail = list_tail(&g_trace.turns);
	struct trace_turn *last = tail ? list_ledata(tail) : NULL;
	*role = NULL;
	*text = NULL;
	if (last && !last->emitted) {
		if (str_dup(role, last->role) || str_dup(text, last->text)) {
			mem_deref(*role);
			mem_deref(*text);
			*role = *text = NULL;
			return;
		}
		last->emitted = true;
	}
}

void trace_add_turn(const char *role, const char *text)
{
	struct trace_turn *t;
	struct le *tail;
	struct trace_turn *last;
	uint64_t now;
	char *done_role = NULL, *done_text = NULL;

	if (!g_trace.inited || !g_trace.enabled)
		return;
	if (!role || !text || !*text)
		return;

	pthread_mutex_lock(&g_trace.mtx);
	now = rel_ms_locked();

	/* Coalesce with the previous turn when it is still pending (not yet emitted), the
	 * same speaker, and close in time (streamed transcription fragments) -- do not emit
	 * yet, the turn is still growing. Once a turn has been emitted (e.g. by trace_flush)
	 * it must not grow further, or the appended text would never be evented. */
	tail = list_tail(&g_trace.turns);
	last = tail ? list_ledata(tail) : NULL;
	if (last && !last->emitted && strcmp(last->role, role) == 0 &&
	    now >= last->end_ms && (now - last->end_ms) <= TRACE_COALESCE_MS) {
		char *merged = NULL;
		if (re_sdprintf(&merged, "%s%s", last->text, text) == 0 && merged) {
			mem_deref(last->text);
			last->text = merged;
			last->end_ms = now;
		}
		pthread_mutex_unlock(&g_trace.mtx);
		return;
	}

	/* A new turn begins -> the previous turn is now complete; snapshot it to emit the
	 * combined text (once) after we release the lock. */
	take_pending_turn_locked(&done_role, &done_text);

	t = mem_zalloc(sizeof(*t), turn_destructor);
	if (t && str_dup(&t->role, role) == 0 && str_dup(&t->text, text) == 0) {
		t->ts_ms = now;
		t->end_ms = now;
		list_append(&g_trace.turns, &t->le, t);
	}
	else {
		mem_deref(t);
	}
	pthread_mutex_unlock(&g_trace.mtx);

	if (done_text) {
		emit_turn(done_role, done_text);
		mem_deref(done_role);
		mem_deref(done_text);
	}
}

/* Emit the final pending turn (the last speaker's combined text), e.g. at call close. */
void trace_flush(void)
{
	char *role = NULL, *text = NULL;

	if (!g_trace.inited || !g_trace.enabled)
		return;

	pthread_mutex_lock(&g_trace.mtx);
	take_pending_turn_locked(&role, &text);
	pthread_mutex_unlock(&g_trace.mtx);

	if (text) {
		emit_turn(role, text);
		mem_deref(role);
		mem_deref(text);
	}
}

void trace_add_toolcall(const char *name, const char *arguments)
{
	struct trace_toolcall *t;

	if (!g_trace.inited || !g_trace.enabled)
		return;
	if (!name || !*name)
		return;

	t = mem_zalloc(sizeof(*t), toolcall_destructor);
	if (!t)
		return;
	if (str_dup(&t->name, name)) {
		mem_deref(t);
		return;
	}
	if (arguments && *arguments)
		(void)str_dup(&t->arguments, arguments);

	pthread_mutex_lock(&g_trace.mtx);
	t->ts_ms = rel_ms_locked();
	list_append(&g_trace.toolcalls, &t->le, t);
	pthread_mutex_unlock(&g_trace.mtx);

	DEBUG_INFO("trace: observable action %s(%s)\n", name, arguments ? arguments : "");
}

void trace_add_event(const char *kind)
{
	struct trace_event *e;

	if (!g_trace.inited || !g_trace.enabled)
		return;
	if (!kind || !*kind)
		return;

	e = mem_zalloc(sizeof(*e), event_destructor);
	if (!e)
		return;
	if (str_dup(&e->kind, kind)) {
		mem_deref(e);
		return;
	}

	pthread_mutex_lock(&g_trace.mtx);
	e->ts_ms = rel_ms_locked();
	list_append(&g_trace.events, &e->le, e);
	pthread_mutex_unlock(&g_trace.mtx);
}

/* Resolve the directory the trace file is written into: the configured
 * openai_rt_trace_dir (the agent points this at the artifact-upload dir), else
 * the baresip config dir as a best-effort fallback. */
static int resolve_trace_dir(char *dir, size_t sz)
{
	if (str_isset(g_oairt.trace_dir)) {
		str_ncpy(dir, g_oairt.trace_dir, sz);
		return 0;
	}
	return conf_path_get(dir, sz);
}

int trace_write_file(void)
{
	char dir[512];
	char path[600];
	struct json_object *root, *turns, *tcs, *evs;
	struct le *le;
	const char *json_str;
	FILE *fp = NULL;
	int err;

	if (!g_trace.inited || !g_trace.enabled)
		return 0;

	err = resolve_trace_dir(dir, sizeof(dir));
	if (err) {
		warning("openai_rt: trace: cannot resolve output dir: %m\n", err);
		return err;
	}
	if (!fs_isdir(dir)) {
		warning("openai_rt: trace: dir '%s' does not exist, skipping trace write\n", dir);
		return ENOENT;
	}

	pthread_mutex_lock(&g_trace.mtx);

	root = json_object_new_object();
	if (!root) {
		pthread_mutex_unlock(&g_trace.mtx);
		return ENOMEM;
	}
	json_object_object_add(root, "schema", json_object_new_string(TRACE_SCHEMA));
	json_object_object_add(root, "duration_ms", json_object_new_int64((int64_t)rel_ms_locked()));

	turns = json_object_new_array();
	for (le = list_head(&g_trace.turns); le; le = le->next) {
		struct trace_turn *t = list_ledata(le);
		struct json_object *o = json_object_new_object();
		json_object_object_add(o, "role", json_object_new_string(t->role));
		json_object_object_add(o, "speaker", json_object_new_string(t->role));
		json_object_object_add(o, "text", json_object_new_string(t->text));
		json_object_object_add(o, "ts_ms", json_object_new_int64((int64_t)t->ts_ms));
		json_object_array_add(turns, o);
	}
	json_object_object_add(root, "turns", turns);

	tcs = json_object_new_array();
	for (le = list_head(&g_trace.toolcalls); le; le = le->next) {
		struct trace_toolcall *t = list_ledata(le);
		struct json_object *o = json_object_new_object();
		struct json_object *args = NULL;
		json_object_object_add(o, "name", json_object_new_string(t->name));
		if (t->arguments)
			args = json_tokener_parse(t->arguments);
		if (args)
			json_object_object_add(o, "arguments", args);
		else
			json_object_object_add(o, "arguments",
				json_object_new_string(t->arguments ? t->arguments : ""));
		json_object_object_add(o, "ts_ms", json_object_new_int64((int64_t)t->ts_ms));
		json_object_array_add(tcs, o);
	}
	json_object_object_add(root, "observable_actions", tcs);

	evs = json_object_new_array();
	for (le = list_head(&g_trace.events); le; le = le->next) {
		struct trace_event *ev = list_ledata(le);
		struct json_object *o = json_object_new_object();
		json_object_object_add(o, "kind", json_object_new_string(ev->kind));
		json_object_object_add(o, "ts_ms", json_object_new_int64((int64_t)ev->ts_ms));
		json_object_array_add(evs, o);
	}
	json_object_object_add(root, "events", evs);

	json_str = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY);

	if (re_snprintf(path, sizeof(path), "%s/%s", dir, TRACE_FILENAME) < 0) {
		warning("openai_rt: trace: output path too long for dir '%s'\n", dir);
		json_object_put(root);
		pthread_mutex_unlock(&g_trace.mtx);
		return EOVERFLOW;
	}

	err = fs_fopen(&fp, path, "w");
	if (!err && fp && json_str) {
		fputs(json_str, fp);
		fclose(fp);
		info("openai_rt: trace: wrote %s (%u turns, %u actions, %u events)\n",
			path, list_count(&g_trace.turns), list_count(&g_trace.toolcalls),
			list_count(&g_trace.events));
	}
	else {
		warning("openai_rt: trace: failed to write %s: %m\n", path, err);
		if (!err)
			err = EIO;
		if (fp)
			fclose(fp);
	}

	json_object_put(root);

	pthread_mutex_unlock(&g_trace.mtx);
	return err;
}
