/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-DMN-NT-01..10: notification texts (src/daemon/notify.c; ARCHITECTURE.md sections 3.2, 3.6, 3.7,
 * 4.1 step 9, 4.5 step 3, 7). The texts are pure; delivery goes to a sink here, so a unit test never
 * shows a banner.
 */
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "check.h"
#include "internal.h"

static char g_last[600];
static ntfsmac_notify_kind g_kind;
static int g_calls;

static void sink(ntfsmac_notify_kind kind, const char *text, void *ctx)
{
	(void)ctx;
	g_kind = kind;
	snprintf(g_last, sizeof g_last, "%s", text);
	g_calls++;
}

static bool has(ntfsmac_notify_kind k, const char *label, const char *reason, const char *want)
{
	char buf[512];

	return ntfsmac_notify_text(k, label, reason, buf, sizeof buf) > 0 && strstr(buf, want) != NULL;
}

/* U-DMN-NT-01: the wordings the architecture fixes. */
static void test_wording(void)
{
	/* section 4.5 step 3 */
	CHECK(has(NTFSMAC_NOTIFY_EJECTED_CLEAN, "Disk", NULL, "ejected, all data written."));
	CHECK(has(NTFSMAC_NOTIFY_EJECTED_NO_FLUSH_CONFIRM, "Disk", NULL,
		  "ejected, all data written; this drive did not confirm that its cache was flushed"));
	CHECK(has(NTFSMAC_NOTIFY_SESSION_UNCLEAN, "Disk", NULL, "read-only next time"));
	/* section 3.6, interrupted marking */
	CHECK(has(NTFSMAC_NOTIFY_VOLMARK_INTERRUPTED, "Disk", "Run chkdsk.",
		  "this Mac was interrupted while marking the volume; data was not changed"));
	CHECK(has(NTFSMAC_NOTIFY_VOLMARK_INTERRUPTED, "Disk", "Run chkdsk.", "Run chkdsk."));
	/* sections 3.2 and 3.7, mirror_split without an entry */
	CHECK(has(NTFSMAC_NOTIFY_MIRROR_SPLIT, "Disk", NULL,
		  "this volume was interrupted while NTFS-3G updated its file table; the data region was "
		  "not touched"));
	CHECK(has(NTFSMAC_NOTIFY_MIRROR_SPLIT, "Disk", NULL, "ntfsmac repair --sync-mirror"));
	/* section 7, first E record */
	CHECK(has(NTFSMAC_NOTIFY_WRITE_ERROR, "Disk", "14:02", "Write error on Disk: unmounting"));
	CHECK(has(NTFSMAC_NOTIFY_WRITE_ERROR, "Disk", "14:02", "Data written after 14:02 may be lost."));
	CHECK(has(NTFSMAC_NOTIFY_WRITE_ERROR, "Disk", NULL, "Data written in this session may be lost."));
	/* section 4.1 step 9 */
	CHECK(has(NTFSMAC_NOTIFY_MOUNTED_RW, "Disk", NULL, "Disk is mounted read/write."));
	CHECK(has(NTFSMAC_NOTIFY_MOUNTED_RO, "Disk", "the engine mounted read-only",
		  "mounted read-only: the engine mounted read-only."));
	CHECK(has(NTFSMAC_NOTIFY_MOUNTED_RO, "Disk", NULL, "Disk is mounted read-only."));
	CHECK(has(NTFSMAC_NOTIFY_REFUSED, "Disk", "damaged", "was not mounted: damaged."));
	CHECK(has(NTFSMAC_NOTIFY_STRAY_MOUNT, "Disk", NULL, "mounted another file system"));
}

/* U-DMN-NT-02: no label, control bytes, limits. */
static void test_safety(void)
{
	char buf[512], small[10];

	CHECK(has(NTFSMAC_NOTIFY_MOUNTED_RW, NULL, NULL, "the volume is mounted read/write."));
	CHECK(has(NTFSMAC_NOTIFY_MOUNTED_RW, "", NULL, "the volume is mounted"));
	CHECK(ntfsmac_notify_text(NTFSMAC_NOTIFY_MOUNTED_RW, "a\nb\x1b" "c", NULL, buf, sizeof buf) > 0);
	CHECK(strchr(buf, '\n') == NULL && strchr(buf, '\x1b') == NULL && strstr(buf, "a?b?c") != NULL);
	CHECK_EQ_INT(ntfsmac_notify_text((ntfsmac_notify_kind)99, "x", NULL, buf, sizeof buf), 0);
	CHECK(buf[0] == '\0');
	CHECK_EQ_INT(ntfsmac_notify_text(NTFSMAC_NOTIFY_MOUNTED_RW, "Disk", NULL, small, sizeof small), 0);
	CHECK_EQ_INT(ntfsmac_notify_text(NTFSMAC_NOTIFY_MOUNTED_RW, "Disk", NULL, NULL, 10), 0);
	CHECK_EQ_INT(ntfsmac_notify_text(NTFSMAC_NOTIFY_MOUNTED_RW, "Disk", NULL, buf, 0), 0);
}

