/**
 * @file dump.c  OpenAI Realtime API - Audio debug dumper
 * 
 * Dumps converted audio (24kHz mono PCM16) to WAV file for debugging
 */

#include "openai_rt.h"
#include <sndfile.h>
#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static SNDFILE *dump_file = NULL;
static SF_INFO dump_sfinfo;
static bool dump_enabled = true;  /* Can be disabled via config later */
static size_t total_samples_written = 0;

static SNDFILE *dump_file_orig = NULL;
static SF_INFO dump_sfinfo_orig;

static SNDFILE *dump_file_resp = NULL;
static SF_INFO dump_sfinfo_resp;

#ifndef WAVE_FORMAT_MULAW
#define WAVE_FORMAT_MULAW 0x0007
#endif
#ifndef WAVE_FORMAT_ALAW
#define WAVE_FORMAT_ALAW  0x0006
#endif

typedef struct {
    FILE    *fp;
    uint32_t samplerate;
    uint16_t channels;
    uint16_t wFormatTag;       /* NEW: mulaw or alaw */
    uint32_t bytes_written;
    long riff_size_pos;
    long data_size_pos;
    long fact_samps_pos;
} g711_wav_t;

static g711_wav_t g711u_wav = {0};
static g711_wav_t g711a_wav = {0};   /* NEW */
static size_t total_g711u_samples_written = 0;
static size_t total_g711a_samples_written = 0; /* NEW */
static size_t total_resp_samples_written = 0;




static void w_u32le(FILE *f, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v), (uint8_t)(v>>8), (uint8_t)(v>>16), (uint8_t)(v>>24)};
    fwrite(b,1,4,f);
}
static void w_u16le(FILE *f, uint16_t v) {
    uint8_t b[2] = { (uint8_t)(v), (uint8_t)(v>>8) };
    fwrite(b,1,2,f);
}

static int g711_wav_open(g711_wav_t *w, const char *path,
    uint32_t sr, uint16_t ch, uint16_t wFormatTag)
{
    memset(w, 0, sizeof(*w));
    w->fp = fopen(path, "wb");
    if (!w->fp) return -1;
    w->samplerate = sr;
    w->channels   = ch;
    w->wFormatTag = wFormatTag;

    fwrite("RIFF",1,4,w->fp);
    w->riff_size_pos = ftell(w->fp);
    w_u32le(w->fp, 0);
    fwrite("WAVE",1,4,w->fp);

    fwrite("fmt ",1,4,w->fp);
    w_u32le(w->fp, 16);
    w_u16le(w->fp, w->wFormatTag);          /* μ or A law */
    w_u16le(w->fp, ch);
    w_u32le(w->fp, sr);
    w_u32le(w->fp, sr * ch * 1);
    w_u16le(w->fp, ch * 1);
    w_u16le(w->fp, 8);

    fwrite("fact",1,4,w->fp);
    w_u32le(w->fp, 4);
    w->fact_samps_pos = ftell(w->fp);
    w_u32le(w->fp, 0);

    fwrite("data",1,4,w->fp);
    w->data_size_pos = ftell(w->fp);
    w_u32le(w->fp, 0);

    return 0;
}


static int g711_wav_write(g711_wav_t *w, const uint8_t *p, size_t n) {
    if (!w || !w->fp || !p || !n) return -1;
    size_t wr = fwrite(p, 1, n, w->fp);
    w->bytes_written += (uint32_t)wr;
    return (wr == n) ? 0 : -1;
}

static int g711_wav_close(g711_wav_t *w)
{
    if (!w || !w->fp) return -1;
    long cur = ftell(w->fp);
    fseek(w->fp, w->data_size_pos, SEEK_SET);
    w_u32le(w->fp, w->bytes_written);
    fseek(w->fp, w->fact_samps_pos, SEEK_SET);
    uint32_t samples_per_channel = (w->channels == 0) ? 0 : (w->bytes_written / w->channels);
    w_u32le(w->fp, samples_per_channel);
    fseek(w->fp, 0, SEEK_END);
    uint32_t file_size = (uint32_t)ftell(w->fp);
    fseek(w->fp, w->riff_size_pos, SEEK_SET);
    w_u32le(w->fp, file_size - 8);
    fseek(w->fp, cur, SEEK_SET);
    fclose(w->fp);
    w->fp = NULL;
    return 0;
}

/* Initialize original audio dumper - call this from dump_init() */
static int dump_init_orig(void)
{
    if (!dump_enabled) {
        return 0;
    }

    /* Original file will have dynamic sample rate/channels */
    dump_file_orig = NULL;

    return 0;
}

