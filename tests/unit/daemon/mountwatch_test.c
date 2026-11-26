/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-DMN-MW-01..17: the mount-table watcher of T-M4-08 (src/daemon/mountwatch.c, T-M4-06 review H1).
 * The decision is a pure function over lists; the bounded child runs real processes but touches
 * no mount. The real mount table and the force-unmount are covered by the VM driver.
 */
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "check.h"
#include "mountwatch.h"

#define UID_A 501u
#define UID_B 502u

static void mnt(ntfsmac_mw_mount *m, const char *path, uid_t owner, bool fuse, uint64_t id)
{
	memset(m, 0, sizeof *m);
	snprintf(m->path, sizeof m->path, "%s", path);
	m->owner = owner;
	m->fuse = fuse;
	m->id = id;
}

static void wrk(ntfsmac_mw_worker *w, uid_t uid, const char *mp, bool live)
{
	memset(w, 0, sizeof *w);
	w->uid = uid;
	snprintf(w->mount_point, sizeof w->mount_point, "%s", mp);
	w->live = live;
}

/* U-DMN-MW-01: the worker's own mount is not stray; an extra FUSE mount of its uid is. */
static void test_basic(void)
{
	ntfsmac_mw_mount m[3];
	ntfsmac_mw_worker w[1];
	bool stray[3], kill_w[1];

	mnt(&m[0], "/", 0, false, 1);
	mnt(&m[1], "/Volumes/Data", UID_A, true, 2);
	wrk(&w[0], UID_A, "/Volumes/Data", true);
	CHECK_EQ_INT(ntfsmac_mw_decide(m, 2, w, 1, NULL, 0, stray, kill_w), 0);
	CHECK(!stray[0] && !stray[1] && !kill_w[0]);

	mnt(&m[2], "/Users/u/trap", UID_A, true, 3);
	CHECK_EQ_INT(ntfsmac_mw_decide(m, 3, w, 1, NULL, 0, stray, kill_w), 1);
	CHECK(!stray[0] && !stray[1] && stray[2] && kill_w[0]);
}

/* U-DMN-MW-02: only FUSE mounts of the worker's own uid count. */
static void test_owner_and_type(void)
{
	ntfsmac_mw_mount m[4];
	ntfsmac_mw_worker w[1];
	bool stray[4], kill_w[1];

	wrk(&w[0], UID_A, "/Volumes/Data", true);
	mnt(&m[0], "/Volumes/Data", UID_A, true, 2);
	mnt(&m[1], "/Volumes/Other", UID_B, true, 3); /* another user's FUSE mount */
	mnt(&m[2], "/Volumes/Apfs", UID_A, false, 4); /* not FUSE, same user */
	mnt(&m[3], "/Volumes/Root", 0, true, 5);      /* root's FUSE mount */
	CHECK_EQ_INT(ntfsmac_mw_decide(m, 4, w, 1, NULL, 0, stray, kill_w), 0);
	CHECK(!kill_w[0]);
}

/* U-DMN-MW-03: with no live worker there is nothing to protect and nothing to kill. */
static void test_no_live_worker(void)
{
	ntfsmac_mw_mount m[2];
	ntfsmac_mw_worker w[1];
	bool stray[2], kill_w[1];

	mnt(&m[0], "/Volumes/Data", UID_A, true, 2);
	mnt(&m[1], "/Users/u/trap", UID_A, true, 3);
	wrk(&w[0], UID_A, "/Volumes/Data", false);
	CHECK_EQ_INT(ntfsmac_mw_decide(m, 2, w, 1, NULL, 0, stray, kill_w), 0);
	CHECK(!kill_w[0]);
	CHECK_EQ_INT(ntfsmac_mw_decide(m, 2, NULL, 0, NULL, 0, stray, NULL), 0);
	CHECK(!stray[0] && !stray[1]);
}

