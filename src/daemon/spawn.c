/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Spawn and supervision of the probe, volmark and the worker (T-M4-01).
 * ARCHITECTURE.md sections 3.1, 3.3, 3.6, 4.1 (steps 4, 7, 8), 5 and 6.
 *
 * Plain POSIX plus small macOS parts (getfsstat, the spawn flag
 * POSIX_SPAWN_CLOEXEC_DEFAULT and libproc for the process tree and the open
 * descriptors of a process; /proc on Linux). No Disk Arbitration and no
 * CoreFoundation.
 *
 * How a child starts: the daemon forks, the child drops to the target user,
 * becomes the leader of its own process group, and then replaces itself with
 * the program. On macOS the replacement is posix_spawn with
 * POSIX_SPAWN_SETEXEC and POSIX_SPAWN_CLOEXEC_DEFAULT, so that only the
 * descriptors 0 to 2 and the ones named by the caller survive. Elsewhere
 * (Linux, for tests) the child closes every other descriptor itself. A
 * pipe that is closed by a successful exec tells the daemon whether the
 * start worked, so a missing program is an error and not an exit code.
 *
 * On macOS the program is not started directly but through sandbox-exec,
 * which applies a profile of integration/sandbox/ and then starts the program
 * (T-M4-06; section "Sandbox" below).
 *
 * A fork in a thread-safe daemon races with the creation of descriptors in
 * other threads, so the descriptor creation and the fork are one critical
 * section.
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

#include "spawn_config.h"
#include "spawn_holder.h"
#include "usermap_file.h"
#include "ntfsmac/log.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__)
#include <libproc.h>
#include <mach-o/dyld.h>
#include <sys/mount.h>
#include <sys/proc_info.h>
#include <sys/resource.h>
#endif
#include <grp.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ntfsmac/ctl_proto.h"
#include "ntfsmac/options.h"
#include "ntfsmac/probe.h"

extern char **environ;

#define EXEC_STATUS_WAIT_MS 10000u /* time for fork and exec to report */
#define REAP_WAIT_MS 5000u         /* time for a SIGKILLed child to be reaped */
#define DEFAULT_POLL_MS 50u
#define MAX_FD 1023
#define SPAWN_FD_MAX 65536

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static ntfsmac_spawn_config g_cfg;
static bool g_cfg_set;

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

static ntfsmac_dmn_status mount_lookup_system(const char *mount_point, bool *found,
					      bool *read_only);

void ntfsmac_spawn_config_default(ntfsmac_spawn_config *cfg)
{
	struct passwd *pw;

	memset(cfg, 0, sizeof(*cfg));
	cfg->probe_path = NTFSMAC_PATH_PROBE;
	cfg->volmark_path = NTFSMAC_PATH_VOLMARK;
	cfg->worker_path = NTFSMAC_PATH_WORKER;
	/* The user `nobody` is -2 on macOS; the fallback is the same number. */
	cfg->unpriv_uid = (uid_t)-2;
	cfg->unpriv_gid = (gid_t)-2;
	pw = getpwnam("nobody");
	if (pw != NULL) {
		cfg->unpriv_uid = pw->pw_uid;
		cfg->unpriv_gid = pw->pw_gid;
	}
#if defined(NTFSMAC_TEST_SEAMS) && defined(__APPLE__)
	cfg->sandbox_dir = NTFSMAC_PATH_SANDBOX_DIR;
#endif
	cfg->child_stderr_fd = -1;
	cfg->mount_poll_ms = DEFAULT_POLL_MS;
	cfg->volmark_write_wait_s = NTFSMAC_VOLMARK_WRITE_WAIT_S;
	cfg->mount_lookup = mount_lookup_system;
}

void ntfsmac_spawn_config_set(const ntfsmac_spawn_config *cfg)
{
	pthread_mutex_lock(&g_lock);
	g_cfg = *cfg;
	g_cfg_set = true;
	pthread_mutex_unlock(&g_lock);
}

/* A private copy of the settings, with the defaults filled in. */
static void config_get(ntfsmac_spawn_config *out)
{
	pthread_mutex_lock(&g_lock);
	if (!g_cfg_set) {
		ntfsmac_spawn_config_default(&g_cfg);
		g_cfg_set = true;
	}
	*out = g_cfg;
	pthread_mutex_unlock(&g_lock);
	if (out->mount_poll_ms == 0)
		out->mount_poll_ms = DEFAULT_POLL_MS;
	if (out->volmark_write_wait_s == 0)
		out->volmark_write_wait_s = NTFSMAC_VOLMARK_WRITE_WAIT_S;
	if (out->mount_lookup == NULL)
		out->mount_lookup = mount_lookup_system;
#if defined(NTFSMAC_TEST_SEAMS)
	if (out->sandbox_exec_path == NULL)
		out->sandbox_exec_path = NTFSMAC_PATH_SANDBOX_EXEC;
#endif
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void sleep_ms(unsigned ms)
{
	struct timespec ts;

	ts.tv_sec = (time_t)(ms / 1000u);
	ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
	nanosleep(&ts, NULL);
}

static void close_fd(int *fd)
{
	if (*fd >= 0) {
		int err = errno; /* error paths report errno: a close must not replace it */

		close(*fd);
		*fd = -1;
		errno = err;
	}
}

/* Move a descriptor to 3 or above, so that it is never a standard stream. */
static int fd_high(int fd)
{
	int n;

	if (fd < 0 || fd >= 3)
		return fd;
	n = fcntl(fd, F_DUPFD_CLOEXEC, 3);
	if (n < 0) {
		int err = errno;

		close(fd);
		errno = err; /* the caller reports errno: a close must not replace it */
		return -1;
	}
	close(fd);
	return n;
}

/* Called with g_lock held, so that no fork sees a pipe without FD_CLOEXEC. */
static int pipe_cloexec(int p[2])
{
	if (pipe(p) != 0)
		return -1;
	if (fcntl(p[0], F_SETFD, FD_CLOEXEC) != 0 || fcntl(p[1], F_SETFD, FD_CLOEXEC) != 0) {
		int err = errno;

		close(p[0]);
		close(p[1]);
		errno = err;
		return -1;
	}
	p[0] = fd_high(p[0]);
	p[1] = fd_high(p[1]);
	if (p[0] < 0 || p[1] < 0) {
		close_fd(&p[0]);
		close_fd(&p[1]);
		return -1;
	}
	return 0;
}

/* Wait for `fd` to be readable or hung up for at most `ms`. 1 ready, 0 timeout, -1 error. */
static int wait_readable(int fd, uint64_t ms)
{
	struct pollfd p;
	int r;

	p.fd = fd;
	p.events = POLLIN;
	p.revents = 0;
	if (ms > 1000u)
		ms = 1000u; /* callers loop; keeps a deadline check at least every second */
	r = poll(&p, 1, (int)ms);
	if (r < 0 && errno == EINTR)
		return 0;
	return r;
}

/*
 * Has `pid` exited or been killed? Looks without reaping, so that the leader
 * stays a zombie and the group id stays reserved while the caller kills the
 * group. A stopped child is not an exit.
 */
static bool child_has_exited(pid_t pid)
{
	siginfo_t si;

	memset(&si, 0, sizeof(si));
	if (waitid(P_PID, (id_t)pid, &si, WEXITED | WNOHANG | WNOWAIT) != 0)
		return false;
	return si.si_pid == pid
	       && (si.si_code == CLD_EXITED || si.si_code == CLD_KILLED || si.si_code == CLD_DUMPED);
}

/* Reap `pid` within `ms`. True if it was reaped; `*status` is the wait status. */
static bool reap_bounded(pid_t pid, int *status, unsigned ms)
{
	uint64_t end = now_ms() + ms;

	for (;;) {
		pid_t r = waitpid(pid, status, WNOHANG);

		if (r == pid)
			return true;
		if (r < 0 && errno != EINTR)
			return false; /* not our child, or already reaped */
		if (now_ms() >= end)
			return false;
		sleep_ms(5);
	}
}

/* ------------------------------------------------------------------ */
/* Process tree and open descriptors (T-M4-06, audit M1)               */
/* ------------------------------------------------------------------ */

#define TREE_MAX 256    /* descendants of one child that are tracked */
#define TREE_ROUNDS 16  /* rounds of the search for new descendants */
#define HOLD_ROUNDS 25  /* looks for a holder of the device, 20 ms apart */
#define HOLD_KILLED_MAX 32

/* Number of the device, as the descriptor the child was given shows it. */
typedef struct dev_ident {
	dev_t dev;
	ino_t ino;
} dev_ident;

#if defined(__APPLE__)

/* Pids whose parent is `ppid`: the number found (at most `cap`), or -1. */
static int proc_children(pid_t ppid, pid_t *out, int cap)
{
	int n = proc_listchildpids(ppid, out, cap * (int)sizeof(pid_t));

	return n < 0 ? -1 : (n > cap ? cap : n);
}

/* The parent pid of `pid`, or -1. */
static pid_t proc_parent_of(pid_t pid)
{
	struct proc_bsdinfo bi;

	if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, (int)sizeof(bi)) != (int)sizeof(bi))
		return -1;
	return (pid_t)bi.pbi_ppid;
}

