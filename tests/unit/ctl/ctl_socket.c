/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-CTL-SOCK-01..05: the socket server of ctl.c, on a temporary socket, with
 * a fake peer lookup and fake operations.
 *   01 start and stop: argument checks, mode 0660, stale socket, regular file kept
 *   02 framing: one reply per line, pipelining, last line without newline
 *   03 limits: oversize line, 8 requests per connection, idle client
 *   04 peer: unknown peer and refused peer are closed with no reply
 *   05 the real peer lookup on a socket pair (uid, pid, audit token, parent chain)
 *   06 a refused or silent peer costs almost nothing (closed at once; 1 s to the first byte)
 *   07 a socket with a live daemon behind it is never taken over
 *   08 the definition of the right is checked at start and before each privileged request
 * Nothing here needs root, a signed client or an authorization.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "check.h"
#include "fakes.h"

static char dir[128];
static char path[160];

static void make_dir(void)
{
	const char *t = getenv("TMPDIR");

	snprintf(dir, sizeof dir, "%s%sntfsmac-ctl-XXXXXX", t != NULL ? t : "/tmp",
		 (t != NULL && t[strlen(t) - 1] == '/') ? "" : "/");
	if (mkdtemp(dir) == NULL) {
		perror("mkdtemp");
		exit(2);
	}
	snprintf(path, sizeof path, "%s/s", dir);
}

static void remove_dir(void)
{
	unlink(path);
	rmdir(dir);
}

static int dial(void)
{
	struct sockaddr_un sa;
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);

	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
	if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
		perror("connect");
		exit(2);
	}
	return fd;
}

static void send_str(int fd, const char *s, size_t n)
{
	while (n > 0) {
		ssize_t w = send(fd, s, n, 0);

		if (w <= 0)
			return;
		s += w;
		n -= (size_t)w;
	}
}

/* Read until `want` newlines, EOF or timeout. Returns bytes read. */
static size_t slurp(int fd, char *buf, size_t cap, int want, int timeout_ms, bool *eof)
{
	size_t have = 0;
	int lines = 0;
	long long end = (long long)time(NULL) * 1000 + timeout_ms;

	*eof = false;
	while (lines < want && have + 1 < cap) {
		struct pollfd p = {fd, POLLIN, 0};
		long long left = end - (long long)time(NULL) * 1000;
		ssize_t r;

		if (left <= 0 || poll(&p, 1, (int)left) <= 0)
			break;
		r = recv(fd, buf + have, cap - 1 - have, 0);
		if (r <= 0) {
			*eof = true;
			break;
		}
		while (r-- > 0)
			if (buf[have++] == '\n')
				lines++;
	}
	buf[have] = '\0';
	return have;
}

typedef struct srv {
	fake_sys_state s;
	ntfsmac_ctl_sys sys;
	ntfsmac_ctl_replay replay;
	ntfsmac_ctl_env env;
	fake_ops_state o;
	ntfsmac_ctl_ops ops;
	ntfsmac_ctl *ctl;
} srv;

static void srv_start(srv *v)
{
	memset(v, 0, sizeof *v);
	fake_sys_init(&v->s, &v->sys);
	ntfsmac_ctl_replay_init(&v->replay);
	v->env.sys = &v->sys;
	v->env.replay = &v->replay;
	v->ops = fake_ops();
	v->o.status = NTFSMAC_DMN_OK;
	v->o.reply = "{\"ok\":true,\"volumes\":[]}";
	CHECK_EQ_INT(ntfsmac_ctl_start_env(path, &v->ops, &v->o, &v->env, &v->ctl), NTFSMAC_DMN_OK);
}

