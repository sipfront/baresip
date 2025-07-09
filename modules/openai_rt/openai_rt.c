#include <re.h>
#include <rem.h>
#include <baresip.h>
#include <curl/curl.h>
#include <libwebsockets.h>

struct ausrc_st {
	struct ausrc_prm prm;
	ausrc_read_h *rh;
	void *arg;
	struct tmr tmr;
	size_t sampc;
};

struct auplay_st {
	struct auplay_prm prm;
	auplay_write_h *wh;
	void *arg;
	struct tmr tmr;
	size_t sampc;
};

struct openai_rt {
	char prompt[256];
	char api_key[256];
	struct lws_context *ws_context;
	struct lws *ws_client;
	struct ausrc *ausrc;
	struct auplay *auplay;

	/* Current instances for routing audio */
	struct auplay_st *play_st;
	struct ausrc_st *src_st;

	/* Audio buffer for OpenAI responses */
	struct mbuf *audio_buffer;
};

static struct openai_rt g_oairt;

/* Forward declarations */
static int connect_openai_ws(void);
static void send_audio_to_openai(struct auframe *af);

static void ausrc_destructor(void *arg)
{
	struct ausrc_st *st = arg;

	tmr_cancel(&st->tmr);

	/* Remove reference if this was the active source */
	if (g_oairt.src_st == st)
		g_oairt.src_st = NULL;
}

static void auplay_destructor(void *arg)
{
	struct auplay_st *st = arg;

	tmr_cancel(&st->tmr);

	/* Remove reference if this was the active playback */
	if (g_oairt.play_st == st)
		g_oairt.play_st = NULL;
}

static void module_destructor(void *arg)
{
	struct openai_rt *ort = arg;

	if (ort->ws_client) {
		lws_close_reason(ort->ws_client, LWS_CLOSE_STATUS_NORMAL, NULL, 0);
		ort->ws_client = NULL;
	}

	if (ort->ws_context) {
		lws_context_destroy(ort->ws_context);
		ort->ws_context = NULL;
	}

	mem_deref(ort->ausrc);
	mem_deref(ort->auplay);
	mem_deref(ort->audio_buffer);
}

static int read_config(void)
{
	conf_get_str(conf_cur(), "openai_rt_prompt", g_oairt.prompt, sizeof(g_oairt.prompt));
	conf_get_str(conf_cur(), "openai_rt_api_key", g_oairt.api_key, sizeof(g_oairt.api_key));

	/* Set defaults if not configured */
	if (!str_isset(g_oairt.prompt))
		str_ncpy(g_oairt.prompt, "You are a helpful assistant.", sizeof(g_oairt.prompt));

	return 0;
}

static int ws_callback(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len)
{
	(void)wsi;
	(void)user;

	switch (reason) {
	case LWS_CALLBACK_CLIENT_ESTABLISHED:
		info("openai_rt: WebSocket connection established\n");
		break;

	case LWS_CALLBACK_CLIENT_RECEIVE:
		/* Received audio data from OpenAI - store it for playback */
		if (len > 0 && g_oairt.audio_buffer) {
			/* Append received audio data to buffer */
			int err = mbuf_write_mem(g_oairt.audio_buffer, in, len);
			if (err) {
				warning("openai_rt: failed to buffer audio data\n");
			}
		}
		break;

	case LWS_CALLBACK_CLIENT_WRITEABLE:
		/* Ready to send data to OpenAI */
		break;

	case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
		warning("openai_rt: WebSocket connection error\n");
		break;

	case LWS_CALLBACK_CLIENT_CLOSED:
		info("openai_rt: WebSocket connection closed\n");
		g_oairt.ws_client = NULL;
		break;

	default:
		break;
	}
	return 0;
}

static const struct lws_protocols protocols[] = {
	{
		.name = "openai_rt",
		.callback = ws_callback,
		.per_session_data_size = 0,
		.rx_buffer_size = 65536,
		.id = 0,
		.user = NULL,
		.tx_packet_size = 0,
	},
	{ NULL, NULL, 0, 0, 0, NULL, 0 } /* terminator */
};

