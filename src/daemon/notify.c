/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * User notifications (T-M4-08; docs/ARCHITECTURE.md sections 3.2, 3.6, 3.7, 4.1 step 9, 4.5 step 3
 * and 7).
 *
 * The text of every notification is built here from a fixed template, the sanitised label
 * (ADR-016) and a fixed reason text of the daemon, never from a string of the disk or of a
 * request (ntfsmac_notify_text(), pure, tested on every host). Delivery goes through a sink:
 * the default shows a banner to the console user with osascript, which the daemon starts in
 * the user's login session with `launchctl asuser`, as that user, and gives the text as an
 * argument vector (never as script source), so no text can act as a script. The call returns at
 * once; a detached thread waits for the helper for at most 10 s and then kills it. When no one
 * is at the console, or off macOS, the text is only logged.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ntfsmac/log.h"

#define NOTIFY_TEXT_MAX 512u
#define NOTIFY_TITLE "ntfs-macos"
#define NOTIFY_HELPER_WAIT_MS 10000u

extern char **environ;

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */

/* Copy `s` into out (cap bytes), replacing every control byte, so that no text breaks a line. */
static void put_safe(char *out, size_t cap, const char *s)
{
	size_t i;

	if (cap == 0)
		return;
	for (i = 0; i + 1 < cap && s != NULL && s[i] != '\0'; i++) {
		unsigned char c = (unsigned char)s[i];

		out[i] = (c < 0x20u || c == 0x7Fu) ? '?' : (char)c;
	}
	out[i] = '\0';
}

size_t ntfsmac_notify_text(ntfsmac_notify_kind kind, const char *label, const char *reason,
			   char *buf, size_t cap)
{
	char name[NTFSMAC_JOURNAL_LABEL_MAX_BYTES * 4u + 8u];
	char why[160];
	const char *vol;
	int n;

	if (buf == NULL || cap == 0)
		return 0;
	buf[0] = '\0';
	put_safe(name, sizeof name, label);
	put_safe(why, sizeof why, reason);
	vol = name[0] != '\0' ? name : "the volume";

	switch (kind) {
	case NTFSMAC_NOTIFY_MOUNTED_RW:
		n = snprintf(buf, cap, "%s is mounted read/write.", vol);
		break;
	case NTFSMAC_NOTIFY_MOUNTED_RO:
		/* Section 4.1 step 9: the mode and, for ro, the reason. */
		if (why[0] != '\0')
			n = snprintf(buf, cap, "%s is mounted read-only: %s.", vol, why);
		else
			n = snprintf(buf, cap, "%s is mounted read-only.", vol);
		break;
	case NTFSMAC_NOTIFY_EJECTED_CLEAN:
		n = snprintf(buf, cap, "%s ejected, all data written.", vol);
		break;
	case NTFSMAC_NOTIFY_EJECTED_NO_FLUSH_CONFIRM:
		/* Section 4.5 step 3, the wording of a `W` record or a failed daemon flush. */
		n = snprintf(buf, cap,
			     "%s ejected, all data written; this drive did not confirm that its "
			     "cache was flushed.",
			     vol);
		break;
	case NTFSMAC_NOTIFY_SESSION_UNCLEAN:
		n = snprintf(buf, cap,
			     "%s did not end its session cleanly. The volume will be read-only next "
			     "time; run chkdsk on Windows, then attach it again.%s%s",
			     vol, why[0] != '\0' ? " " : "", why);
		break;
	case NTFSMAC_NOTIFY_WRITE_ERROR:
		/* Section 7, first `E` record; `reason` is the time the session started. */
		if (why[0] != '\0')
			n = snprintf(buf, cap,
				     "Write error on %s: unmounting. Data written after %s may be lost.",
				     vol, why);
		else
			n = snprintf(buf, cap,
				     "Write error on %s: unmounting. Data written in this session may "
				     "be lost.",
				     vol);
		break;
	case NTFSMAC_NOTIFY_REFUSED:
		if (why[0] != '\0')
			n = snprintf(buf, cap, "%s was not mounted: %s.", vol, why);
		else
			n = snprintf(buf, cap, "%s was not mounted.", vol);
		break;
	case NTFSMAC_NOTIFY_VOLMARK_INTERRUPTED:
		/* Section 3.6, residual risk R-33: the text of the interrupted marking; `reason`
		 * names the remedy of the state. */
		n = snprintf(buf, cap,
			     "%s: this Mac was interrupted while marking the volume; data was not "
			     "changed.%s%s",
			     vol, why[0] != '\0' ? " " : "", why);
		break;
	case NTFSMAC_NOTIFY_MIRROR_SPLIT:
		/* Sections 3.2 and 3.7: no journal entry with an interrupted marker behind it. */
		n = snprintf(buf, cap,
			     "%s: this volume was interrupted while NTFS-3G updated its file table; "
			     "the data region was not touched. Run ntfsmac repair --sync-mirror, or "
			     "Windows chkdsk.",
			     vol);
		break;
	case NTFSMAC_NOTIFY_STRAY_MOUNT:
		/* T-M4-06 review H1 */
		n = snprintf(buf, cap,
			     "%s: the program that serves the volume mounted another file system. "
			     "It was stopped and the volume was detached.",
			     vol);
		break;
	default:
		n = -1;
		break;
	}
	if (n < 0 || (size_t)n >= cap) {
		buf[0] = '\0';
		return 0;
	}
	return (size_t)n;
}

