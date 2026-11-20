/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-DMN-UM-01..05: unmount retry logic (src/daemon/unmount.c) driven through
 * the hooks. No mount is made and nothing sleeps.
 */
#include <errno.h>
#include <string.h>

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
