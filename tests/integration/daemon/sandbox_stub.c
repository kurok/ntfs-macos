/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * T-M4-06: stand-in for the probe, volmark and the worker that tries what a
 * compromised program would try. sandbox_vm starts it through the spawn module,
 * so it runs under the real profile of integration/sandbox/, and once without
 * a profile as a control. Every line goes to stderr, which the driver reads:
 *
 *   PASS: <check>      the result was the expected one
 *   FAIL: <check>: ... it was not
 *
 * The exit status is 0 if no check failed. The role and the kind come from the
 * base name of the path the stub is started under:
 *
 *   probe-sbx volmark-sbx worker-sbx worker-sbx-um   under the profile; the last one with
 *                                                    the usermapping= option set
 *   probe-ctl volmark-ctl worker-ctl worker-ctl-um   no profile: every check must succeed
 *                                                    (the control that shows that a denial
 *                                                    above is the profile and nothing else)
 *   worker-sbx-broad                                 as worker-sbx, under a copy of the profile that
 *                                                    allows every socket and Mach service before its
 *                                                    last two rules: only those two stay denied
 *   worker-sbx-desc, worker-ctl-desc                 as the worker, plus a descendant that
 *                                                    leaves the process group, outlives the
 *                                                    worker and then tries the control socket
 *                                                    and the privileged XPC service; it keeps
 *                                                    running afterwards, holding the device
 *                                                    descriptor, until the teardown kills it
 *   worker-sbx-desc-broad                            the same under the widened profile
 *   worker-sbx-tree, worker-ctl-tree                 the same descendant, but the worker stays
 *                                                    alive until it is killed, so the tree walk
 *                                                    of the teardown must find the descendant
 *   a name with -cwd                                 the control socket is also tried by a
 *                                                    relative path; the driver starts these in
 *                                                    the directory of the socket
 *
 * Stdout carries a probe verdict or the volmark `w`/`d` lines, as the stand-ins
 * of tests/unit/spawn do, so that the spawn module accepts a successful run.
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdbool.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <mach/mach.h>
#include <servers/bootstrap.h>

#include "ntfsmac/ctl_proto.h"
#include "ntfsmac/options.h"
#include "sandbox_test.h"

extern char **environ;

static int g_fails;
static bool g_sbx;   /* under a profile: the checks expect denials */
static bool g_broad; /* the profile also allows every socket and Mach service ahead of its denials */
static bool g_cwd;   /* the start directory is the directory of the control socket */

/* The control socket under the other spelling: /var is a link to /private/var (audit M3). */
#define CTL_ALT_PATH "/private" NTFSMAC_CTL_SOCKET_PATH
/* The mach lookup result of a sandbox denial (BOOTSTRAP_NOT_PRIVILEGED). */
#define LOOKUP_DENIED 1100

static void say(bool ok, const char *name, const char *fmt, ...)
{
	char detail[160];
	va_list ap;

	detail[0] = '\0';
	if (fmt != NULL) {
		va_start(ap, fmt);
		(void)vsnprintf(detail, sizeof(detail), fmt, ap);
		va_end(ap);
	}
	fprintf(stderr, "%s: %s%s%s\n", ok ? "PASS" : "FAIL", name, detail[0] ? ": " : "", detail);
	if (!ok)
		g_fails++;
}

/* A call that must fail with EPERM or EACCES under the profile and succeed without it. */
static void expect(const char *name, int rc, int err)
{
	if (g_sbx)
		say(rc < 0 && (err == EPERM || err == EACCES), name, "denied expected, got %s",
		    rc < 0 ? strerror(err) : "success");
	else
		say(rc >= 0, name, "success expected, got %s", rc < 0 ? strerror(err) : "success");
}

/* As expect(), for a call that the profile allows: success in both runs. */
static void allowed(const char *name, int rc, int err)
{
	say(rc >= 0, name, "allowed expected, got %s", rc < 0 ? strerror(err) : "success");
}

static int try_open(const char *path, int flags)
{
	int fd = open(path, flags | O_CLOEXEC, 0600);
	int e = errno;

	if (fd >= 0)
		close(fd);
	errno = e;
	return fd;
}