/* U-DMN-MW-04: the baseline names mounts by id; a new mount on an old path is still stray. */
static void test_baseline(void)
{
	ntfsmac_mw_mount m[3];
	ntfsmac_mw_worker w[1];
	const uint64_t base[] = {77};
	bool stray[3], kill_w[1];

	wrk(&w[0], UID_A, "/Volumes/Data", true);
	mnt(&m[0], "/Volumes/Data", UID_A, true, 2);
	mnt(&m[1], "/Volumes/sshfs", UID_A, true, 77); /* there before the worker */
	CHECK_EQ_INT(ntfsmac_mw_decide(m, 2, w, 1, base, 1, stray, kill_w), 0);
	CHECK(!kill_w[0]);
	mnt(&m[2], "/Volumes/sshfs", UID_A, true, 78); /* stacked on the same path */
	CHECK_EQ_INT(ntfsmac_mw_decide(m, 3, w, 1, base, 1, stray, kill_w), 1);
	CHECK(!stray[1] && stray[2] && kill_w[0]);
}

/*
 * U-DMN-MW-09 (review items 2 and 5): why the baseline is taken before a worker is live, and why
 * the table is looked at once more in the teardown, while the worker still counts as live.
 */
static void test_order(void)
{
	ntfsmac_mw_mount m[2];
	ntfsmac_mw_worker w[1];
	const uint64_t base[] = {77};
	bool stray[2], kill_w[1];

	mnt(&m[0], "/Volumes/Data", UID_A, true, 2);
	mnt(&m[1], "/Users/u/sshfs", UID_A, true, 77); /* the user's own mount, made before the attach */
	wrk(&w[0], UID_A, "/Volumes/Data", true);
	/* A live worker and no baseline: the earlier mount looks like a stray one (the old order). */
	CHECK_EQ_INT(ntfsmac_mw_decide(m, 2, w, 1, NULL, 0, stray, kill_w), 1);
	CHECK(stray[1] && kill_w[0]);
	/* With the baseline taken first it is never touched. */
	CHECK_EQ_INT(ntfsmac_mw_decide(m, 2, w, 1, base, 1, stray, kill_w), 0);
	CHECK(!stray[1] && !kill_w[0]);
	/* A mount that appeared after the baseline is stray while the worker is live ... */
	mnt(&m[1], "/Users/u/evil", UID_A, true, 99);
	CHECK_EQ_INT(ntfsmac_mw_decide(m, 2, w, 1, base, 1, stray, kill_w), 1);
	/* ... and is not seen once the worker is marked dead: the teardown looks before that. */
	w[0].live = false;
	CHECK_EQ_INT(ntfsmac_mw_decide(m, 2, w, 1, base, 1, stray, kill_w), 0);
}

/* U-DMN-MW-10: the bounded unmount and the unmount as the user refuse bad input and never use root. */
static void test_unmount_calls(void)
{
	pid_t busy = 1;
	ntfsmac_dmn_status st;

	CHECK_EQ_INT(ntfsmac_mw_unmount_bounded(NULL, false, 0, 1000, &busy), NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_mw_unmount_bounded("relative", false, 0, 1000, &busy), NTFSMAC_DMN_E_INVAL);
	/* A path that does not exist: refused by the kernel, quickly, with a status. */
	st = ntfsmac_mw_unmount_bounded("/nonexistent-ntfsmac-test", false, 0, 5000, &busy);
#if defined(__APPLE__)
	CHECK_EQ_INT(st, NTFSMAC_DMN_E_STATE);
#else
	CHECK_EQ_INT(st, NTFSMAC_DMN_E_IO); /* unmount(2) of the daemon is macOS only; no other host unmounts */
#endif
	CHECK_EQ_INT(ntfsmac_mw_unmount_as_user("/tmp", 0, 0, 1000), NTFSMAC_DMN_E_INVAL); /* never root */
	CHECK_EQ_INT(ntfsmac_mw_unmount_as_user(NULL, 501, 20, 1000), NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_mw_unmount_as_user("relative", 501, 20, 1000), NTFSMAC_DMN_E_INVAL);
	if (geteuid() != 0) /* the drop to another user needs root: refused, nothing is unmounted */
		CHECK_EQ_INT(ntfsmac_mw_unmount_as_user("/tmp", geteuid() + 1, 20, 5000), NTFSMAC_DMN_E_DENIED);
}

