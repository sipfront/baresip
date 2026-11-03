/**
 * @file turndetect.c  Speaking-turn detection on the incoming audio
 *
 * Copyright (C) 2026 Sipfront GmbH
 */
#include <string.h>
#include <sys/time.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include "fvad/include/fvad.h"


/**
 * @defgroup turndetect turndetect
 *
 * Speaking-turn detection on the far end's audio
 *
 * An audio filter on the decode path. It watches the audio the far end
 * sends and emits two application events: TURN_START once the far end
 * has spoken for turndetect_min_speech milliseconds, and TURN_END once
 * it has then been quiet for turndetect_min_silence milliseconds -- the
 * hangover, so a pause inside an answer is not the end of it. Both carry
 * the turn's number, the moment as a wall-clock timestamp and as
 * milliseconds since the call was established; TURN_END carries the
 * turn's duration too:
 *
 *   TURN_START turn=3 timestamp=1762183200.123456 since_ms=12340
 *   TURN_END   turn=3 timestamp=1762183204.523456 since_ms=16740
 *              duration_ms=4400
 *
 * Detection runs from CALL_ESTABLISHED to CALL_CLOSED; the turn count
 * starts at one for every call. Early media (ringback) makes no turns.
 *
 * Two engines tell speech from silence, per 10 ms of audio:
 *
 *   webrtc   the WebRTC voice activity detector (libfvad, vendored under
 *            fvad/ -- BSD, no dependency): a GMM over six sub-bands with
 *            an aggressiveness of 0 (lenient) to 3 (strict). It takes
 *            16-bit audio at 8, 16, 32 or 48 kHz; a stream in another
 *            shape falls back to the energy engine. The default.
 *   energy   the frame's level in dBov, read as speech when it is above
 *            an absolute threshold and above a tracked noise floor by a
 *            margin. The floor follows a drop at once and rises slowly,
 *            so a line with comfort noise still reads as silence.
 *
 * The minimum speech and the hangover are applied on top of the per-chunk
 * decisions; that is what makes a turn out of frames, and it is the same
 * for both engines.
 *
 * Configuration (~/.baresip/config):
 *
 *   turndetect_engine        webrtc     # webrtc | energy
 *   turndetect_mode          2          # webrtc aggressiveness 0..3
 *   turndetect_threshold     -40        # energy: speech above, dBov
 *   turndetect_margin        10         # energy: dB over the noise floor
 *   turndetect_min_speech    200        # ms of speech that start a turn
 *   turndetect_min_silence   800        # ms of silence that end it
 *
 * Commands:
 *
 *   turndetect_status        the engine, the settings, the call's state
 */


enum {
	CHUNK_MS = 10,          /* the engines decide per 10 ms of audio  */
	FLOOR_RISE_PERMILLE = 1 /* energy: floor rise per frame, of the gap */
};

enum engine {
	ENGINE_WEBRTC = 0,
	ENGINE_ENERGY
};

static struct {
	enum engine engine;
	uint32_t mode;          /* webrtc aggressiveness 0..3        */
	int32_t threshold;      /* energy: dBov                      */
	uint32_t margin;        /* energy: dB over the noise floor   */
	uint32_t min_speech;    /* ms                                */
	uint32_t min_silence;   /* ms                                */
} cfg = {
	ENGINE_WEBRTC, 2, -40, 10, 200, 800
};

/* The call being listened to: one at a time, as the agent runs them */
static struct {
	bool established;
	uint64_t established_at;  /* jiffies                           */
	uint32_t turn;            /* turns begun in this call          */
	bool speaking;            /* inside a turn                     */
	uint32_t voiced_ms;       /* speech so far, before a turn      */
	uint32_t silence_ms;      /* silence so far, inside a turn     */
	uint64_t turn_start;      /* jiffies                           */
} callst;

struct turndetect_st {
	struct aufilt_dec_st af;  /* inheritance */
	enum engine engine;
	Fvad *vad;
	uint32_t srate;
	uint8_t ch;
	size_t chunk;             /* samples per CHUNK_MS, mono        */
	int16_t *buf;             /* the mono chunk being filled       */
	size_t nbuf;
	double floor;             /* energy: tracked noise floor, dBov */
	bool floor_set;
};

/* An event made on the audio thread, emitted on the main thread */
struct turn_event {
	enum ua_event ev;
	uint32_t turn;
	uint64_t since_ms;
	uint64_t duration_ms;
	double timestamp;
};

static struct mqueue *mq;


static const char *engine_name(enum engine e)
{
	return e == ENGINE_ENERGY ? "energy" : "webrtc";
}


static void mqueue_handler(int id, void *data, void *arg)
{
	struct turn_event *te = data;
	(void)id;
	(void)arg;

	if (te->ev == UA_EVENT_TURN_END) {
		bevent_app_emit(te->ev, NULL,
				"turn=%u timestamp=%.6f since_ms=%llu "
				"duration_ms=%llu",
				te->turn, te->timestamp,
				(unsigned long long)te->since_ms,
				(unsigned long long)te->duration_ms);
	}
	else {
		bevent_app_emit(te->ev, NULL,
				"turn=%u timestamp=%.6f since_ms=%llu",
				te->turn, te->timestamp,
				(unsigned long long)te->since_ms);
	}

	mem_deref(te);
}


