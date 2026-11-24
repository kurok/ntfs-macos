/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Mount-table watcher: the decision, the platform lister and the bounded forced unmount
 * (T-M4-08, T-M4-06 review H1). See mountwatch.h for the rule. Plain POSIX plus getfsstat on
 * macOS; no Disk Arbitration, no CoreFoundation.
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

#include "mountwatch.h"
#include "unmount_impl.h"

#include <errno.h>
#include <grp.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/mount.h>
#include <sys/param.h>
#endif

#define MW_COUNT_MAX 4096u

#if defined(__APPLE__)
_Static_assert(sizeof(fsid_t) == sizeof(uint64_t), "f_fsid is one 64-bit identifier");
#endif

/* ------------------------------------------------------------------ */
/* The decision                                                        */
/* ------------------------------------------------------------------ */

bool ntfsmac_mw_fstype_is_fuse(const char *fstypename)
{
	/* The FSKit backend reports "macfuse" (mount(8): "macfuse, local, ... fskit"). The same
	 * list as reconcile.c: the older kernel backends name themselves osxfuse or fusefs. */
	static const char *const prefixes[] = {"macfuse", "osxfuse", "fusefs"};
	size_t i;

	if (fstypename == NULL)
		return false;
	for (i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++)
		if (strncmp(fstypename, prefixes[i], strlen(prefixes[i])) == 0)
			return true;
	return false;
}

static bool in_baseline(const uint64_t *baseline, size_t n, uint64_t id)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (baseline[i] == id)
			return true;
	return false;
}

size_t ntfsmac_mw_decide(const ntfsmac_mw_mount *mounts, size_t n_mounts,
			 const ntfsmac_mw_worker *workers, size_t n_workers,
			 const uint64_t *baseline, size_t n_baseline,
			 bool *stray, bool *kill)
{
	size_t i, j, count = 0;

	if (n_mounts > MW_COUNT_MAX || n_workers > MW_COUNT_MAX || n_baseline > MW_COUNT_MAX)
		return 0;
	if ((n_mounts > 0 && (mounts == NULL || stray == NULL)) ||
	    (n_workers > 0 && (workers == NULL || kill == NULL)) ||
	    (n_baseline > 0 && baseline == NULL))
		return 0;
	for (i = 0; i < n_mounts; i++)
		stray[i] = false;
	for (j = 0; j < n_workers; j++)
		kill[j] = false;

	for (i = 0; i < n_mounts; i++) {
		bool owner_live = false, own_path = false;

		if (!mounts[i].fuse)
			continue;
		for (j = 0; j < n_workers; j++) {
			if (workers[j].live && workers[j].uid == mounts[i].owner)
				owner_live = true;
			/* Both strings end in a NUL: the mount point is shorter than its field. */
			if (workers[j].mount_point[0] != '\0' &&
			    strncmp(workers[j].mount_point, mounts[i].path,
				    NTFSMAC_MW_MOUNT_POINT_MAX) == 0)
				own_path = true;
		}
		if (!owner_live || own_path || in_baseline(baseline, n_baseline, mounts[i].id))
			continue;
		stray[i] = true;
		count++;
		for (j = 0; j < n_workers; j++)
			if (workers[j].live && workers[j].uid == mounts[i].owner)
				kill[j] = true;
	}
	return count;
}

/* ------------------------------------------------------------------ */
/* Platform lister                                                     */
/* ------------------------------------------------------------------ */

#if defined(__APPLE__)

ntfsmac_dmn_status ntfsmac_mw_list(ntfsmac_mw_mount **out, size_t *n)
{
	struct statfs *fs;
	ntfsmac_mw_mount *v;
	int count, got, i;
	size_t used = 0;

	if (out == NULL || n == NULL)
		return NTFSMAC_DMN_E_INVAL;
	*out = NULL;
	*n = 0;
	/* The table can grow between the two calls; ask for room for 8 more. */
	count = getfsstat(NULL, 0, MNT_NOWAIT);
	if (count < 0)
		return NTFSMAC_DMN_E_IO;
	count += 8;
	fs = calloc((size_t)count, sizeof *fs);
	v = calloc((size_t)count, sizeof *v);
	if (fs == NULL || v == NULL) {
		free(fs);
		free(v);
		return NTFSMAC_DMN_E_IO;
	}
	got = getfsstat(fs, (int)((size_t)count * sizeof *fs), MNT_NOWAIT);
	if (got < 0) {
		free(fs);
		free(v);
		return NTFSMAC_DMN_E_IO;
	}
	for (i = 0; i < got && i < count; i++) {
		size_t len = strnlen(fs[i].f_mntonname, sizeof fs[i].f_mntonname);

		if (len == 0 || len >= sizeof v[used].path)
			continue;
		memcpy(v[used].path, fs[i].f_mntonname, len);
		v[used].path[len] = '\0';
		v[used].owner = fs[i].f_owner;
		v[used].fuse = ntfsmac_mw_fstype_is_fuse(fs[i].f_fstypename);
		memcpy(&v[used].id, &fs[i].f_fsid, sizeof v[used].id);
		used++;
	}
	free(fs);
	if (used == 0) {
		free(v);
		return NTFSMAC_DMN_OK;
	}
	*out = v;
	*n = used;
	return NTFSMAC_DMN_OK;
}

