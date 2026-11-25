/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Unmount of a mount point (ARCHITECTURE.md section 4.5 step 1).
 *
 * A plain unmount(2) is retried a few times on EBUSY, because T-M2-04 saw a
 * transient EBUSY in 2 of 4 runs. If it still fails, the result is E_BUSY and
 * the name of one process that holds the volume. MNT_FORCE is used only when
 * the caller says so (`opts->force`: an explicit forced eject, or the worker
 * or device is already gone). It is never `diskutil unmount force` and never
 * a forced Disk Arbitration unmount.
 *
 * The path is only passed to unmount(2) and to the process lookup. This
 * module never stats, opens or reads a path inside the volume.
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

#include "unmount_impl.h"

#include <errno.h>
#include <string.h>
#include <sys/mount.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <libproc.h>
#include <stdlib.h>
#include <sys/proc_info.h>
#endif

/* ------------------------------------------------------------------ */
/* System hooks                                                        */
/* ------------------------------------------------------------------ */

static int sys_unmount(void *ctx, const char *path, bool force)
{
	(void)ctx;
#if defined(__APPLE__)
	return unmount(path, force ? MNT_FORCE : 0);
#else
	return umount2(path, force ? MNT_FORCE : 0);
#endif
}

static void sys_sleep_ms(void *ctx, unsigned ms)
{
	struct timespec ts;

	(void)ctx;
	ts.tv_sec = (time_t)(ms / 1000u);
	ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
	while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
		;
}

#if defined(__APPLE__)

/* True if `p` is `mp` itself or lies below it. */
static bool path_in_mount(const char *p, const char *mp, size_t mplen)
{
	return strncmp(p, mp, mplen) == 0 && (p[mplen] == '\0' || p[mplen] == '/');
}

/*
 * Ask the kernel which process has the volume as working directory or root,
 * an open file, or a mapped file. proc_pidinfo() and proc_pidfdinfo() read
 * the path from the kernel's name cache; they do not call the file system,
 * so a stuck worker does not block them [A: U20, checked in I-DMN-10..13].
 */
/* True if process `pid` has the volume as cwd or root, an open file, or a mapped file. */
static bool pid_holds(pid_t pid, const char *path, size_t mplen)
{
	struct proc_vnodepathinfo vpi;
	struct proc_regionwithpathinfo rpi;
	struct proc_fdinfo *fds;
	bool found = false;
	int fbytes;
	int j;
	int regions;
	uint64_t addr;

	if (proc_pidinfo(pid, PROC_PIDVNODEPATHINFO, 0, &vpi, sizeof vpi) == (int)sizeof vpi &&
	    (path_in_mount(vpi.pvi_cdir.vip_path, path, mplen) ||
	     path_in_mount(vpi.pvi_rdir.vip_path, path, mplen)))
		return true;
	fbytes = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, NULL, 0);
	if (fbytes > 0 && (fds = malloc((size_t)fbytes)) != NULL) {
		fbytes = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds, fbytes);
		for (j = 0; j < fbytes / (int)sizeof *fds && !found; j++) {
			struct vnode_fdinfowithpath vfi;

			if (fds[j].proc_fdtype != PROX_FDTYPE_VNODE)
				continue;
			if (proc_pidfdinfo(pid, fds[j].proc_fd, PROC_PIDFDVNODEPATHINFO, &vfi, sizeof vfi) ==
				    (int)sizeof vfi &&
			    path_in_mount(vfi.pvip.vip_path, path, mplen))
				found = true;
		}
		free(fds);
	}
	if (found)
		return true;
	/* Mapped files: the case that makes a plain unmount fail (I-MMAP-04). */
	addr = 0;
	for (regions = 0; regions < 4096; regions++) {
		if (proc_pidinfo(pid, PROC_PIDREGIONPATHINFO, addr, &rpi, sizeof rpi) != (int)sizeof rpi)
			break;
		if (path_in_mount(rpi.prp_vip.vip_path, path, mplen))
			return true;
		if (rpi.prp_prinfo.pri_size == 0)
			break;
		addr = rpi.prp_prinfo.pri_address + rpi.prp_prinfo.pri_size;
	}
	return false;
}

/*
 * Ask the kernel which processes have the volume as working directory or root,
 * an open file, or a mapped file. proc_pidinfo() and proc_pidfdinfo() read
 * the path from the kernel's name cache; they do not call the file system,
 * so a stuck worker does not block them [A: U20, checked in I-DMN-10..13].
 * Every holder is collected (up to `cap`), not only the first.
 */