/*
 * Queue an event for the main thread. `at` is the moment the turn really
 * started or ended (jiffies): the decision comes min_speech or min_silence
 * later, and the timestamps are corrected for that.
 */
static void emit(enum ua_event ev, uint32_t turn, uint64_t at,
		 uint64_t duration_ms)
{
	struct turn_event *te;
	struct timeval tv;
	const uint64_t now = tmr_jiffies();

	te = mem_zalloc(sizeof(*te), NULL);
	if (!te)
		return;

	gettimeofday(&tv, NULL);

	te->ev          = ev;
	te->turn        = turn;
	te->duration_ms = duration_ms;
	te->since_ms    = at > callst.established_at
		? at - callst.established_at : 0;
	te->timestamp   = (double)tv.tv_sec + (double)tv.tv_usec / 1e6
		- (double)(now > at ? now - at : 0) / 1e3;

	if (mqueue_push(mq, 0, te))
		mem_deref(te);
}


/* One decision of `ms` milliseconds: voiced or not. Both engines end here. */
static void judge(bool voiced, uint32_t ms)
{
	const uint64_t now = tmr_jiffies();

	if (!callst.speaking) {
		if (!voiced) {
			callst.voiced_ms = 0;
			return;
		}

		callst.voiced_ms += ms;
		if (callst.voiced_ms < cfg.min_speech)
			return;

		callst.speaking   = true;
		callst.turn++;
		callst.turn_start = now - callst.voiced_ms;
		callst.silence_ms = 0;
		emit(UA_EVENT_TURN_START, callst.turn, callst.turn_start, 0);
		return;
	}

	if (voiced) {
		callst.silence_ms = 0;
		return;
	}

	callst.silence_ms += ms;
	if (callst.silence_ms < cfg.min_silence)
		return;

	callst.speaking  = false;
	callst.voiced_ms = 0;
	emit(UA_EVENT_TURN_END, callst.turn, now - callst.silence_ms,
	     now - callst.silence_ms - callst.turn_start);
}


/* The energy engine: the frame's level against the threshold and floor */
static void judge_level(struct turndetect_st *st, double level, uint32_t ms)
{
	bool voiced;

	if (!st->floor_set) {
		st->floor     = level;
		st->floor_set = true;
	}
	else if (level < st->floor) {
		st->floor = level;
	}
	else {
		st->floor += (level - st->floor)
			* FLOOR_RISE_PERMILLE / 1000.0;
	}

	voiced = level > (double)cfg.threshold
		&& level > st->floor + (double)cfg.margin;

	judge(voiced, ms);
}


static bool webrtc_rate_ok(uint32_t srate)
{
	return srate == 8000 || srate == 16000 || srate == 32000
		|| srate == 48000;
}


static void dec_destructor(void *arg)
{
	struct turndetect_st *st = arg;

	list_unlink(&st->af.le);
	if (st->vad)
		fvad_free(st->vad);
	mem_deref(st->buf);
}


static int decode_update(struct aufilt_dec_st **stp, void **ctx,
			 const struct aufilt *af, struct aufilt_prm *prm,
			 const struct audio *au)
{
	struct turndetect_st *st;
	(void)ctx;
	(void)au;

	if (!stp || !af || !prm)
		return EINVAL;

	if (*stp)
		return 0;

	st = mem_zalloc(sizeof(*st), dec_destructor);
	if (!st)
		return ENOMEM;

	st->srate  = prm->srate;
	st->ch     = prm->ch ? prm->ch : 1;
	st->engine = cfg.engine;

	if (st->engine == ENGINE_WEBRTC) {
		if (prm->fmt != AUFMT_S16LE || !webrtc_rate_ok(prm->srate)) {
			warning("turndetect: the webrtc engine takes 16-bit "
				"audio at 8/16/32/48 kHz, this stream is %s "
				"at %u Hz: using the energy engine\n",
				aufmt_name(prm->fmt), prm->srate);
			st->engine = ENGINE_ENERGY;
		}
		else {
			st->vad = fvad_new();
			if (!st->vad) {
				mem_deref(st);
				return ENOMEM;
			}
			fvad_set_mode(st->vad, (int)cfg.mode);
			fvad_set_sample_rate(st->vad, (int)prm->srate);

			st->chunk = prm->srate * CHUNK_MS / 1000;
			st->buf   = mem_zalloc(st->chunk * sizeof(int16_t),
					       NULL);
			if (!st->buf) {
				mem_deref(st);
				return ENOMEM;
			}
		}
	}

	info("turndetect: listening with the %s engine (%u Hz, %u ch)\n",
	     engine_name(st->engine), prm->srate, st->ch);

	*stp = (struct aufilt_dec_st *)st;

	return 0;
}