static void test_start_stop(void)
{
	srv v;
	ntfsmac_ctl *c = (ntfsmac_ctl *)1;
	struct stat st;
	char longpath[300];
	int fd;

	/* arguments */
	memset(&v, 0, sizeof v);
	fake_sys_init(&v.s, &v.sys);
	ntfsmac_ctl_replay_init(&v.replay);
	v.env.sys = &v.sys;
	v.env.replay = &v.replay;
	v.ops = fake_ops();
	CHECK_EQ_INT(ntfsmac_ctl_start_env(NULL, &v.ops, &v.o, &v.env, &c), NTFSMAC_DMN_E_INVAL);
	CHECK(c == NULL);
	CHECK_EQ_INT(ntfsmac_ctl_start_env(path, NULL, &v.o, &v.env, &c), NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_ctl_start_env(path, &v.ops, &v.o, NULL, &c), NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_ctl_start_env(path, &v.ops, &v.o, &v.env, NULL), NTFSMAC_DMN_E_INVAL);
	memset(longpath, 'a', sizeof longpath);
	longpath[sizeof longpath - 1] = '\0';
	CHECK_EQ_INT(ntfsmac_ctl_start_env(longpath, &v.ops, &v.o, &v.env, &c), NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_ctl_start("/nonexistent-dir/x/s", &v.ops, &v.o, &c), NTFSMAC_DMN_E_IO);
	CHECK(c == NULL);
	ntfsmac_ctl_stop(NULL);

	/* a regular file at the path is never removed */
	fd = open(path, O_CREAT | O_WRONLY, 0600);
	CHECK(fd >= 0);
	close(fd);
	CHECK_EQ_INT(ntfsmac_ctl_start_env(path, &v.ops, &v.o, &v.env, &c), NTFSMAC_DMN_E_IO);
	CHECK(stat(path, &st) == 0 && S_ISREG(st.st_mode));
	unlink(path);

	/* normal start: socket with mode 0660; stop removes it */
	srv_start(&v);
	CHECK(stat(path, &st) == 0);
	CHECK(S_ISSOCK(st.st_mode));
	CHECK_EQ_INT(st.st_mode & 07777, 0660);
	ntfsmac_ctl_stop(v.ctl);
	CHECK(stat(path, &st) != 0);

	/* a stale socket file from a crashed daemon is replaced */
	{
		struct sockaddr_un sa;
		int s = socket(AF_UNIX, SOCK_STREAM, 0);

		memset(&sa, 0, sizeof sa);
		sa.sun_family = AF_UNIX;
		snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
		CHECK(bind(s, (struct sockaddr *)&sa, sizeof sa) == 0);
		close(s); /* the file stays, nobody listens */
	}
	srv_start(&v);
	{
		char buf[256];
		bool eof;
		int c1 = dial();

		send_str(c1, "{\"cmd\":\"list\"}\n", 15);
		slurp(c1, buf, sizeof buf, 1, 2000, &eof);
		CHECK(strcmp(buf, "{\"ok\":true,\"volumes\":[]}\n") == 0);
		close(c1);
	}
	ntfsmac_ctl_stop(v.ctl);
}

static void test_framing(void)
{
	srv v;
	char buf[2048];
	bool eof;
	int fd;

	srv_start(&v);

	/* one request, one reply */
	fd = dial();
	send_str(fd, "{\"cmd\":\"list\"}\n", 15);
	slurp(fd, buf, sizeof buf, 1, 2000, &eof);
	CHECK(strcmp(buf, "{\"ok\":true,\"volumes\":[]}\n") == 0);
	/* the connection stays open for the next request */
	send_str(fd, "{\"cmd\":\"status\",\"bsd\":\"disk9s9\"}\n", 33);
	slurp(fd, buf, sizeof buf, 1, 2000, &eof);
	CHECK(strncmp(buf, "{\"ok\":true", 10) == 0);
	CHECK(strcmp(v.o.bsd, "disk9s9") == 0);
	close(fd);

	/* two requests in one write, a bad one in the middle: three replies in order */
	fd = dial();
	{
		const char two[] = "{\"cmd\":\"list\"}\nnot json\n{\"cmd\":\"doctor\"}\n";

		send_str(fd, two, sizeof two - 1);
	}
	slurp(fd, buf, sizeof buf, 3, 2000, &eof);
	CHECK(strstr(buf, "{\"ok\":true,\"volumes\":[]}\n{\"ok\":false,\"error\":\"bad_json\"}\n"
			  "{\"ok\":true,\"volumes\":[]}\n") == buf);
	CHECK_EQ_INT(v.o.calls[NTFSMAC_CTL_CMD_DOCTOR], 1);
	close(fd);

	/* a request cut in two writes */
	fd = dial();
	send_str(fd, "{\"cmd\":", 7);
	usleep(50000);
	send_str(fd, "\"list\"}\n", 8);
	slurp(fd, buf, sizeof buf, 1, 2000, &eof);
	CHECK(strncmp(buf, "{\"ok\":true", 10) == 0);
	close(fd);

	/* last request without a newline, then half-close: still answered */
	fd = dial();
	send_str(fd, "{\"cmd\":\"list\"}", 14);
	shutdown(fd, SHUT_WR);
	slurp(fd, buf, sizeof buf, 1, 2000, &eof);
	CHECK(strncmp(buf, "{\"ok\":true", 10) == 0);
	close(fd);

	/* an empty line is a bad request, not a crash */
	fd = dial();
	send_str(fd, "\n", 1);
	slurp(fd, buf, sizeof buf, 1, 2000, &eof);
	CHECK(strcmp(buf, "{\"ok\":false,\"error\":\"bad_json\"}\n") == 0);
	close(fd);

	/* a client that connects and closes without a word */
	fd = dial();
	close(fd);

	ntfsmac_ctl_stop(v.ctl);
}

