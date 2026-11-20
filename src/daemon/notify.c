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

/* ---- outcome of the last banner (T-M7-06) ---- */

int ntfsmac_banner_status(bool waited, int wst)
{
	if (!waited)
		return -1;
	if (WIFEXITED(wst))
		return WEXITSTATUS(wst);
	if (WIFSIGNALED(wst))
		return 128 + WTERMSIG(wst);
	return -1;
}

bool ntfsmac_banner_killed(bool waited, int wst)
{
	return waited && WIFSIGNALED(wst) && WTERMSIG(wst) == SIGKILL;
}

ntfsmac_banner_outcome ntfsmac_banner_classify(bool started, bool timed_out, int status)
{
	ntfsmac_banner_outcome o = {NTFSMAC_BANNER_OK, 0};

	if (!started) {
		o.state = NTFSMAC_BANNER_START_FAILED;
	} else if (timed_out) {
		o.state = NTFSMAC_BANNER_TIMEOUT;
	} else if (status != 0) {
		o.state = NTFSMAC_BANNER_NONZERO_EXIT;
		o.exit_code = status < 0 || status > 255 ? 255 : status;
	}
	return o;
}

bool ntfsmac_banner_needs_probe(const ntfsmac_banner_outcome *o)
{
	return o->state == NTFSMAC_BANNER_NONZERO_EXIT && o->exit_code == 1;
}

ntfsmac_banner_outcome ntfsmac_banner_refine(ntfsmac_banner_outcome o, int probe_status)
{
	/* `sudo -n -u #UID /usr/bin/true` exits 1 when sudo would need a password; /usr/bin/true
	 * never exits 1 itself. Exit 0 proves sudo allows it, so the 1 came from osascript or
	 * launchctl. Anything else (probe not started, timed out, other status) proves nothing. */
	if (ntfsmac_banner_needs_probe(&o) && probe_status == 1)
		o.state = NTFSMAC_BANNER_SUDO_REFUSED;
	return o;
}

const char *ntfsmac_banner_state_name(ntfsmac_banner_state s)
{
	switch (s) {
	case NTFSMAC_BANNER_NEVER: return "never";
	case NTFSMAC_BANNER_OK: return "ok";
	case NTFSMAC_BANNER_START_FAILED: return "start-failed";
	case NTFSMAC_BANNER_SUDO_REFUSED: return "sudo-refused";
	case NTFSMAC_BANNER_TIMEOUT: return "timeout";
	case NTFSMAC_BANNER_NONZERO_EXIT: return "nonzero-exit";
	default: return "unknown";
	}
}

long ntfsmac_banner_value(const ntfsmac_banner_outcome *o)
{
	return (long)o->state * 1000L + o->exit_code;
}

ntfsmac_doctor_finding ntfsmac_banner_finding(const ntfsmac_banner_outcome *o)
{
	ntfsmac_doctor_finding f;

	f.code = (int)NTFSMAC_DOCTOR_NOTIFY_DELIVERY;
	f.ok = o->state == NTFSMAC_BANNER_NEVER || o->state == NTFSMAC_BANNER_OK;
	f.value = ntfsmac_banner_value(o);
	return f;
}

static pthread_mutex_t g_banner_mu = PTHREAD_MUTEX_INITIALIZER;
static ntfsmac_banner_outcome g_banner_last; /* zero: NEVER */
static uint64_t g_banner_issued;             /* last sequence number handed out */
static uint64_t g_banner_stored;             /* sequence number of g_banner_last */

uint64_t ntfsmac_banner_begin(void)
{
	uint64_t seq;

	pthread_mutex_lock(&g_banner_mu);
	seq = ++g_banner_issued;
	pthread_mutex_unlock(&g_banner_mu);
	return seq;
}

bool ntfsmac_banner_record_seq(uint64_t seq, const ntfsmac_banner_outcome *o)
{
	bool took = false;

	pthread_mutex_lock(&g_banner_mu);
	if (seq >= g_banner_stored) { /* a slow, older banner never overwrites a newer one */
		g_banner_last = *o;
		g_banner_stored = seq;
		took = true;
	}
	pthread_mutex_unlock(&g_banner_mu);
	return took;
}

void ntfsmac_banner_record(const ntfsmac_banner_outcome *o)
{
	(void)ntfsmac_banner_record_seq(ntfsmac_banner_begin(), o);
}

ntfsmac_banner_outcome ntfsmac_banner_last(void)
{
	ntfsmac_banner_outcome o;

	pthread_mutex_lock(&g_banner_mu);
	o = g_banner_last;
	pthread_mutex_unlock(&g_banner_mu);
	return o;
}

#if defined(__APPLE__)

typedef struct banner {
	uid_t uid;
	uint64_t seq;
	char text[NOTIFY_TEXT_MAX];
} banner;

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/*
 * Run one helper with stdio on /dev/null and wait for it at most `wait_ms`, then kill it. Sets
 * `started`, and `status` as ntfsmac_banner_status() reads it; `timed_out` only when our own
 * SIGKILL ended it (a helper that died of another signal keeps its real status).
 */
