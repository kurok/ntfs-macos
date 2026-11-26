/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_DAEMON_MOUNTWATCH_H
#define NTFSMAC_DAEMON_MOUNTWATCH_H

/*
 * Mount-table watcher (T-M4-08, answer to T-M4-06 review finding H1, residual risk R-07).
 *
 * A hijacked worker runs as the console user and may ask macFUSE to mount a second file system
 * on any directory that user owns (I-SBX-09 shows it on macOS 27.2). The sandbox cannot stop
 * that. The daemon therefore looks at the mount table (getfsstat(MNT_NOWAIT): the kernel's
 * cached table, no call into any file system) on every mount-table notification and on a slow
 * poll, and acts on this rule:
 *
 *   A FUSE mount that is owned by the uid of a live worker, that is not the mount point of
 *   any volume of the daemon (live or being torn down), and that was not already there before
 *   that uid's first worker was started, is a stray mount.
 *
 * The decision is a pure function over lists (ntfsmac_mw_decide), tested on every host. What
 * the daemon does about a stray mount is in main.c: kill every live worker of that uid with
 * ntfsmac_child_kill_group(), force-unmount the stray mount with ntfsmac_mw_force_unmount()
 * (a child process, a time bound, no path inside the mount touched), mark each affected
 * session failed in the journal, notify the user, and run ntfsmac_child_device_check() before
 * the volume is reported detached.
 *
 * Known limit: a macFUSE mount that the same user makes by hand (sshfs, for example) while an
 * NTFS volume is mounted looks the same and is treated as stray, because the table does not say
 * which process made it. Mounts that exist before a worker of that user is started are never
 * touched (the baseline is taken before the worker is marked live); the daemon logs every action.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "internal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NTFSMAC_MW_PATH_MAX 1024u /* MNAMELEN of f_mntonname on macOS */
#define NTFSMAC_MW_MOUNT_POINT_MAX 128u /* same as ntfsmac_mountpoint.path */

/* One row of the mount table, reduced to what the rule needs. */
typedef struct ntfsmac_mw_mount {
	char path[NTFSMAC_MW_PATH_MAX]; /* f_mntonname */
	uid_t owner;                    /* f_owner: the user that made the mount */
	bool fuse;                      /* f_fstypename is a macFUSE type */
	uint64_t id;                    /* f_fsid: names this mount, not its path */
} ntfsmac_mw_mount;

/* One volume that the daemon knows a mount point for. */
typedef struct ntfsmac_mw_worker {
	uid_t uid;                                  /* the user the worker runs as */
	char mount_point[NTFSMAC_MW_MOUNT_POINT_MAX]; /* /Volumes/<leaf>, "" if none yet */
	bool live;                                  /* the worker process is running */
} ntfsmac_mw_worker;

/*
 * Pure decision. For `mounts[0..n_mounts)`, `workers[0..n_workers)` and the ids of mounts that
 * were there before (`baseline[0..n_baseline)`, may be NULL with 0):
 *   stray[i]  set to true for each mount i that is a stray mount, false for the others
 *   kill[j]   set to true for each live worker j whose uid owns a stray mount, false for the
 *             others (a dead worker is never marked)
 * The two arrays must have n_mounts and n_workers elements. Returns the number of stray mounts.
 * A NULL array that is needed, more than 65536 mounts, or more than 4096 workers or baseline ids, marks nothing and returns 0 (the caller
 * has nothing safe to act on). The strings are compared as given: the function does not touch
 * any path.
 */
size_t ntfsmac_mw_decide(const ntfsmac_mw_mount *mounts, size_t n_mounts,
			 const ntfsmac_mw_worker *workers, size_t n_workers,
			 const uint64_t *baseline, size_t n_baseline,
			 bool *stray, bool *kill);

/*
 * The table is read in full up to NTFSMAC_MW_LIST_ROWS_MAX rows (ntfsmac_mw_list), and then filtered down to the
 * rows the rule can use (ntfsmac_mw_filter): FUSE mounts whose owner is the uid of a worker of the
 * daemon (or of the user whose baseline is being taken), plus the FUSE mounts named in the
 * baseline. The bound NTFSMAC_MW_RELEVANT_MAX is per uid of that list: mounts of other users or of
 * other file system types never count against a user's limit, and a user over it fails closed
 * for that user alone (T-M7-10).
 */
#define NTFSMAC_MW_RELEVANT_MAX 4096u
#define NTFSMAC_MW_BASELINE_MAX 256u

/*
 * Keep, in place and in order, the rows of `m[0..n)` that the rule needs: FUSE mounts whose owner
 * is the uid of one of `workers[0..n_workers)` or is `extra_uid` (-1: none), and FUSE mounts whose
 * id is in `baseline[0..n_baseline)` (so that the baseline can be pruned). Returns the number of
 * rows kept; the rows after it are left in an unspecified state. Pure.
 */
size_t ntfsmac_mw_filter(ntfsmac_mw_mount *m, size_t n, const ntfsmac_mw_worker *workers,
			 size_t n_workers, long extra_uid, const uint64_t *baseline, size_t n_baseline);