size_t ntfsmac_unmount_holders(const char *path, pid_t *pids, size_t cap)
{
	size_t mplen = strlen(path);
	pid_t self = getpid();
	pid_t *all;
	size_t n = 0;
	int bytes;
	int count;
	int i;

	while (mplen > 1 && path[mplen - 1] == '/')
		mplen--;
	bytes = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);
	if (bytes <= 0)
		return 0;
	all = malloc((size_t)bytes);
	if (all == NULL)
		return 0;
	bytes = proc_listpids(PROC_ALL_PIDS, 0, all, bytes);
	count = bytes > 0 ? bytes / (int)sizeof(pid_t) : 0;
	for (i = 0; i < count && n < cap; i++) {
		if (all[i] <= 0 || all[i] == self)
			continue;
		if (pid_holds(all[i], path, mplen))
			pids[n++] = all[i];
	}
	free(all);
	return n;
}

static pid_t sys_find_busy(void *ctx, const char *path)
{
	pid_t first[1];

	(void)ctx;
	return ntfsmac_unmount_holders(path, first, 1) == 1 ? first[0] : 0;
}

#else

/* No process lookup on other systems (Environment A): the holders are unknown. */
size_t ntfsmac_unmount_holders(const char *path, pid_t *pids, size_t cap)
{
	(void)path;
	(void)pids;
	(void)cap;
	return 0;
}

static pid_t sys_find_busy(void *ctx, const char *path)
{
	(void)ctx;
	(void)path;
	return 0;
}

#endif

const ntfsmac_unmount_hooks ntfsmac_unmount_system_hooks = {
	sys_unmount, sys_find_busy, sys_sleep_ms, NULL
};

/* ------------------------------------------------------------------ */
/* The retry logic                                                     */
/* ------------------------------------------------------------------ */

/* Absolute, no NUL (by construction of a C string), no `.` or `..` component. */
static bool path_ok(const char *p)
{
	const char *c;

	if (p == NULL || p[0] != '/' || strlen(p) >= 1024)
		return false;
	for (c = p + 1;; ) {
		const char *e = strchr(c, '/');
		size_t n = e != NULL ? (size_t)(e - c) : strlen(c);

		if ((n == 1 && c[0] == '.') || (n == 2 && c[0] == '.' && c[1] == '.'))
			return false;
		if (e == NULL)
			break;
		c = e + 1;
	}
	return true;
}

bool ntfsmac_unmount_holder_is_indexer(const char *name)
{
	if (name == NULL)
		return false;
	return strcmp(name, "mds") == 0 || strncmp(name, "mds_", 4) == 0 || strncmp(name, "mdworker", 8) == 0 ||
	       strncmp(name, "QuickLook", 9) == 0 || strncmp(name, "quicklook", 9) == 0;
}

bool ntfsmac_unmount_pid_is_indexer(pid_t pid)
{
#if defined(__APPLE__)
	char name[64];

	if (pid <= 0 || proc_name(pid, name, sizeof name) <= 0)
		return false;
	return ntfsmac_unmount_holder_is_indexer(name);
#else
	(void)pid;
	return false;
#endif
}

ntfsmac_umw_action ntfsmac_unmount_wait_decide(const ntfsmac_umw_in *in)
{
	/* Force wins, a daemon that stops, a volume that is gone: the wait ends, nothing is reported as busy. */
	if (in->force || in->stopping || in->gone)
		return NTFSMAC_UMW_ABORT;
	/* No holder found, or at least one that is not an indexer: that one is reported at once. */
	if (in->nholders == 0 || in->nindexers < in->nholders)
		return NTFSMAC_UMW_BUSY;
	/* The bound is wall clock (a monotonic deadline), not a sum of sleeps. */
	if (in->now_ms >= in->deadline_ms)
		return NTFSMAC_UMW_BUSY;
	return NTFSMAC_UMW_WAIT;
}

size_t ntfsmac_unmount_classify(const pid_t *pids, size_t n, bool (*is_indexer)(pid_t), pid_t *report,
				size_t *nindexers)
{
	size_t i, ind = 0;

	*report = n > 0 ? pids[0] : 0;
	for (i = 0; i < n; i++) {
		if (is_indexer(pids[i]))
			ind++;
	}
	/* The process to name: the first that is not an indexer, else the first holder. */
	for (i = 0; i < n; i++) {
		if (!is_indexer(pids[i])) {
			*report = pids[i];
			break;
		}
	}
	*nindexers = ind;
	return n;
}

/* One look: is the wait to go on? Fills *report. */
static bool umwait_look(ntfsmac_umwait *w, const ntfsmac_umwait_ops *o, pid_t first_busy, bool starting,
			pid_t *report)
{
	pid_t pids[64];
	size_t n, ind = 0, i;
	ntfsmac_umw_in in;

	*report = first_busy;
	/* The pid the unmount call found: when it is no indexer, the answer is busy and the full scan is skipped. */
	if (first_busy > 0 && !o->is_indexer(o->ctx, first_busy))
		return false;
	n = o->holders(o->ctx, pids, sizeof pids / sizeof pids[0]);
	if (n == 0 && first_busy > 0) {
		/* the indexer that the call found is gone from the scan: one more try, not "busy" */
		pids[0] = first_busy;
		n = 1;
	}
	if (n > 0)
		*report = pids[0];
	for (i = 0; i < n; i++) {
		if (o->is_indexer(o->ctx, pids[i]))
			ind++;
	}
	for (i = 0; i < n; i++) {
		if (!o->is_indexer(o->ctx, pids[i])) {
			*report = pids[i];
			break;
		}
	}
	memset(&in, 0, sizeof in);
	in.stopping = o->stopping(o->ctx);
	in.nholders = n;
	in.nindexers = ind;
	in.now_ms = o->now_ms(o->ctx);
	if (starting)
		w->deadline_ms = in.now_ms + o->wait_ms;
	in.deadline_ms = w->deadline_ms;
	return ntfsmac_unmount_wait_decide(&in) == NTFSMAC_UMW_WAIT;
}