/* The executable path of `pid` into `buf`; false if it cannot be read. */
static bool proc_exe_of(pid_t pid, char *buf, size_t cap)
{
	return cap > 0 && proc_pidpath(pid, buf, (uint32_t)cap) > 0;
}

/* The pids of the processes owned by `uid`, in a malloc'd array; NULL on error. */
static pid_t *proc_pids_of_uid(uid_t uid, int *count)
{
	int bytes = proc_listpids(PROC_UID_ONLY, (uint32_t)uid, NULL, 0);
	pid_t *pids;

	*count = 0;
	if (bytes < 0)
		return NULL;
	if (bytes == 0) /* no process of that user: an empty list, not an error */
		return malloc(sizeof(pid_t));
	bytes += 64 * (int)sizeof(pid_t); /* room for processes that start meanwhile */
	pids = malloc((size_t)bytes);
	if (pids == NULL)
		return NULL;
	bytes = proc_listpids(PROC_UID_ONLY, (uint32_t)uid, pids, bytes);
	if (bytes < 0) {
		free(pids);
		return NULL;
	}
	*count = bytes / (int)sizeof(pid_t); /* 0: no process of that user */
	return pids;
}

/* Does `pid` have a file descriptor on the vnode `id`? A process that cannot be read holds nothing. */
static bool proc_holds_device(pid_t pid, const dev_ident *id)
{
	struct proc_fdinfo *fds;
	int bytes, n, i;
	bool held = false;

	bytes = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, NULL, 0);
	if (bytes <= 0)
		return false;
	bytes += 16 * (int)sizeof(*fds);
	fds = malloc((size_t)bytes);
	if (fds == NULL)
		return false;
	bytes = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds, bytes);
	n = bytes > 0 ? bytes / (int)sizeof(*fds) : 0;
	for (i = 0; i < n && !held; i++) {
		struct vnode_fdinfo vi;

		if (fds[i].proc_fdtype != PROX_FDTYPE_VNODE)
			continue;
		if (proc_pidfdinfo(pid, fds[i].proc_fd, PROC_PIDFDVNODEINFO, &vi, (int)sizeof(vi))
		    != (int)sizeof(vi))
			continue;
		held = vi.pvi.vi_stat.vst_dev == (uint32_t)id->dev
		       && vi.pvi.vi_stat.vst_ino == (uint64_t)id->ino;
	}
	free(fds);
	return held;
}

#else /* Linux: /proc, for tests */

static bool proc_numeric(const char *name)
{
	const char *c;

	for (c = name; *c != '\0'; c++)
		if (*c < '0' || *c > '9')
			return false;
	return name[0] != '\0';
}

/* The parent pid in /proc/<pid>/stat: "pid (comm) S ppid ..."; the name may hold ')' and spaces. */
static pid_t proc_parent_of(pid_t pid)
{
	char path[64], line[512];
	FILE *f;
	const char *p;
	long ppid;

	snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
	f = fopen(path, "r");
	if (f == NULL)
		return -1;
	p = fgets(line, sizeof(line), f);
	fclose(f);
	if (p == NULL || (p = strrchr(line, ')')) == NULL)
		return -1;
	if (sscanf(p + 1, " %*c %ld", &ppid) != 1)
		return -1;
	return (pid_t)ppid;
}

/* The executable path of `pid` into `buf`; false if it cannot be read. */
static bool proc_exe_of(pid_t pid, char *buf, size_t cap)
{
	char link[64];
	ssize_t n;

	snprintf(link, sizeof(link), "/proc/%d/exe", (int)pid);
	n = cap > 1 ? readlink(link, buf, cap - 1) : -1;
	if (n <= 0)
		return false;
	buf[n] = '\0';
	return true;
}

static int proc_children(pid_t ppid, pid_t *out, int cap)
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	int n = 0;

	if (d == NULL)
		return -1;
	while (n < cap && (e = readdir(d)) != NULL) {
		if (proc_numeric(e->d_name) && proc_parent_of((pid_t)atoi(e->d_name)) == ppid)
			out[n++] = (pid_t)atoi(e->d_name);
	}
	closedir(d);
	return n;
}

static pid_t *proc_pids_of_uid(uid_t uid, int *count)
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	pid_t *pids = NULL;
	int n = 0, cap = 0;

	*count = 0;
	if (d == NULL)
		return NULL;
	while ((e = readdir(d)) != NULL) {
		char path[64];
		struct stat st;

		if (!proc_numeric(e->d_name))
			continue;
		if (snprintf(path, sizeof(path), "/proc/%s", e->d_name) >= (int)sizeof(path))
			continue; /* not a pid: too long for one */
		if (stat(path, &st) != 0 || st.st_uid != uid)
			continue;
		if (n == cap) {
			pid_t *g = realloc(pids, (size_t)(cap + 64) * sizeof(*pids));

			if (g == NULL) {
				free(pids);
				closedir(d);
				return NULL;
			}
			pids = g;
			cap += 64;
		}
		pids[n++] = (pid_t)atoi(e->d_name);
	}
	closedir(d);
	if (pids == NULL) /* none: an empty list is not an error */
		pids = malloc(sizeof(*pids));
	*count = n;
	return pids;
}

static bool proc_holds_device(pid_t pid, const dev_ident *id)
{
	char dir[64], path[128];
	DIR *d;
	struct dirent *e;
	bool held = false;

	snprintf(dir, sizeof(dir), "/proc/%d/fd", (int)pid);
	d = opendir(dir);
	if (d == NULL)
		return false;
	while (!held && (e = readdir(d)) != NULL) {
		struct stat st;

		if (!proc_numeric(e->d_name))
			continue;
		if (snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path))
			continue; /* not a descriptor number: too long for one */
		held = stat(path, &st) == 0 && st.st_dev == id->dev && st.st_ino == id->ino;
	}
	closedir(d);
	return held;
}

#endif

static bool pid_listed(const pid_t *list, unsigned n, pid_t pid)
{
	unsigned i;

	for (i = 0; i < n; i++)
		if (list[i] == pid)
			return true;
	return false;
}

/*
 * SIGKILL to `root`, its process group and all its descendants, whatever their group. The
 * descendants are found by walking the tree from `root` with proc_listchildpids(). Each process
 * is stopped when it is found, so it cannot fork again before the kill, and the walk repeats
 * until a round finds nobody new (or TREE_ROUNDS rounds). A descendant whose parent already
 * died is not in the tree any more; for those see device_free() below.
 */
static void kill_tree(pid_t root)
{
	pid_t seen[TREE_MAX];
	unsigned nseen = 0, round, i;

	(void)kill(root, SIGSTOP);
	for (round = 0; round < TREE_ROUNDS; round++) {
		unsigned before = nseen, parent;

		for (parent = 0; parent <= before; parent++) {
			pid_t kids[TREE_MAX];
			int nk = proc_children(parent == 0 ? root : seen[parent - 1], kids, TREE_MAX), k;

			for (k = 0; k < nk; k++) {
				if (kids[k] <= 1 || kids[k] == getpid() || pid_listed(seen, nseen, kids[k])
				    || nseen >= TREE_MAX)
					continue;
				seen[nseen++] = kids[k];
				(void)kill(kids[k], SIGSTOP);
			}
		}
		if (nseen == before)
			break;
	}
	(void)kill(-root, SIGKILL);
	(void)kill(root, SIGKILL);
	for (i = 0; i < nseen; i++)
		(void)kill(seen[i], SIGKILL);
}

/*
 * SIGKILL to the whole group of `pid` (the child is the group leader) and to
 * the child itself, in case the group could not be set up, and to every descendant of the child
 * (kill_tree). Never kills pid 0 or 1, nor a negative number: `kill(-1)` would hit every process.
 */
static void kill_group_of(pid_t pid)
{
	if (pid <= 1)
		return;
	kill_tree(pid);
}

/* Fill `h` for `pid`: its parent chain and its executable path (stored in `exe`). */
static void holder_describe(pid_t pid, ntfsmac_holder *h, char *exe, size_t exe_cap)
{
	pid_t p = pid;

	memset(h, 0, sizeof(*h));
	h->pid = pid;
	while (h->nancestors < NTFSMAC_HOLDER_ANC_MAX) {
		p = proc_parent_of(p);
		if (p <= 1)
			break;
		h->ancestors[h->nancestors++] = p;
	}
	h->exe = proc_exe_of(pid, exe, exe_cap) ? exe : NULL;
	if (h->exe != NULL) {
		struct stat st;

		if (stat(h->exe, &st) == 0) {
			h->exe_known = true;
			h->exe_id.dev = st.st_dev;
			h->exe_id.ino = st.st_ino;
		}
	}
}