static void test_limits(void)
{
	srv v;
	char buf[4096];
	char *big;
	bool eof;
	int fd, i;
	time_t t0;

	srv_start(&v);

	/* a line of 4097 bytes without a newline: line_too_long, then the connection is closed */
	big = malloc(NTFSMAC_CTL_LINE_MAX + 100);
	memset(big, 'a', NTFSMAC_CTL_LINE_MAX + 100);
	fd = dial();
	send_str(fd, big, NTFSMAC_CTL_LINE_MAX + 100);
	slurp(fd, buf, sizeof buf, 2, 2000, &eof);
	CHECK(strcmp(buf, "{\"ok\":false,\"error\":\"line_too_long\"}\n") == 0);
	close(fd);

	/* exactly 4096 bytes and a newline is a line (a bad one) */
	fd = dial();
	big[NTFSMAC_CTL_LINE_MAX] = '\n';
	send_str(fd, big, NTFSMAC_CTL_LINE_MAX + 1);
	slurp(fd, buf, sizeof buf, 1, 2000, &eof);
	CHECK(strcmp(buf, "{\"ok\":false,\"error\":\"bad_json\"}\n") == 0);
	close(fd);
	free(big);
	CHECK_EQ_INT(fake_total_calls(&v.o), 0);

	/* at most 8 requests per connection: the 9th is not answered */
	fd = dial();
	for (i = 0; i < 9; i++)
		send_str(fd, "{\"cmd\":\"list\"}\n", 15);
	{
		char *many = malloc(8192);
		int lines = 0;
		size_t n, k;

		n = slurp(fd, many, 8192, 9, 2000, &eof);
		for (k = 0; k < n; k++)
			if (many[k] == '\n')
				lines++;
		CHECK_EQ_INT(lines, 8);
		free(many);
	}
	close(fd);

	/* an idle client does not hold the server for longer than the deadline */
	fd = dial();
	t0 = time(NULL);
	{
		int fd2 = dial();

		send_str(fd2, "{\"cmd\":\"list\"}\n", 15);
		slurp(fd2, buf, sizeof buf, 1, 9000, &eof);
		CHECK(strncmp(buf, "{\"ok\":true", 10) == 0);
		close(fd2);
	}
	CHECK(time(NULL) - t0 <= 7);
	close(fd);

	ntfsmac_ctl_stop(v.ctl);
}

static void test_peer(void)
{
	srv v;
	char buf[512];
	bool eof;
	int fd;

	srv_start(&v);

	/* the peer cannot be identified: closed without a reply, nothing called */
	v.s.peer_rc = -1;
	fd = dial();
	send_str(fd, "{\"cmd\":\"list\"}\n", 15);
	slurp(fd, buf, sizeof buf, 1, 2000, &eof);
	CHECK_EQ_INT(strlen(buf), 0);
	CHECK(eof);
	close(fd);
	CHECK_EQ_INT(fake_total_calls(&v.o), 0);
	v.s.peer_rc = 0;

	/* neither console user nor admin: closed at once, nothing read, nothing sent */
	v.s.socket_peer.console_user = false;
	v.s.socket_peer.admin = false;
	fd = dial();
	send_str(fd, "{\"cmd\":\"list\"}\n", 15);
	slurp(fd, buf, sizeof buf, 1, 2000, &eof);
	CHECK_EQ_INT(strlen(buf), 0);
	CHECK(eof);
	close(fd);
	CHECK_EQ_INT(fake_total_calls(&v.o), 0);

	/* the peer record from the socket reaches the operation (journal_clear) */
	v.s.socket_peer = good_peer();
	v.s.socket_peer.uid = 777;
	fd = dial();
	{
		const char *req = "{\"cmd\":\"journal_clear\",\"bsd\":\"disk3s2\",\"confirm_label\":\"L\","
				  "\"confirm_serial\":\"00000001\",\"auth\":\"" GOOD_AUTH "\"}\n";

		send_str(fd, req, strlen(req));
	}
	slurp(fd, buf, sizeof buf, 1, 2000, &eof);
	CHECK(strncmp(buf, "{\"ok\":true", 10) == 0);
	CHECK_EQ_INT(v.o.peer.uid, 777);
	close(fd);

	ntfsmac_ctl_stop(v.ctl);
}