static int decode(struct aufilt_dec_st *aufilt_dec_st, struct auframe *af)
{
	struct turndetect_st *st = (struct turndetect_st *)aufilt_dec_st;
	const int16_t *sampv;
	uint32_t srate;
	size_t i, n;

	if (!st || !af)
		return EINVAL;

	if (!callst.established || !af->sampc)
		return 0;

	srate = af->srate ? af->srate : st->srate;

	if (st->engine == ENGINE_ENERGY) {
		const uint32_t ms = srate
			? (uint32_t)(af->sampc * 1000 / (srate * st->ch)) : 0;

		if (ms)
			judge_level(st, auframe_level(af), ms);

		return 0;
	}

	if (af->fmt != AUFMT_S16LE)
		return 0;

	/* The first channel of each sample, in chunks of CHUNK_MS */
	sampv = af->sampv;
	n     = af->sampc / st->ch;

	for (i = 0; i < n; i++) {
		st->buf[st->nbuf++] = sampv[i * st->ch];

		if (st->nbuf < st->chunk)
			continue;

		judge(fvad_process(st->vad, st->buf, st->chunk) == 1,
		      CHUNK_MS);
		st->nbuf = 0;
	}

	return 0;
}


static void event_handler(enum ua_event ev, struct bevent *event, void *arg)
{
	(void)event;
	(void)arg;

	switch (ev) {

	case UA_EVENT_CALL_ESTABLISHED:
		memset(&callst, 0, sizeof(callst));
		callst.established    = true;
		callst.established_at = tmr_jiffies();
		info("turndetect: call established, counting turns\n");
		break;

	case UA_EVENT_CALL_CLOSED:
		callst.established = false;
		callst.speaking    = false;
		break;

	default:
		break;
	}
}


static int cmd_status(struct re_printf *pf, void *arg)
{
	(void)arg;

	return re_hprintf(pf,
			  "turndetect: engine=%s mode=%u threshold=%d "
			  "margin=%u min_speech=%u min_silence=%u "
			  "established=%s speaking=%s turn=%u\n",
			  engine_name(cfg.engine), cfg.mode, cfg.threshold,
			  cfg.margin, cfg.min_speech, cfg.min_silence,
			  callst.established ? "yes" : "no",
			  callst.speaking ? "yes" : "no",
			  callst.turn);
}


static const struct cmd cmdv[] = {
	{"turndetect_status", 0, 0,
	 "Turn detection: engine, settings, the call's state", cmd_status},
};


static struct aufilt turndetect = {
	.name    = "turndetect",
	.decupdh = decode_update,
	.dech    = decode
};


static int module_init(void)
{
	char engine[16] = "webrtc";
	int err;

	conf_get_str(conf_cur(), "turndetect_engine", engine,
		     sizeof(engine));
	if (!str_casecmp(engine, "energy"))
		cfg.engine = ENGINE_ENERGY;
	else if (!str_casecmp(engine, "webrtc"))
		cfg.engine = ENGINE_WEBRTC;
	else
		warning("turndetect: unknown engine '%s', using webrtc\n",
			engine);

	conf_get_u32(conf_cur(), "turndetect_mode", &cfg.mode);
	if (cfg.mode > 3) {
		warning("turndetect: mode %u is out of 0..3, using 3\n",
			cfg.mode);
		cfg.mode = 3;
	}

	conf_get_i32(conf_cur(), "turndetect_threshold", &cfg.threshold);
	if (cfg.threshold < (int32_t)AULEVEL_MIN)
		cfg.threshold = (int32_t)AULEVEL_MIN;
	if (cfg.threshold > 0)
		cfg.threshold = 0;

	conf_get_u32(conf_cur(), "turndetect_margin", &cfg.margin);
	conf_get_u32(conf_cur(), "turndetect_min_speech", &cfg.min_speech);
	conf_get_u32(conf_cur(), "turndetect_min_silence",
		     &cfg.min_silence);
	if (cfg.min_speech < CHUNK_MS)
		cfg.min_speech = CHUNK_MS;
	if (cfg.min_silence < CHUNK_MS)
		cfg.min_silence = CHUNK_MS;

	err = mqueue_alloc(&mq, mqueue_handler, NULL);
	if (err)
		return err;

	err = cmd_register(baresip_commands(), cmdv, RE_ARRAY_SIZE(cmdv));
	if (err)
		goto out;

	err = bevent_register(event_handler, NULL);
	if (err)
		goto out;

	aufilt_register(baresip_aufiltl(), &turndetect);

	info("turndetect: module loaded, engine=%s mode=%u threshold=%d "
	     "margin=%u min_speech=%ums min_silence=%ums\n",
	     engine_name(cfg.engine), cfg.mode, cfg.threshold, cfg.margin,
	     cfg.min_speech, cfg.min_silence);

	return 0;

 out:
	cmd_unregister(baresip_commands(), cmdv);
	mq = mem_deref(mq);

	return err;
}


static int module_close(void)
{
	aufilt_unregister(&turndetect);
	bevent_unregister(event_handler);
	cmd_unregister(baresip_commands(), cmdv);
	mq = mem_deref(mq);

	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(turndetect) = {
	"turndetect",
	"filter",
	module_init,
	module_close
};