/*
 * State of the watcher between checks: the baseline (ids of FUSE mounts that were there before the
 * first worker of their user was started) and the run of failed reads. Zero is the initial state.
 */
#define NTFSMAC_MW_FAIL_CHECKS 3u    /* failed checks in a row before live workers are stopped */
#define NTFSMAC_MW_FAIL_MIN_MS 4000u /* and the first of them at least this long ago (2 polls) */

typedef struct ntfsmac_mw_watch {
	uint64_t base[NTFSMAC_MW_BASELINE_MAX];
	size_t nbase;
	unsigned fail_streak;   /* checks in a row whose table could not be judged */
	uint64_t fail_since_ms; /* monotonic time of the first of them */
} ntfsmac_mw_watch;

/*
 * The largest mount table that is read (rows of getfsstat()). A larger table is not a read
 * failure: it cannot be judged, and that fails closed at once (NTFSMAC_MW_READ_TOO_BIG).
 */
#define NTFSMAC_MW_LIST_ROWS_MAX 65536u

/* What ntfsmac_mw_step() is given: the outcome of ntfsmac_mw_list(). */
typedef enum ntfsmac_mw_read {
	NTFSMAC_MW_READ_FAILED = 0,  /* getfsstat() failed (E_IO): tolerated while it is short */
	NTFSMAC_MW_READ_OK = 1,      /* m[0..n_mounts) is the table */
	NTFSMAC_MW_READ_TOO_BIG = 2  /* more than NTFSMAC_MW_LIST_ROWS_MAX rows (E_RANGE): fail closed */
} ntfsmac_mw_read;

typedef struct ntfsmac_mw_step_out {
	bool judged;    /* the table was read and judged: stray[] and kill[] are the rule's */
	bool stop_all;  /* not judged and fail closed for every user: the table was larger than
			 * NTFSMAC_MW_LIST_ROWS_MAX (`too_big`, at once), or it could not be read for
			 * NTFSMAC_MW_FAIL_CHECKS checks over NTFSMAC_MW_FAIL_MIN_MS; kill[] marks every
			 * live worker */
	bool too_big;   /* the table had more than NTFSMAC_MW_LIST_ROWS_MAX rows */
	bool base_full; /* the baseline had no room for a mount of `uid_to_baseline` */
	bool base_overflow;     /* `uid_to_baseline` has more than NTFSMAC_MW_RELEVANT_MAX FUSE mounts */
	unsigned overflow_uids; /* uids over NTFSMAC_MW_RELEVANT_MAX: their live workers are in kill[] */
	size_t kept;    /* rows judged: stray[] covers m[0..kept) */
	size_t nstray;  /* stray mounts plus workers marked for an overflow or a lasting failure */
} ntfsmac_mw_step_out;

/*
 * One check of the table, without any I/O. `rd` NTFSMAC_MW_READ_FAILED: the table could not be
 * read (ntfsmac_mw_list gave E_IO); then `m` is not looked at. A failed read changes nothing but
 * the run of failures: the baseline is not pruned, no stray mount is reported, and a worker is
 * marked in kill[] only once the failure has persisted (`stop_all`); the tolerance is for read
 * failures only. NTFSMAC_MW_READ_TOO_BIG (ntfsmac_mw_list gave E_RANGE), or NTFSMAC_MW_READ_OK
 * with more than NTFSMAC_MW_LIST_ROWS_MAX rows: the table cannot be judged and that fails closed
 * at once: every live worker is marked in kill[] (`stop_all`, `too_big`), `m` is not looked at,
 * the baseline and the run of failures are left as they are, and false is returned (no attach).
 * Otherwise `m[0..n_mounts)` is the table; it is filtered in place (ntfsmac_mw_filter), the
 * baseline is pruned to the ids still in it, and a uid with more than NTFSMAC_MW_RELEVANT_MAX FUSE
 * mounts fails closed at once for itself alone: its live workers are marked in kill[] and its rows
 * are not judged. The rest is decided (ntfsmac_mw_decide), and with `uid_to_baseline` >= 0 every
 * FUSE mount of that user that is neither stray, nor a volume of the daemon, nor known is added to
 * the baseline. `stray` must have `n_mounts` elements (only the first `out->kept` are meaningful)
 * and `kill` `n_workers`. Returns true when the check was judged, `uid_to_baseline` did not
 * overflow and the baseline had room: the caller may start a worker.
 */
bool ntfsmac_mw_step(ntfsmac_mw_watch *s, ntfsmac_mw_read rd, ntfsmac_mw_mount *m, size_t n_mounts,
		     const ntfsmac_mw_worker *workers, size_t n_workers, long uid_to_baseline,
		     uint64_t now_ms, bool *stray, bool *kill, ntfsmac_mw_step_out *out);

/*
 * Is the volume whose teardown asked for this check to be treated as if its worker made a stray
 * mount? Yes when the check was not judged (fail closed: the session is failed and the user told,
 * as for a table that cannot be read) or when its worker was marked in kill[].
 */
