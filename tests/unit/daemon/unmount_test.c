/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-DMN-UM-01..05: unmount retry logic (src/daemon/unmount.c) driven through
 * the hooks. No mount is made and nothing sleeps.
 */
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "check.h"
#include "unmount_impl.h"

typedef struct fake {
	int fail_errno;       /* errno of each failing call */
	unsigned fail_times;  /* number of calls that fail before one succeeds; UINT_MAX = always */
	unsigned calls;
	unsigned forced_calls;
	unsigned sleeps;
	unsigned slept_ms;
	unsigned busy_lookups;
	pid_t busy;
	char last_path[64];
} fake;

static int f_unmount(void *ctx, const char *path, bool force)
{
	fake *f = ctx;

	f->calls++;
	if (force)
		f->forced_calls++;
	snprintf(f->last_path, sizeof f->last_path, "%s", path);
	if (f->fail_times == (unsigned)-1 || f->calls <= f->fail_times) {
		errno = f->fail_errno;
		return -1;
	}
	return 0;
}

static pid_t f_busy(void *ctx, const char *path)
{
	fake *f = ctx;

	(void)path;
	f->busy_lookups++;
	return f->busy;
}

static void f_sleep(void *ctx, unsigned ms)
{
	fake *f = ctx;

	f->sleeps++;
	f->slept_ms += ms;
}

static ntfsmac_dmn_status run(fake *f, const char *path, bool force, unsigned retries, pid_t *busy)
{
	ntfsmac_unmount_hooks h = { f_unmount, f_busy, f_sleep, f };
	ntfsmac_unmount_opts o = { force, retries };

	return ntfsmac_unmount_run(path, &o, busy, &h);
}

static bool fake_is_indexer(pid_t p)
{
	return p == 10 || p == 30;
}

typedef struct wfake {
	pid_t holders[8];
	size_t nholders;
	unsigned scans;
	uint64_t now;
	bool stopping;
	unsigned scheduled;
	unsigned last_gen;
} wfake;

static size_t w_holders(void *c, pid_t *p, size_t cap)
{
	wfake *f = c;
	size_t i;

	f->scans++;
	for (i = 0; i < f->nholders && i < cap; i++)
		p[i] = f->holders[i];
	return f->nholders;
}
static bool w_ind(void *c, pid_t p)
{
	(void)c;
	return fake_is_indexer(p);
}
static uint64_t w_now(void *c)
{
	return ((wfake *)c)->now;
}
static bool w_stop(void *c)
{
	return ((wfake *)c)->stopping;
}
static void w_sched(void *c, unsigned gen)
{
	wfake *f = c;

	f->scheduled++;
	f->last_gen = gen;
}