/* Add the identity of the file `path` to `ids`, if it can be stat'ed. */
static void exe_id_add(ntfsmac_exe_id *ids, unsigned *n, const char *path)
{
	struct stat st;

	if (path != NULL && stat(path, &st) == 0) {
		ids[*n].dev = st.st_dev;
		ids[*n].ino = st.st_ino;
		(*n)++;
	}
}

/*
 * No process of `owner` may hold the device `id` after the worker is gone, and only leftovers of
 * the worker are killed. A holder is killed only if ntfsmac_holder_is_worker_leftover() says so:
 * its executable file (device and inode of stat(proc_pidpath), not the name) is the worker binary
 * cfg.worker_path or the macFUSE helper the sandbox lets the worker exec; or, while the worker is
 * alive (`worker_pid` > 1; callers pass -1 once it is reaped, its descendants are reparented and
 * the pid could be reused), the worker or a descendant of it. The match of the device is only the
 * vnode's number and libproc has no access mode of a descriptor, so every other holder (an lsof,
 * diskutil or backup agent that merely opened the device, or a process whose executable cannot
 * be read) is NOT killed: it is logged once at warning level with its pid and executable path.
 * It still counts as holding the device, because the sweep cannot show that it is not a worker
 * leftover: the table is read again until a round finds no holder (HOLD_ROUNDS looks, 20 ms
 * apart, so a reader that exits meanwhile is fine), and a holder still there gives
 * NTFSMAC_DMN_E_BUSY. The caller must then not report the session clean, so the next attach is
 * read-only. A worker binary replaced by an upgrade while a leftover of the old one runs no
 * longer matches by inode: the leftover is kept, warned about and gives BUSY (fail closed). The
 * daemon itself is never touched, and only the processes of `owner` are looked at.
 */
static ntfsmac_dmn_status device_free(uid_t owner, const dev_ident *id, pid_t worker_pid,
				      unsigned *killed)
{
	pid_t done[HOLD_KILLED_MAX], warned[HOLD_KILLED_MAX];
	unsigned ndone = 0, nwarned = 0, round, nknown = 0;
	ntfsmac_exe_id known[2];
	ntfsmac_spawn_config cfg;

	config_get(&cfg);
	exe_id_add(known, &nknown, cfg.worker_path);
	exe_id_add(known, &nknown, NTFSMAC_PATH_MACFUSE_HELPER);
	if (killed != NULL)
		*killed = 0;
	for (round = 0; round < HOLD_ROUNDS; round++) {
		int n, i;
		pid_t *pids = proc_pids_of_uid(owner, &n);
		unsigned held = 0;

		if (pids == NULL)
			return NTFSMAC_DMN_E_IO;
		for (i = 0; i < n; i++) {
			ntfsmac_holder h;
			char exe[PATH_MAX];

			if (pids[i] <= 1 || pids[i] == getpid() || !proc_holds_device(pids[i], id))
				continue;
			holder_describe(pids[i], &h, exe, sizeof(exe));
			held++;
			if (!ntfsmac_holder_is_worker_leftover(&h, worker_pid, known, nknown)) {
				if (nwarned < HOLD_KILLED_MAX && !pid_listed(warned, nwarned, pids[i])) {
					warned[nwarned++] = pids[i];
					NTFSMAC_LOGW("device holder not killed (not the worker or its leftover): "
						     "pid %d, executable %s", (int)pids[i],
						     h.exe != NULL ? h.exe : "(unreadable)");
				}
				continue;
			}
			(void)kill(pids[i], SIGKILL);
			if (ndone < HOLD_KILLED_MAX && !pid_listed(done, ndone, pids[i]))
				done[ndone++] = pids[i];
		}
		free(pids);
		if (held == 0) {
			if (killed != NULL)
				*killed = ndone;
			return NTFSMAC_DMN_OK;
		}
		sleep_ms(20);
	}
	if (killed != NULL)
		*killed = ndone;
	return NTFSMAC_DMN_E_BUSY;
}

static int exit_code_of(int status)
{
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return 255;
}

/* ------------------------------------------------------------------ */
/* Child start                                                         */
/* ------------------------------------------------------------------ */

typedef struct launch_spec {
	const char *path;
	char *const *argv;
	char *const *envp;
	const int *keep;   /* descriptors (3 or above) inherited at their own number */
	size_t nkeep;
	bool capture;      /* stdout goes to a pipe; its read end is returned */
	int err_fd;        /* stderr; -1 is /dev/null */
	bool drop;         /* change to `uid` and `gid` before the exec */
	uid_t uid;
	gid_t gid;
	bool user_session; /* with `drop`: start it in the bootstrap context of `uid` (see user_session_wrap) */
} launch_spec;

/* In the child: become `uid`, `gid` and prove that root cannot be regained. */
static int drop_privileges(uid_t uid, gid_t gid)
{
	gid_t groups[1];

	groups[0] = gid;
	if (setgroups(1, groups) != 0)
		return -1;
	if (setgid(gid) != 0 || setuid(uid) != 0)
		return -1;
	if (getuid() != uid || geteuid() != uid || getgid() != gid || getegid() != gid)
		return -1;
	if (uid != 0 && setuid(0) == 0)
		return -1;
	return 0;
}

/*
 * Disk I/O of this process becomes passive (macOS only; elsewhere a no-op that returns 0). Called
 * by the daemon's main() before any thread starts, so the daemon's own I/O and every child it
 * starts inherit it, and again in each child of launch() and in the --exec-as step.
 *
 * The daemon runs as an Interactive launchd job (I/O tier 0), and its children inherit that tier.
 * A tier-0 I/O to a disk image node counted as activity that throttled diskimages-helper, which
 * serves the image with tier-1 passive I/O on the disk that holds the image file: in the
 * measurement the helper then served one request every 5.5 to 6 ms, and the worker's writes and
 * reads on the image node were 28 to 89 times slower than the node itself (docs/spikes/M5-perf.md).
 * A passive I/O keeps its own tier but is not counted as activity, so it delays no one, the helper
 * included. Nothing else changes: the order and the completion of the I/O, fsync and the device
 * flush of ADR-022 are the same. One system call, safe between fork and exec.
 * Returns 0, or -1 with errno set.
 */
int ntfsmac_spawn_io_passive(void)
{
#if defined(__APPLE__)
	return setiopolicy_np(IOPOL_TYPE_DISK, IOPOL_SCOPE_PROCESS, IOPOL_PASSIVE);
#else
	return 0;
#endif
}

/* In the child: signals back to their defaults and an empty mask. */
static void reset_signals(void)
{
	sigset_t none;
	int s;

	for (s = 1; s < 32; s++) {
		if (s != SIGKILL && s != SIGSTOP)
			(void)signal(s, SIG_DFL);
	}
	sigemptyset(&none);
	(void)sigprocmask(SIG_SETMASK, &none, NULL);
}

/*
 * Does this process have the right to start a child as `uid`/`gid`? Root does
 * anything; others only the user they already are (tests, a daemon run by
 * hand). `drop` is set when a change of user is needed.
 */
static bool privileges_ok(uid_t uid, gid_t gid, bool *drop)
{
	if (geteuid() == 0) {
		*drop = !(uid == 0 && gid == 0 && getuid() == 0 && getgid() == 0);
		return true;
	}
	if (uid == geteuid() && uid == getuid() && gid == getegid() && gid == getgid()) {
		*drop = false;
		return true;
	}
	return false;
}

/* ------------------------------------------------------------------ */
/* Sandbox                                                             */
/* ------------------------------------------------------------------ */

#define SANDBOX_ARGV_MAX 40
#define SANDBOX_STR_MAX 1100
#define SANDBOX_PARAM_MAX 6
#define SANDBOX_PROFILE_MAX 32768 /* the profiles are about 6 KiB; a larger file is refused */

/*
 * The command line of a sandboxed child (T-M4-06):
 *
 *   sandbox-exec -p <profile text> -D KEY=VALUE ... <program> <its arguments>
 *
 * sandbox-exec applies the profile to itself and then replaces itself with the
 * program, so the program and everything it starts stay inside the profile
 * and cannot leave it. The parameters carry the two names that are defined
 * once in ntfsmac/ctl_proto.h (the control socket and the privileged XPC
 * service) and the path of the program, so no profile repeats them. The control
 * socket is given twice, as written and with the symbolic links of its directory
 * resolved (/var/run is /private/var/run): the sandbox compares the path a process
 * connects to with the rule as spelled, so a spelling that the rule does not
 * have would not match (checked in the guest, audit M3). Applying the profile in the forked
 * child instead would call into libsandbox, which allocates memory, in a child
 * of a multi-threaded daemon.
 */