bool ntfsmac_mw_self_found(const ntfsmac_mw_step_out *out, bool self_killed);

/* Monotonic milliseconds, the clock of ntfsmac_mw_step(). */
uint64_t ntfsmac_mw_now_ms(void);

/* True if a file system type name is one of macFUSE's (reconcile.c uses the same list). */
bool ntfsmac_mw_fstype_is_fuse(const char *fstypename);

/*
 * Platform lister: getfsstat(MNT_NOWAIT) into a new array that the caller frees with free(). The
 * buffer is sized from a first call that counts the mounts and is grown when the table grew in
 * between; a failed call is tried again, at most NTFSMAC_MW_LIST_TRIES times in all, before the
 * lister gives up (E_IO). A table with more than NTFSMAC_MW_LIST_ROWS_MAX rows, counted or read,
 * gives E_RANGE at once (not retried, not a read failure: the caller fails closed). Mounts whose
 * path is longer than the field are skipped. Not implemented off macOS (E_NOTIMPL).
 */
#define NTFSMAC_MW_LIST_TRIES 4u
ntfsmac_dmn_status ntfsmac_mw_list(ntfsmac_mw_mount **out, size_t *n);

/*
 * Run `fn(arg)` in a child process and wait at most `timeout_ms`. OK: the child exited 0.
 * IO: it exited otherwise or could not be started. TIMEOUT: it did not finish; it got SIGKILL
 * and is reaped if the kernel lets go within one second (a process stuck inside a file system
 * can ignore the signal for a while; it is then kept in a list and reaped later, see
 * ntfsmac_mw_reap_pending()). The child has every descriptor of the daemon closed except 0..2,
 * so it cannot keep a device busy. `fn` must call only async-signal-safe functions and use
 * nothing but its argument.
 */
ntfsmac_dmn_status ntfsmac_mw_run_bounded(int (*fn)(void *), void *arg, unsigned timeout_ms);

/* Hand a child that was killed and not yet reaped to the list that ntfsmac_mw_reap_pending() reaps. */
void ntfsmac_mw_track_unreaped(pid_t pid);

/*
 * Reap, without waiting, the killed children of run_bounded that were still alive after their
 * grace second. Called by every bounded run and by the mount table check. Returns how many are
 * still not reaped.
 */
unsigned ntfsmac_mw_reap_pending(void);

/*
 * unmount(2) with MNT_FORCE of `path`, in a child process with a time bound, so that a worker
 * that blocks the call cannot stall the daemon. The path is only handed to unmount(2); nothing
 * inside the mount is stat-ed, opened or read. OK, TIMEOUT or IO as above.
 */
ntfsmac_dmn_status ntfsmac_mw_force_unmount(const char *path, unsigned timeout_ms);

/*
 * unmount(2) of a mount point that the daemon made itself (/Volumes/<leaf>, a root-owned
 * parent), in a child with a time bound, retried on EBUSY `retries` times (at most
 * NTFSMAC_UNMOUNT_RETRIES_MAX). Same statuses as ntfsmac_unmount_path(): OK, E_STATE (not a
 * mount point), E_DENIED, E_BUSY (`*busy_pid` names a holder), E_IO, and E_TIMEOUT when the
 * call did not return in `timeout_ms` (the child is killed; the unmount may still complete
 * later). Every unmount of the daemon goes through this or the next call, so that a stopped or
 * hijacked worker cannot block a volume's queue (T-M4-08 review item 4).
 */
ntfsmac_dmn_status ntfsmac_mw_unmount_bounded(const char *path, bool force, unsigned retries,
					      unsigned timeout_ms, pid_t *busy_pid);

/* What the bounded child reported, for the daemon log. */
typedef struct ntfsmac_mw_um_info {
	int child_code; /* exit code of the child, -1 when it did not exit (timeout, error) */
	int err;        /* errno of the failing unmount(2) when it was not EBUSY, else 0 */
	unsigned ms;    /* duration of the whole call */
} ntfsmac_mw_um_info;

/* As ntfsmac_mw_unmount_bounded(); `info` may be NULL. */
ntfsmac_dmn_status ntfsmac_mw_unmount_bounded_ex(const char *path, bool force, unsigned retries,
						 unsigned timeout_ms, pid_t *busy_pid,
						 ntfsmac_mw_um_info *info);

/*
 * MNT_FORCE unmount of a path that the USER controls (a stray mount of a hijacked worker), made
 * by a child that first becomes `uid`/`gid` (never root). The kernel lets a user unmount only a
 * mount that the user owns (f_owner), so a path that the user swaps for a symbolic link between
 * the table and the call cannot make root unmount another file system (T-M4-08 review item 3;
 * XNU has no unmount by fsid: MNT_BYFSID does not exist in its sys/mount.h). E_INVAL for uid 0.
 * E_DENIED also when the drop failed.
 */
ntfsmac_dmn_status ntfsmac_mw_unmount_as_user(const char *path, uid_t uid, gid_t gid, unsigned timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* NTFSMAC_DAEMON_MOUNTWATCH_H */
