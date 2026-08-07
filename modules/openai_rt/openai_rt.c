/**
 * @file openai_rt.c  OpenAI Realtime API module - main
 */
#include "openai_rt.h"
#include "ai_model.h"
#include "trace.h"

/* Global instance */
struct openai_rt g_oairt;

static void module_destructor(void *arg)
{
	struct openai_rt *ort = arg;
	(void)ort;

	/* Unregister audio drivers first */
	g_oairt.ausrc = mem_deref(g_oairt.ausrc);
	g_oairt.auplay = mem_deref(g_oairt.auplay);

	/* Request immediate WebSocket shutdown to prevent hanging */
	websocket_force_shutdown(500); /* 500ms timeout */

	/* Close all subsystems in reverse order */
	calls_close();
	websocket_close();
	audio_close();
	ai_model_close();
	trace_close();

	/* Clear global state */
	memset(&g_oairt, 0, sizeof(g_oairt));

	DEBUG_INFO("Module destructor completed\n");
}

static int module_init(void)
{
	int err;

	/* Initialize global state */
	memset(&g_oairt, 0, sizeof(g_oairt));

	/* Read configuration */
	err = read_config();
	if (err) {
		warning("openai_rt: failed to read config: %m\n", err);
		return err;
	}

	if (!str_isset(g_oairt.api_key)) {
		warning("openai_rt: No API key configured."
			" Please set openai_rt_api_key in config\n");
		return EINVAL;
	}

	/* Initialize conversation-trace capture (no-op unless
	 * openai_rt_transcribe=yes) */
	trace_init();
	trace_set_enabled(g_oairt.transcribe);
	if (g_oairt.transcribe) {
		info("openai_rt: conversation-trace capture ENABLED"
			" (dir: %s)\n",
			str_isset(g_oairt.trace_dir) ? g_oairt.trace_dir
			: "<config dir>");
	}

	/* Initialize AI model system */
	err = ai_model_init(&g_oairt);
	if (err) {
		warning("openai_rt: failed to initialize AI model: %m\n", err);
		goto out;
	}

	/* Initialize subsystems */
	err = websocket_init();
	if (err) {
		warning("openai_rt: failed to initialize WebSocket:"
			" %m\n", err);
		goto out;
	}


	err = audio_init();
	if (err) {
		warning("openai_rt: failed to initialize audio: %m\n", err);
		goto out;
	}

	audio_register_uplink_filter();

	err = calls_init();
	if (err) {
		warning("openai_rt: failed to initialize call management:"
			" %m\n", err);
		goto out;
	}

	/* Register audio source */

	err = ausrc_register(&g_oairt.ausrc, baresip_ausrcl(), "openai_rt",
			     openai_rt_ausrc_alloc);
	if (err) {
		warning("openai_rt: failed to register audio source:"
			" %m\n", err);
		goto out;
	}

	/* Register audio player */

	err = auplay_register(&g_oairt.auplay, baresip_auplayl(), "openai_rt",
			      openai_rt_auplay_alloc);
	if (err) {
		warning("openai_rt: failed to register audio player:"
			" %m\n", err);
		goto out;
	}

	info("openai_rt: Module initialized successfully\n");
	info("openai_rt: Waiting for calls...\n");

	DEBUG_INFO("All subsystems initialized\n");
	return 0;

out:
	module_destructor(&g_oairt);
	return err;
}

static int module_close(void)
{
	module_destructor(&g_oairt);
	info("openai_rt: Module closed\n");
	return 0;
}

EXPORT_SYM const struct mod_export DECL_EXPORTS(openai_rt) = {
	"openai_rt",
	"sound",
	module_init,
	module_close
};