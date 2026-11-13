/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ntfsmac/log.h"

#include <stdio.h>
#include <string.h>

static int g_count;
static ntfsmac_log_level g_last_level;
static char g_last_msg[600];

static void capture(ntfsmac_log_level level, const char *msg, void *ctx)
{
	(void)ctx;
	g_count++;
	g_last_level = level;
	snprintf(g_last_msg, sizeof(g_last_msg), "%s", msg);
}

#define CHECK(cond)                                                      \
	do {                                                             \
		if (!(cond)) {                                           \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,    \
				__LINE__, #cond);                        \
			return 1;                                        \
		}                                                        \
	} while (0)

int main(void)
{
	char big[2000];

	ntfsmac_log_set_sink(capture, NULL);

	/* Default threshold is INFO. */
	CHECK(ntfsmac_log_get_level() == NTFSMAC_LOG_INFO);
	NTFSMAC_LOGD("dropped %d", 1);
	CHECK(g_count == 0);
	NTFSMAC_LOGI("kept %d", 2);
	CHECK(g_count == 1);
	CHECK(g_last_level == NTFSMAC_LOG_INFO);
	CHECK(strcmp(g_last_msg, "kept 2") == 0);

	/* Raise the threshold. */
	ntfsmac_log_set_level(NTFSMAC_LOG_WARN);
	NTFSMAC_LOGI("dropped");
	CHECK(g_count == 1);
	NTFSMAC_LOGW("warn %s", "x");
	NTFSMAC_LOGE("err");
	CHECK(g_count == 3);
	CHECK(g_last_level == NTFSMAC_LOG_ERROR);

	/* Lower it. */
	ntfsmac_log_set_level(NTFSMAC_LOG_DEBUG);
	NTFSMAC_LOGD("dbg");
	CHECK(g_count == 4);
	CHECK(ntfsmac_log_enabled(NTFSMAC_LOG_DEBUG));

	/* NONE suppresses everything. */
	ntfsmac_log_set_level(NTFSMAC_LOG_NONE);
	NTFSMAC_LOGE("silent");
	CHECK(g_count == 4);
	CHECK(!ntfsmac_log_enabled(NTFSMAC_LOG_ERROR));

	/* Long messages are truncated and marked, not overflowed. */
	ntfsmac_log_set_level(NTFSMAC_LOG_INFO);
	memset(big, 'a', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';
	NTFSMAC_LOGI("%s", big);
	CHECK(g_count == 5);
	CHECK(strlen(g_last_msg) == 511);
	CHECK(strcmp(g_last_msg + 508, "...") == 0);

	/* Default stderr sink must not crash. */
	ntfsmac_log_set_sink(NULL, NULL);
	NTFSMAC_LOGI("stderr sink ok");

	CHECK(strcmp(ntfsmac_log_level_name(NTFSMAC_LOG_WARN), "WARN") == 0);
	return 0;
}