/* U-DMN-NT-03: every kind has a text. */
static void test_all_kinds(void)
{
	char buf[512];
	int k;

	for (k = 0; k <= (int)NTFSMAC_NOTIFY_STRAY_MOUNT; k++) {
		CHECK(ntfsmac_notify_text((ntfsmac_notify_kind)k, "Disk", "x", buf, sizeof buf) > 0);
		CHECK(!strstr(buf, "(null)"));
	}
}

/* U-DMN-NT-04: the sink gets the text and the kind; post refuses a bad kind. */
static void test_sink(void)
{
	ntfsmac_notify_set_sink(sink, NULL);
	CHECK_EQ_INT(ntfsmac_notify_post(NTFSMAC_NOTIFY_EJECTED_CLEAN, "Disk", NULL), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(g_calls, 1);
	CHECK_EQ_INT(g_kind, NTFSMAC_NOTIFY_EJECTED_CLEAN);
	CHECK(strcmp(g_last, "Disk ejected, all data written.") == 0);
	CHECK_EQ_INT(ntfsmac_notify_post((ntfsmac_notify_kind)99, "Disk", NULL), NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(g_calls, 1);
	ntfsmac_notify_set_sink(NULL, NULL);
}

/* U-DMN-NT-05 (T-M7-06): the outcome of a helper run, as a pure function driven by fake outcomes. */
static void test_banner_classify(void)
{
	ntfsmac_banner_outcome o;

	o = ntfsmac_banner_classify(true, false, 0);
	CHECK_EQ_INT(o.state, NTFSMAC_BANNER_OK);
	CHECK_EQ_INT(o.exit_code, 0);
	o = ntfsmac_banner_classify(false, false, 0); /* posix_spawn failed: no status to read */
	CHECK_EQ_INT(o.state, NTFSMAC_BANNER_START_FAILED);
	CHECK_EQ_INT(o.exit_code, 0);
	o = ntfsmac_banner_classify(false, true, 1); /* not started wins over everything */
	CHECK_EQ_INT(o.state, NTFSMAC_BANNER_START_FAILED);
	o = ntfsmac_banner_classify(true, true, 137); /* killed after 10 s: timeout, not exit 137 */
	CHECK_EQ_INT(o.state, NTFSMAC_BANNER_TIMEOUT);
	CHECK_EQ_INT(o.exit_code, 0);
	o = ntfsmac_banner_classify(true, false, 1); /* 1 alone proves nothing: see test_banner_probe */
	CHECK_EQ_INT(o.state, NTFSMAC_BANNER_NONZERO_EXIT);
	CHECK_EQ_INT(o.exit_code, 1);
	o = ntfsmac_banner_classify(true, false, 2);
	CHECK_EQ_INT(o.state, NTFSMAC_BANNER_NONZERO_EXIT);
	CHECK_EQ_INT(o.exit_code, 2);
	o = ntfsmac_banner_classify(true, false, 137); /* 128 + SIGKILL by someone else */
	CHECK_EQ_INT(o.state, NTFSMAC_BANNER_NONZERO_EXIT);
	CHECK_EQ_INT(o.exit_code, 137);
	o = ntfsmac_banner_classify(true, false, -1); /* the wait failed */
	CHECK_EQ_INT(o.state, NTFSMAC_BANNER_NONZERO_EXIT);
	CHECK_EQ_INT(o.exit_code, 255);
}

/* A real wait status of a child that exits with `code` (code >= 0) or dies of signal -code. */
static int child_status(int code)
{
	int st = 0;
	pid_t p = fork();

	if (p == 0) {
		if (code >= 0)
			_exit(code);
		signal(-code, SIG_DFL);
		raise(-code);
		pause();
		_exit(99);
	}
	CHECK(p > 0 && waitpid(p, &st, 0) == p);
	return st;
}

/* U-DMN-NT-08: wait status -> status and "our SIGKILL ended it", with real statuses. */
static void test_banner_status(void)
{
	int s0 = child_status(0), s1 = child_status(1), s77 = child_status(77);
	int sterm = child_status(-SIGTERM), skill = child_status(-SIGKILL);

	CHECK_EQ_INT(ntfsmac_banner_status(true, s0), 0);
	CHECK_EQ_INT(ntfsmac_banner_status(true, s1), 1);
	CHECK_EQ_INT(ntfsmac_banner_status(true, s77), 77);
	CHECK_EQ_INT(ntfsmac_banner_status(true, sterm), 128 + SIGTERM);
	CHECK_EQ_INT(ntfsmac_banner_status(true, skill), 128 + SIGKILL);
	CHECK_EQ_INT(ntfsmac_banner_status(false, s0), -1); /* the wait failed: the status is garbage */
	CHECK(!ntfsmac_banner_killed(true, s0));
	CHECK(!ntfsmac_banner_killed(true, s1));
	CHECK(!ntfsmac_banner_killed(true, sterm)); /* died of another signal: not our timeout */
	CHECK(ntfsmac_banner_killed(true, skill));
	CHECK(!ntfsmac_banner_killed(false, skill));
	/* a child that died of SIGTERM while we were killing it keeps its real status */
	{
		ntfsmac_banner_outcome o = ntfsmac_banner_classify(
			true, ntfsmac_banner_killed(true, sterm), ntfsmac_banner_status(true, sterm));

		CHECK_EQ_INT(o.state, NTFSMAC_BANNER_NONZERO_EXIT);
		CHECK_EQ_INT(o.exit_code, 128 + SIGTERM);
		o = ntfsmac_banner_classify(true, ntfsmac_banner_killed(true, skill),
					    ntfsmac_banner_status(true, skill));
		CHECK_EQ_INT(o.state, NTFSMAC_BANNER_TIMEOUT);
	}
}

/* U-DMN-NT-09: status 1 and the sudo probe: exact SUDO_REFUSED, or nothing claimed. */
static void test_banner_probe(void)
{
	ntfsmac_banner_outcome one = ntfsmac_banner_classify(true, false, 1);
	ntfsmac_banner_outcome two = ntfsmac_banner_classify(true, false, 2);
	ntfsmac_banner_outcome ok = ntfsmac_banner_classify(true, false, 0);
	ntfsmac_banner_outcome o;

	CHECK(ntfsmac_banner_needs_probe(&one));
	CHECK(!ntfsmac_banner_needs_probe(&two));
	CHECK(!ntfsmac_banner_needs_probe(&ok));
	o = ntfsmac_banner_refine(one, 1); /* sudo itself refuses */
	CHECK_EQ_INT(o.state, NTFSMAC_BANNER_SUDO_REFUSED);
	CHECK_EQ_INT(o.exit_code, 1);
	o = ntfsmac_banner_refine(one, 0); /* sudo allows it: osascript or launchctl failed */
	CHECK_EQ_INT(o.state, NTFSMAC_BANNER_NONZERO_EXIT);
	CHECK_EQ_INT(o.exit_code, 1);
	o = ntfsmac_banner_refine(one, -1); /* probe not run, timed out or unreadable */
	CHECK_EQ_INT(o.state, NTFSMAC_BANNER_NONZERO_EXIT);
	o = ntfsmac_banner_refine(one, 2);
	CHECK_EQ_INT(o.state, NTFSMAC_BANNER_NONZERO_EXIT);
	o = ntfsmac_banner_refine(two, 1); /* only status 1 is refined */
	CHECK_EQ_INT(o.state, NTFSMAC_BANNER_NONZERO_EXIT);
	CHECK_EQ_INT(o.exit_code, 2);
	o = ntfsmac_banner_refine(ok, 1);
	CHECK_EQ_INT(o.state, NTFSMAC_BANNER_OK);
	CHECK(strcmp(ntfsmac_banner_state_name(NTFSMAC_BANNER_SUDO_REFUSED), "sudo-refused") == 0);
	CHECK(strcmp(ntfsmac_banner_state_name(NTFSMAC_BANNER_TIMEOUT), "timeout") == 0);
	CHECK(strcmp(ntfsmac_banner_state_name((ntfsmac_banner_state)99), "unknown") == 0);
}

/* U-DMN-NT-10: a slow older banner never overwrites a newer outcome. */
static void test_banner_seq(void)
{
	ntfsmac_banner_outcome ok = {NTFSMAC_BANNER_OK, 0};
	ntfsmac_banner_outcome to = {NTFSMAC_BANNER_TIMEOUT, 0};
	ntfsmac_banner_outcome never = {NTFSMAC_BANNER_NEVER, 0};
	uint64_t older = ntfsmac_banner_begin(), newer = ntfsmac_banner_begin();

	CHECK(newer > older);
	CHECK(ntfsmac_banner_record_seq(newer, &ok));
	CHECK(!ntfsmac_banner_record_seq(older, &to)); /* finished late: dropped */
	CHECK_EQ_INT(ntfsmac_banner_last().state, NTFSMAC_BANNER_OK);
	CHECK(ntfsmac_banner_record_seq(newer, &to)); /* same banner may refine its own record */
	CHECK_EQ_INT(ntfsmac_banner_last().state, NTFSMAC_BANNER_TIMEOUT);
	ntfsmac_banner_record(&never); /* a fresh sequence number: always newest */
	CHECK_EQ_INT(ntfsmac_banner_last().state, NTFSMAC_BANNER_NEVER);
}

/* U-DMN-NT-06: outcome -> doctor finding; no banner yet is ok ("unknown"), not a failure. */
static void test_banner_finding(void)
{
	ntfsmac_banner_outcome never = {NTFSMAC_BANNER_NEVER, 0};
	ntfsmac_banner_outcome ok = {NTFSMAC_BANNER_OK, 0};
	ntfsmac_banner_outcome sf = {NTFSMAC_BANNER_START_FAILED, 0};
	ntfsmac_banner_outcome sr = {NTFSMAC_BANNER_SUDO_REFUSED, 1};
	ntfsmac_banner_outcome to = {NTFSMAC_BANNER_TIMEOUT, 0};
	ntfsmac_banner_outcome nz = {NTFSMAC_BANNER_NONZERO_EXIT, 77};
	ntfsmac_doctor_finding f;

	f = ntfsmac_banner_finding(&never);
	CHECK_EQ_INT(f.code, NTFSMAC_DOCTOR_NOTIFY_DELIVERY);
	CHECK(f.ok);
	CHECK_EQ_INT(f.value, 0);
	f = ntfsmac_banner_finding(&ok);
	CHECK(f.ok);
	CHECK_EQ_INT(f.value, 1000);
	f = ntfsmac_banner_finding(&sf);
	CHECK(!f.ok);
	CHECK_EQ_INT(f.value, 2000);
	f = ntfsmac_banner_finding(&sr);
	CHECK(!f.ok);
	CHECK_EQ_INT(f.value, 3001);
	f = ntfsmac_banner_finding(&to);
	CHECK(!f.ok);
	CHECK_EQ_INT(f.value, 4000);
	f = ntfsmac_banner_finding(&nz);
	CHECK(!f.ok);
	CHECK_EQ_INT(f.value, 5077);
	CHECK_EQ_INT(f.code, NTFSMAC_DOCTOR_NOTIFY_DELIVERY);
}

/* U-DMN-NT-07: the store keeps the last outcome; a sink (tests) delivers nothing and records nothing. */
static void test_banner_store(void)
{
	ntfsmac_banner_outcome sr = {NTFSMAC_BANNER_SUDO_REFUSED, 1};
	ntfsmac_banner_outcome never = {NTFSMAC_BANNER_NEVER, 0};
	ntfsmac_banner_outcome o = ntfsmac_banner_last();

	CHECK_EQ_INT(o.state, NTFSMAC_BANNER_NEVER);
	ntfsmac_notify_set_sink(sink, NULL);
	CHECK_EQ_INT(ntfsmac_notify_post(NTFSMAC_NOTIFY_MOUNTED_RW, "Disk", NULL), NTFSMAC_DMN_OK);
	ntfsmac_notify_set_sink(NULL, NULL);
	g_calls = 0;
	CHECK_EQ_INT(ntfsmac_banner_last().state, NTFSMAC_BANNER_NEVER);
	ntfsmac_banner_record(&sr);
	o = ntfsmac_banner_last();
	CHECK_EQ_INT(o.state, NTFSMAC_BANNER_SUDO_REFUSED);
	CHECK_EQ_INT(o.exit_code, 1);
	ntfsmac_banner_record(&never);
	CHECK_EQ_INT(ntfsmac_banner_last().state, NTFSMAC_BANNER_NEVER);
}

int main(void)
{
	test_banner_status();
	test_banner_probe();
	test_banner_seq();
	test_banner_classify();
	test_banner_finding();
	test_banner_store();
	test_wording();
	test_safety();
	test_all_kinds();
	test_sink();
	if (g_failures != 0) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	return 0;
}
