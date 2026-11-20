/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-SBX-01..07, 10, 11: how src/daemon/spawn.c applies the sandbox profiles (T-M4-06), without a
 * real sandbox. A shell script stands in for /usr/bin/sandbox-exec: it writes the arguments it got
 * to a file and then starts the program the way sandbox-exec does, so the test sees the exact
 * command line (the profile text, `-p`, and the parameters) and the stand-in program of
 * spawn_stub.c still runs and answers.
 *
 * What the profiles themselves allow and refuse is tested in a macOS guest
 * (tests/integration/daemon/sandbox_vm.sh, I-SBX-01..08); this test runs on every host.
 * The profile files are lint-checked by sandbox_lint.sh (U-SBX-08).
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if !defined(NTFSMAC_TEST_SEAMS)
#error "this test needs the test seams of spawn_config.h (NTFSMAC_TEST_SEAMS)"
#endif

#include "check.h"
#include "ntfsmac/ctl_proto.h"
#include "ntfsmac/options.h"
#include "spawn_config.h"

/* snprintf that aborts instead of truncating a path */
#define PATHF(buf, ...) \
	do { \
		if (snprintf((buf), sizeof(buf), __VA_ARGS__) >= (int)sizeof(buf)) { \
			fprintf(stderr, "path too long\n"); \
			abort(); \
		} \
	} while (0)

/* What the profile files hold in these tests: the text sandbox-exec must be given. */
#define PROFILE_TEXT_PROBE "(version 1)\n;; probe-profile\n"
#define PROFILE_TEXT_VOLMARK "(version 1)\n;; volmark-profile\n"
#define PROFILE_TEXT_WORKER "(version 1)\n;; worker-profile\n"

static char g_dir[PATH_MAX];     /* scratch directory */
static char g_sbx[300];     /* sandbox_dir: holds probe.sb, volmark.sb, worker.sb */
static char g_wrap[300];    /* the fake sandbox-exec */
static char g_log[300];     /* what the fake wrote */
static char g_fail[300];    /* if this file exists the fake exits 65 before the program starts */
static char g_stub_real[PATH_MAX];

/* The control socket path with the links of its directory resolved, as the module passes it. */
static const char *ctl_real_param(void)
{
	static char out[PATH_MAX + 40];
	char dir[PATH_MAX], real[PATH_MAX];
	const char *slash = strrchr(NTFSMAC_CTL_SOCKET_PATH, '/');

	PATHF(dir, "%.*s", (int)(slash - NTFSMAC_CTL_SOCKET_PATH), NTFSMAC_CTL_SOCKET_PATH);
	if (realpath(dir, real) == NULL) {
		perror("realpath");
		exit(2);
	}
	PATHF(out, "-D CTL_SOCKET_REAL=%s/%s ", real, slash + 1);
	return out;
}

static ntfsmac_dmn_status fake_lookup(const char *mp, bool *found, bool *ro)
{
	(void)mp;
	*found = true;
	*ro = false;
	return NTFSMAC_DMN_OK;
}

static void write_file(const char *path, const char *text, mode_t mode)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

	if (fd < 0 || write(fd, text, strlen(text)) != (ssize_t)strlen(text)
	    || fchmod(fd, mode) != 0) {
		perror(path);
		exit(2);
	}
	close(fd);
}

static void link_as(const char *name)
{
	char path[300];

	PATHF(path, "%s/%s", g_dir, name);
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

	if (snprintf(b, 300, "%s/%s", g_dir, name) >= 300)
		abort();
	return b;
}

static ntfsmac_spawn_config base_config(void)
{
	ntfsmac_spawn_config c;

	ntfsmac_spawn_config_default(&c);
	c.sandbox_dir = g_sbx;
	c.sandbox_exec_path = g_wrap;
	c.sandbox_owner = geteuid(); /* the profiles here are ours, not root's */
	c.unpriv_uid = getuid();
	c.unpriv_gid = getgid();
	c.mount_poll_ms = 10;
	c.volmark_write_wait_s = 1;
	c.mount_lookup = fake_lookup;
	c.child_stderr_fd = -1;
	return c;
}