typedef struct sandbox_cmd {
	const char *path; /* what to exec: sandbox-exec, or the program itself */
	char *argv[SANDBOX_ARGV_MAX];
	char text[SANDBOX_PROFILE_MAX];
	char param[SANDBOX_PARAM_MAX][SANDBOX_STR_MAX];
} sandbox_cmd;

/*
 * Read a profile that is trusted: the file is opened without following a link, and the checks are
 * made on the open descriptor, so the file that was checked is the file that was read and nothing
 * can be swapped in between. It must be a regular file owned by `owner` that no one else can
 * write, and not larger than the buffer. The text is NUL-terminated.
 */
static bool profile_read(const char *path, uid_t owner, char *buf, size_t cap)
{
	struct stat st;
	size_t len = 0;
	bool ok = false;
	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);

	if (fd < 0)
		return false;
	if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == owner
	    && (st.st_mode & (S_IWGRP | S_IWOTH)) == 0 && st.st_size > 0
	    && (uint64_t)st.st_size < cap) {
		for (;;) {
			ssize_t n = read(fd, buf + len, cap - 1u - len);

			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0) {
				ok = n == 0 && len == (size_t)st.st_size;
				break;
			}
			len += (size_t)n;
			if (len >= cap - 1u)
				break; /* the file grew: refuse it */
		}
	}
	close(fd);
	if (ok)
		buf[len] = '\0';
	return ok && memchr(buf, '\0', len) == NULL;
}

/* Where the profiles are, the tool, and who must own the profiles; NULL `*dir` means no sandbox. */
static void sandbox_settings(const ntfsmac_spawn_config *cfg, const char **dir, const char **tool,
			     uid_t *owner)
{
#if defined(NTFSMAC_TEST_SEAMS)
	*dir = cfg->sandbox_dir;
	*tool = cfg->sandbox_exec_path != NULL ? cfg->sandbox_exec_path : NTFSMAC_PATH_SANDBOX_EXEC;
	*owner = cfg->sandbox_owner;
#else
	(void)cfg;
#  if defined(__APPLE__)
	*dir = NTFSMAC_PATH_SANDBOX_DIR;
#  else
	*dir = NULL; /* no sandbox on this platform: the portable parts only */
#  endif
	*tool = NTFSMAC_PATH_SANDBOX_EXEC;
	*owner = 0;
#endif
}

/* The control socket path with the links of its directory resolved; false if that cannot be done. */
static bool ctl_socket_real(char *out, size_t cap)
{
	const char *path = NTFSMAC_CTL_SOCKET_PATH;
	char dir[PATH_MAX], real[PATH_MAX];
	const char *slash = strrchr(path, '/');
	size_t dlen;
	int w;

	if (slash == NULL)
		return false;
	dlen = (size_t)(slash - path);
	if (dlen >= sizeof(dir))
		return false;
	if (dlen == 0) {
		snprintf(dir, sizeof(dir), "/");
	} else {
		memcpy(dir, path, dlen);
		dir[dlen] = '\0';
	}
	if (realpath(dir, real) == NULL)
		return false;
	w = snprintf(out, cap, "CTL_SOCKET_REAL=%s%s%s", real, strcmp(real, "/") == 0 ? "" : "/", slash + 1);
	return w > 0 && (size_t)w < cap;
}

/*
 * `argv` is the program's own vector (argv[0] is its absolute path). Where there is no sandbox
 * the result is that vector unchanged. `worker` adds the parameters that only the worker profile
 * uses.
 */
static ntfsmac_dmn_status sandbox_wrap(const ntfsmac_spawn_config *cfg, const char *name,
				       bool worker, char *const *argv, sandbox_cmd *out)
{
	size_t n = 0, np = 0, i;
	char real[PATH_MAX], file[SANDBOX_STR_MAX];
	const char *dir, *tool;
	uid_t owner;
	int w;

	sandbox_settings(cfg, &dir, &tool, &owner);
	if (dir == NULL) {
		out->path = argv[0];
		for (i = 0; argv[i] != NULL; i++) {
			if (i >= SANDBOX_ARGV_MAX - 1u)
				return NTFSMAC_DMN_E_INVAL;
			out->argv[i] = argv[i];
		}
		out->argv[i] = NULL;
		return NTFSMAC_DMN_OK;
	}
	w = snprintf(file, sizeof(file), "%s/%s.sb", dir, name);
	if (w < 0 || (size_t)w >= sizeof(file) || !profile_read(file, owner, out->text, sizeof(out->text)))
		return NTFSMAC_DMN_E_DENIED;

	/* The profile names the program by the path the kernel resolves, so follow symlinks here. */
	if (realpath(argv[0], real) == NULL)
		return NTFSMAC_DMN_E_DENIED;
	w = snprintf(out->param[np++], SANDBOX_STR_MAX, "EXEC_PATH=%s", real);
	if (w < 0 || (size_t)w >= SANDBOX_STR_MAX)
		return NTFSMAC_DMN_E_DENIED;
	(void)snprintf(out->param[np++], SANDBOX_STR_MAX, "CTL_SOCKET=%s", NTFSMAC_CTL_SOCKET_PATH);
	if (!ctl_socket_real(out->param[np++], SANDBOX_STR_MAX))
		return NTFSMAC_DMN_E_DENIED;
	(void)snprintf(out->param[np++], SANDBOX_STR_MAX, "XPC_SERVICE=%s", NTFSMAC_CTL_XPC_SERVICE);
	if (worker) {
		(void)snprintf(out->param[np++], SANDBOX_STR_MAX, "USERMAPPING_PATH=%s",
			       NTFSMAC_USERMAPPING_PATH);
		(void)snprintf(out->param[np++], SANDBOX_STR_MAX, "USERMAPPING=%s",
			       cfg->usermapping ? "yes" : "no");
	}

	out->path = tool;
	out->argv[n++] = (char *)tool;
	out->argv[n++] = (char *)"-p";
	out->argv[n++] = out->text;
	for (i = 0; i < np; i++) {
		out->argv[n++] = (char *)"-D";
		out->argv[n++] = out->param[i];
	}
	for (i = 0; argv[i] != NULL; i++) {
		if (n >= SANDBOX_ARGV_MAX - 1u)
			return NTFSMAC_DMN_E_INVAL;
		out->argv[n++] = argv[i];
	}
	out->argv[n] = NULL;
	return NTFSMAC_DMN_OK;
}

/*
 * U6, found by the first run of the wired daemon (T-M4-08): a FUSE server started by a launchd
 * daemon lives in the system bootstrap domain, and the macFUSE mount service answers it
 * "unsatisfiedPeerCodeSigningRequirement" and mounts nothing. The same worker started by a root
 * process that sits in the user's session mounts (T-M4-01 ran that way). So the worker is started
 * through `launchctl asuser UID`, which adopts the bootstrap namespace of the user and leaves the
 * credentials alone, and a second step of this same program (`ntfsmacd --exec-as UID GID PROGRAM
 * ARGS`, ntfsmac_spawn_exec_as) drops from root to the console user the way drop_privileges does,
 * with the same descriptors, and starts the program. Nothing but launchctl and this program run
 * as root in the chain, before the sandbox is applied.
 */
#define LAUNCHCTL_PATH "/bin/launchctl"
#define WRAP_ARGV_MAX (SANDBOX_ARGV_MAX + 8)

typedef struct session_wrap {
	char *argv[WRAP_ARGV_MAX];
	char uid[24];
	char gid[24];
	char self[PATH_MAX];
} session_wrap;

static bool user_session_wrap(const launch_spec *s, session_wrap *w)
{
	size_t n = 0, i;
#if defined(__APPLE__)
	uint32_t size = (uint32_t)sizeof w->self;

	if (_NSGetExecutablePath(w->self, &size) != 0)
		return false;
#else
	return false;
#endif
	if (snprintf(w->uid, sizeof w->uid, "%lu", (unsigned long)s->uid) >= (int)sizeof w->uid ||
	    snprintf(w->gid, sizeof w->gid, "%lu", (unsigned long)s->gid) >= (int)sizeof w->gid)
		return false;
	w->argv[n++] = (char *)LAUNCHCTL_PATH;
	w->argv[n++] = (char *)"asuser";
	w->argv[n++] = w->uid;
	w->argv[n++] = w->self;
	w->argv[n++] = (char *)NTFSMAC_EXEC_AS_FLAG;
	w->argv[n++] = w->uid;
	w->argv[n++] = w->gid;
	w->argv[n++] = (char *)s->path;
	for (i = 1; s->argv[i] != NULL; i++) { /* argv[0] is `path` itself */
		if (n >= WRAP_ARGV_MAX - 1u)
			return false;
		w->argv[n++] = s->argv[i];
	}
	w->argv[n] = NULL;
	return true;
}

/*
 * `ntfsmacd --exec-as UID GID PROGRAM [ARG...]`: the second step of the chain above. Returns
 * only on failure (an exit code for main). Root only, and never to uid 0.
 */
