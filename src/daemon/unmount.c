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
static pid_t sys_find_busy(void *ctx, const char *path)
{
	size_t mplen = strlen(path);
	pid_t self = getpid();
	pid_t found = 0;
	pid_t *pids;
	int bytes;
	int count;
	int i;

	(void)ctx;
	while (mplen > 1 && path[mplen - 1] == '/')
		mplen--;
	bytes = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);
	if (bytes <= 0)
		return 0;
	pids = malloc((size_t)bytes);
	if (pids == NULL)
		return 0;
	bytes = proc_listpids(PROC_ALL_PIDS, 0, pids, bytes);
	count = bytes > 0 ? bytes / (int)sizeof(pid_t) : 0;

	for (i = 0; i < count && found == 0; i++) {
		pid_t pid = pids[i];
		struct proc_vnodepathinfo vpi;
		struct proc_regionwithpathinfo rpi;
		struct proc_fdinfo *fds;
		int fbytes;
		int j;
		int regions;
		uint64_t addr;

		if (pid <= 0 || pid == self)
			continue;
		if (proc_pidinfo(pid, PROC_PIDVNODEPATHINFO, 0, &vpi, sizeof vpi) == (int)sizeof vpi &&
		    (path_in_mount(vpi.pvi_cdir.vip_path, path, mplen) ||
		     path_in_mount(vpi.pvi_rdir.vip_path, path, mplen))) {
			found = pid;
			break;
		}
		fbytes = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, NULL, 0);
		if (fbytes > 0 && (fds = malloc((size_t)fbytes)) != NULL) {
			fbytes = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds, fbytes);
			for (j = 0; j < fbytes / (int)sizeof *fds && found == 0; j++) {
				struct vnode_fdinfowithpath vfi;

				if (fds[j].proc_fdtype != PROX_FDTYPE_VNODE)
					continue;
				if (proc_pidfdinfo(pid, fds[j].proc_fd, PROC_PIDFDVNODEPATHINFO, &vfi,
						   sizeof vfi) == (int)sizeof vfi &&
				    path_in_mount(vfi.pvip.vip_path, path, mplen))
					found = pid;
			}
			free(fds);
		}
		if (found != 0)
			break;
		/* Mapped files: the case that makes a plain unmount fail (I-MMAP-04). */
		addr = 0;
		for (regions = 0; regions < 4096; regions++) {
			if (proc_pidinfo(pid, PROC_PIDREGIONPATHINFO, addr, &rpi, sizeof rpi) !=
			    (int)sizeof rpi)
				break;
			if (path_in_mount(rpi.prp_vip.vip_path, path, mplen)) {
				found = pid;
				break;
			}
			if (rpi.prp_prinfo.pri_size == 0)
				break;
			addr = rpi.prp_prinfo.pri_address + rpi.prp_prinfo.pri_size;
		}
	}
	free(pids);
	return found;
}

#else

/* No process lookup on other systems (Environment A): the holder is unknown. */
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