/* Write original audio samples to dump file */
void dump_audio_orig(const int16_t *samples, size_t sample_count,
                     uint32_t srate, uint8_t channels)
{
    sf_count_t written;

    if (!dump_enabled || !samples || sample_count == 0) {
        info("openai_rt: not dumping original audio, samples=%p, sample_count=%u\n",
             samples, (unsigned)sample_count);
        return;
    }

    /* Open file if not already open or if format changed */
    if (!dump_file_orig ||
        dump_sfinfo_orig.samplerate != (int)srate ||
        dump_sfinfo_orig.channels != channels) {

        /* Close existing file if open */
        if (dump_file_orig) {
            sf_close(dump_file_orig);
            info("openai_rt: Closed original dump file due to format change\n");
        }

        /* Set up SF_INFO for original format */
        memset(&dump_sfinfo_orig, 0, sizeof(dump_sfinfo_orig));
        dump_sfinfo_orig.samplerate = srate;
        dump_sfinfo_orig.channels = channels;
        dump_sfinfo_orig.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

        /* Open file for writing */
        dump_file_orig = sf_open("/tmp/sip_to_oai_orig.wav", SFM_WRITE, &dump_sfinfo_orig);
        if (!dump_file_orig) {
            warning("openai_rt: Could not open original dump file: %s\n",
                    sf_strerror(NULL));
            return;
        }

        info("openai_rt: Opened original dump file: %u Hz, %u channels\n",
             srate, channels);
    } else {
        // debug info
        info("openai_rt: Writing to existing original dump file: %u Hz, %u channels\n",
             srate, channels);
    }

    /* Write samples to file */
    written = sf_write_short(dump_file_orig, samples, sample_count);
    if (written != (sf_count_t)sample_count) {
        warning("openai_rt: Original dump write error: wrote %lld of %zu samples\n",
                (long long)written, sample_count);
    }

    /* Sync periodically */
    static size_t total_orig = 0;
    total_orig += written;
    if (total_orig % srate < sample_count) {  /* Every second */
        sf_write_sync(dump_file_orig);
    }
}

void dump_audio_response(const int16_t *samples, size_t sample_count)
{
    sf_count_t written;
    if (!dump_enabled || !dump_file_resp || !samples || sample_count == 0) {
        return;
    }

    written = sf_write_short(dump_file_resp, samples, sample_count);
    if (written != (sf_count_t)sample_count) {
        warning("openai_rt: Response dump write error: wrote %lld of %zu samples\n",
                (long long)written, sample_count);
    } else {
        total_resp_samples_written += written;
        /* Sync once per second at 24 kHz */
        if (total_resp_samples_written % 24000 < sample_count) {
            sf_write_sync(dump_file_resp);
        }
    }
}

/* Close original audio dumper - call this from dump_close() */
static void dump_close_orig(void)
{
    if (dump_file_orig) {
        sf_close(dump_file_orig);
        dump_file_orig = NULL;
        info("openai_rt: Closed original dump file\n");
    }
}

/* Initialize audio dumper */
int dump_init(void)
{
    DEBUG_ENTER();
    
    if (!dump_enabled) {
        return 0;
    }

    /* Set up SF_INFO for 24kHz mono PCM16 */
    memset(&dump_sfinfo, 0, sizeof(dump_sfinfo));
    dump_sfinfo.samplerate = 24000;
    dump_sfinfo.channels = 1;
    dump_sfinfo.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

    /* Open file for writing */
    dump_file = sf_open("/tmp/sip_to_oai.wav", SFM_WRITE, &dump_sfinfo);
    if (!dump_file) {
        warning("openai_rt: Could not open dump file: %s\n", sf_strerror(NULL));
        dump_enabled = false;
        return EIO;
    }

    info("openai_rt: Dumping converted audio to /tmp/sip_to_oai.wav (24kHz mono PCM16)\n");
    total_samples_written = 0;

    /* μ-law */
    if (g711_wav_open(&g711u_wav, "/tmp/sip_to_oai_g711u.wav", 8000, 1, WAVE_FORMAT_MULAW) != 0) {
        warning("openai_rt: Could not open G711u dump file for writing\n");
    } else {
        info("openai_rt: Dumping G711u to /tmp/sip_to_oai_g711u.wav\n");
        total_g711u_samples_written = 0;
    }

    /* A-law (NEW) */
    if (g711_wav_open(&g711a_wav, "/tmp/sip_to_oai_g711a.wav", 8000, 1, WAVE_FORMAT_ALAW) != 0) {
        warning("openai_rt: Could not open G711a dump file for writing\n");
    } else {
        info("openai_rt: Dumping G711a to /tmp/sip_to_oai_g711a.wav\n");
        total_g711a_samples_written = 0;
    }

    memset(&dump_sfinfo_resp, 0, sizeof(dump_sfinfo_resp));
    dump_sfinfo_resp.samplerate = 24000; /* matches the model’s output */
    dump_sfinfo_resp.channels = 1;
    dump_sfinfo_resp.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    dump_file_resp = sf_open("/tmp/oai_response.wav", SFM_WRITE, &dump_sfinfo_resp);
    if (!dump_file_resp) {
        warning("openai_rt: Could not open OAI response dump file: %s\n",
                sf_strerror(NULL));
    } else {
        info("openai_rt: Dumping OpenAI response audio to /tmp/oai_response.wav\n");
        total_resp_samples_written = 0;
    }    

    return dump_init_orig();
}