static long long mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* U-CTL-SOCK-06: one thread, so a refused or silent peer must cost almost nothing (M3). */
static void test_dos(void)
{
	srv v;
	char buf[512];
	bool eof;
	int silent[6], fd, i;
	long long t0, dt;

	srv_start(&v);

	/* refused peers: each is closed with no read; a line of requests behind them is not delayed */
	v.s.socket_peer.console_user = false;
	v.s.socket_peer.admin = false;
	t0 = mono_ms();
	for (i = 0; i < 6; i++)
		silent[i] = dial(); /* connect and say nothing */
	for (i = 0; i < 6; i++) {
		slurp(silent[i], buf, sizeof buf, 1, 2000, &eof);
		CHECK(eof); /* closed by the server, not by the 5 s deadline */
		CHECK_EQ_INT(strlen(buf), 0);
		close(silent[i]);
	}
	dt = mono_ms() - t0;
	CHECK(dt < 700);
	v.s.socket_peer = good_peer();
	fd = dial();
	send_str(fd, "{\"cmd\":\"list\"}\n", 15);
	slurp(fd, buf, sizeof buf, 1, 2000, &eof);
	CHECK(strncmp(buf, "{\"ok\":true", 10) == 0);
	close(fd);
	CHECK_EQ_INT(fake_total_calls(&v.o), 1);

	/* an allowed peer that sends no byte is dropped after about a second, not five */
	{
		int idle = dial(), fd2;

		t0 = mono_ms();
		fd2 = dial();
		send_str(fd2, "{\"cmd\":\"list\"}\n", 15);
		slurp(fd2, buf, sizeof buf, 1, 4000, &eof);
		dt = mono_ms() - t0;
		CHECK(strncmp(buf, "{\"ok\":true", 10) == 0);
		CHECK(dt >= 800 && dt < 2500);
		slurp(idle, buf, sizeof buf, 1, 500, &eof);
		CHECK(eof); /* the idle one was closed with no reply */
		CHECK_EQ_INT(strlen(buf), 0);
		close(idle);
		close(fd2);
	}

	/* a client that sends one byte in time keeps the longer deadline for the rest of its line */
	fd = dial();
	send_str(fd, "{", 1);
	usleep(1500000);
	send_str(fd, "\"cmd\":\"list\"}\n", 14);
	slurp(fd, buf, sizeof buf, 1, 3000, &eof);
	CHECK(strncmp(buf, "{\"ok\":true", 10) == 0);
	close(fd);

	ntfsmac_ctl_stop(v.ctl);
}

/* U-CTL-SOCK-07: a live daemon is never taken over (L2). */
static void test_live_socket(void)
{
	srv v;
	ntfsmac_ctl *second = (ntfsmac_ctl *)1;
	ntfsmac_dmn_status st;
	char buf[256];
	bool eof;
	int fd;

	srv_start(&v);
	st = ntfsmac_ctl_start_env(path, &v.ops, &v.o, &v.env, &second);
	CHECK_EQ_INT(st, NTFSMAC_DMN_E_BUSY);
	CHECK(second == NULL);

	/* the first daemon still owns the path and still answers */
	fd = dial();
	send_str(fd, "{\"cmd\":\"list\"}\n", 15);
	slurp(fd, buf, sizeof buf, 1, 2000, &eof);
	CHECK(strncmp(buf, "{\"ok\":true", 10) == 0);
	close(fd);
	ntfsmac_ctl_stop(v.ctl);

	/* after a clean stop the path is free again */
	srv_start(&v);
	ntfsmac_ctl_stop(v.ctl);
}