int ntfsmac_spawn_exec_as(int argc, char **argv)
{
	char *end;
	unsigned long u, g;

	if (argc < 5 || argv[1] == NULL || strcmp(argv[1], NTFSMAC_EXEC_AS_FLAG) != 0)
		return 2;
	errno = 0;
	u = strtoul(argv[2], &end, 10);
	if (errno != 0 || argv[2][0] == '\0' || *end != '\0' || u == 0 || u > 0xFFFFFFFEu)
		return 2;
	g = strtoul(argv[3], &end, 10);
	if (errno != 0 || argv[3][0] == '\0' || *end != '\0' || g == 0 || g > 0xFFFFFFFEu)
		return 2;
	if (argv[4][0] != '/')
		return 2;
	if (geteuid() != 0)
		return 126;
	if (drop_privileges((uid_t)u, (gid_t)g) != 0)
		return 126;
	/* Defensive: launchctl asuser was not observed to reset the policy inherited from the daemon. */
	if (ntfsmac_spawn_io_passive() != 0)
		fprintf(stderr, "ntfsmacd --exec-as: passive disk I/O not set: %s\n", strerror(errno));
	/* An explicit environment, as the daemon built it: launchctl may have added its own. */
	{
		char *env[3];
		size_t ne = 0, k;
		static const char errfd[] = "NTFSMAC_ERRFD=";

		env[ne++] = (char *)"PATH=/usr/bin:/bin";
		for (k = 0; environ != NULL && environ[k] != NULL; k++) {
			const char *e = environ[k];
			size_t d;

			if (strncmp(e, errfd, sizeof errfd - 1u) != 0)
				continue;
			for (d = sizeof errfd - 1u; e[d] >= '0' && e[d] <= '9'; d++)
				;
			if (d > sizeof errfd - 1u && e[d] == '\0' && d - (sizeof errfd - 1u) <= 4u) {
				env[ne++] = environ[k];
				break;
			}
		}
		env[ne] = NULL;
		execve(argv[4], &argv[4], env);
	}
	return 127;
}

static ntfsmac_dmn_status launch(const launch_spec *s, pid_t *pid_out, int *capture_rd)
{
	int devnull = -1, outp[2] = { -1, -1 }, stp[2] = { -1, -1 };
	int err_fd = -1;
	pid_t pid;
	size_t i;
	int e = 0;
	ssize_t n;
	uint64_t end;
	const char *path = s->path;
	char *const *argv = s->argv;
	bool drop = s->drop;
	session_wrap wrap;
#if defined(__APPLE__)
	posix_spawnattr_t attr;
	posix_spawn_file_actions_t fa;
	bool attr_ok = false, fa_ok = false;
#else
	int max_fd;
#endif

	*pid_out = -1;
	if (capture_rd != NULL)
		*capture_rd = -1;
	if (s->drop && s->user_session) {
		if (!user_session_wrap(s, &wrap))
			return NTFSMAC_DMN_E_INVAL;
		path = LAUNCHCTL_PATH;
		argv = wrap.argv;
		drop = false; /* root until the --exec-as step */
	}
#if !defined(__APPLE__)
	{
		long m = sysconf(_SC_OPEN_MAX);

		max_fd = (m < 0 || m > SPAWN_FD_MAX) ? SPAWN_FD_MAX : (int)m;
	}
#endif

	pthread_mutex_lock(&g_lock);
	devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
	devnull = fd_high(devnull);
	if (devnull < 0 || pipe_cloexec(stp) != 0 || (s->capture && pipe_cloexec(outp) != 0)) {
		e = errno ? errno : EMFILE;
		goto fail_locked;
	}
	if (s->err_fd >= 0) {
		err_fd = s->err_fd;
	} else {
		err_fd = devnull;
	}
#if defined(__APPLE__)
	if (posix_spawnattr_init(&attr) != 0) {
		e = ENOMEM;
		goto fail_locked;
	}
	attr_ok = true;
	if (posix_spawn_file_actions_init(&fa) != 0) {
		e = ENOMEM;
		goto fail_locked;
	}
	fa_ok = true;
	e = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETEXEC | POSIX_SPAWN_CLOEXEC_DEFAULT);
	for (i = 0; e == 0 && i < 3; i++)
		e = posix_spawn_file_actions_addinherit_np(&fa, (int)i);
	for (i = 0; e == 0 && i < s->nkeep; i++)
		e = posix_spawn_file_actions_addinherit_np(&fa, s->keep[i]);
	if (e != 0)
		goto fail_locked;
#endif
	pid = fork();
	if (pid < 0) {
		e = errno;
		goto fail_locked;
	}
	if (pid == 0) {
		/* Child. Only calls that are safe after fork until the exec. */
		int dummy;

		(void)setpgid(0, 0);
		reset_signals();
		(void)ntfsmac_spawn_io_passive();
		if (dup2(devnull, 0) < 0 || dup2(s->capture ? outp[1] : devnull, 1) < 0
		    || dup2(err_fd, 2) < 0) {
			dummy = errno;
			goto child_fail;
		}
		if (drop && drop_privileges(s->uid, s->gid) != 0) {
			dummy = EPERM;
			goto child_fail;
		}
		for (i = 0; i < s->nkeep; i++) {
			if (fcntl(s->keep[i], F_SETFD, 0) != 0) {
				dummy = errno;
				goto child_fail;
			}
		}
#if defined(__APPLE__)
		dummy = posix_spawn(NULL, path, &fa, &attr, argv, s->envp);
#else
		{
			int fd;

			for (fd = 3; fd < max_fd; fd++) {
				bool keep = (fd == stp[1]);

				for (i = 0; i < s->nkeep; i++)
					if (s->keep[i] == fd)
						keep = true;
				if (!keep)
					(void)close(fd);
			}
			execve(path, argv, s->envp);
			dummy = errno;
		}
#endif
child_fail:
		/* The exec failed (or an earlier step did): report errno and stop. */
		if (write(stp[1], &dummy, sizeof(dummy)) < 0) {
			/* nothing more can be done */
		}
		_exit(127);
	}
	/* Parent. */
#if defined(__APPLE__)
	posix_spawn_file_actions_destroy(&fa);
	posix_spawnattr_destroy(&attr);
#endif
	close_fd(&devnull);
	close_fd(&stp[1]);
	close_fd(&outp[1]);
	pthread_mutex_unlock(&g_lock);

	/* The child set the group itself; do it here too so that it exists at once. */
	(void)setpgid(pid, pid);

	/* The pipe closes at a good exec; a number arrives when anything failed. */
	end = now_ms() + EXEC_STATUS_WAIT_MS;
	for (;;) {
		uint64_t now = now_ms();

		if (now >= end) {
			int st;

			kill_group_of(pid);
			(void)reap_bounded(pid, &st, REAP_WAIT_MS);
			close_fd(&stp[0]);
			close_fd(&outp[0]);
			return NTFSMAC_DMN_E_TIMEOUT;
		}
		if (wait_readable(stp[0], end - now) > 0)
			break;
	}
	do {
		n = read(stp[0], &e, sizeof(e));
	} while (n < 0 && errno == EINTR);
	close_fd(&stp[0]);
	if (n != 0) { /* a number or a read error: the child did not become the program */
		int st;

		kill_group_of(pid);
		(void)reap_bounded(pid, &st, REAP_WAIT_MS);
		close_fd(&outp[0]);
		return NTFSMAC_DMN_E_IO;
	}
	*pid_out = pid;
	if (capture_rd != NULL)
		*capture_rd = outp[0];
	else
		close_fd(&outp[0]);
	return NTFSMAC_DMN_OK;

fail_locked:
#if defined(__APPLE__)
	if (fa_ok)
		posix_spawn_file_actions_destroy(&fa);
	if (attr_ok)
		posix_spawnattr_destroy(&attr);
#endif
	close_fd(&devnull);
	close_fd(&stp[0]);
	close_fd(&stp[1]);
	close_fd(&outp[0]);
	close_fd(&outp[1]);
	pthread_mutex_unlock(&g_lock);
	return NTFSMAC_DMN_E_IO;
}

/* ------------------------------------------------------------------ */
/* Probe                                                               */
/* ------------------------------------------------------------------ */

static char *const g_env_min[] = { (char *)"PATH=/usr/bin:/bin", NULL };

/*
 * Read the child's stdout until end of file, `cap` bytes or the deadline.
 * Returns 0 for end of file, 1 on overflow, 2 on the deadline, -1 on error.
 * For volmark `progress` is non-NULL: it is told each byte read.
 */
