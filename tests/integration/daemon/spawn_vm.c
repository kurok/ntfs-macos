/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * T-M4-01: driver for the spawn module in a macOS guest (Environment B),
 * run as root by spawn_vm.sh. It uses the module the way the daemon will.
 *
 *   spawn_vm [--sandbox DIR] IMAGE WORKER STUB UID GID WORKDIR [CASE]
 *
 * IMAGE   an NTFS image file, mode 000 (the console user cannot open it)
 * WORKER  the patched ntfsmac-worker (ntfs-3g built with the patch series)
 * STUB    spawn_stub (tests/unit/spawn), stands in for the probe and volmark
 * UID GID the console user
 * WORKDIR an empty directory made by the script
 * DIR     with --sandbox: a root-owned directory with the profiles of integration/sandbox/; the
 *         children then start under them (T-M4-06) and WORKER and STUB must be given by real
 *         paths, which is what a profile names
 *
 * Cases (each prints PASS or FAIL lines; exit 0 only if none failed):
 *   priv       I-DMN-09: probe and volmark start as `nobody`, the worker as the console user
 *   rw, ro     U6, I-DMN-09: the worker, started by a root process with an inherited descriptor,
 *              mounts rw and ro, runs as the console user in its own process group, the user
 *              can write (rw) and read, the channel records arrive, the unmount ends the worker
 *   u20        U20: while the worker is stopped (SIGSTOP), getfsstat(MNT_NOWAIT) lists the mount
 *              and stays fast for 30 s
 *   stop       I-DMN-26 b: worker stopped right after the spawn; the wait ends at its deadline,
 *              the process group is killed, nothing is left
 *   nofskit    I-DMN-26 c: `backend=fskit` dropped after argv_check(); the result is recorded
 *
 * No path is ever opened inside the worker-served file system by this process: the user's
 * file test runs in a child that has dropped to the console user and is killed after 20 s.
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
static const char *g_image, *g_worker, *g_stub, *g_dir;
static int g_log_fd = -1;
static const char *g_sandbox_dir; /* --sandbox DIR: run the children under the profiles in DIR */

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

static ntfsmac_spawn_config base_config(void)
{
	ntfsmac_spawn_config c;

	ntfsmac_spawn_config_default(&c);
	c.sandbox_dir = g_sandbox_dir; /* NULL unless --sandbox: the spawn module alone */
	c.worker_path = g_worker;
	c.child_stderr_fd = g_log_fd;
	return c;
}

static void show_log_tail(void)
{
	char cmd[512];

	snprintf(cmd, sizeof(cmd), "tail -n 12 '%s/worker.log' | sed 's/^/  log: /'", g_dir);
	fflush(stdout);
	if (system(cmd) != 0) {
		/* only a diagnostic */
	}
}

static void show_mount(const char *mp)
{
	char cmd[512];

	snprintf(cmd, sizeof(cmd), "/sbin/mount | grep -F ' on %s (' | sed 's/^/  mount: /'", mp);
	fflush(stdout);
	if (system(cmd) != 0)
		printf("  mount: no line for %s\n", mp);
}

/* ps -o columns for one pid, printed on one line; fills `out`. */
static void ps_line(pid_t pid, const char *cols, char *out, size_t cap)
{
	char cmd[128];
	FILE *f;

	out[0] = '\0';
	snprintf(cmd, sizeof(cmd), "/bin/ps -o %s= -p %d", cols, (int)pid);
	f = popen(cmd, "r");
	if (f == NULL)
		return;
	if (fgets(out, (int)cap, f) == NULL)
		out[0] = '\0';
	pclose(f);
	out[strcspn(out, "\n")] = '\0';
}

/* The mount point: made as root, handed to the console user (the daemon does this in T-M4-03). */
static int make_mountpoint(const char *path)
{
	if (mkdir(path, 0700) != 0)
		return -1;
	if (chown(path, g_uid, g_gid) != 0)
		return -1;
	return 0;
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
			/* A call stuck in the kernel may not die; give up after 2 s and leave it. */
			int i;

			kill(p, SIGKILL);
			for (i = 0; i < 100 && waitpid(p, &st, WNOHANG) != p; i++)
				sleep_ms(20);
			return -2;
		}
		sleep_ms(20);
	}
}