static int try_tcp(void)
{
	struct sockaddr_in a;
	int s = socket(AF_INET, SOCK_STREAM, 0), rc, e;

	if (s < 0)
		return -1;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(SBX_TCP_PORT);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	rc = connect(s, (struct sockaddr *)&a, sizeof(a));
	e = errno;
	close(s);
	errno = e;
	return rc;
}

static int try_unix(const char *path)
{
	struct sockaddr_un a;
	int s = socket(AF_UNIX, SOCK_STREAM, 0), rc, e;

	if (s < 0)
		return -1;
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
	rc = connect(s, (struct sockaddr *)&a, sizeof(a));
	e = errno;
	close(s);
	errno = e;
	return rc;
}

/* 0 if the Mach service name resolves; otherwise the bootstrap error (1100 is a sandbox denial). */
static int lookup_service(const char *name)
{
	mach_port_t p = MACH_PORT_NULL;
	kern_return_t kr;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	kr = bootstrap_look_up(bootstrap_port, name, &p);
#pragma clang diagnostic pop
	if (kr == KERN_SUCCESS)
		mach_port_deallocate(mach_task_self(), p);
	return (int)kr;
}

/* Under a profile a lookup must give the sandbox's own code, not some other failure. */
static void expect_lookup(const char *label, const char *name)
{
	int kr = lookup_service(name);

	if (g_sbx && !(g_broad && strcmp(name, NTFSMAC_CTL_XPC_SERVICE) != 0))
		say(kr == LOOKUP_DENIED, label, "sandbox denial (%d) expected, lookup gave %d", LOOKUP_DENIED, kr);
	else
		say(kr == 0, label, "success expected, lookup gave %d", kr);
}

/* The control socket under any spelling the stub tries: only these stay denied in a widened profile. */
static bool is_ctl_path(const char *path)
{
	return strcmp(path, NTFSMAC_CTL_SOCKET_PATH) == 0 || strcmp(path, CTL_ALT_PATH) == 0
	       || strcmp(path, strrchr(NTFSMAC_CTL_SOCKET_PATH, '/') + 1) == 0;
}

static void expect_unix(const char *label, const char *path)
{
	int rc = try_unix(path);

	if (g_broad && !is_ctl_path(path))
		allowed(label, rc, errno);
	else
		expect(label, rc, errno);
}

/*
 * The descendant: its own process group, then (if its parent is to exit) no parent, then the two
 * doors of ADR-019. It tells the driver its pid and, when it is done, runs on holding the device
 * descriptor it inherited; the teardown of the worker has to find and kill it.
 */
static void descendant(int notify_fd, bool wait_orphan)
{
	int i, rc, e;
	pid_t pg;

	(void)setpgid(0, 0);
	pg = getpgrp();
	fprintf(stderr, "DESC-PID %d\n", (int)getpid());
	if (wait_orphan) {
		for (i = 0; i < 200 && getppid() != 1; i++) /* wait until the worker is gone */
			usleep(50000);
		say(getppid() == 1, "desc: lost its parent", "ppid %d", (int)getppid());
	}
	say(pg == getpid(), "desc: leads its own process group", "pgid %d pid %d", (int)pg, (int)getpid());
	rc = try_unix(NTFSMAC_CTL_SOCKET_PATH);
	e = errno;
	if (g_sbx)
		say(rc < 0 && (e == EPERM || e == EACCES), "desc: connect() to the control socket",
		    "denied expected, got %s", rc < 0 ? strerror(e) : "success");
	else
		say(rc == 0, "desc: connect() to the control socket", "success expected, got %s",
		    rc < 0 ? strerror(e) : "success");
	rc = try_unix(CTL_ALT_PATH);
	e = errno;
	if (g_sbx)
		say(rc < 0 && (e == EPERM || e == EACCES), "desc: connect() to the control socket, resolved spelling",
		    "denied expected, got %s", rc < 0 ? strerror(e) : "success");
	else
		say(rc == 0, "desc: connect() to the control socket, resolved spelling", "success expected, got %s",
		    rc < 0 ? strerror(e) : "success");
	expect_lookup("desc: lookup of the privileged XPC service", NTFSMAC_CTL_XPC_SERVICE);
	fprintf(stderr, "DESC-DONE %d\n", g_fails);
	fflush(stderr);
	if (notify_fd >= 0 && write(notify_fd, "x", 1) != 1)
		_exit(1);
	for (;;)
		sleep(60);
}

