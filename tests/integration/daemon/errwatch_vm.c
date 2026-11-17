/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * T-M4-13: driver for the session channel watcher against the real worker in a
 * macOS guest (Environment B), run as root by errwatch_vm.sh. It uses the pipe,
 * the spawn module and the parser the way the daemon will (I-DMN-24 a, b; U16).
 *
 *   errwatch_vm IMAGE WORKER FAULTWRAPPER UID GID WORKDIR [CASE]
 *
 * IMAGE         an NTFS image file, mode 000 (the console user cannot open it)
 * WORKER        the patched ntfsmac-worker
 * FAULTWRAPPER  a script that runs the faultio worker with the plan in WORKDIR/plan
 * UID GID       the console user
 * WORKDIR       an empty directory made by the script
 *
 * Cases (each prints PASS, FAIL or NOTE lines; exit 0 only if none failed):
 *   clean   a rw session ends by unmount: the parser proves it clean; the descriptor table
 *           shows that only the worker holds the write end (U16)
 *   nochan  the same session without NTFSMAC_ERRFD: the end of the channel is EOF with no
 *           record, and the proof is false (I-DMN-24 b)
 *   fault   faultio worker, the first device write of the workload fails with EIO: an E
 *           record arrives, the proof is false (I-DMN-24 a)
 *
 * No path is opened inside the worker-served file system by this process: the user's file
 * test runs in a child that has dropped to the console user and is killed after 20 s.
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "spawn_config.h"

static int g_fails;
static uid_t g_uid;
static gid_t g_gid;
static const char *g_image, *g_worker, *g_wrapper, *g_dir;
static int g_log_fd = -1;