static int read_output(int fd, char *buf, size_t cap, size_t *len, uint64_t *deadline,
		       void (*progress)(char c, uint64_t *deadline, void *arg), void *arg)
{
	*len = 0;
	for (;;) {
		uint64_t now = now_ms();
		int r;

		if (now >= *deadline)
			return 2;
		r = wait_readable(fd, *deadline - now);
		if (r < 0)
			return -1;
		if (r == 0)
			continue;
		{
			char tmp[256];
			ssize_t n = read(fd, tmp, sizeof(tmp));
			ssize_t k;

			if (n < 0) {
				if (errno == EINTR || errno == EAGAIN)
					continue;
				return -1;
			}
			if (n == 0)
				return 0;
			for (k = 0; k < n; k++) {
				if (progress != NULL)
					progress(tmp[k], deadline, arg);
				if (buf != NULL) {
					if (*len >= cap)
						return 1;
					buf[(*len)++] = tmp[k];
				}
			}
		}
	}
}

ntfsmac_dmn_status ntfsmac_spawn_probe(const ntfsmac_probe_run *run,
				       ntfsmac_probe_verdict *verdict)
{
	ntfsmac_spawn_config cfg;
	ntfsmac_dmn_status st;
	launch_spec ls;
	sandbox_cmd sb;
	char fdarg[16];
	char *argv[4];
	int keep[1];
	pid_t pid;
	int rfd, rc, wst = 0;
	bool drop = false, exited;
	uint64_t deadline;
	size_t len;
	char *buf;

	if (verdict == NULL)
		return NTFSMAC_DMN_E_INVAL;
	ntfsmac_probe_verdict_unknown(verdict);
	if (run == NULL || run->device_fd < 3 || run->device_fd > MAX_FD || run->timeout_s == 0)
		return NTFSMAC_DMN_E_INVAL;
	config_get(&cfg);
	if (!privileges_ok(cfg.unpriv_uid, cfg.unpriv_gid, &drop))
		return NTFSMAC_DMN_E_DENIED;

	snprintf(fdarg, sizeof(fdarg), "%d", run->device_fd);
	argv[0] = (char *)cfg.probe_path;
	argv[1] = (char *)"--fd";
	argv[2] = fdarg;
	argv[3] = NULL;
	keep[0] = run->device_fd;
	st = sandbox_wrap(&cfg, "probe", false, argv, &sb);
	if (st != NTFSMAC_DMN_OK)
		return st;
	memset(&ls, 0, sizeof(ls));
	ls.path = sb.path;
	ls.argv = sb.argv;
	ls.envp = g_env_min;
	ls.keep = keep;
	ls.nkeep = 1;
	ls.capture = true;
	ls.err_fd = cfg.child_stderr_fd;
	ls.drop = drop;
	ls.uid = cfg.unpriv_uid;
	ls.gid = cfg.unpriv_gid;

	st = launch(&ls, &pid, &rfd);
	if (st != NTFSMAC_DMN_OK)
		return st;

	buf = malloc(NTFSMAC_PROBE_MAX_JSON_BYTES);
	if (buf == NULL) {
		kill_group_of(pid);
		(void)reap_bounded(pid, &wst, REAP_WAIT_MS);
		close(rfd);
		return NTFSMAC_DMN_E_IO;
	}
	deadline = now_ms() + (uint64_t)run->timeout_s * 1000u;
	rc = read_output(rfd, buf, NTFSMAC_PROBE_MAX_JSON_BYTES, &len, &deadline, NULL, NULL);
	close(rfd);
	if (rc == 2) {
		kill_group_of(pid);
		(void)reap_bounded(pid, &wst, REAP_WAIT_MS);
		free(buf);
		return NTFSMAC_DMN_E_TIMEOUT;
	}
	if (rc != 0) { /* overflow or read error */
		kill_group_of(pid);
		(void)reap_bounded(pid, &wst, REAP_WAIT_MS);
		free(buf);
		return NTFSMAC_DMN_E_IO;
	}
	/* End of output: the probe is about to exit. Wait for it within the deadline. */
	exited = false;
	while (!exited) {
		pid_t r = waitpid(pid, &wst, WNOHANG);

		if (r == pid) {
			exited = true;
		} else if (r < 0 && errno != EINTR) {
			free(buf);
			return NTFSMAC_DMN_E_IO;
		} else if (now_ms() >= deadline) {
			kill_group_of(pid);
			(void)reap_bounded(pid, &wst, REAP_WAIT_MS);
			free(buf);
			return NTFSMAC_DMN_E_TIMEOUT;
		} else {
			sleep_ms(5);
		}
	}
	/* Nothing may outlive the probe. */
	(void)kill(-pid, SIGKILL);
	if (!WIFEXITED(wst) || WEXITSTATUS(wst) != 0) {
		free(buf);
		return NTFSMAC_DMN_E_IO;
	}
	if (ntfsmac_probe_parse(buf, len, verdict) != NTFSMAC_PROBE_OK) {
		free(buf);
		return NTFSMAC_DMN_E_IO; /* the parser left `verdict` as `unknown` */
	}
	free(buf);
	return NTFSMAC_DMN_OK;
}

/* ------------------------------------------------------------------ */
/* Volmark                                                             */
/* ------------------------------------------------------------------ */

typedef struct volmark_watch {
	bool saw_w;
	bool saw_d;
	unsigned write_wait_s;
} volmark_watch;

static void volmark_progress(char c, uint64_t *deadline, void *arg)
{
	volmark_watch *w = arg;

	if (c == 'w' && !w->saw_w) {
		/* From the first byte of `w` the write has begun: a new, longer limit. */
		w->saw_w = true;
		*deadline = now_ms() + (uint64_t)w->write_wait_s * 1000u;
	} else if (c == 'd') {
		w->saw_d = true;
	}
}

ntfsmac_dmn_status ntfsmac_spawn_volmark_op(const ntfsmac_volmark_run *run,
					    ntfsmac_volmark_op op, int *exit_code)
{
	ntfsmac_spawn_config cfg;
	ntfsmac_dmn_status st;
	launch_spec ls;
	sandbox_cmd sb;
	volmark_watch w;
	char fdarg[16], serial[24], sectors[24];
	char *argv[9];
	int keep[1];
	pid_t pid;
	int rfd, rc, wst = 0;
	bool drop = false;
	uint64_t deadline;
	size_t len;

	if (exit_code == NULL)
		return NTFSMAC_DMN_E_INVAL;
	*exit_code = -3;
	if (op != NTFSMAC_VOLMARK_SET && op != NTFSMAC_VOLMARK_CLEAR && op != NTFSMAC_VOLMARK_CLEAR_REPAIR &&
	    op != NTFSMAC_VOLMARK_SYNC_MIRROR)
		return NTFSMAC_DMN_E_INVAL;
	if (run == NULL || run->device_fd < 3 || run->device_fd > MAX_FD || run->timeout_s == 0
	    || run->total_sectors == 0 || run->total_sectors > UINT64_C(0x7fffffffffffffff))
		return NTFSMAC_DMN_E_INVAL;
	config_get(&cfg);
	if (!privileges_ok(cfg.unpriv_uid, cfg.unpriv_gid, &drop))
		return NTFSMAC_DMN_E_DENIED;

	/* Both numbers are formatted here from integers (ARCHITECTURE.md section 3.6). */
	snprintf(fdarg, sizeof(fdarg), "%d", run->device_fd);
	snprintf(serial, sizeof(serial), "%016" PRIX64, run->serial);
	snprintf(sectors, sizeof(sectors), "%" PRIu64, run->total_sectors);
	argv[0] = (char *)cfg.volmark_path;
	argv[1] = (char *)(op == NTFSMAC_VOLMARK_SET		? "set"
			   : op == NTFSMAC_VOLMARK_CLEAR	? "clear"
			   : op == NTFSMAC_VOLMARK_SYNC_MIRROR ? "sync-mirror"
								: "clear-repair");
	argv[2] = (char *)"--fd";
	argv[3] = fdarg;
	argv[4] = (char *)"--expect-serial";
	argv[5] = serial;
	argv[6] = (char *)"--expect-sectors";
	argv[7] = sectors;
	argv[8] = NULL;
	keep[0] = run->device_fd;
	st = sandbox_wrap(&cfg, "volmark", false, argv, &sb);
	if (st != NTFSMAC_DMN_OK)
		return st;
	memset(&ls, 0, sizeof(ls));
	ls.path = sb.path;
	ls.argv = sb.argv;
	ls.envp = g_env_min;
	ls.keep = keep;
	ls.nkeep = 1;
	ls.capture = true;
	ls.err_fd = cfg.child_stderr_fd;
	ls.drop = drop;
	ls.uid = cfg.unpriv_uid;
	ls.gid = cfg.unpriv_gid;

	st = launch(&ls, &pid, &rfd);
	if (st != NTFSMAC_DMN_OK)
		return st;

	memset(&w, 0, sizeof(w));
	w.write_wait_s = cfg.volmark_write_wait_s;
	deadline = now_ms() + (uint64_t)run->timeout_s * 1000u;
	/* Until `w`, the time limit is `timeout_s` and a kill cannot cut a write. */
	rc = read_output(rfd, NULL, 0, &len, &deadline, volmark_progress, &w);
	close(rfd);
	if (rc == 0) {
		/* End of output: wait for the exit within the same deadline. */
		for (;;) {
			pid_t r = waitpid(pid, &wst, WNOHANG);

			if (r == pid) {
				(void)kill(-pid, SIGKILL);
				*exit_code = exit_code_of(wst);
				return *exit_code == 0 ? NTFSMAC_DMN_OK : NTFSMAC_DMN_E_IO;
			}
			if (r < 0 && errno != EINTR)
				return NTFSMAC_DMN_E_IO;
			if (now_ms() >= deadline) {
				rc = 2;
				break;
			}
			sleep_ms(5);
		}
	}
	kill_group_of(pid);
	if (!reap_bounded(pid, &wst, REAP_WAIT_MS)) {
		/* Still not gone after SIGKILL: a process that may yet write to the device. */
		*exit_code = -4;
		return NTFSMAC_DMN_E_BUSY;
	}
	if (rc == 2) {
		*exit_code = w.saw_w ? -2 : -1;
		return NTFSMAC_DMN_E_TIMEOUT;
	}
	return NTFSMAC_DMN_E_IO;
}