/* Write audio samples to dump file */
void dump_audio(const int16_t *samples, size_t sample_count)
{
    sf_count_t written;
    
    if (!dump_enabled || !dump_file || !samples || sample_count == 0) {
        return;
    }

    /* Write samples to file */
    written = sf_write_short(dump_file, samples, sample_count);
    if (written != (sf_count_t)sample_count) {
        warning("openai_rt: Dump write error: wrote %lld of %zu samples\n",
                (long long)written, sample_count);
    } else {
        total_samples_written += written;
        
        /* Log progress every second (24000 samples) */
        if (total_samples_written % 24000 < sample_count) {
            sf_write_sync(dump_file);
            info("openai_rt: Dump progress: %zu samples (%.1f seconds) written\n",
                 total_samples_written, (float)total_samples_written / 24000.0f);
        }
    }
}

void dump_audio_g711u(const uint8_t *g711u_samples, size_t sample_count)
{
    if (!dump_enabled || !g711u_wav.fp || !g711u_samples || sample_count == 0) {
        warning("openai_rt: dump_audio_g711u early return: enabled=%d, file=%p, samples=%p, count=%zu\n",
                dump_enabled, (void*)g711u_wav.fp, g711u_samples, sample_count);
        return;
    }

    /* Optional: peek at first few bytes for debugging */
    if (sample_count >= 5) {
        info("openai_rt: First 5 μ-law bytes: %u, %u, %u, %u, %u\n",
             g711u_samples[0], g711u_samples[1], g711u_samples[2],
             g711u_samples[3], g711u_samples[4]);
    }

    /* Write raw μ-law bytes into the data chunk */
    if (g711_wav_write(&g711u_wav, g711u_samples, sample_count) != 0) {
        warning("openai_rt: G711u write failed\n");
        return;
    }

    total_g711u_samples_written += sample_count;

    /* Log progress about once per second at 8 kHz */
    if (total_g711u_samples_written % 8000 < sample_count) {
        info("openai_rt: G711u dump progress: %zu samples (%.2f seconds)\n",
             total_g711u_samples_written,
             (double)total_g711u_samples_written / 8000.0);
    }
}

void dump_audio_g711a(const uint8_t *g711a, size_t n)
{
    if (!dump_enabled || !g711a_wav.fp || !g711a || n == 0) return;
    if (g711_wav_write(&g711a_wav, g711a, n) != 0) {
        warning("openai_rt: G711a write failed\n");
        return;
    }
    total_g711a_samples_written += n;
}

/* Close audio dumper */
void dump_close(void)
{
    DEBUG_ENTER();
    
    if (dump_file) {
        sf_close(dump_file);
        dump_file = NULL;
        info("openai_rt: Closed dump file. Total samples written: %zu (%.1f seconds)\n",
             total_samples_written, (float)total_samples_written / 24000.0f);
    }
    
    total_samples_written = 0;

    if (g711u_wav.fp) {
        g711_wav_close(&g711u_wav);
        info("openai_rt: Closed G711u: %zu samples (~%.2fs)\n",
             total_g711u_samples_written,
             (double)total_g711u_samples_written / 8000.0);
    }
    if (g711a_wav.fp) {
        g711_wav_close(&g711a_wav);
        info("openai_rt: Closed G711a: %zu samples (~%.2fs)\n",
             total_g711a_samples_written,
             (double)total_g711a_samples_written / 8000.0);
    }
    total_g711u_samples_written = 0;
    total_g711a_samples_written = 0;

    if (dump_file_resp) {
        sf_close(dump_file_resp);
        dump_file_resp = NULL;
        info("openai_rt: Closed OAI response dump file. Total samples written: %zu (%.1f seconds)\n",
             total_resp_samples_written, (float)total_resp_samples_written / 24000.0f);
    }
    total_resp_samples_written = 0;    

    dump_close_orig();
}

/* Enable/disable dumping */
void dump_enable(bool enable)
{
    dump_enabled = enable;
    info("openai_rt: Audio dumping %s\n", enable ? "enabled" : "disabled");
}

/* Create a new dump file with timestamp */
int dump_new_file(void)
{
    struct timeval tv;
    char filename[256];
    
    DEBUG_ENTER();
    
    if (!dump_enabled) {
        return 0;
    }

    /* Close existing file if open */
    if (dump_file) {
        dump_close();
    }

    /* Generate filename with timestamp */
    gettimeofday(&tv, NULL);
    re_snprintf(filename, sizeof(filename), "/tmp/sip_to_oai_%lld.wav",
                (long long)(tv.tv_sec * 1000000LL + tv.tv_usec));

    /* Open new file */
    dump_file = sf_open(filename, SFM_WRITE, &dump_sfinfo);
    if (!dump_file) {
        warning("openai_rt: Could not open new dump file %s: %s\n", 
                filename, sf_strerror(NULL));
        dump_enabled = false;
        return EIO;
    }

    info("openai_rt: Created new dump file: %s\n", filename);
    total_samples_written = 0;
    
    return 0;
}