/* `wait_orphan`: the worker exits at once; else it stays and is told by a byte on `*notify_rd`. */
static void start_descendant(bool wait_orphan, int *notify_rd)
{
	int p[2] = { -1, -1 };
	pid_t pid;

	*notify_rd = -1;
	if (!wait_orphan && pipe(p) != 0) {
		say(false, "desc: pipe", "%s", strerror(errno));
		return;
	}
	pid = fork();
	if (pid < 0) {
		say(false, "desc: fork", "%s", strerror(errno));
		return;
	}
	if (pid == 0) {
		if (p[0] >= 0)
			close(p[0]);
		descendant(p[1], wait_orphan);
	}
	if (p[1] >= 0)
		close(p[1]);
	*notify_rd = p[0];
}

/* ------------------------------------------------------------------ */

static void common_checks(const char *role)
{
	char name[96];
	int rc;
	int st;

	snprintf(name, sizeof(name), "%s: open /etc/passwd", role);
	rc = try_open("/etc/passwd", O_RDONLY);
	expect(name, rc, errno);
	snprintf(name, sizeof(name), "%s: create a file in /private/tmp", role);
	{
		char path[64];

		snprintf(path, sizeof(path), "/private/tmp/sbx-stub-%d", (int)getpid());
		rc = try_open(path, O_WRONLY | O_CREAT | O_EXCL);
		expect(name, rc, errno);
		(void)unlink(path);
	}
	snprintf(name, sizeof(name), "%s: open the directory /Users", role);
	rc = try_open("/Users", O_RDONLY);
	expect(name, rc, errno);
	snprintf(name, sizeof(name), "%s: open /usr/local/etc/ntfs-macos/other", role);
	rc = try_open(SBX_OTHER_FILE, O_RDONLY);
	expect(name, rc, errno);
	snprintf(name, sizeof(name), "%s: TCP connect to 127.0.0.1", role);
	rc = try_tcp();
	if (g_broad)
		allowed(name, rc, errno);
	else
		expect(name, rc, errno);
	snprintf(name, sizeof(name), "%s: connect() to another Unix socket", role);
	expect_unix(name, SBX_OTHER_SOCKET);
	snprintf(name, sizeof(name), "%s: connect() to the control socket", role);
	expect_unix(name, NTFSMAC_CTL_SOCKET_PATH);
	snprintf(name, sizeof(name), "%s: connect() to the control socket, resolved spelling of its path", role);
	expect_unix(name, CTL_ALT_PATH);
	if (g_cwd) {
		snprintf(name, sizeof(name), "%s: connect() to the control socket by a relative path", role);
		expect_unix(name, strrchr(NTFSMAC_CTL_SOCKET_PATH, '/') + 1);
	}
	snprintf(name, sizeof(name), "%s: lookup of the privileged XPC service", role);
	expect_lookup(name, NTFSMAC_CTL_XPC_SERVICE);
	snprintf(name, sizeof(name), "%s: lookup of another Mach service", role);
	expect_lookup(name, SBX_OTHER_SERVICE);

	/* Starting the system's own `true`: not part of any role. */
	{
		char *av[2] = { (char *)"/usr/bin/true", NULL };
		pid_t pid = 0;

		snprintf(name, sizeof(name), "%s: start /usr/bin/true", role);
		rc = posix_spawn(&pid, "/usr/bin/true", NULL, NULL, av, environ);
		if (rc == 0) {
			(void)waitpid(pid, &st, 0);
			rc = 0;
		} else {
			errno = rc;
			rc = -1;
		}
		expect(name, rc, errno);
	}
}

static const char *base_of(const char *path)
{
	const char *s = strrchr(path, '/');

	return s != NULL ? s + 1 : path;
}

static int check_device_fd(int fd)
{
	char b[4];

	return pread(fd, b, 4, 0) == 4 && memcmp(b, "ABCD", 4) == 0;
}