static int connect_openai_ws(void)
{
	struct lws_client_connect_info ccinfo = {0};

	if (g_oairt.ws_client)
		return 0; /* Already connected */

	ccinfo.context = g_oairt.ws_context;
	ccinfo.address = "api.openai.com";
	ccinfo.port = 443;
	ccinfo.path = "/realtime";
	ccinfo.host = ccinfo.address;
	ccinfo.origin = ccinfo.address;
	ccinfo.ssl_connection = LCCSCF_USE_SSL;
	ccinfo.protocol = protocols[0].name;
	ccinfo.userdata = NULL;

	g_oairt.ws_client = lws_client_connect_via_info(&ccinfo);
	return g_oairt.ws_client ? 0 : ENOMEM;
}

static void send_audio_to_openai(struct auframe *af)
{
	if (!g_oairt.ws_client)
		return;

	/* Send audio frame to OpenAI */
	size_t nbytes = auframe_size(af);
	if (nbytes > 0) {
		int ret = lws_write(g_oairt.ws_client, (unsigned char*)af->sampv,
				   nbytes, LWS_WRITE_BINARY);
		if (ret < 0) {
			warning("openai_rt: failed to send audio data\n");
		} else {
			lws_callback_on_writable(g_oairt.ws_client);
		}
	}
}

/* Timer callback to generate silence or OpenAI audio */
static void ausrc_timeout(void *arg)
{
	struct ausrc_st *st = arg;
	struct auframe af;
	void *sampv;
	size_t sampc_frame;

	/* Calculate samples per frame based on ptime */
	sampc_frame = st->prm.srate * st->prm.ch * st->prm.ptime / 1000;

	/* Allocate audio buffer */
	sampv = mem_zalloc(sampc_frame * aufmt_sample_size(st->prm.fmt), NULL);
	if (!sampv) {
		warning("openai_rt: failed to allocate audio buffer\n");
		goto out;
	}

	/* Check if we have audio from OpenAI */
	if (g_oairt.audio_buffer && mbuf_get_left(g_oairt.audio_buffer) > 0) {
		/* Use audio from OpenAI buffer */
		size_t available = mbuf_get_left(g_oairt.audio_buffer);
		size_t needed = sampc_frame * aufmt_sample_size(st->prm.fmt);
		size_t to_copy = (available < needed) ? available : needed;

		memcpy(sampv, mbuf_buf(g_oairt.audio_buffer), to_copy);
		mbuf_advance(g_oairt.audio_buffer, to_copy);

		/* If we copied less than needed, the rest remains silence (zeros) */
	}
	/* else: sampv remains zero-filled (silence) */

	/* Create audio frame and send to baresip */
	auframe_init(&af, st->prm.fmt, sampv, sampc_frame,
		     st->prm.srate, st->prm.ch);

	if (st->rh)
		st->rh(&af, st->arg);

out:
	mem_deref(sampv);

	/* Schedule next frame */
	tmr_start(&st->tmr, st->prm.ptime, ausrc_timeout, st);
}

/* Timer callback to request audio from baresip for playback */
static void auplay_timeout(void *arg)
{
	struct auplay_st *st = arg;
	struct auframe af;
	void *sampv;
	size_t sampc_frame;

	/* Calculate samples per frame based on ptime */
	sampc_frame = st->prm.srate * st->prm.ch * st->prm.ptime / 1000;

	/* Allocate audio buffer */
	sampv = mem_zalloc(sampc_frame * aufmt_sample_size(st->prm.fmt), NULL);
	if (!sampv) {
		warning("openai_rt: failed to allocate playback buffer\n");
		goto out;
	}

	/* Create audio frame and request audio from baresip */
	auframe_init(&af, st->prm.fmt, sampv, sampc_frame,
		     st->prm.srate, st->prm.ch);

	/* Ask baresip to fill the audio frame with call audio */
	if (st->wh)
		st->wh(&af, st->arg);

	/* Now send this audio to OpenAI */
	send_audio_to_openai(&af);

out:
	mem_deref(sampv);

	/* Schedule next frame */
	tmr_start(&st->tmr, st->prm.ptime, auplay_timeout, st);
}