/* U-DMN-MW-05: the mount point of a volume being torn down (worker dead) is still expected. */
static void test_dead_worker_path(void)
{
	ntfsmac_mw_mount m[2];
	ntfsmac_mw_worker w[2];
	bool stray[2], kill_w[2];

	wrk(&w[0], UID_A, "/Volumes/Old", false); /* the worker is gone, the mount lingers */
	wrk(&w[1], UID_A, "/Volumes/New", true);
	mnt(&m[0], "/Volumes/Old", UID_A, true, 2);
	mnt(&m[1], "/Volumes/New", UID_A, true, 3);
	CHECK_EQ_INT(ntfsmac_mw_decide(m, 2, w, 2, NULL, 0, stray, kill_w), 0);
	CHECK(!kill_w[0] && !kill_w[1]);
}

/* U-DMN-MW-06: a stray mount marks every live worker of that uid and no worker of another uid. */
static void test_kill_scope(void)
{
	ntfsmac_mw_mount m[3];
	ntfsmac_mw_worker w[4];
	bool stray[3], kill_w[4];

	wrk(&w[0], UID_A, "/Volumes/A1", true);
	wrk(&w[1], UID_A, "/Volumes/A2", true);
	wrk(&w[2], UID_B, "/Volumes/B1", true);
	wrk(&w[3], UID_A, "/Volumes/A3", false);
	mnt(&m[0], "/Volumes/A1", UID_A, true, 2);
	mnt(&m[1], "/Volumes/B1", UID_B, true, 3);
	mnt(&m[2], "/tmp/trap", UID_A, true, 4);
	CHECK_EQ_INT(ntfsmac_mw_decide(m, 3, w, 4, NULL, 0, stray, kill_w), 1);
	CHECK(kill_w[0] && kill_w[1] && !kill_w[2] && !kill_w[3]);
	CHECK(!stray[0] && !stray[1] && stray[2]);
}

/* U-DMN-MW-07: path comparison is exact: a prefix or a longer name is another path. */
static void test_path_exact(void)
{
	ntfsmac_mw_mount m[3];
	ntfsmac_mw_worker w[1];
	bool stray[3], kill_w[1];

	wrk(&w[0], UID_A, "/Volumes/Data", true);
	mnt(&m[0], "/Volumes/Data", UID_A, true, 2);
	mnt(&m[1], "/Volumes/Data 2", UID_A, true, 3);
	mnt(&m[2], "/Volumes/Dat", UID_A, true, 4);
	CHECK_EQ_INT(ntfsmac_mw_decide(m, 3, w, 1, NULL, 0, stray, kill_w), 2);
	CHECK(!stray[0] && stray[1] && stray[2]);
	/* A worker with no mount point yet protects nothing. */
	wrk(&w[0], UID_A, "", true);
	CHECK_EQ_INT(ntfsmac_mw_decide(m, 1, w, 1, NULL, 0, stray, kill_w), 1);
}

static int fn_ok(void *a)
{
	(void)a;
	return 0;
}
static int fn_fail(void *a)
{
	(void)a;
	return 1;
}
static int fn_hang(void *a)
{
	(void)a;
	for (;;)
		pause();
	return 0;
}

static int g_probe_fd = -1;

/* In the child: 0 if the descriptor that the parent had open is closed here. */
static int fn_fd_closed(void *a)
{
	(void)a;
	return fcntl(g_probe_fd, F_GETFD) == -1 && errno == EBADF ? 0 : 1;
}

/* In the child: stdin, stdout and stderr stay. */
static int fn_std_open(void *a)
{
	(void)a;
	return fcntl(0, F_GETFD) != -1 && fcntl(1, F_GETFD) != -1 && fcntl(2, F_GETFD) != -1 ? 0 : 1;
}