static void test_umwait(void)
{
	wfake f;
	ntfsmac_umwait w;
	ntfsmac_umwait_ops o = { w_holders, w_ind, w_now, w_stop, w_sched, 60000u, &f };
	pid_t rep;
	unsigned g;

	/* first try: a holder that is no indexer is reported at once, with no scan */
	memset(&f, 0, sizeof f);
	memset(&w, 0, sizeof w);
	CHECK(!ntfsmac_umwait_start(&w, &o, 20, &rep));
	CHECK_EQ_INT(rep, 20);
	CHECK_EQ_INT(f.scans, 0);
	CHECK(!w.waiting && f.scheduled == 0);

	/* only indexers: the wait starts, a retry is queued with the new generation */
	f.holders[0] = 10; f.holders[1] = 30; f.nholders = 2; f.now = 1000;
	CHECK(ntfsmac_umwait_start(&w, &o, 10, &rep));
	CHECK(w.waiting && f.scheduled == 1 && f.last_gen == w.gen);
	g = w.gen;
	CHECK_EQ_INT(w.deadline_ms, 61000);

	/* a retry of this generation tries; still busy with indexers: queued again, same generation */
	CHECK_EQ_INT(ntfsmac_umwait_retry(&w, &o, g, true), NTFSMAC_UMR_TRY);
	f.now = 5000;
	CHECK_EQ_INT(ntfsmac_umwait_after(&w, &o, NTFSMAC_DMN_E_BUSY, 10, &rep), NTFSMAC_UMA_AGAIN);
	CHECK(f.scheduled == 2 && f.last_gen == g);

	/* a mix appears: the non-indexer is reported */
	f.holders[2] = 20; f.nholders = 3;
	CHECK_EQ_INT(ntfsmac_umwait_after(&w, &o, NTFSMAC_DMN_E_BUSY, 10, &rep), NTFSMAC_UMA_BUSY);
	CHECK_EQ_INT(rep, 20);
	CHECK(!w.waiting);

	/* force cancel: the pending retry is stale */
	f.nholders = 2; f.now = 0;
	CHECK(ntfsmac_umwait_start(&w, &o, 10, &rep));
	g = w.gen;
	ntfsmac_umwait_cancel(&w);
	CHECK(!w.waiting && w.gen != g);
	CHECK_EQ_INT(ntfsmac_umwait_retry(&w, &o, g, true), NTFSMAC_UMR_STALE);

	/* a stale generation after a restart */
	CHECK(ntfsmac_umwait_start(&w, &o, 10, &rep));
	CHECK(w.gen != g);
	CHECK_EQ_INT(ntfsmac_umwait_retry(&w, &o, g, true), NTFSMAC_UMR_STALE);
	CHECK(w.waiting);

	/* shutdown during the wait, and a volume that left Unmounting: STOP, nothing to report */
	f.stopping = true;
	CHECK_EQ_INT(ntfsmac_umwait_retry(&w, &o, w.gen, true), NTFSMAC_UMR_STOP);
	CHECK(!w.waiting);
	f.stopping = false;
	CHECK(ntfsmac_umwait_start(&w, &o, 10, &rep));
	CHECK_EQ_INT(ntfsmac_umwait_retry(&w, &o, w.gen, false), NTFSMAC_UMR_STOP);

	/* the first pid is an indexer but the scan finds nobody: one more try, not busy */
	f.nholders = 0; f.now = 0;
	CHECK(ntfsmac_umwait_start(&w, &o, 10, &rep));
	CHECK_EQ_INT(ntfsmac_umwait_after(&w, &o, NTFSMAC_DMN_E_BUSY, 10, &rep), NTFSMAC_UMA_AGAIN);
	ntfsmac_umwait_cancel(&w);
	f.holders[0] = 10; f.holders[1] = 30; f.nholders = 2;

	/* the deadline ends it with the holder; other errors are not "busy"; OK ends it */
	f.now = 0;
	CHECK(ntfsmac_umwait_start(&w, &o, 10, &rep));
	f.now = 60000;
	CHECK_EQ_INT(ntfsmac_umwait_after(&w, &o, NTFSMAC_DMN_E_BUSY, 10, &rep), NTFSMAC_UMA_BUSY);
	f.now = 0;
	CHECK(ntfsmac_umwait_start(&w, &o, 10, &rep));
	CHECK_EQ_INT(ntfsmac_umwait_after(&w, &o, NTFSMAC_DMN_E_TIMEOUT, 0, &rep), NTFSMAC_UMA_FAILED);
	CHECK(ntfsmac_umwait_start(&w, &o, 10, &rep));
	CHECK_EQ_INT(ntfsmac_umwait_after(&w, &o, NTFSMAC_DMN_OK, 0, &rep), NTFSMAC_UMA_DONE);
	CHECK(!w.waiting);
}