static void run_helper(char *const argv[], unsigned wait_ms, bool *started, bool *timed_out,
		       int *status)
{
	posix_spawn_file_actions_t fa;
	pid_t pid;
	char *envp[] = {(char *)"PATH=/usr/bin:/bin:/usr/sbin:/sbin", NULL};
	int st = 0;
	bool waited = false;

	*started = false;
	*timed_out = false;
	*status = -1;
	if (posix_spawn_file_actions_init(&fa) != 0)
		return;
	(void)posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	(void)posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
	(void)posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
	if (posix_spawn(&pid, argv[0], &fa, NULL, argv, envp) == 0) {
		uint64_t deadline = now_ms() + wait_ms;

		*started = true;
		for (;;) {
			pid_t r = waitpid(pid, &st, WNOHANG);
			struct timespec ts = {0, 50 * 1000000L};

			if (r == pid) {
				waited = true;
				break;
			}
			if (r < 0 && errno != EINTR)
				break;
			if (now_ms() >= deadline) {
				pid_t k;

				(void)kill(pid, SIGKILL);
				do {
					k = waitpid(pid, &st, 0);
				} while (k < 0 && errno == EINTR);
				waited = k == pid;
				*timed_out = ntfsmac_banner_killed(waited, st);
				break;
			}
			nanosleep(&ts, NULL);
		}
		*status = ntfsmac_banner_status(waited, st);
	}
	posix_spawn_file_actions_destroy(&fa);
}

static void *banner_thread(void *arg)
{
	banner *b = arg;
	char uidtxt[16], who[24];
	/* osascript gives the arguments after the scripts to `on run argv`: the text is data. */
	char *argv[] = {(char *)"/bin/launchctl", (char *)"asuser", uidtxt,
			(char *)"/usr/bin/sudo", (char *)"-n", (char *)"-u", who,
			(char *)"/usr/bin/osascript",
			(char *)"-e", (char *)"on run argv",
			(char *)"-e", (char *)"display notification (item 1 of argv) with title (item 2 of argv)",
			(char *)"-e", (char *)"end run",
			/* "--": the text is never read as an option, even if it starts with "-" (M4 F-01) */
			(char *)"--", b->text, (char *)NOTIFY_TITLE, NULL};
	char *probe[] = {(char *)"/usr/bin/sudo", (char *)"-n", (char *)"-u", who,
			 (char *)"/usr/bin/true", NULL};
	bool started, timed_out, pstarted, ptimed_out, stored;
	int status, pstatus;
	ntfsmac_banner_outcome o;

	snprintf(uidtxt, sizeof uidtxt, "%u", (unsigned)b->uid);
	snprintf(who, sizeof who, "#%u", (unsigned)b->uid);
	run_helper(argv, NTFSMAC_BANNER_WAIT_MS, &started, &timed_out, &status);
	o = ntfsmac_banner_classify(started, timed_out, status);
	if (ntfsmac_banner_needs_probe(&o)) {
		/* Exit 1 has two possible sources; ask sudo alone, bounded, in this thread (never in
		 * doctor), to tell them apart. */
		run_helper(probe, NTFSMAC_BANNER_PROBE_WAIT_MS, &pstarted, &ptimed_out, &pstatus);
		o = ntfsmac_banner_refine(o, pstarted && !ptimed_out ? pstatus : -1);
	}
	/* Only the report learns of a failure; nothing else reads it (T-M7-06). */
	/* Every failure is logged; only the store depends on the order (a newer outcome wins). */
	stored = ntfsmac_banner_record_seq(b->seq, &o);
	if (o.state != NTFSMAC_BANNER_OK)
		NTFSMAC_LOGW("notification banner not delivered: %s, exit status %d%s",
			     ntfsmac_banner_state_name(o.state), o.exit_code,
			     stored ? "" : " (superseded by a newer banner)");
	free(b);
	return NULL;
}

/* No helper thread could be made: the banner is not shown; the report says so. */
static ntfsmac_dmn_status banner_not_started(uint64_t seq)
{
	ntfsmac_banner_outcome o = ntfsmac_banner_classify(false, false, 0);

	(void)ntfsmac_banner_record_seq(seq, &o);
	NTFSMAC_LOGW("notification banner not delivered: %s",
		     ntfsmac_banner_state_name(o.state));
	return NTFSMAC_DMN_E_IO;
}

static ntfsmac_dmn_status deliver_banner(const char *text)
{
	struct stat sb;
	banner *b;
	pthread_t t;
	pthread_attr_t at;
	uint64_t seq;

	/* The owner of /dev/console is the user at the console; root means nobody is logged in. */
	if (stat("/dev/console", &sb) != 0 || sb.st_uid == 0)
		return NTFSMAC_DMN_E_STATE;
	seq = ntfsmac_banner_begin();
	b = calloc(1, sizeof *b);
	if (b == NULL)
		return banner_not_started(seq);
	b->uid = sb.st_uid;
	b->seq = seq;
	snprintf(b->text, sizeof b->text, "%s", text);
	if (pthread_attr_init(&at) != 0) {
		free(b);
		return banner_not_started(seq);
	}
	(void)pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&t, &at, banner_thread, b) != 0) {
		pthread_attr_destroy(&at);
		free(b);
		return banner_not_started(seq);
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
