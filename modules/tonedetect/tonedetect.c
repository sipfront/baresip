/**
 * @file tonedetect.c  Audio filter module for tone generation and detection
 *
 * Copyright (C) 2025
 */

#include <math.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>

#define PI 3.14159265358979323846
#define TONE_AMPLITUDE 0.3f
#define DETECTION_WINDOW_MS 20   /* shorter window for lower timestamp quantization */
#define DETECTION_HOP_MS    2    /* evaluate every 2ms for finer detection timing */

/* Detection tuning (receiver) - balanced for reliable detection */
#define DETECT_RATIO_THRESHOLD       0.15  /* stricter for real-audio environments */
#define DETECT_PEAK_SEPARATION       1.30  /* stronger separation from other peaks */
#define DETECT_MIN_BLOCK_ENERGY      2.5e8 /* scaled down for shorter windows */
#define DETECT_CONSECUTIVE_BLOCKS    3     /* add temporal stability against speech/music transients */
#define DETECT_SUPPRESS_MS           3000  /* suppress repeat events */
#define DETECT_MIN_MAGNITUDE         80.0   /* reduce weak false positives */
#define DETECT_FIRST_SEEN_HOLD_MS    120   /* keep first_seen across short same-pair dropouts */
#define DETECT_MAX_CONFIRM_DELAY_MS  25.0  /* reject/re-anchor stale first_seen timestamps */
#define DETECT_DUAL_BALANCE_MIN      0.55  /* second peak must be close enough to first */
#define DETECT_TOP2_SHARE_MIN        0.78  /* top 2 peaks must dominate tracked target energy */
#define RTP_WARMUP_SUPPRESS_MS       1000  /* ignore startup transients right after RTP establish */

/* Sender tone shaping to reduce spectral leakage */
#define TONE_RAMP_MS                 2     /* fade-in/out (2ms) for 15ms tones - reduces spectral leakage */

/**
 * @defgroup tonedetect tonedetect
 *
 * Audio filter module that can:
 * - Generate and inject tones of specified frequencies into the encoder path
 * - Detect tones of specified frequencies in the decoder path
 * - Emit events when tones are sent or received
 *
 * Configuration:
 * \verbatim
 *  audio_filter        tonedetect
 * \endverbatim
 */

struct tonedetect_st {
	union {
		struct aufilt_enc_st eaf;
		struct aufilt_dec_st daf;
	} u;

	/* Tone generation (encoder) */
	struct {
		bool active;
		uint32_t frequency;
		uint32_t frequency2;
		uint32_t duration_ms;
		uint64_t start_time;
		uint64_t last_tone_end_time;  /* Time when last tone ended */
		double phase;
		double phase2;
		uint32_t srate;
		size_t sample_index;
		size_t total_samples;
		size_t ramp_samples;
		size_t current_tone_index; /* index in pair-list */
		size_t tone_id;  /* ID of currently active tone */
		double first_packet_timestamp;  /* Host timestamp when tone generation starts */
	} gen;

	/* Tone detection (decoder) */
	struct {
		uint32_t *frequencies;      /* Array of frequencies to detect */
		size_t num_frequencies;
		double *goertzel_coeffs;    /* Goertzel coefficients */
		double *goertzel_q1;        /* Goertzel state Q1 */
		double *goertzel_q2;        /* Goertzel state Q2 */
		size_t detection_window_samples;  /* Window size in samples */
		size_t hop_samples;               /* Hop size in samples */
		int16_t *ring;                    /* Ring buffer for windowed evaluation */
		size_t ring_pos;                  /* Next write position (points to oldest sample) */
		size_t ring_count;                /* Number of valid samples in ring */
		size_t hop_count;                 /* Samples since last evaluation */
		double *window;            /* Window coefficients (Hamming) */
		size_t candidate_pair_index;
		uint8_t candidate_count;
		double first_packet_timestamp;  /* Unix timestamp when first packet with tone is decoded */
		uint64_t last_emit_time;
		size_t last_emit_index;    /* 0-based index */
		bool last_emit_valid;
		uint32_t srate;
	} det;
};