/* Audio source implementation - generates audio (silence or OpenAI responses) */
static int openai_rt_ausrc_alloc(struct ausrc_st **stp, const struct ausrc *as,
                                  struct ausrc_prm *prm, const char *dev,
                                  ausrc_read_h *rh, ausrc_error_h *errh, void *arg)
{
	struct ausrc_st *st;
	int err;

	(void)as;
	(void)dev;
	(void)errh;

	info("openai_rt: opening capture (%u Hz, %d channels, ptime %u)\n",
	     prm->srate, prm->ch, prm->ptime);

	st = mem_zalloc(sizeof(*st), ausrc_destructor);
	if (!st)
		return ENOMEM;

	st->prm = *prm;
	st->rh = rh;
	st->arg = arg;
	st->sampc = prm->srate * prm->ch * prm->ptime / 1000;

	/* Register this source instance globally */
	g_oairt.src_st = st;

	/* Connect to OpenAI if not already connected */
	err = connect_openai_ws();
	if (err) {
		warning("openai_rt: failed to connect WebSocket\n");
		mem_deref(st);
		return err;
	}

	/* Start the audio generation timer */
	tmr_start(&st->tmr, st->prm.ptime, ausrc_timeout, st);

	*stp = st;
	return 0;
}

/* Audio playback implementation - requests audio from calls and sends to OpenAI */
static int openai_rt_auplay_alloc(struct auplay_st **stp, const struct auplay *ap,
                                   struct auplay_prm *prm, const char *dev,
                                   auplay_write_h *wh, void *arg)
{
	struct auplay_st *st;

	(void)ap;
	(void)dev;

	info("openai_rt: opening playback (%u Hz, %d channels, ptime %u)\n",
	     prm->srate, prm->ch, prm->ptime);

	st = mem_zalloc(sizeof(*st), auplay_destructor);
	if (!st)
		return ENOMEM;

	st->prm = *prm;
	st->wh = wh;
	st->arg = arg;
	st->sampc = prm->srate * prm->ch * prm->ptime / 1000;

	/* Register this playback instance globally */
	g_oairt.play_st = st;

	/* Start the audio request timer */
	tmr_start(&st->tmr, st->prm.ptime, auplay_timeout, st);

	*stp = st;
	return 0;
}

static int module_init(void)
{
	struct lws_context_creation_info ctx_info = {0};
	int err;

	read_config();

	/* Initialize audio buffer for OpenAI responses */
	g_oairt.audio_buffer = mbuf_alloc(8192);
	if (!g_oairt.audio_buffer)
		return ENOMEM;

	/* Create WebSocket context */
	ctx_info.port = CONTEXT_PORT_NO_LISTEN;
	ctx_info.protocols = protocols;
	ctx_info.gid = -1;
	ctx_info.uid = -1;

	g_oairt.ws_context = lws_create_context(&ctx_info);
	if (!g_oairt.ws_context) {
		err = ENOMEM;
		goto out;
	}

	/* Register both audio source and playback */
	err = ausrc_register(&g_oairt.ausrc, baresip_ausrcl(),
			     "openai_rt", openai_rt_ausrc_alloc);
	if (err)
		goto out;

	err = auplay_register(&g_oairt.auplay, baresip_auplayl(),
			      "openai_rt", openai_rt_auplay_alloc);
	if (err)
		goto out;

	info("openai_rt module initialized\n");
	return 0;

out:
	module_destructor(&g_oairt);
	return err;
}

static int module_close(void)
{
	module_destructor(&g_oairt);
	info("openai_rt module closed\n");
	return 0;
}

EXPORT_SYM const struct mod_export DECL_EXPORTS(openai_rt) = {
	"openai_rt",
	"audio",
	module_init,
	module_close
};