/* U-CTL-SOCK-08: the definition of the right is checked at start and before each request (M2). */
static void test_right(void)
{
	srv v;
	char buf[512];
	bool eof;
	int fd, calls;
	char req[256];

	srv_start(&v);
	CHECK(v.s.right_calls >= 1); /* at start */
	calls = v.s.right_calls;

	snprintf(req, sizeof req, "{\"cmd\":\"journal_clear\",\"bsd\":\"disk3s2\",\"confirm_label\":\"L\","
		 "\"confirm_serial\":\"00000001\",\"auth\":\"%s\"}\n", GOOD_AUTH);

	/* missing or wrong definition: refused, the form is not even verified */
	v.s.right_answer = NTFSMAC_CTL_SYS_NO;
	fd = dial();
	send_str(fd, req, strlen(req));
	slurp(fd, buf, sizeof buf, 1, 2000, &eof);
	CHECK(strstr(buf, "\"reason\":\"authorization_right_invalid\"") != NULL);
	close(fd);
	CHECK(v.s.right_calls > calls);
	CHECK_EQ_INT(v.s.authz_calls, 0);
	CHECK_EQ_INT(v.o.calls[NTFSMAC_CTL_CMD_JOURNAL_CLEAR], 0);

	/* it is read again for each request: fixed, the same request now passes */
	v.s.right_answer = NTFSMAC_CTL_SYS_YES;
	fd = dial();
	send_str(fd, req, strlen(req));
	slurp(fd, buf, sizeof buf, 1, 2000, &eof);
	CHECK(strncmp(buf, "{\"ok\":true", 10) == 0);
	close(fd);
	CHECK_EQ_INT(v.o.calls[NTFSMAC_CTL_CMD_JOURNAL_CLEAR], 1);

	/* plain requests do not depend on the right */
	v.s.right_answer = NTFSMAC_CTL_SYS_NO;
	fd = dial();
	send_str(fd, "{\"cmd\":\"list\"}\n", 15);
	slurp(fd, buf, sizeof buf, 1, 2000, &eof);
	CHECK(strncmp(buf, "{\"ok\":true", 10) == 0);
	close(fd);
	ntfsmac_ctl_stop(v.ctl);
}

/* The real lookups, on facts a unit test can see: its own uid, pid and parent chain. */
static void test_real_sys(void)
{
	const ntfsmac_ctl_sys *sys = ntfsmac_ctl_sys_default();
	int sv[2];
	ntfsmac_ctl_peer p;
	pid_t ppid = -1, pgid = -1;
	ntfsmac_ctl_sys_real r;

	CHECK(sys != NULL && sys->peer_from_fd != NULL && sys->proc_parent != NULL);
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	CHECK_EQ_INT(sys->peer_from_fd(sys->ctx, sv[0], &p), 0);
	CHECK_EQ_INT(p.uid, getuid());
	CHECK_EQ_INT(p.pid, getpid());
#if defined(__APPLE__)
	CHECK(p.has_audit_token);
	CHECK_EQ_INT(p.audit_token[5], (uint32_t)getpid()); /* pid is word 5 of audit_token_t */
	CHECK_EQ_INT(sys->proc_parent(sys->ctx, getpid(), &ppid, &pgid), NTFSMAC_CTL_PROC_OK);
	CHECK_EQ_INT(ppid, getppid());
	CHECK_EQ_INT(pgid, getpgrp());
	CHECK_EQ_INT(sys->proc_parent(sys->ctx, 0x7ffffff0, &ppid, &pgid), NTFSMAC_CTL_PROC_GONE);
#endif
	close(sv[0]);
	close(sv[1]);

	/* the right: a name that is not defined is never YES (NO on macOS, UNAVAILABLE elsewhere) */
	ntfsmac_ctl_sys_real_init(&r, "/unused", "org.ntfs-macos.test-no-such-right");
	CHECK(r.sys.right_definition != NULL);
	CHECK(r.sys.right_definition(r.sys.ctx) != NTFSMAC_CTL_SYS_YES);
#if defined(__APPLE__)
	CHECK_EQ_INT(r.sys.right_definition(r.sys.ctx), NTFSMAC_CTL_SYS_NO);
#else
	CHECK_EQ_INT(r.sys.right_definition(r.sys.ctx), NTFSMAC_CTL_SYS_UNAVAILABLE);
#endif

	/* the signature check cannot say without its requirement file: UNAVAILABLE, never YES */
	ntfsmac_ctl_sys_real_init(&r, "/nonexistent/ntfsmac-requirement", NTFSMAC_CTL_RIGHT_NAME);
	CHECK(r.sys.client_signed(r.sys.ctx, &p) != NTFSMAC_CTL_SYS_YES);
	p.has_audit_token = false;
	CHECK_EQ_INT(r.sys.client_signed(r.sys.ctx, &p), NTFSMAC_CTL_SYS_UNAVAILABLE);
}

int main(void)
{
	make_dir();
	test_start_stop();
	test_framing();
	test_limits();
	test_peer();
	test_dos();
	test_live_socket();
	test_right();
	test_real_sys();
	remove_dir();
	if (g_failures != 0) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	return 0;
}
