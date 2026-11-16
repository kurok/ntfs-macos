/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Spawn and supervision of the probe, volmark and the worker (T-M4-01).
 * ARCHITECTURE.md sections 3.1, 3.3, 3.6, 4.1 (steps 4, 7, 8), 5 and 6.
 *
 * Plain POSIX plus two small macOS parts (getfsstat and the spawn flag
 * POSIX_SPAWN_CLOEXEC_DEFAULT). No Disk Arbitration and no CoreFoundation.
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

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__)
#include <sys/mount.h>
#endif
#include <grp.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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
		close(*fd);
		*fd = -1;
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
		close(fd);
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
		close(p[0]);
		close(p[1]);
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

/*
 * SIGKILL to the whole group of `pid` (the child is the group leader) and to
 * the child itself, in case the group could not be set up. Never kills pid 0
 * or 1, nor a negative number: `kill(-1)` would hit every process.
 */
static void kill_group_of(pid_t pid)
{
	if (pid <= 1)
		return;
	(void)kill(-pid, SIGKILL);
	(void)kill(pid, SIGKILL);
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

static ntfsmac_dmn_status launch(const launch_spec *s, pid_t *pid_out, int *capture_rd)
{
	int devnull = -1, outp[2] = { -1, -1 }, stp[2] = { -1, -1 };
	int err_fd = -1;
	pid_t pid;
	size_t i;
	int e = 0;
	ssize_t n;
	uint64_t end;
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
		if (dup2(devnull, 0) < 0 || dup2(s->capture ? outp[1] : devnull, 1) < 0
		    || dup2(err_fd, 2) < 0) {
			dummy = errno;
			goto child_fail;
		}
		if (s->drop && drop_privileges(s->uid, s->gid) != 0) {
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
		dummy = posix_spawn(NULL, s->path, &fa, &attr, s->argv, s->envp);
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
			execve(s->path, s->argv, s->envp);
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
	memset(&ls, 0, sizeof(ls));
	ls.path = cfg.probe_path;
	ls.argv = argv;
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

ntfsmac_dmn_status ntfsmac_spawn_volmark(const ntfsmac_volmark_run *run, bool set,
					 int *exit_code)
{
	ntfsmac_spawn_config cfg;
	ntfsmac_dmn_status st;
	launch_spec ls;
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
	argv[1] = (char *)(set ? "set" : "clear");
	argv[2] = (char *)"--fd";
	argv[3] = fdarg;
	argv[4] = (char *)"--expect-serial";
	argv[5] = serial;
	argv[6] = (char *)"--expect-sectors";
	argv[7] = sectors;
	argv[8] = NULL;
	keep[0] = run->device_fd;
	memset(&ls, 0, sizeof(ls));
	ls.path = cfg.volmark_path;
	ls.argv = argv;
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
	(void)reap_bounded(pid, &wst, REAP_WAIT_MS);
	if (rc == 2) {
		*exit_code = w.saw_w ? -2 : -1;
		return NTFSMAC_DMN_E_TIMEOUT;
	}
	return NTFSMAC_DMN_E_IO;
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

/* The check of ARCHITECTURE.md section 3.3 before a spawn with `usermapping=`. */
static bool usermapping_file_ok(void)
{
	struct stat st;

	if (lstat(NTFSMAC_USERMAPPING_PATH, &st) != 0)
		return false;
	return S_ISREG(st.st_mode) && st.st_uid == 0 && (st.st_mode & 07777) == 0644;
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
	char errenv[40];
	char *envp[3];
	char *argv[NTFSMAC_WORKER_ARGC + 1];
	int keep[2];
	size_t nkeep = 0, i;
	const char *leaf;
	pid_t pid;
	bool drop = false;
	uint64_t deadline;

	if (child == NULL || mounted_ro == NULL)
		return NTFSMAC_DMN_E_INVAL;
	child->pid = -1;
	child->status_fd = -1;
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
	in.usermapping = cfg.usermapping;
	if (cfg.usermapping && !usermapping_file_ok())
		return NTFSMAC_DMN_E_DENIED;
	if (ntfsmac_options_build(&in, &av) != NTFSMAC_OPTIONS_OK)
		return NTFSMAC_DMN_E_DENIED;

	memset(&expect, 0, sizeof(expect));
	expect.worker_path = cfg.worker_path;
	expect.leaf = leaf;
	expect.decision = decision;
	expect.console_uid = (uint32_t)run->console_uid;
	expect.console_gid = (uint32_t)run->console_gid;
	if (ntfsmac_argv_check(av.argv, NTFSMAC_WORKER_ARGC, &expect) != NTFSMAC_ARGV_OK)
		return NTFSMAC_DMN_E_DENIED;
	if (cfg.after_check_hook != NULL)
		cfg.after_check_hook(&av);

	for (i = 0; i < NTFSMAC_WORKER_ARGC; i++)
		argv[i] = (char *)av.argv[i];
	argv[NTFSMAC_WORKER_ARGC] = NULL;
	/* A fixed, small environment: nothing of the daemon's own reaches the worker. */
	envp[0] = (char *)"PATH=/usr/bin:/bin";
	envp[1] = NULL;
	envp[2] = NULL;
	keep[nkeep++] = run->device_fd;
	if (run->errfd_write >= 0) {
		snprintf(errenv, sizeof(errenv), "NTFSMAC_ERRFD=%d", run->errfd_write);
		envp[1] = errenv;
		keep[nkeep++] = run->errfd_write;
	}

	memset(&ls, 0, sizeof(ls));
	ls.path = cfg.worker_path;
	ls.argv = argv;
	ls.envp = envp;
	ls.keep = keep;
	ls.nkeep = nkeep;
	ls.capture = false;
	ls.err_fd = cfg.child_stderr_fd;
	ls.drop = drop;
	ls.uid = run->console_uid;
	ls.gid = run->console_gid;

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
			(void)reap_bounded(pid, &wst, REAP_WAIT_MS);
			return NTFSMAC_DMN_E_IO;
		}
		if (now_ms() >= deadline) {
			int wst;

			kill_group_of(pid);
			if (!reap_bounded(pid, &wst, REAP_WAIT_MS))
				child->pid = pid; /* stuck: the caller keeps polling */
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
	/* The leader is a zombie: its group id is still reserved. Clear the group, then reap. */
	(void)kill(-child->pid, SIGKILL);
	if (waitpid(child->pid, &wst, 0) != child->pid)
		return NTFSMAC_DMN_E_IO;
	*exited = true;
	*exit_status = exit_code_of(wst);
	child->pid = -1;
	return NTFSMAC_DMN_OK;
}

ntfsmac_dmn_status ntfsmac_child_kill_group(ntfsmac_child *child)
{
	if (child == NULL)
		return NTFSMAC_DMN_E_INVAL;
	if (child->pid <= 1)
		return NTFSMAC_DMN_E_STATE;
	if (kill(-child->pid, SIGKILL) != 0 && errno != ESRCH)
		return NTFSMAC_DMN_E_IO;
	return NTFSMAC_DMN_OK;
}
