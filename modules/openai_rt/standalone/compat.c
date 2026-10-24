/**
 * @file compat.c  The baresip symbols the shared openai_rt sources use, for the standalone
 *
 * baresip.h turns info()/warning()/debug() into _info()/_warning()/_debug() (with libre's
 * type-checked RE_VA_ARGS when HAVE_RE_ARG is set, as in baresip's own build), so those are
 * what has to exist; they format with re_vsnprintf(_s) like baresip's log.c and write one line
 * per call to stderr, keeping stdout for the JSON event stream. conf_cur() hands out the config
 * main.c loaded with libre's conf module (the same "openai_rt_*" keys as the baresip config);
 * conf_path_get() is only used by trace.c as the fallback directory for the trace file.
 */
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <re.h>
#include <baresip.h>
#include "compat.h"

static struct conf *g_conf;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

static void vlog_line(bool safe, const char *level, const char *fmt, va_list ap)
{
	char buf[8192];
	int n;

	n = safe ? re_vsnprintf_s(buf, sizeof(buf), fmt, ap)
		 : re_vsnprintf(buf, sizeof(buf), fmt, ap);
	if (n < 0)
		return;
	pthread_mutex_lock(&log_mutex);
	fprintf(stderr, "[%s] %s%s", level, buf,
		(n > 0 && buf[n - 1] == '\n') ? "" : "\n");
	fflush(stderr);
	pthread_mutex_unlock(&log_mutex);
}

void _loglv(bool safe, enum log_level level, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vlog_line(safe, level >= LEVEL_WARN ? "warn" : level == LEVEL_INFO ? "info" : "debug",
		  fmt, ap);
	va_end(ap);
}

void _debug(bool safe, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vlog_line(safe, "debug", fmt, ap);
	va_end(ap);
}

void _info(bool safe, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vlog_line(safe, "info", fmt, ap);
	va_end(ap);
}

void _warning(bool safe, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vlog_line(safe, "warn", fmt, ap);
	va_end(ap);
}

void compat_set_conf(struct conf *conf)
{
	g_conf = conf;
}

struct conf *conf_cur(void)
{
	return g_conf;
}

int conf_path_get(char *path, size_t sz)
{
	if (!path || !sz)
		return EINVAL;
	if (!getcwd(path, sz))
		return errno;
	return 0;
}