/* ------------------------------------------------------------------ */
/* Delivery                                                            */
/* ------------------------------------------------------------------ */

static pthread_mutex_t g_sink_mu = PTHREAD_MUTEX_INITIALIZER;
static ntfsmac_notify_sink_fn g_sink;
static void *g_sink_ctx;

void ntfsmac_notify_set_sink(ntfsmac_notify_sink_fn fn, void *ctx)
{
	pthread_mutex_lock(&g_sink_mu);
	g_sink = fn;
	g_sink_ctx = ctx;
	pthread_mutex_unlock(&g_sink_mu);
}

#if defined(__APPLE__)

typedef struct banner {
	uid_t uid;
	char text[NOTIFY_TEXT_MAX];
} banner;

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void *banner_thread(void *arg)
{
	banner *b = arg;
	char uidtxt[16], who[24];
	posix_spawn_file_actions_t fa;
	pid_t pid;
	char *envp[] = {(char *)"PATH=/usr/bin:/bin:/usr/sbin:/sbin", NULL};
	/* osascript gives the arguments after the scripts to `on run argv`: the text is data. */
	char *argv[] = {(char *)"/bin/launchctl", (char *)"asuser", uidtxt,
			(char *)"/usr/bin/sudo", (char *)"-n", (char *)"-u", who,
			(char *)"/usr/bin/osascript",
			(char *)"-e", (char *)"on run argv",
			(char *)"-e", (char *)"display notification (item 1 of argv) with title (item 2 of argv)",
			(char *)"-e", (char *)"end run",
			b->text, (char *)NOTIFY_TITLE, NULL};
	uint64_t deadline;
	int st;

	snprintf(uidtxt, sizeof uidtxt, "%u", (unsigned)b->uid);
	snprintf(who, sizeof who, "#%u", (unsigned)b->uid);
	if (posix_spawn_file_actions_init(&fa) == 0) {
		(void)posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
		(void)posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
		(void)posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
		if (posix_spawn(&pid, argv[0], &fa, NULL, argv, envp) == 0) {
			deadline = now_ms() + NOTIFY_HELPER_WAIT_MS;
			for (;;) {
				pid_t r = waitpid(pid, &st, WNOHANG);
				struct timespec ts = {0, 50 * 1000000L};

				if (r == pid || (r < 0 && errno != EINTR))
					break;
				if (now_ms() >= deadline) {
					(void)kill(pid, SIGKILL);
					(void)waitpid(pid, &st, 0);
					break;
				}
				nanosleep(&ts, NULL);
			}
		}
		posix_spawn_file_actions_destroy(&fa);
	}
	free(b);
	return NULL;
}

static ntfsmac_dmn_status deliver_banner(const char *text)
{
	struct stat sb;
	banner *b;
	pthread_t t;
	pthread_attr_t at;

	/* The owner of /dev/console is the user at the console; root means nobody is logged in. */
	if (stat("/dev/console", &sb) != 0 || sb.st_uid == 0)
		return NTFSMAC_DMN_E_STATE;
	b = calloc(1, sizeof *b);
	if (b == NULL)
		return NTFSMAC_DMN_E_IO;
	b->uid = sb.st_uid;
	snprintf(b->text, sizeof b->text, "%s", text);
	if (pthread_attr_init(&at) != 0) {
		free(b);
		return NTFSMAC_DMN_E_IO;
	}
	(void)pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&t, &at, banner_thread, b) != 0) {
		pthread_attr_destroy(&at);
		free(b);
		return NTFSMAC_DMN_E_IO;
	}
	pthread_attr_destroy(&at);
	return NTFSMAC_DMN_OK;
}

#else

static ntfsmac_dmn_status deliver_banner(const char *text)
{
	(void)text;
	return NTFSMAC_DMN_E_NOTIMPL;
}

#endif

ntfsmac_dmn_status ntfsmac_notify_post(ntfsmac_notify_kind kind, const char *label,
				       const char *reason)
{
	char text[NOTIFY_TEXT_MAX];
	ntfsmac_notify_sink_fn sink;
	void *sctx;

	if (ntfsmac_notify_text(kind, label, reason, text, sizeof text) == 0)
		return NTFSMAC_DMN_E_INVAL;
	NTFSMAC_LOGI("notification: %s", text);
	pthread_mutex_lock(&g_sink_mu);
	sink = g_sink;
	sctx = g_sink_ctx;
	pthread_mutex_unlock(&g_sink_mu);
	if (sink != NULL) {
		sink(kind, text, sctx);
		return NTFSMAC_DMN_OK;
	}
	return deliver_banner(text);
}