/* Global configuration */
static struct {
	uint32_t *send_frequencies;     /* Frequencies to send (low + high sets) */
	size_t num_send_frequencies;
	size_t num_low_frequencies;     /* Number of low frequencies (first N) */
	size_t num_high_frequencies;    /* Number of high frequencies (remaining) */
	uint8_t *send_pair_a;           /* Pair-list: index into send_frequencies (low) */
	uint8_t *send_pair_b;           /* Pair-list: index into send_frequencies (high) */
	size_t num_send_pairs;
	uint32_t *detect_frequencies;   /* Frequencies to detect */
	size_t num_detect_frequencies;
	size_t num_detect_low;          /* Number of low frequencies in detect set */
	size_t num_detect_high;         /* Number of high frequencies in detect set */
	uint32_t tone_duration_ms;      /* Duration of generated tones */
	bool enable_tone_generation;    /* Enable/disable tone generation */
} config = {
	.send_frequencies = NULL,
	.num_send_frequencies = 0,
	.send_pair_a = NULL,
	.send_pair_b = NULL,
	.num_send_pairs = 0,
	.num_low_frequencies = 0,
	.num_high_frequencies = 0,
	.detect_frequencies = NULL,
	.num_detect_frequencies = 0,
	.num_detect_low = 0,
	.num_detect_high = 0,
	.tone_duration_ms = 80,   /* longer tone improves robust lock with short windows */
	.enable_tone_generation = false  /* Default: disabled */
};

/* Global state to track if call is ready for tone generation/detection */
static struct {
	bool call_established;  /* CALL_ESTABLISHED event received */
	bool rtp_established;   /* CALL_RTPESTAB event received (for audio) */
	uint64_t rtp_established_time; /* jiffies when audio RTP became established */
} tonedetect_call_state = {
	.call_established = false,
	.rtp_established = false,
	.rtp_established_time = 0
};

static void enc_destructor(void *arg)
{
	struct tonedetect_st *st = arg;
	list_unlink(&st->u.eaf.le);
	/* Note: mem_deref(st) is called automatically by the mem system */
}

static void dec_destructor(void *arg)
{
	struct tonedetect_st *st = arg;
	list_unlink(&st->u.daf.le);
	mem_deref(st->det.frequencies);
	mem_deref(st->det.goertzel_coeffs);
	mem_deref(st->det.goertzel_q1);
	mem_deref(st->det.goertzel_q2);
	mem_deref(st->det.window);
	mem_deref(st->det.ring);
	/* Note: mem_deref(st) is called automatically by the mem system */
}

static size_t pair_index_from_two(size_t i, size_t j, size_t n)
{
	/* i < j, n >= 2 */
	const size_t base = i * (n - 1) - (i * (i - 1)) / 2;
	return base + (j - i - 1);
}

/**
 * Initialize Goertzel algorithm for a specific frequency
 */
static double goertzel_init_coeff(uint32_t target_freq, uint32_t srate,
				  size_t block_size)
{
	(void)block_size; /* Reserved for future use */
	double normalized_freq = (double)target_freq / (double)srate;
	return 2.0 * cos(2.0 * PI * normalized_freq);
}

/**
 * Process a sample through Goertzel filter
 */
static void goertzel_process(double *q1, double *q2, double coeff,
			     double sample)
{
	double q0 = coeff * (*q1) - (*q2) + sample;
	*q2 = *q1;
	*q1 = q0;
}

