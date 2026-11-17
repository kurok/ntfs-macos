/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Stand-in for ntfsmac-probe, ntfsmac-volmark and ntfsmac-worker in the tests
 * of src/daemon/spawn.c. The behaviour comes from the base name of argv[0].
 * Every mode first checks what the spawn module promises about the child's
 * state (descriptors, process group, signals, argument vector, environment);
 * a failed check shows as a bad output or a non-zero exit, which the test sees.
 * Diagnostics go to stderr, which the test points at a file.
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
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

static const char *g_why;

static int fd_open(int fd)
{
	return fcntl(fd, F_GETFD) != -1;
}

/* Only 0 to 2 and the named descriptors are open. */
static int only_open(const int *keep, int nkeep)
{
	int fd, i;

	for (fd = 3; fd < 256; fd++) {
		int want = 0;

		for (i = 0; i < nkeep; i++)
			if (keep[i] == fd)
				want = 1;
		if (fd_open(fd) != want) {
			fprintf(stderr, "fd %d is %s\n", fd, want ? "closed" : "open");
			g_why = "descriptors";
			return 0;
		}
	}
	return 1;
}

static int state_ok(void)
{
	struct sigaction sa;
	sigset_t cur;
	int s;

	if (getpgrp() != getpid()) {
		g_why = "pgrp";
		return 0;
	}
	for (s = 1; s < 32; s++) {
		if (s == SIGKILL || s == SIGSTOP)
			continue;
		if (sigaction(s, NULL, &sa) != 0 || sa.sa_handler != SIG_DFL) {
			fprintf(stderr, "signal %d not default\n", s);
			g_why = "signals";
			return 0;
		}
	}
	sigprocmask(SIG_SETMASK, NULL, &cur);
	for (s = 1; s < 32; s++) {
		if (sigismember(&cur, s) == 1) {
			g_why = "sigmask";
			return 0;
		}
	}
	return 1;
}

static const char *base(const char *p)
{
	const char *b = strrchr(p, '/');

	return b ? b + 1 : p;
}

/* A second process in the same group that ignores SIGTERM; its pid is reported. */
static void spawn_grandchild(void)
{
	pid_t g = fork();

	if (g == 0) {
		signal(SIGTERM, SIG_IGN);
		for (;;)
			sleep(60);
	}
	fprintf(stderr, "pids %d %d\n", (int)getpid(), (int)g);
	fflush(stderr);
}

/*
 * worker-tree: a child that leaves the process group (setpgid) and has a child of its own that
 * leaves the session (setsid). Both keep running. Reported: "tree <worker> <child> <grandchild>".
 * worker-orphan: one child that leaves the group; the worker exits after a moment, and the child
 * stays behind with the descriptors it inherited. Reported: "orphan <worker> <child>".
 */
static void spawn_leavers(bool deep)
{
	pid_t c, g = 0;
	int p[2];

	if (deep && pipe(p) != 0)
		return;
	c = fork();
	if (c == 0) {
		(void)setpgid(0, 0);
		if (deep) {
			g = fork();
			if (g == 0) {
				(void)setsid();
				for (;;)
					sleep(60);
			}
			if (write(p[1], &g, sizeof(g)) != (ssize_t)sizeof(g))
				_exit(1);
		}
		for (;;)
			sleep(60);
	}
	if (deep) {
		close(p[1]);
		if (read(p[0], &g, sizeof(g)) != (ssize_t)sizeof(g))
			g = 0;
		close(p[0]);
		fprintf(stderr, "tree %d %d %d\n", (int)getpid(), (int)c, (int)g);
	} else {
		fprintf(stderr, "orphan %d %d\n", (int)getpid(), (int)c);
	}
	fflush(stderr);
}

/* NTFSMAC_ERRFD value, NULL if absent, (const char *)-1 if the environment is wrong. */
static const char *env_errfd(void)
{
	char **e;
	const char *r = NULL;
	int n = 0;

	for (e = environ; *e; e++) {
		n++;
		if (strncmp(*e, "NTFSMAC_ERRFD=", 14) == 0)
			r = *e + 14;
	}
	/* PATH, and NTFSMAC_ERRFD when there is a channel: nothing else. */
	if (n != (r ? 2 : 1))
		return (const char *)-1;
	return r;
}