/* The text the fake wrote, or "" if it never ran. */
static const char *wrapped(void)
{
	static char buf[4096];
	FILE *f = fopen(g_log, "r");
	size_t n;

	buf[0] = '\0';
	if (f == NULL)
		return buf;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	buf[n] = '\0';
	fclose(f);
	return buf;
}

static void forget(void)
{
	(void)unlink(g_log);
}

static bool has(const char *text, const char *needle)
{
	return strstr(text, needle) != NULL;
}

static int open_device(void)
{
	char path[300];
	int fd;

	PATHF(path, "%s/device.img", g_dir);
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0 || write(fd, "ABCDEFGH", 8) != 8) {
		perror("device");
		exit(2);
	}
	return fd;
}

static void kill_worker(ntfsmac_child *child)
{
	bool exited = false;
	int st, i;

	(void)ntfsmac_child_kill_group(child);
	for (i = 0; i < 200 && !exited; i++) {
		struct timespec ts = { 0, 20 * 1000000L };

		if (ntfsmac_child_poll(child, &exited, &st) != NTFSMAC_DMN_OK)
			break;
		if (!exited)
			nanosleep(&ts, NULL);
	}
}

/* ---------------------------------------------------------------- */

/* U-SBX-01: the probe starts through sandbox-exec with probe.sb and the three parameters. */
static void test_probe(void)
{
	ntfsmac_spawn_config c = base_config();
	ntfsmac_probe_verdict v;
	ntfsmac_probe_run run;
	char want[400];
	const char *w;
	int fd = open_device();

	link_as("probe-ok");
	c.probe_path = path_of("probe-ok");
	ntfsmac_spawn_config_set(&c);
	run.device_fd = fd;
	run.timeout_s = 5;
	forget();
	CHECK_EQ_INT(ntfsmac_spawn_probe(&run, &v), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(v.state, NTFSMAC_PROBE_STATE_CLEAN);
	w = wrapped();
	CHECK(has(w, "-p " PROFILE_TEXT_PROBE)); /* the text of the file, not its name */
	CHECK(!has(w, " -f "));
	CHECK(!has(w, "probe.sb"));
	PATHF(want, "-D EXEC_PATH=%s ", g_stub_real); /* the real path, not the link */
	CHECK(has(w, want));
	CHECK(has(w, "-D CTL_SOCKET=" NTFSMAC_CTL_SOCKET_PATH " "));
	CHECK(has(w, ctl_real_param())); /* U-SBX-10 */
	CHECK(has(w, "-D XPC_SERVICE=" NTFSMAC_CTL_XPC_SERVICE " "));
	PATHF(want, "%s/probe-ok --fd ", g_dir);
	CHECK(has(w, want)); /* the program and its own arguments follow the options */
	CHECK(!has(w, "USERMAPPING"));
	close(fd);
}

/* U-SBX-02: volmark likewise, with volmark.sb. */
static void test_volmark(void)
{
	ntfsmac_spawn_config c = base_config();
	ntfsmac_volmark_run run;
	const char *w;
	int fd = open_device(), code = -9;

	link_as("volmark-set");
	c.volmark_path = path_of("volmark-set");
	ntfsmac_spawn_config_set(&c);
	run.device_fd = fd;
	run.serial = 0x12AB;
	run.total_sectors = 2048;
	run.timeout_s = 5;
	forget();
	CHECK_EQ_INT(ntfsmac_spawn_volmark(&run, true, &code), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(code, 0);
	w = wrapped();
	CHECK(has(w, "-p " PROFILE_TEXT_VOLMARK));
	CHECK(has(w, ctl_real_param()));
	CHECK(has(w, "-D XPC_SERVICE=" NTFSMAC_CTL_XPC_SERVICE " "));
	CHECK(has(w, "set --fd "));
	CHECK(!has(w, "USERMAPPING"));
	close(fd);
}

/* U-SBX-03: the worker gets worker.sb and the two UserMapping parameters, set to "no". */
static void test_worker(void)
{
	ntfsmac_spawn_config c = base_config();
	ntfsmac_worker_run run;
	ntfsmac_child child;
	char want[400];
	const char *w;
	bool ro = true;
	int fd = open_device(), i;

	link_as("worker-ok");
	c.worker_path = path_of("worker-ok");
	ntfsmac_spawn_config_set(&c);
	memset(&run, 0, sizeof(run));
	run.device_fd = fd;
	run.errfd_write = -1;
	run.mount_point = "/Volumes/SBXTEST";
	run.console_uid = getuid();
	run.console_gid = getgid();
	run.mount_wait_s = 5;
	forget();
	CHECK_EQ_INT(ntfsmac_spawn_worker(&run, &child, &ro), NTFSMAC_DMN_OK);
	/* The call returns once the shell script is running; it writes its file a moment later. */
	for (i = 0; i < 300 && wrapped()[0] == '\0'; i++) {
		struct timespec ts = { 0, 10 * 1000000L };

		nanosleep(&ts, NULL);
	}
	w = wrapped();
	CHECK(has(w, "-p " PROFILE_TEXT_WORKER));
	CHECK(has(w, "-D CTL_SOCKET=" NTFSMAC_CTL_SOCKET_PATH " "));
	CHECK(has(w, ctl_real_param()));
	CHECK(has(w, "-D XPC_SERVICE=" NTFSMAC_CTL_XPC_SERVICE " "));
	CHECK(has(w, "-D USERMAPPING_PATH=" NTFSMAC_USERMAPPING_PATH " "));
	CHECK(has(w, "-D USERMAPPING=no "));
	PATHF(want, "%s/worker-ok /dev/fd/%d /Volumes/SBXTEST -o ", g_dir, fd);
	CHECK(has(w, want));
	kill_worker(&child);
	close(fd);
}

/* U-SBX-04, U-SBX-11: a profile that is missing, not root's and private, or not readable as one
 * piece of text, stops the spawn before anything starts. */
static void test_bad_profile(void)
{
	ntfsmac_spawn_config c = base_config();
	ntfsmac_probe_verdict v;
	ntfsmac_probe_run run;
	char good[320], other[320];
	int fd = open_device();

	link_as("probe-ok");
	c.probe_path = path_of("probe-ok");
	run.device_fd = fd;
	run.timeout_s = 5;
	PATHF(good, "%s/probe.sb", g_sbx);
	PATHF(other, "%s/probe-copy.sb", g_sbx);

	/* missing */
	(void)unlink(good);
	ntfsmac_spawn_config_set(&c);
	forget();
	v.state = NTFSMAC_PROBE_STATE_CLEAN;
	CHECK_EQ_INT(ntfsmac_spawn_probe(&run, &v), NTFSMAC_DMN_E_DENIED);
	CHECK_EQ_INT(v.state, NTFSMAC_PROBE_STATE_UNKNOWN);
	CHECK(!has(wrapped(), "probe"));

	/* writable by the group, writable by others */
	write_file(good, "(version 1)\n", 0664);
	forget();
	CHECK_EQ_INT(ntfsmac_spawn_probe(&run, &v), NTFSMAC_DMN_E_DENIED);
	CHECK(!has(wrapped(), "probe"));
	write_file(good, "(version 1)\n", 0646);
	CHECK_EQ_INT(ntfsmac_spawn_probe(&run, &v), NTFSMAC_DMN_E_DENIED);
	CHECK(!has(wrapped(), "probe"));

	/* a link to a good file: the profile must be the file itself */
	write_file(other, "(version 1)\n", 0644);
	(void)unlink(good);
	if (symlink(other, good) != 0) {
		perror("symlink");
		exit(2);
	}
	CHECK_EQ_INT(ntfsmac_spawn_probe(&run, &v), NTFSMAC_DMN_E_DENIED);
	CHECK(!has(wrapped(), "probe"));
	(void)unlink(good);

	/* a directory */
	if (mkdir(good, 0755) != 0) {
		perror("mkdir");
		exit(2);
	}
	CHECK_EQ_INT(ntfsmac_spawn_probe(&run, &v), NTFSMAC_DMN_E_DENIED);
	CHECK(!has(wrapped(), "probe"));
	(void)rmdir(good);

	/* U-SBX-11: a file over the size limit (32 KiB), an empty file, and a file with a NUL byte */
	{
		size_t len = 40000;
		char *big = malloc(len + 1);
		int fd;

		if (big == NULL)
			exit(2);
		memset(big, ' ', len);
		big[len] = '\0';
		memcpy(big, "(version 1)\n", 12);
		write_file(good, big, 0644);
		free(big);
		forget();
		CHECK_EQ_INT(ntfsmac_spawn_probe(&run, &v), NTFSMAC_DMN_E_DENIED);
		CHECK(!has(wrapped(), "probe"));
		write_file(good, "", 0644);
		CHECK_EQ_INT(ntfsmac_spawn_probe(&run, &v), NTFSMAC_DMN_E_DENIED);
		CHECK(!has(wrapped(), "probe"));
		fd = open(good, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0 || write(fd, "(version 1)\0(deny default)\n", 27) != 27)
			exit(2);
		close(fd);
		CHECK_EQ_INT(ntfsmac_spawn_probe(&run, &v), NTFSMAC_DMN_E_DENIED);
		CHECK(!has(wrapped(), "probe"));
	}

	/* owned by someone else than root (here: than the user who must own it) */
	write_file(good, "(version 1)\n", 0644);
	c.sandbox_owner = geteuid() + 1;
	ntfsmac_spawn_config_set(&c);
	CHECK_EQ_INT(ntfsmac_spawn_probe(&run, &v), NTFSMAC_DMN_E_DENIED);
	CHECK(!has(wrapped(), "probe"));
	c.sandbox_owner = geteuid();
	ntfsmac_spawn_config_set(&c);

	/* the good file works again: the refusals above were about the file */
	CHECK_EQ_INT(ntfsmac_spawn_probe(&run, &v), NTFSMAC_DMN_OK);
	CHECK(has(wrapped(), "-p (version 1)\n -D EXEC_PATH="));
	close(fd);
}

/* U-SBX-05: if the profile does not load, the program does not run; it never runs unsandboxed. */
static void test_wrapper_failure(void)
{
	ntfsmac_spawn_config c = base_config();
	ntfsmac_probe_verdict v;
	ntfsmac_probe_run run;
	char marker[320];
	int fd = open_device();

	link_as("probe-ok");
	c.probe_path = path_of("probe-ok");
	run.device_fd = fd;
	run.timeout_s = 5;

	/* sandbox-exec exits non-zero before it starts the program */
	write_file(g_fail, "x\n", 0600);
	ntfsmac_spawn_config_set(&c);
	CHECK_EQ_INT(ntfsmac_spawn_probe(&run, &v), NTFSMAC_DMN_E_IO);
	CHECK_EQ_INT(v.state, NTFSMAC_PROBE_STATE_UNKNOWN);
	(void)unlink(g_fail);

	/* sandbox-exec is missing: the start fails, nothing falls back to the bare program */
	PATHF(marker, "%s/no-such-sandbox-exec", g_dir);
	c.sandbox_exec_path = marker;
	ntfsmac_spawn_config_set(&c);
	forget();
	CHECK_EQ_INT(ntfsmac_spawn_probe(&run, &v), NTFSMAC_DMN_E_IO);
	CHECK_EQ_INT(v.state, NTFSMAC_PROBE_STATE_UNKNOWN);
	CHECK(!has(wrapped(), "probe"));
	close(fd);
}

/* U-SBX-06: with no sandbox_dir (tests only) nothing is wrapped. */
static void test_no_sandbox(void)
{
	ntfsmac_spawn_config c = base_config();
	ntfsmac_probe_verdict v;
	ntfsmac_probe_run run;
	int fd = open_device();

	link_as("probe-ok");
	c.probe_path = path_of("probe-ok");
	c.sandbox_dir = NULL;
	ntfsmac_spawn_config_set(&c);
	run.device_fd = fd;
	run.timeout_s = 5;
	forget();
	CHECK_EQ_INT(ntfsmac_spawn_probe(&run, &v), NTFSMAC_DMN_OK);
	CHECK(!has(wrapped(), "probe"));
	close(fd);
}

/* U-SBX-07: the default configuration of the daemon is sandboxed on macOS, and names the install paths. */
static void test_defaults(void)
{
	ntfsmac_spawn_config c;

	ntfsmac_spawn_config_default(&c);
#if defined(__APPLE__)
	CHECK(c.sandbox_dir != NULL);
	CHECK(c.sandbox_dir != NULL && strcmp(c.sandbox_dir, NTFSMAC_PATH_SANDBOX_DIR) == 0);
#else
	CHECK(c.sandbox_dir == NULL);
#endif
	CHECK(c.sandbox_exec_path == NULL); /* filled in with NTFSMAC_PATH_SANDBOX_EXEC at use */
	CHECK_EQ_INT(c.sandbox_owner, 0);
}

int main(void)
{
	const char *tmp = getenv("TMPDIR");
	char tmpl[256], text[600];
	char cmd[300];

	if (tmp == NULL || tmp[0] == '\0')
		tmp = "/tmp";
	PATHF(tmpl, "%s/sandbox-test.XXXXXX", tmp);
	if (mkdtemp(tmpl) == NULL) {
		perror("mkdtemp");
		return 2;
	}
	if (realpath(tmpl, g_dir) == NULL || realpath(SPAWN_STUB_PATH, g_stub_real) == NULL) {
		perror("realpath");
		return 2;
	}
	PATHF(g_sbx, "%s/sandbox", g_dir);
	PATHF(g_wrap, "%s/sandbox-exec", g_dir);
	PATHF(g_log, "%s/wrap.log", g_dir);
	PATHF(g_fail, "%s/wrap.fail", g_dir);
	if (mkdir(g_sbx, 0755) != 0) {
		perror("mkdir");
		return 2;
	}
	{
		const char *names[] = { "probe.sb", "volmark.sb", "worker.sb" };
		const char *texts[] = { PROFILE_TEXT_PROBE, PROFILE_TEXT_VOLMARK, PROFILE_TEXT_WORKER };
		size_t i;

		for (i = 0; i < 3; i++) {
			PATHF(cmd, "%s/%s", g_sbx, names[i]);
			write_file(cmd, texts[i], 0644);
		}
	}
	PATHF(text,
		 "#!/bin/sh\n"
		 "printf '%%s\\n' \"$*\" > '%s'\n"
		 "[ -e '%s' ] && exit 65\n"
		 "shift 2\n"
		 "while [ \"$1\" = -D ]; do shift 2; done\n"
		 "exec \"$@\"\n",
		 g_log, g_fail);
	write_file(g_wrap, text, 0755);
	signal(SIGPIPE, SIG_IGN);

	test_probe();
	test_volmark();
	test_worker();
	test_bad_profile();
	test_wrapper_failure();
	test_no_sandbox();
	test_defaults();

	PATHF(cmd, "rm -rf '%s'", g_dir);
	if (system(cmd) != 0)
		fprintf(stderr, "warning: could not remove %s\n", g_dir);
	if (g_failures != 0) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	printf("test_sandbox: ok\n");
	return 0;
}