ntfsmac_dmn_status ntfsmac_spawn_volmark(const ntfsmac_volmark_run *run, bool set,
					 int *exit_code)
{
	return ntfsmac_spawn_volmark_op(run, set ? NTFSMAC_VOLMARK_SET : NTFSMAC_VOLMARK_CLEAR,
					exit_code);
}

/* ------------------------------------------------------------------ */
/* Mount table                                                         */
/* ------------------------------------------------------------------ */

#if defined(__APPLE__)

/* getfsstat(MNT_NOWAIT) returns the kernel's cached table; it does not call the file system. */
static ntfsmac_dmn_status mount_lookup_system(const char *mount_point, bool *found,
					      bool *read_only)
{
	struct statfs *fs;
	int n, i, tries;

	*found = false;
	*read_only = false;
	for (tries = 0; tries < 3; tries++) {
		int cnt = getfsstat(NULL, 0, MNT_NOWAIT);

		if (cnt < 0)
			return NTFSMAC_DMN_E_IO;
		fs = calloc((size_t)cnt + 8u, sizeof(*fs));
		if (fs == NULL)
			return NTFSMAC_DMN_E_IO;
		n = getfsstat(fs, (int)(((size_t)cnt + 8u) * sizeof(*fs)), MNT_NOWAIT);
		if (n < 0) {
			free(fs);
			return NTFSMAC_DMN_E_IO;
		}
		if ((size_t)n > (size_t)cnt + 7u) {
			free(fs); /* the table grew while we looked; once more */
			continue;
		}
		for (i = 0; i < n; i++) {
			if (strcmp(fs[i].f_mntonname, mount_point) == 0) {
				*found = true;
				*read_only = (fs[i].f_flags & MNT_RDONLY) != 0;
				break;
			}
		}
		free(fs);
		return NTFSMAC_DMN_OK;
	}
	return NTFSMAC_DMN_E_IO;
}

#else /* Linux: /proc/self/mountinfo, for tests */

/* mountinfo writes a space as \040 and similar; undo the octal escapes. */
static void unescape_octal(char *s)
{
	char *o = s;

	while (*s != '\0') {
		if (s[0] == '\\' && s[1] >= '0' && s[1] <= '3' && s[2] >= '0' && s[2] <= '7'
		    && s[3] >= '0' && s[3] <= '7') {
			*o++ = (char)(((s[1] - '0') << 6) | ((s[2] - '0') << 3) | (s[3] - '0'));
			s += 4;
		} else {
			*o++ = *s++;
		}
	}
	*o = '\0';
}

static ntfsmac_dmn_status mount_lookup_system(const char *mount_point, bool *found,
					      bool *read_only)
{
	FILE *f = fopen("/proc/self/mountinfo", "r");
	char line[4096];

	*found = false;
	*read_only = false;
	if (f == NULL)
		return NTFSMAC_DMN_E_IO;
	while (fgets(line, sizeof(line), f) != NULL) {
		char *p = line, *field[6];
		int i;

		for (i = 0; i < 6; i++) {
			field[i] = strsep(&p, " ");
			if (field[i] == NULL)
				break;
		}
		if (i < 6)
			continue;
		unescape_octal(field[4]);
		if (strcmp(field[4], mount_point) == 0) {
			char *opts = field[5], *tok;

			*found = true;
			while ((tok = strsep(&opts, ",")) != NULL) {
				if (strcmp(tok, "ro") == 0)
					*read_only = true;
			}
			/* Keep scanning: the last entry for a path is the visible one. */
		}
	}
	fclose(f);
	return NTFSMAC_DMN_OK;
}

#endif

/* ------------------------------------------------------------------ */
/* Worker                                                              */
/* ------------------------------------------------------------------ */

/*
 * The check of ARCHITECTURE.md section 3.3 for `usermapping=`: the daemon opens the file itself,
 * checks the descriptor, reads and parses it. True only for a file it accepted (`um->fd` is then
 * open); otherwise the reason is logged, with the line number for a content refusal, and the mount
 * goes ahead without the option.
 */
static void usermapping_target(const ntfsmac_spawn_config *cfg, const char **path, uid_t *owner)
{
	*path = NTFSMAC_USERMAPPING_PATH;
	*owner = 0;
#if defined(NTFSMAC_TEST_SEAMS)
	if (cfg->usermapping_path != NULL) {
		*path = cfg->usermapping_path;
		*owner = cfg->usermapping_owner;
	}
#else
	(void)cfg;
#endif
}

static bool usermapping_accept(const ntfsmac_spawn_config *cfg, ntfsmac_usermap_file *um)
{
	const char *path;
	uid_t owner;
	unsigned line = 0;
	ntfsmac_usermap_parse p = NTFSMAC_USERMAP_P_OK;
	ntfsmac_usermap_verdict v;

	usermapping_target(cfg, &path, &owner);
	v = ntfsmac_usermap_file_open(path, owner, um, &line, &p);
	if (v == NTFSMAC_USERMAP_V_ENABLE)
		return true;
	if (v == NTFSMAC_USERMAP_V_PARSE && line != 0)
		NTFSMAC_LOGW("usermapping= not used: %s line %u: %s", path, line,
			     ntfsmac_usermap_parse_str(p));
	else if (v == NTFSMAC_USERMAP_V_PARSE)
		NTFSMAC_LOGW("usermapping= not used: %s: %s", path, ntfsmac_usermap_parse_str(p));
	else
		NTFSMAC_LOGW("usermapping= not used: %s: %s", path, ntfsmac_usermap_verdict_str(v));
	return false;
}

/* The worker is gone: nothing of the user may keep its device (the result is for the later check). */
static void worker_gone(const ntfsmac_child *child, pid_t worker_pid)
{
	if (child->dev_known) {
		dev_ident id;

		id.dev = child->dev_dev;
		id.ino = child->dev_ino;
		(void)device_free(child->owner, &id, worker_pid, NULL);
	}
}

