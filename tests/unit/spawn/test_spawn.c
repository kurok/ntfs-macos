/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-SPN-01..: tests of src/daemon/spawn.c (T-M4-01) with a stand-in program
 * (spawn_stub.c). They run without root: the "unprivileged" user is the
 * current user. The change of user itself needs root and is tested in
 * tests/integration/daemon/spawn_priv.sh (I-DMN-09).
 *
 * The stand-in checks inside the child what the module promises: only the
 * named descriptors are open, the child leads its own process group, signals
 * are at their defaults, the argument vector and environment are the fixed
 * ones. A failed check shows as a failed spawn call.
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "check.h"
#include "spawn_config.h"

static char g_dir[256];
static int g_polls_until_mount;
static int g_polls;
static bool g_mount_ro;
static int g_hook_calls;

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int count_open_fds(void)
{
	int fd, n = 0;

	for (fd = 3; fd < 256; fd++)
		if (fcntl(fd, F_GETFD) != -1)
			n++;
	return n;
}

static void link_as(const char *name)
{
	char path[300];

	snprintf(path, sizeof(path), "%s/%s", g_dir, name);
	(void)unlink(path);
	if (symlink(SPAWN_STUB_PATH, path) != 0) {
		perror("symlink");
		exit(2);
	}
}

static const char *path_of(const char *name)
{
	static char bufs[4][300];
	static int k;
	char *b = bufs[k++ & 3];

	snprintf(b, 300, "%s/%s", g_dir, name);
	return b;
}

/* Fake mount table: the mount "appears" after g_polls_until_mount calls. */
static ntfsmac_dmn_status fake_lookup(const char *mp, bool *found, bool *ro)
{
	(void)mp;
	g_polls++;
	*found = g_polls_until_mount >= 0 && g_polls > g_polls_until_mount;
	*ro = g_mount_ro;
	return NTFSMAC_DMN_OK;
}

static void drop_fskit_hook(ntfsmac_worker_argv *av)
{
	char *p = strstr(av->optstr, "backend=fskit");

	g_hook_calls++;
	if (p != NULL)
		memcpy(p, "backend=xxxxx", 13);
}

static ntfsmac_spawn_config base_config(void)
{
	ntfsmac_spawn_config c;

	ntfsmac_spawn_config_default(&c);
	c.sandbox_dir = NULL; /* these tests run stand-ins; sandbox tests: test_sandbox.c */
	c.unpriv_uid = getuid();
	c.unpriv_gid = getgid();
	c.mount_poll_ms = 10;
	c.volmark_write_wait_s = 1;
	c.mount_lookup = fake_lookup;
	return c;
}

static int open_device(int flags)
{
	char path[300];
	int fd;

	snprintf(path, sizeof(path), "%s/device.img", g_dir);
	fd = open(path, flags | O_CLOEXEC);
	if (fd < 0) {
		perror("open device");
		exit(2);
	}
	return fd;
}

static void make_device(void)
{
	char path[300];
	int fd;

	snprintf(path, sizeof(path), "%s/device.img", g_dir);
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0 || write(fd, "ABCDEFGH", 8) != 8) {
		perror("make device");
		exit(2);
	}
	close(fd);
}

/* pids reported by a worker stand-in on its stderr file */
static bool read_pids(const char *file, pid_t *a, pid_t *b)
{
	FILE *f = fopen(file, "r");
	char line[128];
	bool ok = false;

	if (f == NULL)
		return false;
	while (fgets(line, sizeof(line), f) != NULL) {
		int x, y;

		if (sscanf(line, "pids %d %d", &x, &y) == 2) {
			*a = (pid_t)x;
			*b = (pid_t)y;
			ok = true;
		}
	}
	fclose(f);
	return ok;
}

/* "<tag> a b [c]" reported by a stand-in on its stderr file; the number of pids found (0 if none). */
static int read_tagged(const char *file, const char *tag, pid_t *out, int want)
{
	FILE *f;
	char line[128], fmt[64];
	int n = 0, i;

	for (i = 0; i < 200; i++) { /* the stand-in reports a moment after the mount seems to be there */
		f = fopen(file, "r");
		if (f != NULL) {
			snprintf(fmt, sizeof(fmt), "%s %%d %%d %%d", tag);
			while (fgets(line, sizeof(line), f) != NULL) {
				int a = 0, b = 0, c = 0;
				int r = sscanf(line, fmt, &a, &b, &c);

				if (r >= want) {
					out[0] = (pid_t)a;
					out[1] = (pid_t)b;
					out[2] = (pid_t)c;
					n = r;
				}
			}
			fclose(f);
		}
		if (n >= want)
			return n;
		usleep(10000);
	}
	return 0;
}