int main(void)
{
	fake f;
	pid_t busy = 99;
	ntfsmac_unmount_hooks h = { f_unmount, f_busy, f_sleep, &f };
	ntfsmac_unmount_opts o = { false, 3 };

	/* U-DMN-UM-01: first call works: no retry, no sleep, no lookup. */
	memset(&f, 0, sizeof f);
	CHECK_EQ_INT(run(&f, "/Volumes/X", false, 3, &busy), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(f.calls, 1);
	CHECK_EQ_INT(f.sleeps, 0);
	CHECK_EQ_INT(f.busy_lookups, 0);
	CHECK_EQ_INT(busy, 0);
	CHECK_EQ_INT(f.forced_calls, 0);

	/* U-DMN-UM-02: transient EBUSY (T-M2-04): one or two failures, then success, with delays. */
	memset(&f, 0, sizeof f);
	f.fail_errno = EBUSY;
	f.fail_times = 1;
	CHECK_EQ_INT(run(&f, "/Volumes/X", false, 3, &busy), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(f.calls, 2);
	CHECK_EQ_INT(f.sleeps, 1);
	CHECK_EQ_INT(f.slept_ms, NTFSMAC_UNMOUNT_RETRY_DELAY_MS);
	CHECK_EQ_INT(f.busy_lookups, 0);
	CHECK_EQ_INT(f.forced_calls, 0); /* never escalates to force on its own */
	memset(&f, 0, sizeof f);
	f.fail_errno = EBUSY;
	f.fail_times = 3;
	CHECK_EQ_INT(run(&f, "/Volumes/X", false, 3, &busy), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(f.calls, 4);

	/* U-DMN-UM-03: permanent EBUSY: 1 + retries calls, then Busy with the holder's pid. */
	memset(&f, 0, sizeof f);
	f.fail_errno = EBUSY;
	f.fail_times = (unsigned)-1;
	f.busy = 4242;
	busy = 0;
	CHECK_EQ_INT(run(&f, "/Volumes/X", false, 3, &busy), NTFSMAC_DMN_E_BUSY);
	CHECK_EQ_INT(f.calls, 4);
	CHECK_EQ_INT(f.sleeps, 3);
	CHECK_EQ_INT(f.busy_lookups, 1);
	CHECK_EQ_INT(busy, 4242);
	CHECK_EQ_INT(f.forced_calls, 0);
	CHECK_EQ_INT(errno, EBUSY);
	/* No retries and an unknown holder. */
	memset(&f, 0, sizeof f);
	f.fail_errno = EBUSY;
	f.fail_times = (unsigned)-1;
	busy = 7;
	CHECK_EQ_INT(run(&f, "/Volumes/X", false, 0, &busy), NTFSMAC_DMN_E_BUSY);
	CHECK_EQ_INT(f.calls, 1);
	CHECK_EQ_INT(f.sleeps, 0);
	CHECK_EQ_INT(busy, 0);
	CHECK_EQ_INT(run(&f, "/Volumes/X", false, 0, NULL), NTFSMAC_DMN_E_BUSY); /* busy_pid may be NULL */
	/* Absurd retry count is capped. */
	memset(&f, 0, sizeof f);
	f.fail_errno = EBUSY;
	f.fail_times = (unsigned)-1;
	CHECK_EQ_INT(run(&f, "/Volumes/X", false, 1000000, &busy), NTFSMAC_DMN_E_BUSY);
	CHECK_EQ_INT(f.calls, 1 + NTFSMAC_UNMOUNT_RETRIES_MAX);

	/* U-DMN-UM-06: Spotlight and Quick Look holders are waited for, anything else is not. */
	CHECK_EQ_INT(ntfsmac_unmount_holder_is_indexer("mds"), 1);
	CHECK_EQ_INT(ntfsmac_unmount_holder_is_indexer("mds_stores"), 1);
	CHECK_EQ_INT(ntfsmac_unmount_holder_is_indexer("mdworker_shared"), 1);
	CHECK_EQ_INT(ntfsmac_unmount_holder_is_indexer("QuickLookUIService"), 1);
	CHECK_EQ_INT(ntfsmac_unmount_holder_is_indexer("quicklookd"), 1);
	CHECK_EQ_INT(ntfsmac_unmount_holder_is_indexer("mdsx"), 0);
	CHECK_EQ_INT(ntfsmac_unmount_holder_is_indexer("Finder"), 0);
	CHECK_EQ_INT(ntfsmac_unmount_holder_is_indexer("bash"), 0);
	CHECK_EQ_INT(ntfsmac_unmount_holder_is_indexer(""), 0);
	CHECK_EQ_INT(ntfsmac_unmount_holder_is_indexer(NULL), 0);
	CHECK_EQ_INT(ntfsmac_unmount_pid_is_indexer(0), 0);
	CHECK_EQ_INT(ntfsmac_unmount_pid_is_indexer(getpid()), 0); /* this test is no indexer */

	/* U-DMN-UM-07: the loop decisions (pure). */
	{
		ntfsmac_umw_in in;

		memset(&in, 0, sizeof in);
		in.nholders = 2;
		in.nindexers = 2;
		in.now_ms = 1000;
		in.deadline_ms = 61000;
		CHECK_EQ_INT(ntfsmac_unmount_wait_decide(&in), NTFSMAC_UMW_WAIT);
		in.now_ms = 60999; /* the bound is the deadline on the monotonic clock */
		CHECK_EQ_INT(ntfsmac_unmount_wait_decide(&in), NTFSMAC_UMW_WAIT);
		in.now_ms = 61000;
		CHECK_EQ_INT(ntfsmac_unmount_wait_decide(&in), NTFSMAC_UMW_BUSY);
		in.now_ms = 1000;
		in.nindexers = 1; /* one indexer and one other process: report the other one at once */
		CHECK_EQ_INT(ntfsmac_unmount_wait_decide(&in), NTFSMAC_UMW_BUSY);
		in.nindexers = 0;
		CHECK_EQ_INT(ntfsmac_unmount_wait_decide(&in), NTFSMAC_UMW_BUSY);
		in.nholders = 0; /* no holder known: busy as before */
		CHECK_EQ_INT(ntfsmac_unmount_wait_decide(&in), NTFSMAC_UMW_BUSY);
		in.nholders = 2;
		in.nindexers = 2;
		in.force = true; /* force wins */
		CHECK_EQ_INT(ntfsmac_unmount_wait_decide(&in), NTFSMAC_UMW_ABORT);
		in.force = false;
		in.stopping = true; /* the daemon stops */
		CHECK_EQ_INT(ntfsmac_unmount_wait_decide(&in), NTFSMAC_UMW_ABORT);
		in.stopping = false;
		in.gone = true; /* the disk is gone or the worker ended */
		CHECK_EQ_INT(ntfsmac_unmount_wait_decide(&in), NTFSMAC_UMW_ABORT);
	}
	/* classify: a mix of indexer and other holders names the other one */
	{
		pid_t pids[3] = { 10, 20, 30 }; /* 10 and 30 are indexers, 20 is not */
		pid_t rep = 0;
		size_t ind = 9;

		CHECK_EQ_INT(ntfsmac_unmount_classify(pids, 3, fake_is_indexer, &rep, &ind), 3);
		CHECK_EQ_INT(ind, 2);
		CHECK_EQ_INT(rep, 20);
		pids[1] = 10;
		CHECK_EQ_INT(ntfsmac_unmount_classify(pids, 3, fake_is_indexer, &rep, &ind), 3);
		CHECK_EQ_INT(ind, 3);
		CHECK_EQ_INT(rep, 10); /* all indexers: the first holder */
		CHECK_EQ_INT(ntfsmac_unmount_classify(pids, 0, fake_is_indexer, &rep, &ind), 0);
		CHECK_EQ_INT(rep, 0);
	}

	/* U-DMN-UM-08: the wait state machine with fakes. */
	test_umwait();

	/* U-DMN-UM-04: forced unmount passes the flag on every attempt, and only then. */
	memset(&f, 0, sizeof f);
	CHECK_EQ_INT(run(&f, "/Volumes/X", true, 3, &busy), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(f.calls, 1);
	CHECK_EQ_INT(f.forced_calls, 1);
	memset(&f, 0, sizeof f);
	f.fail_errno = EBUSY;
	f.fail_times = 1;
	CHECK_EQ_INT(run(&f, "/Volumes/X", true, 3, &busy), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(f.forced_calls, 2);

	/* U-DMN-UM-05: other errors are not retried and are mapped. */
	{
		static const struct {
			int err;
			ntfsmac_dmn_status want;
		} t[] = {
			{ EINVAL, NTFSMAC_DMN_E_STATE }, { ENOENT, NTFSMAC_DMN_E_STATE },
			{ EPERM, NTFSMAC_DMN_E_DENIED },  { EACCES, NTFSMAC_DMN_E_DENIED },
			{ EIO, NTFSMAC_DMN_E_IO },        { ENXIO, NTFSMAC_DMN_E_IO },
		};
		size_t i;

		for (i = 0; i < sizeof t / sizeof t[0]; i++) {
			memset(&f, 0, sizeof f);
			f.fail_errno = t[i].err;
			f.fail_times = (unsigned)-1;
			busy = 5;
			CHECK_EQ_INT(run(&f, "/Volumes/X", false, 3, &busy), t[i].want);
			CHECK_EQ_INT(f.calls, 1);
			CHECK_EQ_INT(f.busy_lookups, 0);
			CHECK_EQ_INT(busy, 0);
		}
	}
	/* EINTR is retried without a delay. */
	memset(&f, 0, sizeof f);
	f.fail_errno = EINTR;
	f.fail_times = 2;
	CHECK_EQ_INT(run(&f, "/Volumes/X", false, 3, &busy), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(f.calls, 3);
	CHECK_EQ_INT(f.sleeps, 0);

	/* Arguments: nothing is called for a bad path or a NULL option block. */
	{
		static const char *const bad[] = {
			NULL, "", "relative", "/Volumes/../etc", "/Volumes/./X", "/..", "/Volumes/X/..",
		};
		size_t i;

		for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
			memset(&f, 0, sizeof f);
			busy = 3;
			CHECK_EQ_INT(run(&f, bad[i], false, 1, &busy), NTFSMAC_DMN_E_INVAL);
			CHECK_EQ_INT(f.calls, 0);
			CHECK_EQ_INT(busy, 0);
		}
	}
	memset(&f, 0, sizeof f);
	CHECK_EQ_INT(ntfsmac_unmount_run("/Volumes/X", NULL, NULL, &h), NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_unmount_run("/Volumes/X", &o, NULL, NULL), NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(f.calls, 0);
	/* Names with dots and spaces that are not "." or ".." are fine. */
	CHECK_EQ_INT(run(&f, "/Volumes/a..b/.c d", false, 0, NULL), NTFSMAC_DMN_OK);
	CHECK(strcmp(f.last_path, "/Volumes/a..b/.c d") == 0);
	CHECK_EQ_INT(ntfsmac_unmount_path("relative", &o, &busy), NTFSMAC_DMN_E_INVAL);

	/*
	 * U-DMN-UM-xx: rule 3 of the clean-session proof. The mount must be gone AND the unmount must
	 * have been seen: requested by the daemon, or reported by Disk Arbitration (macFUSE's disk of
	 * the volume gone). A mount that is merely gone (a killed FSKit module, T-M5-05) is not enough.
	 * The inputs are monotone flags, so the result does not depend on the order in which the
	 * request, the DA report and the mount-table change arrived.
	 */
	CHECK(!ntfsmac_unmount_seen(true, false, false));
	CHECK(ntfsmac_unmount_seen(true, true, false));
	CHECK(ntfsmac_unmount_seen(true, false, true));
	CHECK(ntfsmac_unmount_seen(true, true, true));
	CHECK(!ntfsmac_unmount_seen(false, false, false));
	CHECK(!ntfsmac_unmount_seen(false, true, false));
	CHECK(!ntfsmac_unmount_seen(false, false, true));
	CHECK(!ntfsmac_unmount_seen(false, true, true));

	if (g_failures != 0) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	return 0;
}