ntfsmac_dmn_status ntfsmac_spawn_worker(const ntfsmac_worker_run *run,
					ntfsmac_child *child, bool *mounted_ro)
{
	ntfsmac_spawn_config cfg;
	ntfsmac_options_input in;
	ntfsmac_argv_expect expect;
	ntfsmac_worker_argv av;
	ntfsmac_policy_decision decision;
	ntfsmac_dmn_status st;
	launch_spec ls;
	sandbox_cmd sb;
	char errenv[40];
	char *envp[3];
	char *argv[NTFSMAC_WORKER_ARGC + 1];
	int keep[2];
	size_t nkeep = 0, i;
	const char *leaf;
	pid_t pid;
	bool drop = false;
	uint64_t deadline;
	ntfsmac_usermap_file um;
	bool use_um;

	if (child == NULL || mounted_ro == NULL)
		return NTFSMAC_DMN_E_INVAL;
	ntfsmac_usermap_file_init(&um);
	child->pid = -1;
	child->status_fd = -1;
	child->owner = 0;
	child->dev_dev = 0;
	child->dev_ino = 0;
	child->dev_known = false;
	*mounted_ro = false;
	if (run == NULL || run->mount_point == NULL || run->mount_wait_s == 0
	    || run->device_fd < NTFSMAC_WORKER_FD_MIN || run->device_fd > NTFSMAC_WORKER_FD_MAX
	    || run->errfd_write == run->device_fd || run->errfd_write < -1
	    || (run->errfd_write >= 0 && (run->errfd_write < 3 || run->errfd_write > MAX_FD)))
		return NTFSMAC_DMN_E_INVAL;
	if (strncmp(run->mount_point, NTFSMAC_VOLUMES_PREFIX, sizeof(NTFSMAC_VOLUMES_PREFIX) - 1u) != 0)
		return NTFSMAC_DMN_E_INVAL;
	leaf = run->mount_point + (sizeof(NTFSMAC_VOLUMES_PREFIX) - 1u);

	config_get(&cfg);
	if (!privileges_ok(run->console_uid, run->console_gid, &drop))
		return NTFSMAC_DMN_E_DENIED;
	/* What ntfsmac_child_device_check() looks for once the worker is gone. */
	{
		struct stat dst;

		if (fstat(run->device_fd, &dst) == 0) {
			child->owner = run->console_uid;
			child->dev_dev = dst.st_dev;
			child->dev_ino = dst.st_ino;
			child->dev_known = true;
		}
	}

	decision = run->read_only ? NTFSMAC_POLICY_RO : NTFSMAC_POLICY_RW;
	memset(&in, 0, sizeof(in));
	in.worker_path = cfg.worker_path;
	in.device_fd = run->device_fd;
	in.leaf = leaf;
	in.decision = decision;
	in.console_uid = (uint32_t)run->console_uid;
	in.console_gid = (uint32_t)run->console_gid;
	in.hide_hid_files = cfg.hide_hid_files;
	in.permissions = cfg.permissions;
	/*
	 * `usermapping=` is enabled only for a file the daemon opened, checked on the descriptor, read
	 * and parsed itself. A file it does not accept is not an error: the mount is built as without
	 * the option (never a guess, never a failed mount because of it).
	 */
	use_um = cfg.usermapping && usermapping_accept(&cfg, &um);
rebuild:
	cfg.usermapping = use_um;
	in.usermapping = use_um;
	if (ntfsmac_options_build(&in, &av) != NTFSMAC_OPTIONS_OK) {
		ntfsmac_usermap_file_close(&um);
		return NTFSMAC_DMN_E_DENIED;
	}

	memset(&expect, 0, sizeof(expect));
	expect.worker_path = cfg.worker_path;
	expect.leaf = leaf;
	expect.decision = decision;
	expect.console_uid = (uint32_t)run->console_uid;
	expect.console_gid = (uint32_t)run->console_gid;
	if (ntfsmac_argv_check(av.argv, NTFSMAC_WORKER_ARGC, &expect) != NTFSMAC_ARGV_OK) {
		ntfsmac_usermap_file_close(&um);
		return NTFSMAC_DMN_E_DENIED;
	}
#if defined(NTFSMAC_TEST_SEAMS)
	if (cfg.after_check_hook != NULL)
		cfg.after_check_hook(&av);
#endif

	for (i = 0; i < NTFSMAC_WORKER_ARGC; i++)
		argv[i] = (char *)av.argv[i];
	argv[NTFSMAC_WORKER_ARGC] = NULL;
	/* A fixed, small environment: nothing of the daemon's own reaches the worker. */
	envp[0] = (char *)"PATH=/usr/bin:/bin";
	envp[1] = NULL;
	envp[2] = NULL;
	nkeep = 0;
	keep[nkeep++] = run->device_fd;
	if (run->errfd_write >= 0) {
		snprintf(errenv, sizeof(errenv), "NTFSMAC_ERRFD=%d", run->errfd_write);
		envp[1] = errenv;
		keep[nkeep++] = run->errfd_write;
	}

	st = sandbox_wrap(&cfg, "worker", true, argv, &sb);
	if (st != NTFSMAC_DMN_OK) {
		ntfsmac_usermap_file_close(&um);
		return st;
	}
	/*
	 * The check/use gap (audit M4 F-03): the worker opens the file by path, inside the sandbox, so
	 * the daemon compares the path with the descriptor it read (device, inode, type, owner, mode,
	 * size, mtime; the name must not be a link) as the last step before the spawn. A mismatch
	 * refuses the option and the worker vector is built again without it. What stays is the time
	 * from this check to the worker's own open (the sandbox-exec start-up); see section 3.3.
	 */
	if (use_um) {
		const char *um_path;
		uid_t um_owner;
		ntfsmac_usermap_verdict uv;

		usermapping_target(&cfg, &um_path, &um_owner);
		uv = ntfsmac_usermap_file_reverify(um_path, um_owner, &um);

		ntfsmac_usermap_file_close(&um);
		if (uv != NTFSMAC_USERMAP_V_ENABLE) {
			NTFSMAC_LOGW("usermapping= not used: %s: %s", um_path, ntfsmac_usermap_verdict_str(uv));
			use_um = false;
			goto rebuild;
		}
	}
	memset(&ls, 0, sizeof(ls));
	ls.path = sb.path;
	ls.argv = sb.argv;
	ls.envp = envp;
	ls.keep = keep;
	ls.nkeep = nkeep;
	ls.capture = false;
	ls.err_fd = cfg.child_stderr_fd;
	ls.drop = drop;
	ls.uid = run->console_uid;
	ls.gid = run->console_gid;
	ls.user_session = true; /* only the worker: it talks to the mount service of the user's session */

	st = launch(&ls, &pid, NULL);
	if (st != NTFSMAC_DMN_OK)
		return st;

	/*
	 * Wait for the mount. The worker runs in the foreground and does not
	 * return when the mount appears, so the wait polls the table and the
	 * worker's state with a deadline of its own, and never blocks on a call
	 * into the worker-served file system.
	 */
	deadline = now_ms() + (uint64_t)run->mount_wait_s * 1000u;
	for (;;) {
		bool found = false, ro = false;

		if (cfg.mount_lookup(run->mount_point, &found, &ro) == NTFSMAC_DMN_OK && found) {
			child->pid = pid;
			*mounted_ro = ro;
			return NTFSMAC_DMN_OK;
		}
		/* Has the worker exited? Peek without reaping, so that the group is
		 * still named by a live (zombie) leader when the kill goes out. */
		if (child_has_exited(pid)) {
			int wst;

			kill_group_of(pid);
			worker_gone(child, reap_bounded(pid, &wst, REAP_WAIT_MS) ? -1 : pid);
			return NTFSMAC_DMN_E_IO;
		}
		if (now_ms() >= deadline) {
			int wst;

			kill_group_of(pid);
			if (!reap_bounded(pid, &wst, REAP_WAIT_MS))
				child->pid = pid; /* stuck: the caller keeps polling */
			else
				worker_gone(child, -1); /* reaped: its pid may be reused */
			return NTFSMAC_DMN_E_TIMEOUT;
		}
		sleep_ms(cfg.mount_poll_ms);
	}
}

/* ------------------------------------------------------------------ */
/* Supervision                                                         */
/* ------------------------------------------------------------------ */

ntfsmac_dmn_status ntfsmac_child_poll(ntfsmac_child *child, bool *exited, int *exit_status)
{
	int wst;

	if (child == NULL || exited == NULL || exit_status == NULL)
		return NTFSMAC_DMN_E_INVAL;
	*exited = false;
	if (child->pid <= 1)
		return NTFSMAC_DMN_E_STATE;
	if (!child_has_exited(child->pid)) {
		/* Still running, or stopped; a pid that is not our child is an error. */
		if (kill(child->pid, 0) != 0 && errno == ESRCH)
			return NTFSMAC_DMN_E_IO;
		return NTFSMAC_DMN_OK;
	}
	/*
	 * The leader is a zombie: its group id is still reserved. Clear the group, then reap. A
	 * descendant that left the group is an orphan now and cannot be found through the tree; it
	 * is found by the descriptor it still holds on the device.
	 */
	(void)kill(-child->pid, SIGKILL);
	if (waitpid(child->pid, &wst, 0) != child->pid)
		return NTFSMAC_DMN_E_IO;
	*exited = true;
	*exit_status = exit_code_of(wst);
	child->pid = -1;
	worker_gone(child, -1); /* reaped: its pid may be reused */
	return NTFSMAC_DMN_OK;
}

ntfsmac_dmn_status ntfsmac_child_kill_group(ntfsmac_child *child)
{
	if (child == NULL)
		return NTFSMAC_DMN_E_INVAL;
	if (child->pid <= 1)
		return NTFSMAC_DMN_E_STATE;
	kill_tree(child->pid);
	/* kill_tree() has sent the kill already; a group that is only a zombie now answers ESRCH or EPERM. */
	if (kill(-child->pid, SIGKILL) != 0 && errno != ESRCH && errno != EPERM)
		return NTFSMAC_DMN_E_IO;
	return NTFSMAC_DMN_OK;
}

ntfsmac_dmn_status ntfsmac_child_device_check(const ntfsmac_child *child, unsigned *killed)
{
	dev_ident id;

	if (killed != NULL)
		*killed = 0;
	if (child == NULL)
		return NTFSMAC_DMN_E_INVAL;
	if (!child->dev_known)
		return NTFSMAC_DMN_E_STATE;
	id.dev = child->dev_dev;
	id.ino = child->dev_ino;
	return device_free(child->owner, &id, child->pid, killed);
}