#else /* !__APPLE__ */

ntfsmac_dmn_status ntfsmac_mw_list(ntfsmac_mw_mount **out, size_t *n)
{
	if (out != NULL)
		*out = NULL;
	if (n != NULL)
		*n = 0;
	return NTFSMAC_DMN_E_NOTIMPL;
}

#endif

/* ------------------------------------------------------------------ */
/* Bounded child                                                       */
/* ------------------------------------------------------------------ */

static void mw_sleep_ms(unsigned ms)
{
	struct timespec ts;

	ts.tv_sec = (time_t)(ms / 1000u);
	ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
	while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
		;
}

static uint64_t mw_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Wait for `pid` until `deadline` (ms, monotonic). 1: reaped, `*st` set. 0: still running. */
static int wait_until(pid_t pid, uint64_t deadline, int *st)
{
	for (;;) {
		pid_t r = waitpid(pid, st, WNOHANG);

		if (r == pid)
			return 1;
		if (r < 0 && errno != EINTR)
			return 1; /* not our child (any more): nothing to wait for */
		if (mw_now_ms() >= deadline)
			return 0;
		mw_sleep_ms(10);
	}
}

/*
 * Children that got SIGKILL and were not reaped within the grace second (stuck inside a file
 * system call, in the kernel, where the signal waits). Each is reaped later by pid with
 * waitpid(WNOHANG): at the next bounded run, and from ntfsmac_mw_reap_pending(). Nothing in the
 * daemon waits for -1, so these pids cannot be taken by another reaper and the daemon's other
 * waitpid() calls name their own pid.
 */
#define MW_PENDING_MAX 64

static pthread_mutex_t g_pend_mu = PTHREAD_MUTEX_INITIALIZER;
static pid_t g_pend[MW_PENDING_MAX];
static unsigned g_npend;

void ntfsmac_mw_track_unreaped(pid_t pid)
{
	pthread_mutex_lock(&g_pend_mu);
	if (g_npend < MW_PENDING_MAX)
		g_pend[g_npend++] = pid;
	/* A full list means 64 children stuck at once: the 65th stays unreaped, the daemon goes on. */
	pthread_mutex_unlock(&g_pend_mu);
}

unsigned ntfsmac_mw_reap_pending(void)
{
	unsigned i = 0, left;
	int st;

	pthread_mutex_lock(&g_pend_mu);
	while (i < g_npend) {
		pid_t r = waitpid(g_pend[i], &st, WNOHANG);

		if (r == 0 || (r < 0 && errno == EINTR)) {
			i++;
			continue;
		}
		/* Reaped, or not our child (any more): drop it. */
		g_pend[i] = g_pend[--g_npend];
	}
	left = g_npend;
	pthread_mutex_unlock(&g_pend_mu);
	return left;
}

/*
 * In the child, after fork(). The daemon has threads, and the child holds a copy of every
 * descriptor of the daemon (O_CLOEXEC only acts at exec, and the child never execs): a session's
 * block device descriptor in it keeps the disk busy for as long as the child lives, for example
 * in a forced unmount that blocks. Closing is only close(2), which is async-signal-safe; the
 * limit `max` (the descriptor table size) is read by the parent before the fork.
 * Descriptors 0..2 stay.
 */
static void close_inherited_fds(int max)
{
	int fd;

	for (fd = 3; fd < max; fd++)
		(void)close(fd);
}

/* As ntfsmac_mw_run_bounded(), and `*code` is the exit code of the child (fn's value, 0..255). */
static ntfsmac_dmn_status run_bounded_code(int (*fn)(void *), void *arg, unsigned timeout_ms, int *code)
{
	pid_t pid;
	int st = 0;
	long openmax;

	*code = -1;
	if (fn == NULL)
		return NTFSMAC_DMN_E_INVAL;
	(void)ntfsmac_mw_reap_pending();
	openmax = sysconf(_SC_OPEN_MAX);
	if (openmax < 256 || openmax > 1048576)
		openmax = openmax < 256 ? 256 : 1048576;
	pid = fork();
	if (pid < 0)
		return NTFSMAC_DMN_E_IO;
	if (pid == 0) {
		/* Own group, so that nothing else the daemon does can signal it by mistake. */
		(void)setpgid(0, 0);
		close_inherited_fds((int)openmax);
		_exit(fn(arg) & 0xff);
	}
	if (wait_until(pid, mw_now_ms() + timeout_ms, &st)) {
		if (!WIFEXITED(st))
			return NTFSMAC_DMN_E_IO;
		*code = WEXITSTATUS(st);
		return NTFSMAC_DMN_OK;
	}
	(void)kill(pid, SIGKILL);
	if (!wait_until(pid, mw_now_ms() + 1000u, &st))
		ntfsmac_mw_track_unreaped(pid);
	return NTFSMAC_DMN_E_TIMEOUT;
}

