/**
 * @file main.c  sf-voiceai: the openai_rt voice AI as a standalone sound device
 *
 * Runs one voice-AI session against two PulseAudio/PipeWire devices, the way the baresip module
 * runs one against a SIP call: the far end's audio (a source) goes to the model, the model's
 * speech goes to a sink. The agent starts it when a call is up, in place of pw-play, and reads
 * its stdout for events (transcript turns, tool calls); stderr carries the log.
 *
 *   sf-voiceai --config FILE [--source NAME] [--sink NAME] [--trace-dir DIR] [--exit-on-hangup]
 *
 * FILE holds the same "openai_rt_*" keys as a baresip config (openai_rt_api_key,
 * openai_rt_backend, openai_rt_prompt, openai_rt_tool_calls, openai_rt_wait_for_greeting,
 * openai_rt_transcribe, openai_rt_trace_dir, ...; see utils.c read_config). Defaults: source
 * SfVirtualSpeakerSink.monitor, sink SfVirtualMicSink (the agent's virtual devices).
 *
 * Stops on SIGTERM/SIGINT, on "stop" from stdin, or on stdin EOF; with --exit-on-hangup also
 * when the model calls hangup_call. On stop the conversation trace is written (when enabled)
 * and the WebSocket is closed; exit status 0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include "openai_rt.h"
#include "ai_model.h"
#include "trace.h"
#include "compat.h"

struct openai_rt g_oairt;

static struct {
	const char *config_file;
	const char *source;
	const char *sink;
	const char *trace_dir;
	bool exit_on_hangup;
	bool stdin_cmds;
	struct conf *conf;
	struct re_fhs *stdin_fhs;
} app = {
	.source = "SfVirtualSpeakerSink.monitor",
	.sink = "SfVirtualMicSink",
	.stdin_cmds = true,
};

static void usage(void)
{
	fprintf(stderr,
		"usage: sf-voiceai --config FILE [options]\n"
		"  --config FILE      openai_rt_* keys, baresip config syntax (required)\n"
		"  --source NAME      PulseAudio source with the far end's audio\n"
		"                     (default SfVirtualSpeakerSink.monitor)\n"
		"  --sink NAME        PulseAudio sink the model speaks into (default SfVirtualMicSink)\n"
		"  --trace-dir DIR    where conversation-trace.json goes (overrides the config)\n"
		"  --exit-on-hangup   end when the model calls hangup_call (default: only emit the event)\n"
		"  --no-stdin         do not read commands from stdin (stop, hangup)\n"
		"stdout: JSON events, one per line; stderr: log\n");
}

static void signal_handler(int sig)
{
	info("openai_rt: signal %d, stopping\n", sig);
	re_cancel();
}

/* "stop" on stdin, or EOF, ends the session like SIGTERM */
static void stdin_handler(int flags, void *arg)
{
	char buf[256];
	ssize_t n;
	(void)arg;

	if (!(flags & FD_READ))
		return;
	n = read(STDIN_FILENO, buf, sizeof(buf) - 1);
	if (n <= 0) {
		info("openai_rt: stdin closed, stopping\n");
		app.stdin_fhs = fd_close(app.stdin_fhs);
		re_cancel();
		return;
	}
	buf[n] = '\0';
	if (strstr(buf, "stop") || strstr(buf, "hangup")) {
		info("openai_rt: stop command on stdin\n");
		re_cancel();
	}
}