static bool gone(pid_t p)
{
	int i;

	for (i = 0; i < 200; i++) { /* up to 2 s: init reaps the orphan */
		if (kill(p, 0) != 0 && errno == ESRCH)
			return true;
		usleep(10000);
	}
	return false;
}

static int g_err_fd = -1;
static char g_err_path[300];

static void new_err_file(void)
{
	if (g_err_fd >= 0)
		close(g_err_fd);
	snprintf(g_err_path, sizeof(g_err_path), "%s/stderr.txt", g_dir);
	g_err_fd = open(g_err_path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
}

/* ---------------------------------------------------------------- */

static void test_probe(void)
{
	ntfsmac_spawn_config c = base_config();
	ntfsmac_probe_verdict v;
	ntfsmac_probe_run run;
	uint64_t t0;
	int fd, before;

	link_as("probe-ok");
	link_as("probe-hang");
	link_as("probe-garbage");
	link_as("probe-exit3");
	link_as("probe-abort");
	link_as("probe-big");
	new_err_file();
	c.child_stderr_fd = g_err_fd;
	fd = open_device(O_RDONLY);
	run.device_fd = fd;
	run.timeout_s = 5;
	before = count_open_fds();

	/* U-SPN-01: the verdict arrives parsed; the child saw exactly the descriptors it should. */
	c.probe_path = path_of("probe-ok");
	ntfsmac_spawn_config_set(&c);
	memset(&v, 0, sizeof(v));
	CHECK_EQ_INT(ntfsmac_spawn_probe(&run, &v), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(v.state, NTFSMAC_PROBE_STATE_CLEAN);
	CHECK(v.serial == UINT64_C(0x1A2B3C4D5E6F7788));
	CHECK_EQ_INT(v.total_sectors, 131071);

	/* U-SPN-02: a probe that hangs is killed at its deadline; the verdict is `unknown`. */
	c.probe_path = path_of("probe-hang");
	ntfsmac_spawn_config_set(&c);
	run.timeout_s = 1;
	t0 = now_ms();
	CHECK_EQ_INT(ntfsmac_spawn_probe(&run, &v), NTFSMAC_DMN_E_TIMEOUT);
	CHECK(now_ms() - t0 >= 900 && now_ms() - t0 < 4000);
	CHECK_EQ_INT(v.state, NTFSMAC_PROBE_STATE_UNKNOWN);
	run.timeout_s = 5;

	/* U-SPN-03: garbage, a non-zero exit, a crash and oversize output all give `unknown`. */
	{
		static const char *const modes[] = { "probe-garbage", "probe-exit3", "probe-abort",
						     "probe-big" };
		size_t i;

		for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
			c.probe_path = path_of(modes[i]);
			ntfsmac_spawn_config_set(&c);
			v.state = NTFSMAC_PROBE_STATE_CLEAN;
			CHECK_EQ_INT(ntfsmac_spawn_probe(&run, &v), NTFSMAC_DMN_E_IO);
			CHECK_EQ_INT(v.state, NTFSMAC_PROBE_STATE_UNKNOWN);
		}
	}

	/* U-SPN-04: a missing program is a failed start, reported at once. */
	c.probe_path = path_of("no-such-program");
	ntfsmac_spawn_config_set(&c);
	t0 = now_ms();
	CHECK_EQ_INT(ntfsmac_spawn_probe(&run, &v), NTFSMAC_DMN_E_IO);
	CHECK(now_ms() - t0 < 2000);
	CHECK_EQ_INT(v.state, NTFSMAC_PROBE_STATE_UNKNOWN);

	/* U-SPN-05: bad arguments. */
	c.probe_path = path_of("probe-ok");
	ntfsmac_spawn_config_set(&c);
	CHECK_EQ_INT(ntfsmac_spawn_probe(NULL, &v), NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_spawn_probe(&run, NULL), NTFSMAC_DMN_E_INVAL);
	run.device_fd = 2;
	CHECK_EQ_INT(ntfsmac_spawn_probe(&run, &v), NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(v.state, NTFSMAC_PROBE_STATE_UNKNOWN);
	run.device_fd = fd;
	run.timeout_s = 0;
	CHECK_EQ_INT(ntfsmac_spawn_probe(&run, &v), NTFSMAC_DMN_E_INVAL);
	run.timeout_s = 5;

	/* U-SPN-06: a user this process cannot become is refused (not testable as root). */
	if (geteuid() != 0) {
		c.unpriv_uid = geteuid() + 1;
		ntfsmac_spawn_config_set(&c);
		CHECK_EQ_INT(ntfsmac_spawn_probe(&run, &v), NTFSMAC_DMN_E_DENIED);
		CHECK_EQ_INT(v.state, NTFSMAC_PROBE_STATE_UNKNOWN);
	}

	/* U-SPN-07: no descriptor of ours is left behind by any of the above. */
	CHECK_EQ_INT(count_open_fds(), before);
	close(fd);
}

static void test_volmark(void)
{
	ntfsmac_spawn_config c = base_config();
	ntfsmac_volmark_run run;
	uint64_t t0;
	int fd, code, before;

	link_as("volmark-set");
	link_as("volmark-clear");
	link_as("volmark-exit11");
	link_as("volmark-hang");
	link_as("volmark-hangafterw");
	link_as("volmark-slowwrite");
	fd = open_device(O_RDWR);
	run.device_fd = fd;
	run.serial = 0x12AB;
	run.total_sectors = 2048;
	run.timeout_s = 5;
	before = count_open_fds();

	/* U-SPN-10: `set` and `clear` pass the integers formatted as the tool expects. */
	c.volmark_path = path_of("volmark-set");
	ntfsmac_spawn_config_set(&c);
	code = 99;
	CHECK_EQ_INT(ntfsmac_spawn_volmark(&run, true, &code), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(code, 0);
	c.volmark_path = path_of("volmark-clear");
	ntfsmac_spawn_config_set(&c);
	CHECK_EQ_INT(ntfsmac_spawn_volmark(&run, false, &code), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(code, 0);

	/* U-SPN-11: an exit code is passed on. */
	c.volmark_path = path_of("volmark-exit11");
	ntfsmac_spawn_config_set(&c);
	CHECK_EQ_INT(ntfsmac_spawn_volmark(&run, true, &code), NTFSMAC_DMN_E_IO);
	CHECK_EQ_INT(code, 11);

	/* U-SPN-12: silence until the deadline is a kill with no write begun (-1). */
	c.volmark_path = path_of("volmark-hang");
	ntfsmac_spawn_config_set(&c);
	run.timeout_s = 1;
	t0 = now_ms();
	CHECK_EQ_INT(ntfsmac_spawn_volmark(&run, true, &code), NTFSMAC_DMN_E_TIMEOUT);
	CHECK_EQ_INT(code, -1);
	CHECK(now_ms() - t0 >= 900 && now_ms() - t0 < 4000);

	/*
	 * U-SPN-13: once `w` is printed the first limit no longer applies: a stand-in that
	 * writes for 2 s with timeout_s 1 is not killed.
	 */
	c.volmark_path = path_of("volmark-slowwrite");
	c.volmark_write_wait_s = 10;
	ntfsmac_spawn_config_set(&c);
	CHECK_EQ_INT(ntfsmac_spawn_volmark(&run, true, &code), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(code, 0);

	/* U-SPN-14: after `w` the further limit applies and the result says a write may be cut (-2). */
	c.volmark_path = path_of("volmark-hangafterw");
	c.volmark_write_wait_s = 1;
	ntfsmac_spawn_config_set(&c);
	t0 = now_ms();
	CHECK_EQ_INT(ntfsmac_spawn_volmark(&run, true, &code), NTFSMAC_DMN_E_TIMEOUT);
	CHECK_EQ_INT(code, -2);
	CHECK(now_ms() - t0 >= 900 && now_ms() - t0 < 4000);

	/* U-SPN-15: bad arguments; a failed start leaves the code at -3. */
	c.volmark_path = path_of("no-such-program");
	ntfsmac_spawn_config_set(&c);
	CHECK_EQ_INT(ntfsmac_spawn_volmark(&run, true, &code), NTFSMAC_DMN_E_IO);
	CHECK_EQ_INT(code, -3);
	run.total_sectors = 0;
	CHECK_EQ_INT(ntfsmac_spawn_volmark(&run, true, &code), NTFSMAC_DMN_E_INVAL);
	run.total_sectors = 2048;
	CHECK_EQ_INT(ntfsmac_spawn_volmark(NULL, true, &code), NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_spawn_volmark(&run, true, NULL), NTFSMAC_DMN_E_INVAL);

	CHECK_EQ_INT(count_open_fds(), before);
	close(fd);
}

static ntfsmac_worker_run worker_run(int dev, int errw, bool ro, unsigned wait_s)
{
	ntfsmac_worker_run r;

	memset(&r, 0, sizeof(r));
	r.device_fd = dev;
	r.errfd_write = errw;
	r.mount_point = "/Volumes/TESTVOL";
	r.read_only = ro;
	r.console_uid = getuid();
	r.console_gid = getgid();
	r.mount_wait_s = wait_s;
	return r;
}

static void test_worker(void)
{
	ntfsmac_spawn_config c = base_config();
	ntfsmac_worker_run run;
	ntfsmac_child child;
	pid_t p1 = 0, p2 = 0;
	uint64_t t0;
	int dev, ep[2], before, status = 0, i;
	bool ro = true, exited;
	char buf[16];

	link_as("worker-ok");
	link_as("worker-exit0");
	link_as("worker-hang");
	link_as("worker-stop");
	link_as("worker-nofskit");
	link_as("worker-tree");
	link_as("worker-orphan");
	dev = open_device(O_RDWR);
	if (pipe(ep) != 0)
		exit(2);
	fcntl(ep[0], F_SETFD, FD_CLOEXEC);
	fcntl(ep[1], F_SETFD, FD_CLOEXEC);
	before = count_open_fds();

	/* U-SPN-20: a rw worker with a session channel; the mount appears after 3 polls. */
	new_err_file();
	c.child_stderr_fd = g_err_fd;
	c.worker_path = path_of("worker-ok");
	ntfsmac_spawn_config_set(&c);
	g_polls = 0;
	g_polls_until_mount = 3;
	g_mount_ro = false;
	run = worker_run(dev, ep[1], false, 5);
	ro = true;
	CHECK_EQ_INT(ntfsmac_spawn_worker(&run, &child, &ro), NTFSMAC_DMN_OK);
	CHECK(!ro);
	CHECK(child.pid > 1);
	if (child.pid > 1) {
		/* The worker leads its own group and wrote its record on the channel. */
		CHECK_EQ_INT(getpgid(child.pid), child.pid);
		memset(buf, 0, sizeof(buf));
		CHECK_EQ_INT(read(ep[0], buf, 5), 5);
		CHECK(memcmp(buf, "S rw\n", 5) == 0);
		exited = true;
		CHECK_EQ_INT(ntfsmac_child_poll(&child, &exited, &status), NTFSMAC_DMN_OK);
		CHECK(!exited);
		/* U-SPN-21: kill the group; the poll then reports the signal and clears the handle. */
		CHECK_EQ_INT(ntfsmac_child_kill_group(&child), NTFSMAC_DMN_OK);
		exited = false;
		for (i = 0; i < 200 && !exited; i++) {
			CHECK_EQ_INT(ntfsmac_child_poll(&child, &exited, &status), NTFSMAC_DMN_OK);
			usleep(10000);
		}
		CHECK(exited);
		CHECK_EQ_INT(status, 128 + SIGKILL);
		CHECK_EQ_INT(child.pid, -1);
		CHECK_EQ_INT(ntfsmac_child_poll(&child, &exited, &status), NTFSMAC_DMN_E_STATE);
		CHECK_EQ_INT(ntfsmac_child_kill_group(&child), NTFSMAC_DMN_E_STATE);
	}
	/* The grandchild in the worker's group died with it. */
	if (read_pids(g_err_path, &p1, &p2))
		CHECK(gone(p2));
	else
		CHECK(0 && "no pids reported");

	/* U-SPN-30 (audit M1): a descendant that left the group, with a child of its own that left the
	 * session, is killed with the worker by ntfsmac_child_kill_group(), while the worker is alive. */
	{
		pid_t t[3] = { 0, 0, 0 };
		bool all_exist;

		new_err_file();
		c.child_stderr_fd = g_err_fd;
		c.worker_path = path_of("worker-tree");
		ntfsmac_spawn_config_set(&c);
		g_polls = 0;
		g_polls_until_mount = 0;
		run = worker_run(dev, -1, false, 5);
		CHECK_EQ_INT(ntfsmac_spawn_worker(&run, &child, &ro), NTFSMAC_DMN_OK);
		CHECK_EQ_INT(read_tagged(g_err_path, "tree", t, 3), 3);
		all_exist = t[1] > 1 && t[2] > 1 && kill(t[1], 0) == 0 && kill(t[2], 0) == 0;
		CHECK(all_exist);
		CHECK(all_exist && getpgid(t[1]) == t[1] && getpgid(t[1]) != getpgid(t[0])); /* it left the group */
		CHECK(all_exist && getsid(t[2]) == t[2]);                                   /* and this one the session */
		CHECK_EQ_INT(ntfsmac_child_kill_group(&child), NTFSMAC_DMN_OK);
		exited = false;
		for (i = 0; i < 300 && !exited; i++) {
			(void)ntfsmac_child_poll(&child, &exited, &status);
			usleep(10000);
		}
		CHECK(exited);
		CHECK(gone(t[1]));
		CHECK(gone(t[2]));
	}

	/* U-SPN-31 (audit M1): the worker exits by itself and leaves a descendant that left the group and
	 * has no parent. The tree cannot find it, but it holds the device descriptor it inherited, so
	 * the poll that reports the exit kills it. */
	{
		pid_t t[3] = { 0, 0, 0 };
		unsigned killed = 99;

		new_err_file();
		c.child_stderr_fd = g_err_fd;
		c.worker_path = path_of("worker-orphan");
		ntfsmac_spawn_config_set(&c);
		g_polls = 0;
		g_polls_until_mount = 0;
		run = worker_run(dev, -1, false, 5);
		CHECK_EQ_INT(ntfsmac_spawn_worker(&run, &child, &ro), NTFSMAC_DMN_OK);
		CHECK_EQ_INT(read_tagged(g_err_path, "orphan", t, 2), 2);
		CHECK(t[1] > 1 && kill(t[1], 0) == 0);
		exited = false;
		for (i = 0; i < 300 && !exited; i++) {
			(void)ntfsmac_child_poll(&child, &exited, &status);
			usleep(10000);
		}
		CHECK(exited);
		CHECK(gone(t[1]));
		CHECK_EQ_INT(ntfsmac_child_device_check(&child, &killed), NTFSMAC_DMN_OK);
		CHECK_EQ_INT(killed, 0);
	}

	/* U-SPN-32: the device check kills a holder of the device that belongs to the user of the worker,
	 * and looks at no process of another user; a process that does not hold the device is left. */
	{
		int hp[2];
		pid_t holder, bystander;
		unsigned killed = 0;
		ntfsmac_child other = child;

		if (pipe(hp) != 0)
			exit(2);
		holder = fork();
		if (holder == 0) { /* holds `dev` (inherited) and is in a group of its own */
			(void)setpgid(0, 0);
			close(hp[0]);
			for (;;)
				sleep(60);
		}
		bystander = fork();
		if (bystander == 0) { /* does not hold it: it closes the descriptor first */
			close(dev);
			for (;;)
				sleep(60);
		}
		close(hp[0]);
		close(hp[1]);
		usleep(100000);
		CHECK(child.dev_known);
		other.owner = getuid() + 1; /* nothing of this user holds the device: nothing is looked at */
		CHECK_EQ_INT(ntfsmac_child_device_check(&other, &killed), NTFSMAC_DMN_OK);
		CHECK_EQ_INT(killed, 0);
		CHECK(kill(holder, 0) == 0);
		CHECK_EQ_INT(ntfsmac_child_device_check(&child, &killed), NTFSMAC_DMN_OK);
		/* the test process holds `dev` too, but the check never touches its own process */
		CHECK_EQ_INT(killed, 1);
		CHECK_EQ_INT(waitpid(holder, &status, 0), holder);
		CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
		CHECK(kill(bystander, 0) == 0);
		(void)kill(bystander, SIGKILL);
		(void)waitpid(bystander, &status, 0);
		other.dev_known = false;
		CHECK_EQ_INT(ntfsmac_child_device_check(&other, NULL), NTFSMAC_DMN_E_STATE);
		CHECK_EQ_INT(ntfsmac_child_device_check(NULL, NULL), NTFSMAC_DMN_E_INVAL);
	}

	/* U-SPN-22: the mode of the mount is reported as the table shows it. */
	g_polls = 0;
	g_polls_until_mount = 0;
	g_mount_ro = true;
	run = worker_run(dev, -1, true, 5); /* no channel: no NTFSMAC_ERRFD at all */
	ro = false;
	{
		int rdev = open_device(O_RDONLY);

		run.device_fd = rdev;
		CHECK_EQ_INT(ntfsmac_spawn_worker(&run, &child, &ro), NTFSMAC_DMN_OK);
		CHECK(ro);
		(void)ntfsmac_child_kill_group(&child);
		for (i = 0; i < 200; i++) {
			exited = false;
			(void)ntfsmac_child_poll(&child, &exited, &status);
			if (exited)
				break;
			usleep(10000);
		}
		close(rdev);
	}

	/* U-SPN-23: a worker that exits before the mount fails fast, not at the deadline. */
	g_polls = 0;
	g_polls_until_mount = -1;
	c.worker_path = path_of("worker-exit0");
	ntfsmac_spawn_config_set(&c);
	run = worker_run(dev, ep[1], false, 20);
	t0 = now_ms();
	CHECK_EQ_INT(ntfsmac_spawn_worker(&run, &child, &ro), NTFSMAC_DMN_E_IO);
	CHECK(now_ms() - t0 < 5000);
	CHECK_EQ_INT(child.pid, -1);

	/* U-SPN-24 (I-DMN-26 c, unit form): a worker that ignores SIGTERM and never mounts is
	 * killed with its whole group at the deadline. */
	new_err_file();
	c.child_stderr_fd = g_err_fd;
	c.worker_path = path_of("worker-hang");
	ntfsmac_spawn_config_set(&c);
	run = worker_run(dev, ep[1], false, 1);
	t0 = now_ms();
	CHECK_EQ_INT(ntfsmac_spawn_worker(&run, &child, &ro), NTFSMAC_DMN_E_TIMEOUT);
	CHECK(now_ms() - t0 >= 900 && now_ms() - t0 < 6000);
	CHECK_EQ_INT(child.pid, -1);
	if (read_pids(g_err_path, &p1, &p2)) {
		CHECK(gone(p1));
		CHECK(gone(p2));
	} else {
		CHECK(0 && "no pids reported");
	}

	/* U-SPN-25 (I-DMN-26 b, unit form): a worker stopped with SIGSTOP is killed too. */
	new_err_file();
	c.child_stderr_fd = g_err_fd;
	c.worker_path = path_of("worker-stop");
	ntfsmac_spawn_config_set(&c);
	t0 = now_ms();
	CHECK_EQ_INT(ntfsmac_spawn_worker(&run, &child, &ro), NTFSMAC_DMN_E_TIMEOUT);
	CHECK(now_ms() - t0 < 6000);
	CHECK_EQ_INT(child.pid, -1);
	if (read_pids(g_err_path, &p1, &p2)) {
		CHECK(gone(p1));
		CHECK(gone(p2));
	} else {
		CHECK(0 && "no pids reported");
	}

	/* U-SPN-26 (I-DMN-26 c, hook form): the test seam can change the vector after the check. */
	new_err_file();
	c.child_stderr_fd = g_err_fd;
	c.worker_path = path_of("worker-nofskit");
	c.after_check_hook = drop_fskit_hook;
	ntfsmac_spawn_config_set(&c);
	g_hook_calls = 0;
	CHECK_EQ_INT(ntfsmac_spawn_worker(&run, &child, &ro), NTFSMAC_DMN_E_IO);
	CHECK_EQ_INT(g_hook_calls, 1);
	{
		char line[64] = "";
		FILE *f = fopen(g_err_path, "r");

		if (f != NULL) {
			if (fgets(line, sizeof(line), f) == NULL)
				line[0] = '\0';
			fclose(f);
		}
		CHECK(strcmp(line, "fskit=0\n") == 0);
	}
	c.after_check_hook = NULL;
	ntfsmac_spawn_config_set(&c);

	/* U-SPN-27: refusals before any start. */
	c.worker_path = path_of("worker-ok");
	ntfsmac_spawn_config_set(&c);
	run = worker_run(dev, ep[1], true, 1); /* ro decision, O_RDWR descriptor: rule 7 */
	CHECK_EQ_INT(ntfsmac_spawn_worker(&run, &child, &ro), NTFSMAC_DMN_E_DENIED);
	run = worker_run(dev, ep[1], false, 1);
	run.mount_point = "/tmp/TESTVOL"; /* not under /Volumes */
	CHECK_EQ_INT(ntfsmac_spawn_worker(&run, &child, &ro), NTFSMAC_DMN_E_INVAL);
	run.mount_point = "/Volumes/a/b"; /* a leaf with a slash: the builder refuses */
	CHECK_EQ_INT(ntfsmac_spawn_worker(&run, &child, &ro), NTFSMAC_DMN_E_DENIED);
	run = worker_run(dev, dev, false, 1); /* channel and device must differ */
	CHECK_EQ_INT(ntfsmac_spawn_worker(&run, &child, &ro), NTFSMAC_DMN_E_INVAL);
	run = worker_run(2, ep[1], false, 1);
	CHECK_EQ_INT(ntfsmac_spawn_worker(&run, &child, &ro), NTFSMAC_DMN_E_INVAL);
	run = worker_run(dev, ep[1], false, 0);
	CHECK_EQ_INT(ntfsmac_spawn_worker(&run, &child, &ro), NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_spawn_worker(NULL, &child, &ro), NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_spawn_worker(&run, NULL, &ro), NTFSMAC_DMN_E_INVAL);
	if (geteuid() != 0) {
		run = worker_run(dev, ep[1], false, 1);
		run.console_uid = geteuid() + 1; /* a user we cannot become */
		CHECK_EQ_INT(ntfsmac_spawn_worker(&run, &child, &ro), NTFSMAC_DMN_E_DENIED);
	}
	/* `usermapping=` needs the root-owned file; without it nothing is started. */
	{
		struct stat sb;

		if (lstat(NTFSMAC_USERMAPPING_PATH, &sb) != 0) {
			c.usermapping = true;
			ntfsmac_spawn_config_set(&c);
			run = worker_run(dev, ep[1], false, 1);
			CHECK_EQ_INT(ntfsmac_spawn_worker(&run, &child, &ro), NTFSMAC_DMN_E_DENIED);
			c.usermapping = false;
			ntfsmac_spawn_config_set(&c);
		}
	}

	/* U-SPN-28: handle checks never touch pid 0, 1 or a negative number. */
	child.pid = 0;
	CHECK_EQ_INT(ntfsmac_child_kill_group(&child), NTFSMAC_DMN_E_STATE);
	child.pid = 1;
	CHECK_EQ_INT(ntfsmac_child_kill_group(&child), NTFSMAC_DMN_E_STATE);
	child.pid = -1;
	CHECK_EQ_INT(ntfsmac_child_kill_group(&child), NTFSMAC_DMN_E_STATE);
	CHECK_EQ_INT(ntfsmac_child_kill_group(NULL), NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_child_poll(NULL, &exited, &status), NTFSMAC_DMN_E_INVAL);

	/* U-SPN-29: no descriptor of ours is left behind (the stderr file is
	 * replaced by new_err_file(), so the count stays the same). */
	CHECK_EQ_INT(count_open_fds(), before);
	close(ep[0]);
	close(ep[1]);
	close(dev);
}

int main(void)
{
	const char *tmp = getenv("TMPDIR");
	char tmpl[256];
	char cmd[300];

	if (tmp == NULL || tmp[0] == '\0')
		tmp = "/tmp";
	snprintf(tmpl, sizeof(tmpl), "%s/spawn-test.XXXXXX", tmp);
	if (mkdtemp(tmpl) == NULL) {
		perror("mkdtemp");
		return 2;
	}
	snprintf(g_dir, sizeof(g_dir), "%s", tmpl);
	make_device();
	signal(SIGPIPE, SIG_IGN); /* as a daemon would: the child must still get the default */

	test_probe();
	test_volmark();
	test_worker();

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
	if (system(cmd) != 0)
		fprintf(stderr, "warning: could not remove %s\n", g_dir);
	if (g_failures != 0) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	printf("test_spawn: ok\n");
	return 0;
}
