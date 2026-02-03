/**
 * @file tonedetect.c  Audio filter module for tone generation and detection
 *
 * Copyright (C) 2025
 */

#include <math.h>
#include <stdint.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>

#define PI 3.14159265358979323846
#define TONE_AMPLITUDE 0.3f
#define DETECTION_WINDOW_MS 50   /* 20ms window (for 50ms tone detection) */
#define DETECTION_HOP_MS    10    /* evaluate every 10ms (overlapping windows reduces missed tones) */

/* Detection tuning (receiver) - balanced for reliable detection */
#define DETECT_RATIO_THRESHOLD       0.18  /* stricter threshold to reduce false positives */
#define DETECT_PEAK_SEPARATION       1.35  /* require better peak separation to reduce false positives */
#define DETECT_MIN_BLOCK_ENERGY      8.0e8 /* higher energy requirement */
#define DETECT_CONSECUTIVE_BLOCKS    3     /* require 3 consecutive blocks for more reliable detection */
#define DETECT_SUPPRESS_MS           3000  /* suppress repeat events */
#define DETECT_MIN_MAGNITUDE         100.0  /* higher magnitude threshold to filter weak false positives */

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
	.tone_duration_ms = 30   /* 50ms tone (increased for testing) */
};

static void enc_destructor(void *arg)
{
	struct tonedetect_st *st = arg;
	list_unlink(&st->u.eaf.le);
	mem_deref(st);
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
	mem_deref(st);
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
				  size_t tone_id, uint32_t srate)
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

	info("tonedetect: tone start: frequency=%u frequency2=%u duration=%u tone_id=%zu\n",
	     freq1, freq2, st->gen.duration_ms, tone_id);

	/* Emit event when tone starts */
	bevent_app_emit(UA_EVENT_AUDIO_LATENCY_OUTGOING, NULL,
			"frequency=%u frequency2=%u duration=%u tone_id=%zu",
			freq1, freq2, st->gen.duration_ms, tone_id);
}

static int encode(struct aufilt_enc_st *aufilt_enc_st, struct auframe *af)
{
	struct tonedetect_st *st = (struct tonedetect_st *)aufilt_enc_st;
	size_t i;
	int16_t *sampv;
	uint64_t now;

	if (!st || !af)
		return EINVAL;

	sampv = (int16_t *)af->sampv;
	now = tmr_jiffies();

	/* Check if we should start a new tone */
	if (!st->gen.active && config.num_send_pairs > 0) {
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

			start_tone_generation(st, f1, f2, tone_id, af->srate);

			st->gen.current_tone_index =
				(st->gen.current_tone_index + 1) % config.num_send_pairs;
		}
	}

	/* Generate tone if active */
	if (st->gen.active) {
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

	if (!st || !af || st->det.num_frequencies == 0 ||
	    st->det.detection_window_samples == 0) {
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
			const bool passes =
				(best_ratio >= DETECT_RATIO_THRESHOLD) &&
				(second_ratio >= DETECT_RATIO_THRESHOLD) &&
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
				st->det.candidate_pair_index = pair_index;
				st->det.candidate_count = 1;
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
						"frequency=%u frequency2=%u magnitude=%.3f tone_id=0",
						detected_f1, detected_f2, magnitude);
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
						"frequency=%u frequency2=%u magnitude=%.3f tone_id=0",
						detected_f1, detected_f2, magnitude);
				continue;
			}

			/* Calculate tone_id: (low_index * num_high) + high_index + 1 */
			/* tone_id is 1-based (1..9) */
			size_t tone_id = (low_idx * config.num_detect_high) + high_idx + 1;
			if (low_idx >= config.num_detect_low || high_idx >= config.num_detect_high)
				tone_id = 0;

			info("tonedetect: tone detect: frequency=%u frequency2=%u magnitude=%.3f tone_id=%zu (low_idx=%zu high_idx=%zu)\n",
			     detected_f1, detected_f2, magnitude, tone_id, low_idx, high_idx);

			bevent_app_emit(UA_EVENT_AUDIO_LATENCY_INCOMING, NULL,
					"frequency=%u frequency2=%u magnitude=%.3f tone_id=%zu",
					detected_f1, detected_f2, magnitude, tone_id);

			st->det.last_emit_time = now;
			st->det.last_emit_index = pair_index;
			st->det.last_emit_valid = true;
		}
	}

	return 0;
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

	aufilt_register(baresip_aufiltl(), &tonedetect);
	info("tonedetect: module loaded - %zu low + %zu high frequencies = %zu tone IDs\n",
	     config.num_low_frequencies, config.num_high_frequencies, config.num_send_pairs);

	return 0;
}

static int module_close(void)
{
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