/* A plain unmount right after a mount can meet EBUSY for a moment while macOS services
 * (Spotlight, fseventsd) look at the new volume; seen once in the full local CI on a read-only
 * mount that was unmounted at once, and not in 4 reruns. The check is that a plain unmount
 * works, so retry it on EBUSY for up to 10 s, noting each retry, before calling it a failure. */
static int unmount_plain(const char *mp, const char *tag)
{
	int rc = unmount_bounded(mp, 0, 30), tries;

	for (tries = 1; rc == EBUSY && tries < 10; tries++) {
		NOTE("%s: unmount(2) busy (EBUSY), retry %d after 1 s", tag, tries);
		sleep_ms(1000);
		rc = unmount_bounded(mp, 0, 30);
	}
	return rc;
}

/* In a child as the console user: write and read back a file in `mp`; 0 if it worked. */
static int user_file_test(const char *mp, bool write_it)
{
	pid_t p = fork();
	int st;
	uint64_t end;

	if (p < 0)
		return -1;
	if (p == 0) {
		char path[256], buf[64];
		gid_t groups[1] = { g_gid };
		int fd;
		ssize_t n;

		if (setgroups(1, groups) != 0 || setgid(g_gid) != 0 || setuid(g_uid) != 0)
			_exit(10);
		snprintf(path, sizeof(path), "%s/spawn_vm.txt", mp);
		if (write_it) {
			fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (fd < 0 || write(fd, "spawn-vm-payload\n", 17) != 17 || fsync(fd) != 0)
				_exit(11);
			close(fd);
		}
		fd = open(path, O_RDONLY);
		if (fd < 0)
			_exit(12);
		n = read(fd, buf, sizeof(buf));
		close(fd);
		if (n != 17 || memcmp(buf, "spawn-vm-payload\n", 17) != 0)
			_exit(13);
		if (!write_it) {
			/* a read-only mount must refuse a write */
			snprintf(path, sizeof(path), "%s/spawn_vm_ro.txt", mp);
			fd = open(path, O_WRONLY | O_CREAT, 0644);
			if (fd >= 0)
				_exit(14);
		}
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

static int open_image(int flags)
{
	int fd = open(g_image, flags | O_CLOEXEC);

	if (fd < 0)
		printf("open %s: %s\n", g_image, strerror(errno));
	return fd;
}

static void read_channel(int rfd, char *out, size_t cap)
{
	size_t len = 0;
	ssize_t n;

	while (len + 1 < cap && (n = read(rfd, out + len, cap - 1 - len)) > 0)
		len += (size_t)n;
	out[len] = '\0';
}

/* ---------------------------------------------------------------- */

static void case_priv(void)
{
	ntfsmac_spawn_config c;
	ntfsmac_probe_run pr;
	ntfsmac_probe_verdict v;
	ntfsmac_volmark_run vr;
	char path[256], line[128];
	int fd, code = 0;
	FILE *f;
	unsigned u = 1, g = 1;
	ntfsmac_dmn_status st;

	printf("== priv: probe and volmark as nobody (I-DMN-09)\n");
	snprintf(path, sizeof(path), "%s/priv-device", g_dir);
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0 || write(fd, "ABCDEFGH", 8) != 8) {
		FAIL("priv: cannot make the stand-in device");
		return;
	}
	snprintf(path, sizeof(path), "%s/priv.log", g_dir);
	c = base_config();
	c.child_stderr_fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	snprintf(path, sizeof(path), "%s/probe-ok", g_dir);
	(void)unlink(path);
	if (link(g_stub, path) != 0)
		FAIL("link probe-ok");
	c.probe_path = strdup(path);
	snprintf(path, sizeof(path), "%s/volmark-set", g_dir);
	(void)unlink(path);
	if (link(g_stub, path) != 0)
		FAIL("link volmark-set");
	c.volmark_path = strdup(path);
	ntfsmac_spawn_config_set(&c);

	pr.device_fd = fd;
	pr.timeout_s = 10;
	st = ntfsmac_spawn_probe(&pr, &v);
	CHECK(st == NTFSMAC_DMN_OK, "priv: probe stand-in ran (status %s); its checks of descriptors, group and signals held",
	      ntfsmac_dmn_status_name(st));
	vr.device_fd = fd;
	vr.serial = 0x12AB;
	vr.total_sectors = 2048;
	vr.timeout_s = 10;
	st = ntfsmac_spawn_volmark(&vr, true, &code);
	CHECK(st == NTFSMAC_DMN_OK, "priv: volmark stand-in ran (status %s, code %d)", ntfsmac_dmn_status_name(st), code);

	snprintf(path, sizeof(path), "%s/priv.log", g_dir);
	f = fopen(path, "r");
	if (f != NULL) {
		int n = 0;

		while (fgets(line, sizeof(line), f) != NULL) {
			if (sscanf(line, "uid=%u gid=%u", &u, &g) == 2) {
				n++;
				CHECK(u != 0 && g != 0, "priv: child %d ran as uid %u gid %u (not root)", n, u, g);
				CHECK(u == (unsigned)c.unpriv_uid && g == (unsigned)c.unpriv_gid,
				      "priv: child %d is the configured unprivileged user (uid %u)", n, u);
			}
		}
		fclose(f);
		CHECK(n == 2, "priv: both stand-ins reported their user (%d)", n);
	} else {
		FAIL("priv: no log");
	}
	close(fd);
	close(c.child_stderr_fd);
}

static void report_exit(ntfsmac_child *child, const char *what)
{
	bool exited = false;
	int status = 0, i;

	for (i = 0; i < 600 && !exited; i++) {
		if (ntfsmac_child_poll(child, &exited, &status) != NTFSMAC_DMN_OK)
			break;
		if (!exited)
			sleep_ms(50);
	}
	if (exited)
		PASS("%s: worker exited with status %d", what, status);
	else {
		FAIL("%s: worker still running 30 s after the unmount", what);
		(void)ntfsmac_child_kill_group(child);
	}
}

static void case_mount(bool read_only)
{
	const char *name = read_only ? "ro" : "rw";
	ntfsmac_spawn_config c = base_config();
	ntfsmac_worker_run run;
	ntfsmac_child child;
	char mp[128], leaf[64], ps[160], rec[256];
	int dev, ep[2], rc;
	bool mounted_ro = !read_only;
	uint64_t t0;
	ntfsmac_dmn_status st;

	printf("== %s: worker as the console user from a root process (U6, I-DMN-09)\n", name);
	snprintf(leaf, sizeof(leaf), "spawnvm%s", name);
	snprintf(mp, sizeof(mp), "/Volumes/%s", leaf);
	ntfsmac_spawn_config_set(&c);
	if (make_mountpoint(mp) != 0) {
		FAIL("%s: cannot make %s: %s", name, mp, strerror(errno));
		return;
	}
	dev = open_image(read_only ? O_RDONLY : O_RDWR);
	if (dev < 0 || pipe(ep) != 0) {
		FAIL("%s: cannot open the image or make the channel", name);
		rmdir(mp);
		return;
	}
	fcntl(ep[0], F_SETFD, FD_CLOEXEC);
	fcntl(ep[1], F_SETFD, FD_CLOEXEC);
	fcntl(ep[0], F_SETFL, O_NONBLOCK);

	memset(&run, 0, sizeof(run));
	run.device_fd = dev;
	run.errfd_write = ep[1];
	run.mount_point = mp;
	run.read_only = read_only;
	run.console_uid = g_uid;
	run.console_gid = g_gid;
	run.mount_wait_s = 30;
	t0 = now_ms();
	st = ntfsmac_spawn_worker(&run, &child, &mounted_ro);
	printf("  spawn_worker: %s after %llu ms, mounted_ro=%d\n", ntfsmac_dmn_status_name(st),
	       (unsigned long long)(now_ms() - t0), (int)mounted_ro);
	CHECK(st == NTFSMAC_DMN_OK, "%s: the mount appeared", name);
	if (st != NTFSMAC_DMN_OK) {
		show_log_tail();
		close(dev);
		close(ep[0]);
		close(ep[1]);
		rmdir(mp);
		return;
	}
	CHECK(mounted_ro == read_only, "%s: MNT_RDONLY of the mount (%d) matches the decision", name, (int)mounted_ro);
	show_mount(mp);
	ps_line(child.pid, "uid=,pgid=,command", ps, sizeof(ps));
	printf("  worker: pid %d: %s\n", (int)child.pid, ps);
	{
		unsigned u = 0;
		int pg = 0;

		if (sscanf(ps, "%u %d", &u, &pg) == 2) {
			CHECK(u == (unsigned)g_uid, "%s: the worker runs as the console user (uid %u)", name, u);
			CHECK(pg == (int)child.pid, "%s: the worker leads its own process group", name);
		} else {
			FAIL("%s: could not read the worker's uid and group", name);
		}
	}
	rc = user_file_test(mp, !read_only);
	CHECK(rc == 0, "%s: the console user %s through the mount (code %d)", name,
	      read_only ? "reads the file of the rw case and cannot write" : "writes and reads a file", rc);
	read_channel(ep[0], rec, sizeof(rec));
	{
		char *p;

		for (p = rec; *p; p++)
			if (*p == '\n')
				*p = '|';
	}
	printf("  channel so far: %s\n", rec);
	CHECK(strncmp(rec, read_only ? "S ro|" : "S rw|", 5) == 0, "%s: the first record is S %s", name, name);

	rc = unmount_plain(mp, name);
	CHECK(rc == 0, "%s: unmount(2) (code %d)", name, rc);
	if (rc != 0) {
		rc = unmount_bounded(mp, MNT_FORCE, 30);
		NOTE("%s: forced unmount gave %d", name, rc);
	}
	report_exit(&child, name);
	read_channel(ep[0], rec + strlen(rec), sizeof(rec) - strlen(rec));
	{
		char *p;

		for (p = rec; *p; p++)
			if (*p == '\n')
				*p = '|';
	}
	printf("  channel at the end: %s\n", rec);
	if (!read_only)
		CHECK(strstr(rec, "U 0|") != NULL && strstr(rec, "E ") == NULL,
		      "%s: records show a clean end (S rw ... U 0, no E)", name);
	CHECK(!mounted_now(mp), "%s: the mount is gone", name);
	if (rmdir(mp) != 0)
		NOTE("%s: rmdir %s: %s", name, mp, strerror(errno));
	close(dev);
	close(ep[0]);
	close(ep[1]);
	show_log_tail();
}

/* U20: the worker stopped after the mount; the table still answers, fast. */
static void case_u20(void)
{
	ntfsmac_spawn_config c = base_config();
	ntfsmac_worker_run run;
	ntfsmac_child child;
	char mp[] = "/Volumes/spawnvmu20";
	int dev, ep[2], rc, calls = 0, listed = 0;
	bool mounted_ro = false;
	uint64_t t0, end, max_us = 0;
	ntfsmac_dmn_status st;

	printf("== u20: getfsstat(MNT_NOWAIT) while the worker is stopped (U20)\n");
	ntfsmac_spawn_config_set(&c);
	if (make_mountpoint(mp) != 0) {
		FAIL("u20: cannot make %s", mp);
		return;
	}
	dev = open_image(O_RDONLY);
	if (dev < 0 || pipe(ep) != 0) {
		FAIL("u20: cannot open the image or make the channel");
		rmdir(mp);
		return;
	}
	fcntl(ep[0], F_SETFD, FD_CLOEXEC);
	fcntl(ep[1], F_SETFD, FD_CLOEXEC);
	memset(&run, 0, sizeof(run));
	run.device_fd = dev;
	run.errfd_write = ep[1];
	run.mount_point = mp;
	run.read_only = true;
	run.console_uid = g_uid;
	run.console_gid = g_gid;
	run.mount_wait_s = 30;
	t0 = now_ms();
	st = ntfsmac_spawn_worker(&run, &child, &mounted_ro);
	CHECK(st == NTFSMAC_DMN_OK, "u20: the mount appeared after %llu ms (the wait polls getfsstat)",
	      (unsigned long long)(now_ms() - t0));
	if (st != NTFSMAC_DMN_OK) {
		show_log_tail();
		close(dev);
		rmdir(mp);
		return;
	}
	kill(child.pid, SIGSTOP);
	NOTE("u20: SIGSTOP sent to pid %d; polling for 30 s", (int)child.pid);
	end = now_ms() + 30000u;
	while (now_ms() < end) {
		uint64_t us;
		struct timespec s0, s1;

		clock_gettime(CLOCK_MONOTONIC, &s0);
		if (mounted_now(mp))
			listed++;
		clock_gettime(CLOCK_MONOTONIC, &s1);
		us = (uint64_t)(s1.tv_sec - s0.tv_sec) * 1000000u + (uint64_t)((s1.tv_nsec - s0.tv_nsec) / 1000);
		if (us > max_us)
			max_us = us;
		calls++;
		sleep_ms(250);
	}
	CHECK(listed == calls, "u20: the mount was listed in %d of %d calls while the worker was stopped", listed, calls);
	CHECK(max_us < 200000u, "u20: the slowest getfsstat call took %llu us (limit 200000)", (unsigned long long)max_us);
	/* A stopped worker still dies to SIGKILL, and the dead mount needs the forced unmount. */
	(void)ntfsmac_child_kill_group(&child);
	report_exit(&child, "u20");
	rc = unmount_bounded(mp, MNT_FORCE, 30);
	NOTE("u20: forced unmount after the kill gave %d (0 is success)", rc);
	CHECK(!mounted_now(mp), "u20: the mount is gone");
	rmdir(mp);
	close(dev);
	close(ep[0]);
	close(ep[1]);
}

static void write_wrapper(const char *path)
{
	FILE *f = fopen(path, "w");

	if (f == NULL) {
		FAIL("cannot write %s", path);
		return;
	}
	fprintf(f, "#!/bin/sh\necho $$ > '%s/wrapper.pid'\nkill -STOP $$\nexec '%s' \"$@\"\n", g_dir, g_worker);
	fclose(f);
	chmod(path, 0755);
}

static void case_stop(void)
{
	ntfsmac_spawn_config c = base_config();
	ntfsmac_worker_run run;
	ntfsmac_child child;
	char mp[] = "/Volumes/spawnvmstop", wrapper[256], pidfile[256];
	int dev, ep[2], pid = 0;
	bool mounted_ro = false;
	uint64_t t0, dt;
	ntfsmac_dmn_status st;
	FILE *f;

	printf("== stop: worker stopped right after the spawn (I-DMN-26 b)\n");
	snprintf(wrapper, sizeof(wrapper), "%s/ntfsmac-worker-stopped", g_dir);
	write_wrapper(wrapper);
	c.worker_path = wrapper;
	ntfsmac_spawn_config_set(&c);
	if (make_mountpoint(mp) != 0) {
		FAIL("stop: cannot make %s", mp);
		return;
	}
	dev = open_image(O_RDONLY);
	if (dev < 0 || pipe(ep) != 0) {
		FAIL("stop: cannot open the image or make the channel");
		rmdir(mp);
		return;
	}
	fcntl(ep[0], F_SETFD, FD_CLOEXEC);
	fcntl(ep[1], F_SETFD, FD_CLOEXEC);
	memset(&run, 0, sizeof(run));
	run.device_fd = dev;
	run.errfd_write = ep[1];
	run.mount_point = mp;
	run.read_only = true;
	run.console_uid = g_uid;
	run.console_gid = g_gid;
	run.mount_wait_s = 10;
	t0 = now_ms();
	st = ntfsmac_spawn_worker(&run, &child, &mounted_ro);
	dt = now_ms() - t0;
	CHECK(st == NTFSMAC_DMN_E_TIMEOUT, "stop: the wait ended with %s", ntfsmac_dmn_status_name(st));
	CHECK(dt >= 9500 && dt < 20000, "stop: after %llu ms (deadline 10 s)", (unsigned long long)dt);
	CHECK(child.pid == -1, "stop: the leader was reaped");
	snprintf(pidfile, sizeof(pidfile), "%s/wrapper.pid", g_dir);
	f = fopen(pidfile, "r");
	if (f != NULL && fscanf(f, "%d", &pid) == 1) {
		int i;
		bool gone = false;

		for (i = 0; i < 100 && !gone; i++) {
			gone = kill(pid, 0) != 0 && errno == ESRCH;
			if (!gone)
				sleep_ms(50);
		}
		CHECK(gone, "stop: no worker process is left (pid %d)", pid);
	} else {
		FAIL("stop: the wrapper wrote no pid");
	}
	if (f != NULL)
		fclose(f);
	CHECK(!mounted_now(mp), "stop: no mount");
	rmdir(mp);
	close(dev);
	close(ep[0]);
	close(ep[1]);
}

static void drop_fskit(ntfsmac_worker_argv *av)
{
	char *p = strstr(av->optstr, "backend=fskit");

	if (p != NULL)
		memcpy(p, "backend=xxxxx", 13);
}

static void case_nofskit(void)
{
	ntfsmac_spawn_config c = base_config();
	ntfsmac_worker_run run;
	ntfsmac_child child;
	char mp[] = "/Volumes/spawnvmnofs";
	int dev, ep[2];
	bool mounted_ro = false;
	uint64_t t0, dt;
	ntfsmac_dmn_status st;

	printf("== nofskit: backend=fskit dropped after argv_check (I-DMN-26 c)\n");
	c.after_check_hook = drop_fskit;
	ntfsmac_spawn_config_set(&c);
	if (make_mountpoint(mp) != 0) {
		FAIL("nofskit: cannot make %s", mp);
		return;
	}
	dev = open_image(O_RDONLY);
	if (dev < 0 || pipe(ep) != 0) {
		FAIL("nofskit: cannot open the image or make the channel");
		rmdir(mp);
		return;
	}
	fcntl(ep[0], F_SETFD, FD_CLOEXEC);
	fcntl(ep[1], F_SETFD, FD_CLOEXEC);
	memset(&run, 0, sizeof(run));
	run.device_fd = dev;
	run.errfd_write = ep[1];
	run.mount_point = mp;
	run.read_only = true;
	run.console_uid = g_uid;
	run.console_gid = g_gid;
	run.mount_wait_s = 10;
	t0 = now_ms();
	st = ntfsmac_spawn_worker(&run, &child, &mounted_ro);
	dt = now_ms() - t0;
	NOTE("nofskit: %s after %llu ms", ntfsmac_dmn_status_name(st), (unsigned long long)dt);
	CHECK(st == NTFSMAC_DMN_E_TIMEOUT || st == NTFSMAC_DMN_E_IO,
	      "nofskit: no mount appeared and the wait ended (%s)", ntfsmac_dmn_status_name(st));
	CHECK(child.pid == -1, "nofskit: the worker is reaped, none left");
	CHECK(!mounted_now(mp), "nofskit: no mount");
	show_log_tail();
	rmdir(mp);
	close(dev);
	close(ep[0]);
	close(ep[1]);
}

int main(int argc, char **argv)
{
	/* The worker's start runs this program once more as `--exec-as` inside the user's session. */
	if (argc >= 2 && strcmp(argv[1], NTFSMAC_EXEC_AS_FLAG) == 0)
		return ntfsmac_spawn_exec_as(argc, argv);
	char path[300];
	const char *only;

	if (argc >= 3 && !strcmp(argv[1], "--sandbox")) {
		g_sandbox_dir = argv[2];
		argc -= 2;
		argv += 2;
		argv[0] = (char *)"spawn_vm";
	}
	if (argc < 7) {
		fprintf(stderr, "usage: spawn_vm [--sandbox DIR] IMAGE WORKER STUB UID GID WORKDIR [CASE]\n");
		return 2;
	}
	g_image = argv[1];
	g_worker = argv[2];
	g_stub = argv[3];
	g_uid = (uid_t)strtoul(argv[4], NULL, 10);
	g_gid = (gid_t)strtoul(argv[5], NULL, 10);
	g_dir = argv[6];
	only = argc > 7 ? argv[7] : NULL;
	if (geteuid() != 0 || g_uid == 0) {
		fprintf(stderr, "spawn_vm: run as root, with a console user that is not root\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGPIPE, SIG_IGN); /* as the daemon does; the children must still get the default */
	snprintf(path, sizeof(path), "%s/worker.log", g_dir);
	g_log_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);

	if (only == NULL || !strcmp(only, "priv"))
		case_priv();
	if (only == NULL || !strcmp(only, "rw"))
		case_mount(false);
	if (only == NULL || !strcmp(only, "ro"))
		case_mount(true);
	if (only == NULL || !strcmp(only, "u20"))
		case_u20();
	if (only == NULL || !strcmp(only, "stop"))
		case_stop();
	if (only == NULL || !strcmp(only, "nofskit"))
		case_nofskit();
	printf("== result: %d failure(s)\n", g_fails);
	return g_fails == 0 ? 0 : 1;
}