int main(int argc, char **argv)
{
	const char *mode = base_of(argv[0]);
	bool probe = strncmp(mode, "probe-", 6) == 0, volmark = strncmp(mode, "volmark-", 8) == 0;
	bool worker = strncmp(mode, "worker-", 7) == 0;
	bool um = strstr(mode, "-um") != NULL, desc = strstr(mode, "-desc") != NULL;
	bool tree = strstr(mode, "-tree") != NULL;
	const char *role = probe ? "probe" : volmark ? "volmark" : "worker";
	char name[96];
	int fd = -1, rc;

	g_sbx = strstr(mode, "-sbx") != NULL;
	g_broad = strstr(mode, "-broad") != NULL;
	g_cwd = strstr(mode, "-cwd") != NULL;
	if (!probe && !volmark && !worker)
		return 99;
	say(true, "start", "%s %s", mode, g_sbx ? "(profile applied)" : "(no profile)");

	/* The device descriptor is the one thing every role must still be able to use. */
	if (probe && argc == 3 && strcmp(argv[1], "--fd") == 0)
		fd = atoi(argv[2]);
	else if (volmark && argc == 8)
		fd = atoi(argv[3]);
	else if (worker && argc == 5 && strncmp(argv[1], "/dev/fd/", 8) == 0)
		fd = atoi(argv[1] + 8);
	if (fd < 3) {
		say(false, "arguments", "no descriptor in the vector");
		return 2;
	}
	snprintf(name, sizeof(name), "%s: read the inherited descriptor", role);
	say(check_device_fd(fd), name, NULL);

	common_checks(role);

	if (worker) {
		/* /dev/fd/N is a path: opening it is not allowed, the descriptor is enough. */
		snprintf(name, sizeof(name), "%s: open the path /dev/fd/N", role);
		rc = try_open(argv[1], O_RDONLY);
		expect(name, rc, errno);

		/* The one file read that the opt-in allows (ADR-012). */
		snprintf(name, sizeof(name), "%s: open the UserMapping file", role);
		rc = try_open(NTFSMAC_USERMAPPING_PATH, O_RDONLY);
		if (g_sbx && !um)
			expect(name, rc, errno);
		else
			allowed(name, rc, errno);

		/* The Mach service of macFUSE is the one lookup the worker needs. */
		snprintf(name, sizeof(name), "%s: lookup of io.macfuse.mount", role);
		rc = lookup_service("io.macfuse.mount") == 0 ? 0 : -1;
		allowed(name, rc, EPERM);

		/* The macFUSE helper is started by a fork, so fork is allowed. */
		snprintf(name, sizeof(name), "%s: fork", role);
		{
			pid_t p = fork();

			if (p == 0)
				_exit(0);
			if (p > 0)
				(void)waitpid(p, NULL, 0);
			allowed(name, p < 0 ? -1 : 0, errno);
		}
		if (desc || tree) {
			int rd;
			char b;

			start_descendant(desc, &rd);
			if (tree) {
				/* Stay alive until the descendant is done, then until the driver kills the tree. */
				if (rd < 0 || read(rd, &b, 1) != 1)
					say(false, "tree: the descendant did not report", NULL);
				for (;;)
					sleep(60);
			}
		}
	} else {
		snprintf(name, sizeof(name), "%s: fork", role);
		{
			pid_t p = fork();

			if (p == 0)
				_exit(0);
			if (p > 0)
				(void)waitpid(p, NULL, 0);
			expect(name, p < 0 ? -1 : 0, errno);
		}
		snprintf(name, sizeof(name), "%s: open the path /dev/fd/%d", role, fd);
		{
			char path[32];

			snprintf(path, sizeof(path), "/dev/fd/%d", fd);
			rc = try_open(path, O_RDONLY);
			expect(name, rc, errno);
		}
	}

	if (probe) {
		if (g_fails == 0)
			printf("{\"schema\":1,\"is_ntfs\":true,\"ntfs_version\":\"3.1\",\"label\":\"DATA\","
			       "\"serial\":\"0x1A2B3C4D5E6F7788\",\"total_sectors\":131071,\"cluster_size\":4096,"
			       "\"sector_size\":512,\"volume_flags\":[],\"logfile\":\"clean\","
			       "\"logfile_lsn\":\"0x0000000001A2B3C4\",\"hiberfile\":\"absent\","
			       "\"bitlocker\":false,\"state\":\"clean\",\"reason_codes\":[],"
			       "\"nonnfc_count\":0,\"nonnfc_paths\":[]}\n");
		else
			printf("BAD:checks failed\n");
		fflush(stdout);
	} else if (volmark) {
		printf("w\nd\n");
		fflush(stdout);
	}
	return g_fails == 0 ? 0 : 1;
}
