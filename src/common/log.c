/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ntfsmac/log.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LOG_BUF_SIZE 512

static _Atomic int g_level = NTFSMAC_LOG_INFO;
static ntfsmac_log_sink_fn g_sink;
static void *g_sink_ctx;

const char *ntfsmac_log_level_name(ntfsmac_log_level level)
{
	switch (level) {
	case NTFSMAC_LOG_DEBUG: return "DEBUG";
	case NTFSMAC_LOG_INFO:  return "INFO";
	case NTFSMAC_LOG_WARN:  return "WARN";
	case NTFSMAC_LOG_ERROR: return "ERROR";
	default:                return "NONE";
	}
}

void ntfsmac_log_set_level(ntfsmac_log_level level)
{
	atomic_store(&g_level, (int)level);
}

ntfsmac_log_level ntfsmac_log_get_level(void)
{
	return (ntfsmac_log_level)atomic_load(&g_level);
}

int ntfsmac_log_enabled(ntfsmac_log_level level)
{
	return level != NTFSMAC_LOG_NONE && (int)level >= atomic_load(&g_level);
}

void ntfsmac_log_set_sink(ntfsmac_log_sink_fn fn, void *ctx)
{
	g_sink = fn;
	g_sink_ctx = ctx;
}

/* Default sink: "LEVEL: message\n" in a single write(2) to stderr. */
static void stderr_sink(ntfsmac_log_level level, const char *msg, void *ctx)
{
	char line[LOG_BUF_SIZE + 16];
	int n;
	(void)ctx;

	n = snprintf(line, sizeof(line), "%s: %s\n", ntfsmac_log_level_name(level), msg);
	if (n < 0)
		return;
	if ((size_t)n >= sizeof(line))
		n = (int)sizeof(line) - 1;
	/* Best effort; a failed log write must not affect the caller. */
	if (write(STDERR_FILENO, line, (size_t)n) < 0) {
		/* ignored */
	}
}

void ntfsmac_log(ntfsmac_log_level level, const char *fmt, ...)
{
	char buf[LOG_BUF_SIZE];
	va_list ap;
	int n;

	if (!ntfsmac_log_enabled(level))
		return;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if ((size_t)n >= sizeof(buf)) {
		/* Truncated: mark it. */
		memcpy(buf + sizeof(buf) - 4, "...", 4);
	}

	if (g_sink)
		g_sink(level, buf, g_sink_ctx);
	else
		stderr_sink(level, buf, NULL);
}
