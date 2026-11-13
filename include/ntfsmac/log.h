/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_LOG_H
#define NTFSMAC_LOG_H

/*
 * Minimal logging helper. printf-style, one line per call, written to stderr
 * with a single write; no heap allocation (fixed on-stack buffer, long
 * messages are truncated and marked with "...").
 *
 * Seam for macOS: all output goes through ntfsmac_log_sink_fn. A future
 * os_log backend installs its own sink with ntfsmac_log_set_sink(); callers
 * do not change. The default sink is stderr on every platform.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ntfsmac_log_level {
	NTFSMAC_LOG_DEBUG = 0,
	NTFSMAC_LOG_INFO,
	NTFSMAC_LOG_WARN,
	NTFSMAC_LOG_ERROR,
	NTFSMAC_LOG_NONE /* threshold only: suppress everything */
} ntfsmac_log_level;

/* Receives a formatted message, without trailing newline, that passed the filter. */
typedef void (*ntfsmac_log_sink_fn)(ntfsmac_log_level level, const char *msg, void *ctx);

#if defined(__GNUC__) || defined(__clang__)
#define NTFSMAC_PRINTF_FORMAT(fmt, args) __attribute__((format(printf, fmt, args)))
#else
#define NTFSMAC_PRINTF_FORMAT(fmt, args)
#endif

/* Messages below the threshold are dropped. Default: NTFSMAC_LOG_INFO. */
void ntfsmac_log_set_level(ntfsmac_log_level level);
ntfsmac_log_level ntfsmac_log_get_level(void);

/* Returns 1 if a message at this level would be emitted. */
int ntfsmac_log_enabled(ntfsmac_log_level level);

/* Install a sink (NULL restores the stderr sink). Call before starting threads. */
void ntfsmac_log_set_sink(ntfsmac_log_sink_fn fn, void *ctx);

/* Short upper-case name: "DEBUG", "INFO", "WARN", "ERROR". */
const char *ntfsmac_log_level_name(ntfsmac_log_level level);

void ntfsmac_log(ntfsmac_log_level level, const char *fmt, ...) NTFSMAC_PRINTF_FORMAT(2, 3);

#define NTFSMAC_LOGD(...) ntfsmac_log(NTFSMAC_LOG_DEBUG, __VA_ARGS__)
#define NTFSMAC_LOGI(...) ntfsmac_log(NTFSMAC_LOG_INFO, __VA_ARGS__)
#define NTFSMAC_LOGW(...) ntfsmac_log(NTFSMAC_LOG_WARN, __VA_ARGS__)
#define NTFSMAC_LOGE(...) ntfsmac_log(NTFSMAC_LOG_ERROR, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* NTFSMAC_LOG_H */
