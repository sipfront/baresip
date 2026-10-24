/**
 * @file compat.h  Standalone-only glue between main.c, compat.c, audio_pulse.c and calls_cli.c
 */
#ifndef SF_VOICEAI_COMPAT_H
#define SF_VOICEAI_COMPAT_H

#include <stdbool.h>
#include <stddef.h>

struct conf;

/* the config loaded by main.c, served to the shared sources through conf_cur() */
void compat_set_conf(struct conf *conf);

/* JSON event stream on stdout (one object per line); any thread may call it */
void events_emit(const char *fmt, ...);

/* PulseAudio in/out (audio_pulse.c) */
int  audio_pulse_start(const char *source, const char *sink, unsigned uplink_srate,
		       const char *app_name);
void audio_pulse_stop(void);

/* the call layer (calls_cli.c): when the model asks to hang up, the binary keeps running
 * until the caller stops it (the event tells the caller); with exit_on_hangup it ends itself */
void calls_cli_set_exit_on_hangup(bool enable);

#endif