static double unix_time_now(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

static int encode_update(struct aufilt_enc_st **stp, void **ctx,
			 const struct aufilt *af, struct aufilt_prm *prm,
			 const struct audio *au)
{
	struct tonedetect_st *st;
	(void)af;
	(void)ctx;
	(void)au;

	if (!stp || !prm)
		return EINVAL;

	if (*stp)
		return 0;

	st = mem_zalloc(sizeof(*st), enc_destructor);
	if (!st)
		return ENOMEM;

	st->gen.active = false;
	st->gen.frequency = 0;
	st->gen.frequency2 = 0;
	st->gen.duration_ms = config.tone_duration_ms;
	st->gen.last_tone_end_time = 0;
	st->gen.first_packet_timestamp = 0.0;
	st->gen.phase = 0.0;
	st->gen.phase2 = 0.0;
	st->gen.srate = prm->srate;
	st->gen.sample_index = 0;
	st->gen.total_samples = 0;
	st->gen.ramp_samples = 0;
	/* Randomize starting tone index to avoid caller/callee sending same tone simultaneously */
	if (config.num_send_pairs > 0)
		st->gen.current_tone_index = rand_u32() % config.num_send_pairs;
	else
		st->gen.current_tone_index = 0;
	st->gen.tone_id = 0;

	*stp = (struct aufilt_enc_st *)st;

	debug("tonedetect: encoder initialized: num_send_frequencies=%zu duration_ms=%u srate=%u ch=%u starting_tone_index=%zu\n",
	      config.num_send_frequencies, config.tone_duration_ms, prm->srate, prm->ch, st->gen.current_tone_index);

	return 0;
}

static int decode_update(struct aufilt_dec_st **stp, void **ctx,
			 const struct aufilt *af, struct aufilt_prm *prm,
			 const struct audio *au)
{
	struct tonedetect_st *st;
	size_t i;
	(void)af;
	(void)ctx;
	(void)au;

	if (!stp || !prm)
		return EINVAL;

	if (*stp)
		return 0;

	st = mem_zalloc(sizeof(*st), dec_destructor);
	if (!st)
		return ENOMEM;

	st->det.srate = prm->srate;
	/* Calculate number of samples for detection window */
	st->det.detection_window_samples = (prm->srate * DETECTION_WINDOW_MS) / 1000;
	st->det.hop_samples = (prm->srate * DETECTION_HOP_MS) / 1000;
	if (st->det.hop_samples == 0)
		st->det.hop_samples = 1;
	if (st->det.hop_samples >= st->det.detection_window_samples)
		st->det.hop_samples = st->det.detection_window_samples;
	st->det.ring_pos = 0;
	st->det.ring_count = 0;
	st->det.hop_count = 0;
	st->det.candidate_pair_index = 0;
	st->det.candidate_count = 0;
	st->det.first_packet_timestamp = 0.0;
	st->det.last_emit_time = 0;
	st->det.last_emit_index = 0;
	st->det.last_emit_valid = false;
	st->det.num_frequencies = config.num_detect_frequencies;

	if (st->det.num_frequencies > 0) {
		size_t alloc_size = st->det.num_frequencies * sizeof(uint32_t);
		st->det.frequencies = mem_zalloc(alloc_size, NULL);
		st->det.goertzel_coeffs = mem_zalloc(
			st->det.num_frequencies * sizeof(double), NULL);
		st->det.goertzel_q1 = mem_zalloc(
			st->det.num_frequencies * sizeof(double), NULL);
		st->det.goertzel_q2 = mem_zalloc(
			st->det.num_frequencies * sizeof(double), NULL);
		st->det.window = mem_zalloc(
			st->det.detection_window_samples * sizeof(double), NULL);
		st->det.ring = mem_zalloc(
			st->det.detection_window_samples * sizeof(int16_t), NULL);

		if (!st->det.frequencies || !st->det.goertzel_coeffs ||
		    !st->det.goertzel_q1 || !st->det.goertzel_q2 ||
		    !st->det.window || !st->det.ring) {
			mem_deref(st);
			return ENOMEM;
		}

		/* Precompute Hamming window */
		for (i = 0; i < st->det.detection_window_samples; i++) {
			st->det.window[i] = 0.54 - 0.46 * cos(
				(2.0 * PI * (double)i) /
				((double)st->det.detection_window_samples - 1.0));
		}

		/* Copy frequencies and initialize Goertzel coefficients */
		for (i = 0; i < st->det.num_frequencies; i++) {
			st->det.frequencies[i] = config.detect_frequencies[i];
			st->det.goertzel_coeffs[i] = goertzel_init_coeff(
				config.detect_frequencies[i], prm->srate,
				st->det.detection_window_samples);
			st->det.goertzel_q1[i] = 0.0;
			st->det.goertzel_q2[i] = 0.0;
		}
	}

	*stp = (struct aufilt_dec_st *)st;

	debug("tonedetect: decoder initialized: num_detect_frequencies=%zu srate=%u ch=%u window_samples=%zu hop_samples=%zu\n",
	      st->det.num_frequencies, prm->srate, prm->ch,
	      st->det.detection_window_samples, st->det.hop_samples);

	return 0;
}


/**
 * Start generating a tone
 */
static void start_tone_generation(struct tonedetect_st *st,
				  uint32_t freq1, uint32_t freq2,
				  size_t tone_id, uint32_t srate,
				  double tone_start_host_ts)
{
	if (!st)
		return;

	st->gen.active = true;
	st->gen.frequency = freq1;
	st->gen.frequency2 = freq2;
	st->gen.tone_id = tone_id;
	st->gen.start_time = tmr_jiffies();
	st->gen.phase = 0.0;
	st->gen.phase2 = 0.0;
	st->gen.srate = srate;
	st->gen.sample_index = 0;
	st->gen.total_samples = ((size_t)st->gen.duration_ms * (size_t)srate) / 1000;
	st->gen.ramp_samples = ((size_t)TONE_RAMP_MS * (size_t)srate) / 1000;
	if (st->gen.ramp_samples * 2 > st->gen.total_samples)
		st->gen.ramp_samples = st->gen.total_samples / 2;

	/* Host timestamp when we start generating this tone. */
	st->gen.first_packet_timestamp = tone_start_host_ts;

	info("tonedetect: tone start: frequency=%u frequency2=%u duration=%u tone_id=%zu ref=tone_start timestamp=%.6f\n",
	     freq1, freq2, st->gen.duration_ms, tone_id,
	     st->gen.first_packet_timestamp);

	/* Emit event using a deterministic reference point: first generated sample. */
	bevent_app_emit(UA_EVENT_AUDIO_LATENCY_OUTGOING, NULL,
			"tone_id=%zu timestamp=%.6f",
			tone_id, st->gen.first_packet_timestamp);
}

static int encode(struct aufilt_enc_st *aufilt_enc_st, struct auframe *af)
{
	struct tonedetect_st *st = (struct tonedetect_st *)aufilt_enc_st;
	size_t i;
	int16_t *sampv;
	uint64_t now;
	bool rtp_warmup_done;

	if (!st || !af)
		return EINVAL;

	sampv = (int16_t *)af->sampv;
	now = tmr_jiffies();
	rtp_warmup_done = tonedetect_call_state.rtp_established_time != 0 &&
			  (now - tonedetect_call_state.rtp_established_time) >=
				  RTP_WARMUP_SUPPRESS_MS;

	/* Stop any active tone if generation is disabled */
	if (!config.enable_tone_generation && st->gen.active) {
		st->gen.active = false;
		st->gen.last_tone_end_time = now;
	}

	/* Check if we should start a new tone (only if generation is enabled and call is ready) */
	if (config.enable_tone_generation && tonedetect_call_state.call_established && tonedetect_call_state.rtp_established &&
	    rtp_warmup_done && !st->gen.active && config.num_send_pairs > 0) {
		uint64_t time_since_last_tone = now - st->gen.last_tone_end_time;
		uint64_t spacing_ms = 5000;  /* 5 seconds between tones */

		/* Only start a new tone if 5 seconds have passed since last tone ended */
		/* (or if no tone has been sent yet, i.e., last_tone_end_time is 0) */
		if (st->gen.last_tone_end_time == 0 || time_since_last_tone >= spacing_ms) {
			/* Round-robin through frequency pairs */
			size_t pair_index = st->gen.current_tone_index;
			size_t tone_id = pair_index + 1; /* 1..Npairs */
			const uint8_t ia = config.send_pair_a[pair_index];
			const uint8_t ib = config.send_pair_b[pair_index];
			const uint32_t f1 = config.send_frequencies[ia];
			const uint32_t f2 = config.send_frequencies[ib];
			const double tone_start_unix_ts = unix_time_now();

			start_tone_generation(st, f1, f2, tone_id, af->srate,
					      tone_start_unix_ts);

			st->gen.current_tone_index =
				(st->gen.current_tone_index + 1) % config.num_send_pairs;
		}
	}

	/* Generate tone if active and generation is enabled */
	if (config.enable_tone_generation && st->gen.active) {
		uint64_t elapsed_ms = (now - st->gen.start_time);

		if (elapsed_ms >= st->gen.duration_ms) {
			st->gen.active = false;
			st->gen.last_tone_end_time = now;  /* Record when tone ended */
			/* Event already emitted when tone started, no need to emit again */
		}
		else {
			/* Calculate phase increment */
			double phase_inc1 = 2.0 * PI * st->gen.frequency /
					    (double)af->srate;
			double phase_inc2 = 2.0 * PI * st->gen.frequency2 /
					    (double)af->srate;

			/* Mix tone into audio */
			for (i = 0; i < af->sampc; i++) {
				/* Envelope to reduce spectral splatter */
				double env = 1.0;
				if (st->gen.ramp_samples) {
					if (st->gen.sample_index < st->gen.ramp_samples) {
						env = (double)st->gen.sample_index /
						      (double)st->gen.ramp_samples;
					}
					else if (st->gen.total_samples &&
						 st->gen.sample_index >
							 st->gen.total_samples - st->gen.ramp_samples) {
						size_t tail = st->gen.total_samples - st->gen.sample_index;
						env = (double)tail / (double)st->gen.ramp_samples;
					}
				}

				/* Dual-tone (DTMF-style): sum of two sines, scaled to keep level */
				const double s1 = sin(st->gen.phase);
				const double s2 = sin(st->gen.phase2);
				st->gen.phase += phase_inc1;
				st->gen.phase2 += phase_inc2;
				if (st->gen.phase >= 2.0 * PI)
					st->gen.phase -= 2.0 * PI;
				if (st->gen.phase2 >= 2.0 * PI)
					st->gen.phase2 -= 2.0 * PI;

				const double dual = (s1 + s2) * 0.5;
				int16_t tone_sample = (int16_t)(dual * TONE_AMPLITUDE * 32767.0 * env);
				/* Mix with existing audio (simple addition) */
				int32_t mixed = (int32_t)sampv[i] + tone_sample;
				/* Clamp to int16_t range */
				if (mixed > 32767)
					mixed = 32767;
				else if (mixed < -32768)
					mixed = -32768;
				sampv[i] = (int16_t)mixed;

				st->gen.sample_index++;
			}
		}
	}

	return 0;
}

static int decode(struct aufilt_dec_st *aufilt_dec_st, struct auframe *af)
{
	struct tonedetect_st *st = (struct tonedetect_st *)aufilt_dec_st;
	size_t i, j;
	int16_t *sampv;
	const uint64_t now = tmr_jiffies();

	if (!st || !af || st->det.num_frequencies == 0 ||
	    st->det.detection_window_samples == 0) {
		return 0;
	}

	/* Only detect tones if call is established and RTP is established */
	if (!tonedetect_call_state.call_established || !tonedetect_call_state.rtp_established) {
		return 0;
	}
	if (tonedetect_call_state.rtp_established_time == 0 ||
	    (now - tonedetect_call_state.rtp_established_time) <
		    RTP_WARMUP_SUPPRESS_MS) {
		return 0;
	}

	sampv = (int16_t *)af->sampv;

	/* Feed samples into a ring buffer and evaluate overlapping windows.
	 * This greatly reduces "missed tones" when a short tone straddles a
	 * window boundary, without loosening false-positive thresholds.
	 */
	for (i = 0; i < af->sampc; i++) {
		/* Ring buffer store */
		if (st->det.ring) {
			st->det.ring[st->det.ring_pos] = sampv[i];
			st->det.ring_pos =
				(st->det.ring_pos + 1) % st->det.detection_window_samples;
			if (st->det.ring_count < st->det.detection_window_samples)
				st->det.ring_count++;
			st->det.hop_count++;
		}

		/* Not enough samples yet */
		if (!st->det.ring ||
		    st->det.ring_count < st->det.detection_window_samples) {
			continue;
		}

		/* Evaluate on hop boundary */
		if (st->det.hop_count < st->det.hop_samples)
			continue;
		st->det.hop_count = 0;

		{
			double best_ratio = 0.0;
			double second_ratio = 0.0;
			double third_ratio = 0.0;
			double sum_ratio = 0.0;
			double best_power = 0.0;
			double second_power = 0.0;
			size_t best_index = (size_t)-1;
			size_t second_index = (size_t)-1;
			const uint64_t now = tmr_jiffies();
			double block_energy = 0.0;

			/* Reset Goertzel state for this evaluation */
			for (j = 0; j < st->det.num_frequencies; j++) {
				st->det.goertzel_q1[j] = 0.0;
				st->det.goertzel_q2[j] = 0.0;
			}

			/* Compute Goertzel over the current window (oldest sample at ring_pos) */
			for (size_t k = 0; k < st->det.detection_window_samples; k++) {
				const size_t idx = (st->det.ring_pos + k) %
						   st->det.detection_window_samples;
				const double w = st->det.window ? st->det.window[k] : 1.0;
				const double x = (double)st->det.ring[idx] * w;

				for (j = 0; j < st->det.num_frequencies; j++) {
					goertzel_process(&st->det.goertzel_q1[j],
							 &st->det.goertzel_q2[j],
							 st->det.goertzel_coeffs[j],
							 x);
				}

				block_energy += x * x;
			}

			if (block_energy >= DETECT_MIN_BLOCK_ENERGY) {
				for (j = 0; j < st->det.num_frequencies; j++) {
					const double q1 = st->det.goertzel_q1[j];
					const double q2 = st->det.goertzel_q2[j];
					const double c = st->det.goertzel_coeffs[j];
					const double power = q1 * q1 + q2 * q2 - q1 * q2 * c;
					const double ratio = power / block_energy;
					sum_ratio += ratio;

					if (ratio > best_ratio) {
						third_ratio = second_ratio;
						second_ratio = best_ratio;
						best_ratio = ratio;
						second_power = best_power;
						best_power = power;
						second_index = best_index;
						best_index = j;
					}
					else if (ratio > second_ratio) {
						third_ratio = second_ratio;
						second_ratio = ratio;
						second_power = power;
						second_index = j;
					}
					else if (ratio > third_ratio) {
						third_ratio = ratio;
					}
				}
			}

			if (best_index == (size_t)-1 || second_index == (size_t)-1) {
				st->det.candidate_count = 0;
				continue;
			}

			/* Dual-tone requirement: top2 must both be present, and separated from others */
			const double dual_balance =
				(best_ratio > 0.0) ? (second_ratio / best_ratio) : 0.0;
			const double top2_share =
				(sum_ratio > 0.0) ? ((best_ratio + second_ratio) / sum_ratio) : 0.0;
			const bool passes =
				(best_ratio >= DETECT_RATIO_THRESHOLD) &&
				(second_ratio >= DETECT_RATIO_THRESHOLD) &&
				(dual_balance >= DETECT_DUAL_BALANCE_MIN) &&
				(top2_share >= DETECT_TOP2_SHARE_MIN) &&
				(third_ratio <= 0.0 ||
				 (best_ratio >= (third_ratio * DETECT_PEAK_SEPARATION) &&
				  second_ratio >= (third_ratio * DETECT_PEAK_SEPARATION)));

			if (!passes) {
				st->det.candidate_count = 0;
				continue;
			}

			/* Early validation: ensure top 2 frequencies are one low + one high */
			/* This prevents false positives from detecting two low or two high frequencies */
			size_t a = best_index;
			size_t b = second_index;
			/* Check if indices correspond to low or high frequencies */
			/* Low frequencies are first num_detect_low in the array */
			size_t num_low = (config.num_detect_low > 0) ? config.num_detect_low : 
				((st->det.num_frequencies >= 6) ? 3 : st->det.num_frequencies / 2);
			bool a_is_low = (a < num_low);
			bool b_is_low = (b < num_low);
			
			/* Reject if both are low or both are high (must be one of each) */
			if (a_is_low == b_is_low) {
				/* Both from same set - reject this detection early */
				st->det.candidate_count = 0;
				continue;
			}

			/* Ensure a is low and b is high for consistent processing */
			if (!a_is_low) {
				/* Swap so a is low, b is high */
				size_t tmp = a;
				a = b;
				b = tmp;
			}

			/* Calculate pair_index for debouncing (using local indices) */
			const size_t pair_index = pair_index_from_two(a, b, st->det.num_frequencies);
			const size_t local_npairs = (st->det.num_frequencies * (st->det.num_frequencies - 1)) / 2;
			if (pair_index >= local_npairs)
				continue;

			/* Debounce on the pair-index */
			if (st->det.candidate_count == 0 ||
			    st->det.candidate_pair_index != pair_index) {
				const double now_ts = unix_time_now();
				const bool same_pair_reacquire =
					(st->det.candidate_pair_index == pair_index) &&
					(st->det.first_packet_timestamp > 0.0) &&
					((now_ts - st->det.first_packet_timestamp) * 1000.0 <=
					 DETECT_FIRST_SEEN_HOLD_MS);

				st->det.candidate_pair_index = pair_index;
				st->det.candidate_count = 1;
				/* Keep first_seen for short same-pair dropouts to avoid jumpy latency. */
				if (!same_pair_reacquire)
					st->det.first_packet_timestamp = now_ts;
			}
			else if (st->det.candidate_count < 255) {
				st->det.candidate_count++;
			}

			if (st->det.candidate_count < DETECT_CONSECUTIVE_BLOCKS)
				continue;

			/* Suppress repeats for the same pair while it's still present */
			if (st->det.last_emit_valid &&
			    st->det.last_emit_index == pair_index &&
			    (now - st->det.last_emit_time) < DETECT_SUPPRESS_MS) {
				continue;
			}

			/* Keep "magnitude" semantics similar to prior code */
			double mag1 = sqrt(best_power) / (double)st->det.detection_window_samples;
			double mag2 = sqrt(second_power) / (double)st->det.detection_window_samples;
			double magnitude = mag1 < mag2 ? mag1 : mag2;

			/* Absolute guard: ignore very weak detections */
			if (mag1 < DETECT_MIN_MAGNITUDE || mag2 < DETECT_MIN_MAGNITUDE) {
				continue;
			}
			const double detect_timestamp = unix_time_now();
			const double first_seen_timestamp =
				(st->det.first_packet_timestamp > 0.0)
					? st->det.first_packet_timestamp
					: detect_timestamp;
			const double confirm_delay_ms =
				(detect_timestamp - first_seen_timestamp) * 1000.0;

			/* Guard against stale first_seen causing high-latency outliers. */
			if (confirm_delay_ms < 0.0 ||
			    confirm_delay_ms > DETECT_MAX_CONFIRM_DELAY_MS) {
				st->det.first_packet_timestamp = detect_timestamp;
				st->det.candidate_count = 1;
				continue;
			}

			/* Calculate tone_id based on detected frequencies */
			/* First, find the indices of detected frequencies in config arrays */
			uint32_t detected_f1 = st->det.frequencies[a];
			uint32_t detected_f2 = st->det.frequencies[b];
			size_t config_idx1 = (size_t)-1;
			size_t config_idx2 = (size_t)-1;

			/* Find indices in config.detect_frequencies (must match config.send_frequencies) */
			for (size_t k = 0; k < config.num_detect_frequencies; k++) {
				if (config.detect_frequencies[k] == detected_f1 && config_idx1 == (size_t)-1)
					config_idx1 = k;
				if (config.detect_frequencies[k] == detected_f2 && config_idx2 == (size_t)-1)
					config_idx2 = k;
			}

			/* Validate that we found both frequencies in config */
			if (config_idx1 == (size_t)-1 || config_idx2 == (size_t)-1 ||
			    config.num_detect_frequencies != st->det.num_frequencies) {
				/* Frequencies don't match config - report as unidentified */
				info("tonedetect: tone detect (unidentified): frequency=%u frequency2=%u magnitude=%.3f\n",
				     detected_f1, detected_f2, magnitude);
				bevent_app_emit(UA_EVENT_AUDIO_LATENCY_INCOMING, NULL,
						"magnitude=%.3f tone_id=0 timestamp=%.6f",
						magnitude, first_seen_timestamp);
				continue;
			}

			/* Determine which is low and which is high */
			size_t low_idx, high_idx;
			if (config_idx1 < config.num_detect_low && config_idx2 >= config.num_detect_low) {
				/* idx1 is low, idx2 is high */
				low_idx = config_idx1;
				high_idx = config_idx2 - config.num_detect_low;
			}
			else if (config_idx2 < config.num_detect_low && config_idx1 >= config.num_detect_low) {
				/* idx2 is low, idx1 is high */
				low_idx = config_idx2;
				high_idx = config_idx1 - config.num_detect_low;
			}
			else {
				/* Both are low or both are high - invalid for this scheme */
				info("tonedetect: tone detect (invalid pair): frequency=%u frequency2=%u (both low or both high)\n",
				     detected_f1, detected_f2);
				bevent_app_emit(UA_EVENT_AUDIO_LATENCY_INCOMING, NULL,
						"magnitude=%.3f tone_id=0 timestamp=%.6f",
						magnitude, first_seen_timestamp);
				continue;
			}

			/* Calculate tone_id: (low_index * num_high) + high_index + 1 */
			/* tone_id is 1-based (1..9) */
			size_t tone_id = (low_idx * config.num_detect_high) + high_idx + 1;
			if (low_idx >= config.num_detect_low || high_idx >= config.num_detect_high)
				tone_id = 0;

			info("tonedetect: tone detect: frequency=%u frequency2=%u magnitude=%.3f tone_id=%zu (low_idx=%zu high_idx=%zu) ref=rx_first_seen timestamp=%.6f detected_timestamp=%.6f\n",
			     detected_f1, detected_f2, magnitude, tone_id, low_idx, high_idx,
			     first_seen_timestamp, detect_timestamp);

			bevent_app_emit(UA_EVENT_AUDIO_LATENCY_INCOMING, NULL,
					"magnitude=%.3f tone_id=%zu timestamp=%.6f",
					magnitude, tone_id, first_seen_timestamp);

			st->det.last_emit_time = now;
			st->det.last_emit_index = pair_index;
			st->det.last_emit_valid = true;
		}
	}

	return 0;
}

/* Event handler for call events */
static void event_handler(enum ua_event ev, struct bevent *event, void *arg)
{
	struct call *call = bevent_get_call(event);
	const char *prm = bevent_get_text(event);
	(void)arg;

	switch (ev) {
	case UA_EVENT_CALL_ESTABLISHED:
		/* Only set state if we have a valid call */
		if (call) {
			tonedetect_call_state.call_established = true;
			info("tonedetect: CALL_ESTABLISHED - call ready for tone generation/detection\n");
		}
		break;

	case UA_EVENT_CALL_RTPESTAB:
		/* Only enable if it's an audio stream - call may not be available */
		if (prm && strstr(prm, "audio")) {
			tonedetect_call_state.rtp_established = true;
			tonedetect_call_state.rtp_established_time = tmr_jiffies();
			info("tonedetect: CALL_RTPESTAB (audio) - RTP ready for tone generation/detection\n");
		}
		break;

	case UA_EVENT_CALL_HOLD:
		/* When call is put on hold, pause tone generation/detection */
		tonedetect_call_state.rtp_established = false;
		tonedetect_call_state.rtp_established_time = 0;
		info("tonedetect: CALL_HOLD - pausing tone generation/detection\n");
		break;

	case UA_EVENT_CALL_RESUME:
		/* When call is resumed, re-enable if call is still established */
		if (tonedetect_call_state.call_established) {
			/* RTP will be re-established via CALL_RTPESTAB event */
			info("tonedetect: CALL_RESUME - waiting for RTP re-establishment\n");
		}
		break;

	case UA_EVENT_CALL_CLOSED:
	case UA_EVENT_CALL_ENDED_LOCAL:
	case UA_EVENT_CALL_ENDED_REMOTE:
		/* Reset state when call ends - don't require call object as it may be freed */
		tonedetect_call_state.call_established = false;
		tonedetect_call_state.rtp_established = false;
		tonedetect_call_state.rtp_established_time = 0;
		info("tonedetect: Call ended - resetting state\n");
		break;

	default:
		/* Ignore all other events */
		break;
	}
}

static struct aufilt tonedetect = {
	.name = "tonedetect",
	.encupdh = encode_update,
	.ench = encode,
	.decupdh = decode_update,
	.dech = decode
};

static int module_init(void)
{
	/* Two sets: 3 low frequencies + 3 high frequencies = 9 tone IDs */
	/* Low: 400Hz, 500Hz, 600Hz */
	/* High: 2000Hz, 2500Hz, 3000Hz */
	/* Tone IDs: 1=400+2000, 2=400+2500, 3=400+3000, 4=500+2000, ..., 9=600+3000 */
	uint32_t default_send[] = {400, 500, 600, 2000, 2500, 3000};
	uint32_t default_detect[] = {400, 500, 600, 2000, 2500, 3000};

	config.send_frequencies = mem_zalloc(sizeof(default_send), NULL);
	config.detect_frequencies = mem_zalloc(sizeof(default_detect), NULL);

	if (config.send_frequencies && config.detect_frequencies) {
		memcpy(config.send_frequencies, default_send,
		       sizeof(default_send));
		config.num_send_frequencies =
			sizeof(default_send) / sizeof(default_send[0]);
		config.num_low_frequencies = 3;
		config.num_high_frequencies = 3;
		memcpy(config.detect_frequencies, default_detect,
		       sizeof(default_detect));
		config.num_detect_frequencies =
			sizeof(default_detect) / sizeof(default_detect[0]);
		config.num_detect_low = 3;
		config.num_detect_high = 3;
	}

	/* Build pair list: all combinations of low x high (3x3 = 9 pairs) */
	config.num_send_pairs = 0;
	mem_deref(config.send_pair_a);
	mem_deref(config.send_pair_b);
	config.send_pair_a = NULL;
	config.send_pair_b = NULL;
	if (config.num_low_frequencies > 0 && config.num_high_frequencies > 0) {
		size_t npairs = config.num_low_frequencies * config.num_high_frequencies;
		config.send_pair_a = mem_zalloc(npairs * sizeof(uint8_t), NULL);
		config.send_pair_b = mem_zalloc(npairs * sizeof(uint8_t), NULL);
		if (config.send_pair_a && config.send_pair_b) {
			size_t k = 0;
			size_t i, j;
			/* Low frequencies are indices 0..(num_low-1) */
			/* High frequencies are indices num_low..(num_low+num_high-1) */
			for (i = 0; i < config.num_low_frequencies; i++) {
				for (j = 0; j < config.num_high_frequencies; j++) {
					config.send_pair_a[k] = (uint8_t)i;  /* Low frequency index */
					config.send_pair_b[k] = (uint8_t)(config.num_low_frequencies + j);  /* High frequency index */
					k++;
				}
			}
			config.num_send_pairs = npairs;
		}
	}

	/* Read configuration parameter for tone generation */
	conf_get_bool(conf_cur(), "tone_generation",
		      &config.enable_tone_generation);

	aufilt_register(baresip_aufiltl(), &tonedetect);
	
	/* Register event handler for call events */
	bevent_register(event_handler, NULL);
	
	info("tonedetect: module loaded - %zu low + %zu high frequencies = %zu tone IDs, generation=%s\n",
	     config.num_low_frequencies, config.num_high_frequencies, config.num_send_pairs,
	     config.enable_tone_generation ? "enabled" : "disabled");

	return 0;
}

static int module_close(void)
{
	/* Unregister event handler */
	bevent_unregister(event_handler);
	
	/* Reset call state */
	tonedetect_call_state.call_established = false;
	tonedetect_call_state.rtp_established = false;
	tonedetect_call_state.rtp_established_time = 0;
	
	mem_deref(config.send_frequencies);
	mem_deref(config.send_pair_a);
	mem_deref(config.send_pair_b);
	mem_deref(config.detect_frequencies);
	aufilt_unregister(&tonedetect);

	return 0;
}

EXPORT_SYM const struct mod_export DECL_EXPORTS(tonedetect) = {
	"tonedetect",
	"filter",
	module_init,
	module_close
};

