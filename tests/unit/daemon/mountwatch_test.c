/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-DMN-MW-01..08: the mount-table watcher of T-M4-08 (src/daemon/mountwatch.c, T-M4-06 review H1).
 * The decision is a pure function over lists; the bounded child runs real processes but touches
 * no mount. The real mount table and the force-unmount are covered by the VM driver.
 */
#include <signal.h>
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
	CHECK_EQ_INT(ntfsmac_mw_decide(m, 5000, w, 1, NULL, 0, stray, kill_w), 0);

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
	if (g_failures != 0) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	return 0;
}