bool ntfsmac_umwait_start(ntfsmac_umwait *w, const ntfsmac_umwait_ops *o, pid_t first_busy, pid_t *report)
{
	if (!umwait_look(w, o, first_busy, true, report))
		return false;
	w->waiting = true;
	w->gen++;
	o->schedule(o->ctx, w->gen);
	return true;
}

void ntfsmac_umwait_cancel(ntfsmac_umwait *w)
{
	if (w->waiting)
		w->gen++;
	w->waiting = false;
}

ntfsmac_umw_retry ntfsmac_umwait_retry(ntfsmac_umwait *w, const ntfsmac_umwait_ops *o, unsigned gen,
				       bool volume_ok)
{
	if (!w->waiting || gen != w->gen)
		return NTFSMAC_UMR_STALE;
	if (o->stopping(o->ctx) || !volume_ok) {
		ntfsmac_umwait_cancel(w);
		return NTFSMAC_UMR_STOP;
	}
	return NTFSMAC_UMR_TRY;
}

ntfsmac_umw_after ntfsmac_umwait_after(ntfsmac_umwait *w, const ntfsmac_umwait_ops *o, ntfsmac_dmn_status st,
				       pid_t first_busy, pid_t *report)
{
	*report = first_busy;
	if (!w->waiting)
		return NTFSMAC_UMA_DONE; /* cancelled while the unmount call ran */
	if (st == NTFSMAC_DMN_OK) {
		w->waiting = false;
		return NTFSMAC_UMA_DONE;
	}
	if (st != NTFSMAC_DMN_E_BUSY) {
		w->waiting = false;
		return NTFSMAC_UMA_FAILED;
	}
	if (umwait_look(w, o, first_busy, false, report)) {
		o->schedule(o->ctx, w->gen);
		return NTFSMAC_UMA_AGAIN;
	}
	w->waiting = false;
	return NTFSMAC_UMA_BUSY;
}

bool ntfsmac_unmount_seen(bool mount_gone, bool daemon_requested, bool da_volume_gone)
{
	return mount_gone && (daemon_requested || da_volume_gone);
}

ntfsmac_dmn_status ntfsmac_unmount_run(const char *mount_point, const ntfsmac_unmount_opts *opts,
				       pid_t *busy_pid, const ntfsmac_unmount_hooks *hooks)
{
	unsigned retries;
	unsigned attempt;

	if (busy_pid != NULL)
		*busy_pid = 0;
	if (!path_ok(mount_point) || opts == NULL || hooks == NULL || hooks->unmount == NULL)
		return NTFSMAC_DMN_E_INVAL;
	retries = opts->retries > NTFSMAC_UNMOUNT_RETRIES_MAX ? NTFSMAC_UNMOUNT_RETRIES_MAX
							       : opts->retries;

	for (attempt = 0;; attempt++) {
		int e;

		if (hooks->unmount(hooks->ctx, mount_point, opts->force) == 0)
			return NTFSMAC_DMN_OK;
		e = errno;
		if (e == EINTR && attempt < retries) {
			continue;
		}
		if (e != EBUSY) {
			errno = e;
			switch (e) {
			case EINVAL: /* not a mount point (any more) */
			case ENOENT:
				return NTFSMAC_DMN_E_STATE;
			case EPERM:
			case EACCES:
				return NTFSMAC_DMN_E_DENIED;
			default:
				return NTFSMAC_DMN_E_IO;
			}
		}
		if (attempt >= retries)
			break;
		if (hooks->sleep_ms != NULL)
			hooks->sleep_ms(hooks->ctx, NTFSMAC_UNMOUNT_RETRY_DELAY_MS);
	}
	if (busy_pid != NULL && hooks->find_busy != NULL)
		*busy_pid = hooks->find_busy(hooks->ctx, mount_point);
	errno = EBUSY;
	return NTFSMAC_DMN_E_BUSY;
}

ntfsmac_dmn_status ntfsmac_unmount_path(const char *mount_point,
					const ntfsmac_unmount_opts *opts,
					pid_t *busy_pid)
{
	return ntfsmac_unmount_run(mount_point, opts, busy_pid, &ntfsmac_unmount_system_hooks);
}