ntfsmac_dmn_status ntfsmac_mw_run_bounded(int (*fn)(void *), void *arg, unsigned timeout_ms)
{
	int code;
	ntfsmac_dmn_status st = run_bounded_code(fn, arg, timeout_ms, &code);

	if (st != NTFSMAC_DMN_OK)
		return st;
	return code == 0 ? NTFSMAC_DMN_OK : NTFSMAC_DMN_E_IO;
}

static int do_force_unmount(void *arg)
{
	const char *path = arg;

#if defined(__APPLE__)
	return unmount(path, MNT_FORCE);
#else
	(void)path;
	return -1;
#endif
}

ntfsmac_dmn_status ntfsmac_mw_force_unmount(const char *path, unsigned timeout_ms)
{
	if (path == NULL || path[0] != '/' || strlen(path) >= NTFSMAC_MW_PATH_MAX)
		return NTFSMAC_DMN_E_INVAL;
	return ntfsmac_mw_run_bounded(do_force_unmount, (void *)path, timeout_ms);
}

/* ------------------------------------------------------------------ */
/* Unmount with a bound, and the unmount of a path the user controls   */
/* ------------------------------------------------------------------ */

typedef struct um_arg {
	const char *path;
	bool force;
	unsigned retries;
	bool drop;
	uid_t uid;
	gid_t gid;
} um_arg;

enum { UM_OK = 0, UM_STATE = 10, UM_DENIED = 11, UM_BUSY = 12, UM_IO = 13, UM_NODROP = 14 };

/* In the child. Only unmount(2), nanosleep and the credential calls: no allocation. */
static int do_unmount(void *p)
{
	const um_arg *a = p;
	unsigned attempt;

#if defined(__APPLE__)
	if (a->drop) {
		gid_t groups[1];

		groups[0] = a->gid;
		if (setgroups(1, groups) != 0 || setgid(a->gid) != 0 || setuid(a->uid) != 0 ||
		    getuid() != a->uid || geteuid() != a->uid)
			return UM_NODROP;
		if (a->uid != 0 && setuid(0) == 0)
			return UM_NODROP;
	}
	for (attempt = 0;; attempt++) {
		int e;

		if (unmount(a->path, a->force ? MNT_FORCE : 0) == 0)
			return UM_OK;
		e = errno;
		if (e == EINTR && attempt < a->retries)
			continue;
		if (e != EBUSY) {
			switch (e) {
			case EINVAL:
			case ENOENT:
				return UM_STATE;
			case EPERM:
			case EACCES:
				return UM_DENIED;
			default:
				return UM_IO;
			}
		}
		if (attempt >= a->retries)
			return UM_BUSY;
		mw_sleep_ms(NTFSMAC_UNMOUNT_RETRY_DELAY_MS);
	}
#else
	(void)a;
	(void)attempt;
	return UM_IO;
#endif
}

static ntfsmac_dmn_status um_map(ntfsmac_dmn_status st, int code)
{
	if (st != NTFSMAC_DMN_OK)
		return st;
	switch (code) {
	case UM_OK:
		return NTFSMAC_DMN_OK;
	case UM_STATE:
		return NTFSMAC_DMN_E_STATE;
	case UM_DENIED:
	case UM_NODROP:
		return NTFSMAC_DMN_E_DENIED;
	case UM_BUSY:
		return NTFSMAC_DMN_E_BUSY;
	default:
		return NTFSMAC_DMN_E_IO;
	}
}

ntfsmac_dmn_status ntfsmac_mw_unmount_bounded(const char *path, bool force, unsigned retries,
					      unsigned timeout_ms, pid_t *busy_pid)
{
	um_arg a;
	int code;
	ntfsmac_dmn_status st;

	if (busy_pid != NULL)
		*busy_pid = 0;
	if (path == NULL || path[0] != '/' || strlen(path) >= NTFSMAC_MW_PATH_MAX)
		return NTFSMAC_DMN_E_INVAL;
	memset(&a, 0, sizeof a);
	a.path = path;
	a.force = force;
	a.retries = retries > NTFSMAC_UNMOUNT_RETRIES_MAX ? NTFSMAC_UNMOUNT_RETRIES_MAX : retries;
	st = um_map(run_bounded_code(do_unmount, &a, timeout_ms, &code), code);
	if (st == NTFSMAC_DMN_E_BUSY && busy_pid != NULL && ntfsmac_unmount_system_hooks.find_busy != NULL)
		*busy_pid = ntfsmac_unmount_system_hooks.find_busy(NULL, path);
	return st;
}

ntfsmac_dmn_status ntfsmac_mw_unmount_as_user(const char *path, uid_t uid, gid_t gid, unsigned timeout_ms)
{
	um_arg a;
	int code;

	if (path == NULL || path[0] != '/' || strlen(path) >= NTFSMAC_MW_PATH_MAX || uid == 0)
		return NTFSMAC_DMN_E_INVAL;
	memset(&a, 0, sizeof a);
	a.path = path;
	a.force = true;
	a.drop = true;
	a.uid = uid;
	a.gid = gid;
	return um_map(run_bounded_code(do_unmount, &a, timeout_ms, &code), code);
}