static int parse_args(int argc, char *argv[])
{
	static const struct option opts[] = {
		{ "config", required_argument, NULL, 'c' },
		{ "source", required_argument, NULL, 's' },
		{ "sink", required_argument, NULL, 'k' },
		{ "trace-dir", required_argument, NULL, 't' },
		{ "exit-on-hangup", no_argument, NULL, 'x' },
		{ "no-stdin", no_argument, NULL, 'n' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};
	int c;
	while ((c = getopt_long(argc, argv, "c:s:k:t:xnh", opts, NULL)) != -1) {
		switch (c) {
		case 'c': app.config_file = optarg; break;
		case 's': app.source = optarg; break;
		case 'k': app.sink = optarg; break;
		case 't': app.trace_dir = optarg; break;
		case 'x': app.exit_on_hangup = true; break;
		case 'n': app.stdin_cmds = false; break;
		default: usage(); return EINVAL;
		}
	}
	if (!app.config_file) {
		usage();
		return EINVAL;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	int err;
	unsigned uplink_srate;

	err = parse_args(argc, argv);
	if (err)
		return 2;

	err = libre_init();
	if (err) {
		re_fprintf(stderr, "libre_init: %m\n", err);
		return 1;
	}
	(void)sys_coredump_set(true);

	memset(&g_oairt, 0, sizeof(g_oairt));

	err = conf_alloc(&app.conf, app.config_file);
	if (err) {
		re_fprintf(stderr, "cannot read config %s: %m\n", app.config_file, err);
		goto out;
	}
	compat_set_conf(app.conf);

	err = read_config();
	if (err)
		goto out;
	if (!str_isset(g_oairt.api_key)) {
		warning("openai_rt: no API key configured (openai_rt_api_key)\n");
		err = EINVAL;
		goto out;
	}
	if (app.trace_dir)
		str_ncpy(g_oairt.trace_dir, app.trace_dir, sizeof(g_oairt.trace_dir));

	/* OpenAI listens at 24 kHz, Gemini at 16 kHz; both speak at 24 kHz */
	uplink_srate = g_oairt.backend_type == AI_BACKEND_GEMINI_LIVE ? 16000 : 24000;

	trace_init();
	trace_set_enabled(g_oairt.transcribe);
	if (g_oairt.transcribe)
		info("openai_rt: conversation trace enabled (dir: %s)\n",
		     str_isset(g_oairt.trace_dir) ? g_oairt.trace_dir : "<cwd>");

	err = ai_model_init(&g_oairt);
	if (err) {
		warning("openai_rt: ai_model_init: %m\n", err);
		goto out;
	}
	err = websocket_init();
	if (err)
		goto out;
	err = audio_init();
	if (err)
		goto out;
	err = calls_init();
	if (err)
		goto out;
	calls_cli_set_exit_on_hangup(app.exit_on_hangup);

	/* the "call" is up from the start: the agent launches us once the modem/phone call is
	 * active, exactly where it started pw-play before */
	g_oairt.call_active = true;
	trace_reset();
	audio_reset_for_new_call();
	err = audio_pulse_start(app.source, app.sink, uplink_srate, "sf-voiceai");
	if (err) {
		events_emit("\"event\":\"error\",\"error\":\"cannot open the audio devices\"");
		goto out;
	}
	audio_queue_event(EVENT_CALL_START, NULL);

	if (app.stdin_cmds)
		fd_listen(&app.stdin_fhs, STDIN_FILENO, FD_READ, stdin_handler, NULL);

	events_emit("\"event\":\"started\",\"backend\":\"%s\",\"source\":\"%s\",\"sink\":\"%s\"",
		    g_oairt.backend, app.source, app.sink);

	err = re_main(signal_handler);
	if (err)
		warning("openai_rt: re_main: %m\n", err);

	app.stdin_fhs = fd_close(app.stdin_fhs);

	/* same order as the module's CALL_CLOSED + destructor */
	g_oairt.call_active = false;
	audio_pulse_stop();
	audio_stop_threads();
	audio_queue_event(EVENT_CALL_END, NULL);
	trace_flush();
	events_emit("\"event\":\"stopped\"");

out:
	websocket_force_shutdown(500);
	calls_close();     /* writes the conversation trace */
	websocket_close();
	audio_close();
	ai_model_close();
	trace_close();
	app.conf = mem_deref(app.conf);
	libre_close();
	return err ? 1 : 0;
}