/* U-DMN-MW-09: the bounded child holds no descriptor of the daemon; a killed child is not left a zombie. */
static void test_child_fds_and_reap(void)
{
	int p[2], fd;
	pid_t c;
	unsigned i;
	int st;

	CHECK(pipe(p) == 0);
	/* A descriptor without O_CLOEXEC, as the device descriptor of a session is: still closed. */
	fd = open("/dev/null", O_RDWR);
	CHECK(fd >= 3);
	g_probe_fd = fd;
	CHECK_EQ_INT(ntfsmac_mw_run_bounded(fn_fd_closed, NULL, 2000), NTFSMAC_DMN_OK);
	g_probe_fd = p[1];
	CHECK_EQ_INT(ntfsmac_mw_run_bounded(fn_fd_closed, NULL, 2000), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(ntfsmac_mw_run_bounded(fn_std_open, NULL, 2000), NTFSMAC_DMN_OK);
	/* The parent keeps its own descriptors. */
	CHECK(fcntl(p[1], F_GETFD) != -1 && fcntl(fd, F_GETFD) != -1);
	close(fd);
	close(p[0]);
	close(p[1]);

	/* A hung child is killed and reaped at once: nothing left to wait for. */
	CHECK_EQ_INT(ntfsmac_mw_run_bounded(fn_hang, NULL, 200), NTFSMAC_DMN_E_TIMEOUT);
	CHECK_EQ_INT(ntfsmac_mw_reap_pending(), 0);
	CHECK(waitpid(-1, &st, WNOHANG) == -1 && errno == ECHILD);

	/* A child that was killed but is still there (as one stuck in the kernel) is kept and reaped later. */
	c = fork();
	CHECK(c >= 0);
	if (c == 0) {
		for (;;)
			pause();
	}
	ntfsmac_mw_track_unreaped(c);
	CHECK_EQ_INT(ntfsmac_mw_reap_pending(), 1);
	CHECK(kill(c, SIGKILL) == 0);
	for (i = 0; i < 200 && ntfsmac_mw_reap_pending() != 0; i++)
		usleep(10000);
	CHECK_EQ_INT(ntfsmac_mw_reap_pending(), 0);
	CHECK(waitpid(c, &st, WNOHANG) == -1 && errno == ECHILD);
}

/* U-DMN-MW-08: bad arguments mark nothing; the type name list; the bounded child. */
static void test_misc(void)
{
	ntfsmac_mw_mount m[1];
	ntfsmac_mw_worker w[1];
	bool stray[1] = {true}, kill_w[1] = {true};
	time_t t0;

	mnt(&m[0], "/x", UID_A, true, 1);
	wrk(&w[0], UID_A, "/Volumes/Data", true);
	CHECK_EQ_INT(ntfsmac_mw_decide(NULL, 1, w, 1, NULL, 0, stray, kill_w), 0);
	CHECK_EQ_INT(ntfsmac_mw_decide(m, 1, w, 1, NULL, 0, NULL, kill_w), 0);
	CHECK_EQ_INT(ntfsmac_mw_decide(m, 1, w, 1, NULL, 1, stray, kill_w), 0);
	CHECK_EQ_INT(ntfsmac_mw_decide(m, 65537, w, 1, NULL, 0, stray, kill_w), 0); /* over the rows a table may have */

	CHECK(ntfsmac_mw_fstype_is_fuse("macfuse"));
	CHECK(ntfsmac_mw_fstype_is_fuse("osxfuse"));
	CHECK(!ntfsmac_mw_fstype_is_fuse("apfs"));
	CHECK(!ntfsmac_mw_fstype_is_fuse("ntfs"));
	CHECK(!ntfsmac_mw_fstype_is_fuse(NULL));

	CHECK_EQ_INT(ntfsmac_mw_run_bounded(fn_ok, NULL, 2000), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(ntfsmac_mw_run_bounded(fn_fail, NULL, 2000), NTFSMAC_DMN_E_IO);
	CHECK_EQ_INT(ntfsmac_mw_run_bounded(NULL, NULL, 2000), NTFSMAC_DMN_E_INVAL);
	t0 = time(NULL);
	CHECK_EQ_INT(ntfsmac_mw_run_bounded(fn_hang, NULL, 300), NTFSMAC_DMN_E_TIMEOUT);
	CHECK(time(NULL) - t0 <= 3);
	CHECK_EQ_INT(ntfsmac_mw_force_unmount("relative", 100), NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_mw_force_unmount(NULL, 100), NTFSMAC_DMN_E_INVAL);
}

/*
 * U-DMN-MW-11 (T-M7-10): a table of 10000 rows of other users and other file system types kills no
 * worker and leaves the check judged (an attach is not refused); the filter keeps only the FUSE
 * mounts of the workers' users and of the baseline.
 */
static void test_big_table(void)
{
	const size_t n = 10000;
	ntfsmac_mw_mount *m = calloc(n, sizeof *m);
	bool *stray = calloc(n, sizeof *stray);
	ntfsmac_mw_worker w[1];
	ntfsmac_mw_watch *s = calloc(1, sizeof *s);
	ntfsmac_mw_step_out o;
	bool kill_w[1];
	size_t i;
	char p[64];

	CHECK(m != NULL && stray != NULL && s != NULL);
	if (m == NULL || stray == NULL || s == NULL)
		goto out;
	for (i = 0; i < n; i++) {
		snprintf(p, sizeof p, "/Volumes/img%zu", i);
		/* half FUSE mounts of another user, half other types of the worker's own user */
		mnt(&m[i], p, (i & 1) ? UID_B : UID_A, (i & 1) != 0, 1000 + i);
	}
	mnt(&m[n - 1], "/Volumes/Data", UID_A, true, 2); /* the worker's own volume */
	wrk(&w[0], UID_A, "/Volumes/Data", true);
	CHECK(ntfsmac_mw_step(s, true, m, n, w, 1, -1, 0, stray, kill_w, &o));
	CHECK(o.judged && !o.stop_all && !o.base_full);
	CHECK_EQ_INT((int)o.kept, 1);
	CHECK_EQ_INT((int)o.nstray, 0);
	CHECK(!kill_w[0]);
	CHECK_EQ_INT((int)s->fail_streak, 0);

	/* An attach of the other user takes its baseline from the same big table: its 5000 FUSE
	 * mounts are more than can be judged for it, so that attach is refused (fail closed for
	 * that user), and the worker of the first user is not killed for it. */
	for (i = 0; i < n; i++) {
		snprintf(p, sizeof p, "/Volumes/img%zu", i);
		mnt(&m[i], p, (i & 1) ? UID_B : UID_A, (i & 1) != 0, 1000 + i);
	}
	CHECK(!ntfsmac_mw_step(s, true, m, n, w, 1, (long)UID_B, 0, stray, kill_w, &o));
	CHECK(o.judged && o.base_overflow && o.overflow_uids == 1 && !o.stop_all && !kill_w[0]);
	CHECK_EQ_INT((int)s->fail_streak, 0);
	/* With 300 of them they are judged, but do not fit in the baseline: refused as well. */
	for (i = 0; i < n; i++) {
		snprintf(p, sizeof p, "/Volumes/img%zu", i);
		mnt(&m[i], p, (i < 600 && (i & 1)) ? UID_B : UID_A, i < 600 && (i & 1) != 0, 1000 + i);
	}
	CHECK(!ntfsmac_mw_step(s, true, m, n, w, 1, (long)UID_B, 0, stray, kill_w, &o));
	CHECK(o.judged && o.base_full && !kill_w[0]);
	CHECK_EQ_INT((int)s->fail_streak, 0);
out:
	free(m);
	free(stray);
	free(s);
}

/* U-DMN-MW-12 (T-M7-10): the filter keeps FUSE rows of the wanted users and of the baseline, in order. */
static void test_filter(void)
{
	ntfsmac_mw_mount m[6];
	ntfsmac_mw_worker w[1];
	const uint64_t base[] = {50};

	wrk(&w[0], UID_A, "/Volumes/Data", true);
	mnt(&m[0], "/", 0, false, 1);
	mnt(&m[1], "/Volumes/Data", UID_A, true, 2);
	mnt(&m[2], "/Volumes/b", UID_B, true, 3);
	mnt(&m[3], "/Volumes/old", 503, true, 50); /* in the baseline: kept for the prune */
	mnt(&m[4], "/Volumes/a2", UID_A, false, 5);
	mnt(&m[5], "/Volumes/c", 504, true, 6);
	CHECK_EQ_INT((int)ntfsmac_mw_filter(m, 6, w, 1, 504, base, 1), 3);
	CHECK(m[0].id == 2 && m[1].id == 50 && m[2].id == 6);
	CHECK_EQ_INT((int)ntfsmac_mw_filter(NULL, 6, w, 1, -1, NULL, 0), 0);
}

/*
 * U-DMN-MW-13 (T-M7-10): one failed read followed by a good one kills no worker and leaves the
 * baseline as it was; a failure that lasts NTFSMAC_MW_FAIL_CHECKS checks over
 * NTFSMAC_MW_FAIL_MIN_MS stops every live worker (and a dead one stays unmarked). Every failed
 * check refuses a new worker.
 */
static void test_failed_reads(void)
{
	ntfsmac_mw_mount m[3];
	ntfsmac_mw_worker w[2];
	ntfsmac_mw_watch s;
	ntfsmac_mw_step_out o;
	bool stray[3], kill_w[2];
	uint64_t t = 1000000;
	unsigned k;

	memset(&s, 0, sizeof s);
	s.base[0] = 77;
	s.nbase = 1;
	wrk(&w[0], UID_A, "/Volumes/Data", true);
	wrk(&w[1], UID_B, "", false);
	mnt(&m[0], "/Volumes/Data", UID_A, true, 2);
	mnt(&m[1], "/Users/u/sshfs", UID_A, true, 77);

	/* One failure: nothing killed, the baseline is not emptied, the attach is refused. */
	CHECK(!ntfsmac_mw_step(&s, false, NULL, 0, w, 2, (long)UID_A, t, NULL, kill_w, &o));
	CHECK(!o.judged && !o.stop_all && !kill_w[0] && !kill_w[1]);
	CHECK_EQ_INT((int)s.nbase, 1);
	CHECK(s.base[0] == 77);
	/* A good read next: still nothing killed, the user's earlier mount still known. */
	CHECK(ntfsmac_mw_step(&s, true, m, 2, w, 2, -1, t + 2000, stray, kill_w, &o));
	CHECK(o.judged && o.nstray == 0 && !kill_w[0]);
	CHECK_EQ_INT((int)s.nbase, 1);
	CHECK_EQ_INT((int)s.fail_streak, 0);

	/* A failure that lasts: the checks before the limit kill nothing ... */
	for (k = 0; k + 1 < NTFSMAC_MW_FAIL_CHECKS; k++) {
		CHECK(!ntfsmac_mw_step(&s, false, NULL, 0, w, 2, -1, t + 10000 + k * 2000u, NULL, kill_w, &o));
		CHECK(!o.stop_all && !kill_w[0]);
	}
	/* ... enough checks within less than the minimum time kill nothing either (a burst of events) ... */
	{
		ntfsmac_mw_watch b = s;

		b.fail_streak = 0;
		for (k = 0; k < NTFSMAC_MW_FAIL_CHECKS + 2; k++)
			(void)ntfsmac_mw_step(&b, false, NULL, 0, w, 2, -1, t + 50000 + k, NULL, kill_w, &o);
		CHECK(!o.stop_all && !kill_w[0]);
	}
	/* ... and the check that completes it stops the live worker only. */
	CHECK(!ntfsmac_mw_step(&s, false, NULL, 0, w, 2, -1, t + 10000 + NTFSMAC_MW_FAIL_MIN_MS, NULL,
			       kill_w, &o));
	CHECK(o.stop_all && kill_w[0] && !kill_w[1]);
	CHECK_EQ_INT((int)o.nstray, 1);
	CHECK_EQ_INT((int)s.nbase, 1); /* still untouched */
}

/*
 * U-DMN-MW-14 (T-M7-10, review): more FUSE mounts of a live worker's own uid than can be judged
 * fail closed at once (no tolerance, no window for a new stray mount): that worker is marked,
 * in the same check. A flood by uid B never marks the worker of uid A, and A's own stray mount is
 * still found in the same table.
 */
static void test_too_many_relevant(void)
{
	const size_t n = NTFSMAC_MW_RELEVANT_MAX + 3;
	ntfsmac_mw_mount *m = calloc(n, sizeof *m);
	bool *stray = calloc(n, sizeof *stray);
	ntfsmac_mw_watch *s = calloc(1, sizeof *s);
	ntfsmac_mw_worker w[2];
	ntfsmac_mw_step_out o;
	bool kill_w[2];
	size_t i;

	CHECK(m != NULL && stray != NULL && s != NULL);
	if (m == NULL || stray == NULL || s == NULL)
		goto out;
	/* A floods: its worker is stopped in this check. */
	wrk(&w[0], UID_A, "/Volumes/Data", true);
	for (i = 0; i < n; i++)
		mnt(&m[i], "/Users/u/x", UID_A, true, 10 + i);
	CHECK(ntfsmac_mw_step(s, true, m, n, w, 1, -1, 0, stray, kill_w, &o));
	CHECK(o.judged && !o.stop_all && o.overflow_uids == 1 && kill_w[0]);
	CHECK_EQ_INT((int)s->fail_streak, 0);

	/* B floods while A has a worker and one stray mount: only A's worker, for its stray mount. */
	wrk(&w[0], UID_A, "/Volumes/Data", true);
	wrk(&w[1], UID_B, "/Volumes/Other", true);
	for (i = 0; i < n; i++)
		mnt(&m[i], "/Users/b/x", UID_B, true, 10 + i);
	mnt(&m[0], "/Volumes/Data", UID_A, true, 2);
	CHECK(ntfsmac_mw_step(s, true, m, n, w, 2, -1, 0, stray, kill_w, &o));
	CHECK(o.judged && o.overflow_uids == 1 && !kill_w[0] && kill_w[1]);
	for (i = 0; i < n; i++)
		mnt(&m[i], "/Users/b/x", UID_B, true, 10 + i);
	mnt(&m[0], "/Volumes/Data", UID_A, true, 2);
	mnt(&m[1], "/Users/a/evil", UID_A, true, 5);
	w[1].live = false; /* B's worker already gone: B's flood still marks nobody else */
	CHECK(ntfsmac_mw_step(s, true, m, n, w, 2, -1, 0, stray, kill_w, &o));
	CHECK(o.judged && kill_w[0] && !kill_w[1]);
	CHECK_EQ_INT((int)o.kept, 2);
	CHECK(stray[1] && strcmp(m[1].path, "/Users/a/evil") == 0);
out:
	free(m);
	free(stray);
	free(s);
}

/*
 * U-DMN-MW-16 (T-M7-10, review): the teardown's last look fails closed when the check was not
 * judged (a failed read, before the tolerance has run out), as it does when its worker is marked.
 */
static void test_self_found(void)
{
	ntfsmac_mw_step_out o;
	ntfsmac_mw_watch s;
	ntfsmac_mw_worker w[1];
	bool kill_w[1];

	memset(&s, 0, sizeof s);
	wrk(&w[0], UID_A, "/Volumes/Data", true);
	CHECK(!ntfsmac_mw_step(&s, false, NULL, 0, w, 1, -1, 0, NULL, kill_w, &o));
	CHECK(!o.judged && !o.stop_all && !kill_w[0]);
	CHECK(ntfsmac_mw_self_found(&o, false)); /* not judged: found */
	memset(&o, 0, sizeof o);
	o.judged = true;
	CHECK(!ntfsmac_mw_self_found(&o, false));
	CHECK(ntfsmac_mw_self_found(&o, true));
	CHECK(ntfsmac_mw_self_found(NULL, false));
}

/*
 * U-DMN-MW-17 (T-M7-10, re-review): a table larger than NTFSMAC_MW_LIST_ROWS_MAX is no read failure.
 * Given as such (E_RANGE of the lister) or as a read table with more rows, it fails closed in the
 * same check: every live worker of every user is marked, no attach, the teardown's look counts it
 * as found, and the run of read failures and the baseline are left alone. The table is not looked
 * at (a one-row array with a larger count would be read out of bounds otherwise).
 */
static void test_too_big(void)
{
	ntfsmac_mw_watch s;
	ntfsmac_mw_worker w[3];
	ntfsmac_mw_mount m[1];
	ntfsmac_mw_step_out o;
	bool kill_w[3], stray[1];

	memset(&s, 0, sizeof s);
	s.base[0] = 77;
	s.nbase = 1;
	wrk(&w[0], UID_A, "/Volumes/Data", true);
	wrk(&w[1], UID_B, "/Volumes/Other", true);
	wrk(&w[2], UID_B, "/Volumes/Gone", false);
	mnt(&m[0], "/Volumes/Data", UID_A, true, 2);

	/* the lister said E_RANGE */
	CHECK(!ntfsmac_mw_step(&s, NTFSMAC_MW_READ_TOO_BIG, NULL, 0, w, 3, (long)UID_A, 0, NULL, kill_w, &o));
	CHECK(o.too_big && o.stop_all && !o.judged && kill_w[0] && kill_w[1] && !kill_w[2]);
	CHECK_EQ_INT((int)o.nstray, 2);
	CHECK(ntfsmac_mw_self_found(&o, false));
	CHECK_EQ_INT((int)s.fail_streak, 0);
	CHECK(s.nbase == 1 && s.base[0] == 77);

	/* a read table over the bound (the lister's buffer had headroom): the same, at once */
	CHECK(!ntfsmac_mw_step(&s, NTFSMAC_MW_READ_OK, m, (size_t)NTFSMAC_MW_LIST_ROWS_MAX + 1, w, 3, -1, 10,
			       stray, kill_w, &o));
	CHECK(o.too_big && o.stop_all && !o.judged && kill_w[0] && kill_w[1] && !kill_w[2]);
	CHECK(ntfsmac_mw_self_found(&o, false));
	CHECK_EQ_INT((int)s.fail_streak, 0);
	CHECK(s.nbase == 1 && s.base[0] == 77);

	/* a failed read after it still has its tolerance */
	CHECK(!ntfsmac_mw_step(&s, NTFSMAC_MW_READ_FAILED, NULL, 0, w, 3, -1, 20, NULL, kill_w, &o));
	CHECK(!o.too_big && !o.stop_all && !kill_w[0] && !kill_w[1]);
	CHECK_EQ_INT((int)s.fail_streak, 1);
}

/*
 * U-DMN-MW-15 (T-M7-10): through the step, a real stray FUSE mount of the worker's uid is still
 * found and stops the worker, and the baseline is pruned of mounts that are gone.
 */
static void test_step_stray(void)
{
	ntfsmac_mw_mount m[4];
	ntfsmac_mw_worker w[1];
	ntfsmac_mw_watch s;
	ntfsmac_mw_step_out o;
	bool stray[4], kill_w[1];

	memset(&s, 0, sizeof s);
	wrk(&w[0], UID_A, "", false);
	mnt(&m[0], "/", 0, false, 1);
	mnt(&m[1], "/Users/u/sshfs", UID_A, true, 77);
	/* Before the worker is live: the baseline of its user takes the earlier mount. */
	CHECK(ntfsmac_mw_step(&s, true, m, 2, w, 0, (long)UID_A, 0, stray, kill_w, &o));
	CHECK_EQ_INT((int)s.nbase, 1);
	/* The worker is live and has mounted; then a mount of its user appears elsewhere. */
	wrk(&w[0], UID_A, "/Volumes/Data", true);
	mnt(&m[0], "/", 0, false, 1);
	mnt(&m[1], "/Users/u/sshfs", UID_A, true, 77);
	mnt(&m[2], "/Volumes/Data", UID_A, true, 2);
	mnt(&m[3], "/Users/u/evil", UID_A, true, 99);
	CHECK(ntfsmac_mw_step(&s, true, m, 4, w, 1, -1, 0, stray, kill_w, &o));
	CHECK(o.judged && o.nstray == 1 && kill_w[0]);
	CHECK_EQ_INT((int)o.kept, 3);
	CHECK(stray[2] && strcmp(m[2].path, "/Users/u/evil") == 0);
	/* The earlier mount went away: it leaves the baseline on the next judged check. */
	mnt(&m[0], "/Volumes/Data", UID_A, true, 2);
	CHECK(ntfsmac_mw_step(&s, true, m, 1, w, 1, -1, 0, stray, kill_w, &o));
	CHECK_EQ_INT((int)s.nbase, 0);
}

int main(void)
{
	test_basic();
	test_owner_and_type();
	test_no_live_worker();
	test_baseline();
	test_order();
	test_unmount_calls();
	test_dead_worker_path();
	test_kill_scope();
	test_path_exact();
	test_misc();
	test_child_fds_and_reap();
	test_big_table();
	test_filter();
	test_failed_reads();
	test_too_many_relevant();
	test_step_stray();
	test_self_found();
	test_too_big();
	if (g_failures != 0) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	return 0;
}
