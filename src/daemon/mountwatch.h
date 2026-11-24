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
 * A NULL array that is needed, or a count above 4096, marks nothing and returns 0 (the caller
 * has nothing safe to act on). The strings are compared as given: the function does not touch
 * any path.
 */
size_t ntfsmac_mw_decide(const ntfsmac_mw_mount *mounts, size_t n_mounts,
			 const ntfsmac_mw_worker *workers, size_t n_workers,
			 const uint64_t *baseline, size_t n_baseline,
			 bool *stray, bool *kill);

/* True if a file system type name is one of macFUSE's (reconcile.c uses the same list). */
bool ntfsmac_mw_fstype_is_fuse(const char *fstypename);

/*
 * Platform lister: getfsstat(MNT_NOWAIT) into a new array that the caller frees with free().
 * Mounts whose path is longer than the field are skipped. Not implemented off macOS (E_NOTIMPL).
 */
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