#define PASS(...) \
	do { printf("PASS: " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#define FAIL(...) \
	do { printf("FAIL: " __VA_ARGS__); printf("\n"); fflush(stdout); g_fails++; } while (0)
#define NOTE(...) \
	do { printf("NOTE: " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#define CHECK(cond, ...) \
	do { if (cond) PASS(__VA_ARGS__); else FAIL(__VA_ARGS__); } while (0)

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void sleep_ms(unsigned ms)
{
	struct timespec ts = { ms / 1000u, (long)(ms % 1000u) * 1000000L };

	nanosleep(&ts, NULL);
}

static void show_log_tail(void)
{
	char cmd[512];

	snprintf(cmd, sizeof(cmd), "tail -n 8 '%s/worker.log' | sed 's/^/  log: /'", g_dir);
	fflush(stdout);
	if (system(cmd) != 0) {
		/* only a diagnostic */
	}
}

static bool mounted_now(const char *mp)
{
	struct statfs *fs;
	int n, i;
	bool found = false;

	n = getfsstat(NULL, 0, MNT_NOWAIT);
	if (n <= 0)
		return false;
	fs = calloc((size_t)n + 8, sizeof(*fs));
	if (fs == NULL)
		return false;
	n = getfsstat(fs, (int)(((size_t)n + 8) * sizeof(*fs)), MNT_NOWAIT);
	for (i = 0; i < n; i++)
		if (strcmp(fs[i].f_mntonname, mp) == 0)
			found = true;
	free(fs);
	return found;
}

/* unmount(2) in a child so that a hung call cannot stall this process; 0 on success. */
static int unmount_bounded(const char *mp, int flags, unsigned limit_s)
{
	pid_t p = fork();
	int st;
	uint64_t end;

	if (p < 0)
		return -1;
	if (p == 0)
		_exit(unmount(mp, flags) == 0 ? 0 : (errno > 0 && errno < 200 ? errno : 1));
	end = now_ms() + limit_s * 1000u;
	for (;;) {
		pid_t r = waitpid(p, &st, WNOHANG);

		if (r == p)
			return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
		if (now_ms() >= end) {
			int i;

			kill(p, SIGKILL);
			for (i = 0; i < 100 && waitpid(p, &st, WNOHANG) != p; i++)
				sleep_ms(20);
			return -2;
		}
		sleep_ms(20);
	}
}

/* In a child as the console user: create, write 1 MiB, fsync and close a file in `mp`. */
static int user_write_test(const char *mp)
{
	pid_t p = fork();
	int st;
	uint64_t end;

	if (p < 0)
		return -1;
	if (p == 0) {
		char path[256], buf[4096];
		gid_t groups[1] = { g_gid };
		int fd, i;

		if (setgroups(1, groups) != 0 || setgid(g_gid) != 0 || setuid(g_uid) != 0)
			_exit(10);
		memset(buf, 'e', sizeof(buf));
		snprintf(path, sizeof(path), "%s/errwatch_vm.bin", mp);
		fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0)
			_exit(11);
		for (i = 0; i < 256; i++)
			if (write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf))
				_exit(12);
		if (fsync(fd) != 0)
			_exit(13);
		if (close(fd) != 0)
			_exit(14);
		_exit(0);
	}
	end = now_ms() + 20000u;
	for (;;) {
		pid_t r = waitpid(p, &st, WNOHANG);

		if (r == p)
			return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
		if (now_ms() >= end) {
			int i;

			kill(p, SIGKILL);
			for (i = 0; i < 100 && waitpid(p, &st, WNOHANG) != p; i++)
				sleep_ms(20);
			return -2;
		}
		sleep_ms(20);
	}
}

/*
 * U16: which processes hold the other end of our read end? On macOS `lsof -F pfdn` gives a pipe
 * end its own address in field d and the address of the other end in field n ("->0xADDR").
 * Our read end has address A; the entries whose n is "->A" are the holders of the write end.
 */
#define HOLDERS_MAX 16
#define LSOF_ENTRIES_MAX 4096
static int pipe_peer_holders(int rd, pid_t *holders, int cap)
{
	FILE *f = popen("/usr/sbin/lsof -n -w -F pfdn 2>/dev/null", "r");
	static char dn[LSOF_ENTRIES_MAX][2][40];
	static int pids[LSOF_ENTRIES_MAX];
	char line[512], mine[40] = "", cur_d[40] = "";
	int n = 0, i, count = 0, cur_fd = -1;
	pid_t cur = -1;

	if (f == NULL)
		return -1;
	while (fgets(line, sizeof(line), f) != NULL) {
		line[strcspn(line, "\n")] = '\0';
		if (line[0] == 'p') {
			cur = (pid_t)atoi(line + 1);
		} else if (line[0] == 'f') {
			cur_fd = atoi(line + 1);
			cur_d[0] = '\0';
		} else if (line[0] == 'd') {
			snprintf(cur_d, sizeof(cur_d), "%s", line + 1);
		} else if (line[0] == 'n' && strncmp(line + 1, "->0x", 4) == 0 && n < LSOF_ENTRIES_MAX) {
			if (cur == getpid() && cur_fd == rd)
				snprintf(mine, sizeof(mine), "%s", cur_d);
			pids[n] = (int)cur;
			snprintf(dn[n][0], sizeof(dn[n][0]), "%s", line + 3); /* the peer address */
			n++;
		}
	}
	pclose(f);
	if (mine[0] == '\0')
		return -1;
	for (i = 0; i < n; i++)
		if (strcmp(dn[i][0], mine) == 0 && count < cap)
			holders[count++] = (pid_t)pids[i];
	return count;
}

static void channel_line(const ntfsmac_errwatch *w, char *out, size_t cap)
{
	snprintf(out, cap, "flags 0x%x (S_RW %d S_RO %d E %d W %d U %d U0 %d PROTO %d EOF %d), %u records",
		 (unsigned)w->flags, !!(w->flags & NTFSMAC_EW_SAW_S_RW), !!(w->flags & NTFSMAC_EW_SAW_S_RO),
		 !!(w->flags & NTFSMAC_EW_SAW_E), !!(w->flags & NTFSMAC_EW_SAW_W),
		 !!(w->flags & NTFSMAC_EW_SAW_U), !!(w->flags & NTFSMAC_EW_SAW_U0),
		 !!(w->flags & NTFSMAC_EW_PROTOCOL_ERROR), !!(w->flags & NTFSMAC_EW_EOF), w->records);
}

static bool read_state(const char *path, long *out)
{
	FILE *f = fopen(path, "r");
	bool ok = false;

	if (f != NULL) {
		ok = fscanf(f, "%ld", out) == 1;
		fclose(f);
	}
	return ok;
}

typedef struct case_result {
	ntfsmac_errwatch w;
	bool proof;
	bool mounted;
	int exit_status;
	long writes_at_mount; /* faultio state file, when used */
} case_result;

/*
 * One session: the daemon's sequence of section 4.5 as far as this module goes. Spawn with the
 * channel (or without), user workload, unmount, worker exit, drain to EOF, proof.
 */
static void run_session(const char *tag, const char *worker, bool channel, bool workload,
			bool check_holders, case_result *res)
{
	ntfsmac_spawn_config c;
	ntfsmac_worker_run run;
	ntfsmac_child child;
	char mp[128], state[300];
	int dev, rd = -1, wr = -1, rc, i;
	bool mounted_ro = true, exited = false;
	ntfsmac_dmn_status st;
	pid_t holders[HOLDERS_MAX];

	memset(res, 0, sizeof(*res));
	ntfsmac_errwatch_init(&res->w);
	res->exit_status = -1;
	ntfsmac_spawn_config_default(&c);
	c.worker_path = worker;
	c.child_stderr_fd = g_log_fd;
	ntfsmac_spawn_config_set(&c);
	snprintf(mp, sizeof(mp), "/Volumes/errwatchvm%s", tag);
	snprintf(state, sizeof(state), "%s/fault-state", g_dir);
	if (mkdir(mp, 0700) != 0 || chown(mp, g_uid, g_gid) != 0) {
		FAIL("%s: cannot make %s: %s", tag, mp, strerror(errno));
		return;
	}
	dev = open(g_image, O_RDWR | O_CLOEXEC);
	if (dev < 0 || ntfsmac_errwatch_pipe_open(&rd, &wr) != NTFSMAC_DMN_OK) {
		FAIL("%s: cannot open the image or make the channel", tag);
		rmdir(mp);
		return;
	}
	memset(&run, 0, sizeof(run));
	run.device_fd = dev;
	run.errfd_write = channel ? wr : -1;
	run.mount_point = mp;
	run.read_only = false;
	run.console_uid = g_uid;
	run.console_gid = g_gid;
	run.mount_wait_s = 30;
	st = ntfsmac_spawn_worker(&run, &child, &mounted_ro);
	close(wr); /* the daemon keeps only the read end */
	CHECK(st == NTFSMAC_DMN_OK && !mounted_ro, "%s: the rw mount appeared (%s)", tag, ntfsmac_dmn_status_name(st));
	if (st != NTFSMAC_DMN_OK) {
		show_log_tail();
		close(dev);
		close(rd);
		rmdir(mp);
		return;
	}
	res->mounted = true;
	/* S rw arrives after the mount; the pump is how the daemon sees it while the worker runs. */
	for (i = 0; i < 40 && channel && (res->w.flags & NTFSMAC_EW_SAW_S_RW) == 0u; i++) {
		(void)ntfsmac_errwatch_pump(&res->w, rd);
		sleep_ms(50);
	}
	if (check_holders) {
		int n = pipe_peer_holders(rd, holders, HOLDERS_MAX), k;

		printf("  write-end holders:");
		for (k = 0; k < n; k++)
			printf(" %d", (int)holders[k]);
		printf(" (worker %d)\n", (int)child.pid);
		CHECK(n == 1 && holders[0] == child.pid, "%s: only the worker holds the write end (U16)", tag);
		CHECK(fcntl(wr, F_GETFD) == -1 && errno == EBADF, "%s: the daemon's copy of the write end is closed", tag);
	}
	if (read_state(state, &res->writes_at_mount))
		NOTE("%s: device writes at mount time: %ld", tag, res->writes_at_mount);
	if (workload) {
		rc = user_write_test(mp);
		NOTE("%s: the user's write test returned %d", tag, rc);
		(void)ntfsmac_errwatch_pump(&res->w, rd);
	}

	rc = unmount_bounded(mp, 0, 30);
	CHECK(rc == 0, "%s: unmount(2) (code %d)", tag, rc);
	if (rc != 0)
		NOTE("%s: forced unmount gave %d", tag, unmount_bounded(mp, MNT_FORCE, 30));
	for (i = 0; i < 600 && !exited; i++) {
		if (ntfsmac_child_poll(&child, &exited, &res->exit_status) != NTFSMAC_DMN_OK)
			break;
		if (!exited)
			sleep_ms(50);
	}
	CHECK(exited, "%s: the worker exited (status %d)", tag, res->exit_status);
	if (!exited)
		(void)ntfsmac_child_kill_group(&child);
	{
		uint64_t t0 = now_ms();

		st = ntfsmac_errwatch_drain(&res->w, rd, NTFSMAC_ERRWATCH_EOF_WAIT_MS);
		CHECK(st == NTFSMAC_DMN_OK, "%s: end-of-file within %d ms of the exit (%s, %llu ms)", tag,
		      NTFSMAC_ERRWATCH_EOF_WAIT_MS, ntfsmac_dmn_status_name(st), (unsigned long long)(now_ms() - t0));
	}
	res->proof = ntfsmac_errwatch_clean_proof(&res->w, true, false, exited ? res->exit_status : -1);
	{
		char line[200];

		channel_line(&res->w, line, sizeof(line));
		printf("  channel: %s\n", line);
	}
	CHECK(!mounted_now(mp), "%s: the mount is gone", tag);
	rmdir(mp);
	close(dev);
	close(rd);
	show_log_tail();
}

static void case_clean(void)
{
	case_result r;

	printf("== clean: rw session, workload, unmount (U16, I-DMN-24 happy path)\n");
	run_session("clean", g_worker, true, true, true, &r);
	CHECK(r.mounted && r.proof, "clean: the positive proof holds (exit status %d)", r.exit_status);
	CHECK((r.w.flags & NTFSMAC_EW_PROTOCOL_ERROR) == 0u && (r.w.flags & NTFSMAC_EW_SAW_E) == 0u,
	      "clean: no protocol error and no E record");
	if ((r.w.flags & NTFSMAC_EW_SAW_W) != 0u)
		NOTE("clean: a W record arrived (the report changes, the proof does not)");
}

static void case_nochan(void)
{
	case_result r;

	printf("== nochan: the worker without NTFSMAC_ERRFD (I-DMN-24 b)\n");
	run_session("nochan", g_worker, false, false, false, &r);
	CHECK(r.mounted && !r.proof, "nochan: silence is not a clean session");
	CHECK((r.w.flags & NTFSMAC_EW_EOF) != 0u && r.w.records == 0u, "nochan: EOF and no record");
}

/* Empty the counter file of faultio (the worker, a different user, owns no right to create it). */
static void reset_state(void)
{
	char path[300];
	FILE *f;

	snprintf(path, sizeof(path), "%s/fault-state", g_dir);
	f = fopen(path, "w");
	if (f != NULL)
		fclose(f);
}

static void case_fault(void)
{
	case_result count, r;
	char path[300];
	FILE *f;

	printf("== fault: first device write of the workload fails with EIO (I-DMN-24 a)\n");
	reset_state();
	snprintf(path, sizeof(path), "%s/plan", g_dir);
	f = fopen(path, "w");
	if (f == NULL) {
		FAIL("fault: cannot write the plan");
		return;
	}
	fputs("count\n", f);
	fclose(f);
	run_session("faultcount", g_wrapper, true, false, false, &count);
	CHECK(count.mounted && count.writes_at_mount > 0, "fault: the count run saw %ld device writes at mount",
	      count.writes_at_mount);
	CHECK(count.proof, "fault: a faultio worker without a fault still ends clean");
	reset_state();
	snprintf(path, sizeof(path), "%s/plan", g_dir);
	f = fopen(path, "w");
	if (f == NULL) {
		FAIL("fault: cannot write the plan");
		return;
	}
	fprintf(f, "eio:%ld\n", count.writes_at_mount + 1);
	fclose(f);
	run_session("faulteio", g_wrapper, true, true, false, &r);
	CHECK(r.mounted, "fault: the session started with eio:%ld", count.writes_at_mount + 1);
	CHECK((r.w.flags & NTFSMAC_EW_SAW_E) != 0u, "fault: an E record arrived");
	CHECK((r.w.flags & NTFSMAC_EW_PROTOCOL_ERROR) == 0u, "fault: the records are within the grammar");
	CHECK(!r.proof, "fault: the session is not clean");
}

int main(int argc, char **argv)
{
	/* The worker's start runs this program once more as `--exec-as` inside the user's session. */
	if (argc >= 2 && strcmp(argv[1], NTFSMAC_EXEC_AS_FLAG) == 0)
		return ntfsmac_spawn_exec_as(argc, argv);
	char path[300];
	const char *only;

	if (argc < 7) {
		fprintf(stderr, "usage: errwatch_vm IMAGE WORKER FAULTWRAPPER UID GID WORKDIR [CASE]\n");
		return 2;
	}
	g_image = argv[1];
	g_worker = argv[2];
	g_wrapper = argv[3];
	g_uid = (uid_t)strtoul(argv[4], NULL, 10);
	g_gid = (gid_t)strtoul(argv[5], NULL, 10);
	g_dir = argv[6];
	only = argc > 7 ? argv[7] : NULL;
	if (geteuid() != 0 || g_uid == 0) {
		fprintf(stderr, "errwatch_vm: run as root, with a console user that is not root\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGPIPE, SIG_IGN); /* as the daemon does */
	snprintf(path, sizeof(path), "%s/worker.log", g_dir);
	g_log_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);

	if (only == NULL || !strcmp(only, "clean"))
		case_clean();
	if (only == NULL || !strcmp(only, "nochan"))
		case_nochan();
	if (only == NULL || !strcmp(only, "fault"))
		case_fault();
	printf("== result: %d failure(s)\n", g_fails);
	return g_fails == 0 ? 0 : 1;
}