static int do_probe(int argc, char **argv, const char *mode)
{
	int fd;
	char b[4];

	if (!strcmp(mode, "probe-hang")) {
		for (;;)
			sleep(60);
	}
	if (!strcmp(mode, "probe-abort"))
		abort();
	if (!strcmp(mode, "probe-garbage")) {
		fputs("{not json", stdout);
		return 0;
	}
	if (!strcmp(mode, "probe-big")) {
		int i;

		for (i = 0; i < 70000; i++)
			putchar(' ');
		return 0;
	}
	if (argc != 3 || strcmp(argv[1], "--fd") != 0)
		return 2;
	fprintf(stderr, "uid=%u gid=%u\n", (unsigned)getuid(), (unsigned)getgid());
	fd = atoi(argv[2]);
	if (!fd_open(fd) || pread(fd, b, 4, 0) != 4 || memcmp(b, "ABCD", 4) != 0) {
		g_why = "device";
	} else {
		int keep[1];

		keep[0] = fd;
		if (state_ok())
			(void)only_open(keep, 1);
	}
	if (g_why) {
		printf("BAD:%s\n", g_why);
		return 0;
	}
	printf("{\"schema\":1,\"is_ntfs\":true,\"ntfs_version\":\"3.1\",\"label\":\"DATA\","
	       "\"serial\":\"0x1A2B3C4D5E6F7788\",\"total_sectors\":131071,\"cluster_size\":4096,"
	       "\"sector_size\":512,\"volume_flags\":[],\"logfile\":\"clean\","
	       "\"logfile_lsn\":\"0x0000000001A2B3C4\",\"hiberfile\":\"absent\","
	       "\"bitlocker\":false,\"state\":\"clean\",\"reason_codes\":[],"
	       "\"nonnfc_count\":0,\"nonnfc_paths\":[]}\n");
	fflush(stdout);
	return !strcmp(mode, "probe-exit3") ? 3 : 0;
}

static int do_volmark(int argc, char **argv, const char *mode)
{
	int keep[1];

	if (!strcmp(mode, "volmark-hang")) {
		for (;;)
			sleep(60);
	}
	if (!strcmp(mode, "volmark-exit11"))
		return 11;
	if (argc != 8 || strcmp(argv[2], "--fd") != 0 || strcmp(argv[4], "--expect-serial") != 0
	    || strcmp(argv[5], "00000000000012AB") != 0 || strcmp(argv[6], "--expect-sectors") != 0
	    || strcmp(argv[7], "2048") != 0)
		return 2;
	if ((!strcmp(mode, "volmark-set") && strcmp(argv[1], "set") != 0)
	    || (!strcmp(mode, "volmark-clear") && strcmp(argv[1], "clear") != 0)
	    || (!strcmp(mode, "volmark-clearrepair") && strcmp(argv[1], "clear-repair") != 0))
		return 2;
	fprintf(stderr, "uid=%u gid=%u\n", (unsigned)getuid(), (unsigned)getgid());
	keep[0] = atoi(argv[3]);
	if (!fd_open(keep[0]) || !state_ok() || !only_open(keep, 1))
		return 3;
	printf("w\n");
	fflush(stdout);
	if (!strcmp(mode, "volmark-hangafterw")) {
		for (;;)
			sleep(60);
	}
	if (!strcmp(mode, "volmark-slowwrite"))
		sleep(2);
	printf("d\n");
	fflush(stdout);
	return 0;
}

static int do_worker(int argc, char **argv, const char *mode)
{
	const char *e;
	int keep[2], nkeep = 1, fd, ef = -1;

	if (argc != 5 || strncmp(argv[1], "/dev/fd/", 8) != 0 || strncmp(argv[2], "/Volumes/", 9) != 0
	    || strcmp(argv[3], "-o") != 0) {
		fprintf(stderr, "bad vector\n");
		return 2;
	}
	fd = atoi(argv[1] + 8);
	e = env_errfd();
	if (e == (const char *)-1) {
		fprintf(stderr, "bad environment\n");
		return 2;
	}
	keep[0] = fd;
	if (e) {
		ef = atoi(e);
		keep[nkeep++] = ef;
	}
	if (!fd_open(fd) || !state_ok() || !only_open(keep, nkeep)) {
		fprintf(stderr, "bad state: %s\n", g_why ? g_why : "device");
		return 3;
	}
	if (!strcmp(mode, "worker-nofskit") && strstr(argv[4], "backend=fskit") == NULL) {
		fprintf(stderr, "fskit=0\n");
		return 0;
	}
	if (!strcmp(mode, "worker-exit0"))
		return 0;
	if (ef >= 0 && write(ef, "S rw\n", 5) != 5)
		return 4;
	if (!strcmp(mode, "worker-tree")) {
		spawn_leavers(true);
		for (;;)
			sleep(60);
	}
	if (!strcmp(mode, "worker-orphan")) {
		spawn_leavers(false);
		usleep(300000);
		return 0;
	}
	spawn_grandchild();
	if (!strcmp(mode, "worker-hang"))
		signal(SIGTERM, SIG_IGN);
	if (!strcmp(mode, "worker-stop"))
		raise(SIGSTOP);
	for (;;)
		sleep(60);
}

int main(int argc, char **argv)
{
	const char *mode = base(argv[0]);

	if (!strncmp(mode, "probe-", 6))
		return do_probe(argc, argv, mode);
	if (!strncmp(mode, "volmark-", 8))
		return do_volmark(argc, argv, mode);
	if (!strncmp(mode, "worker-", 7))
		return do_worker(argc, argv, mode);
	return 99;
}
