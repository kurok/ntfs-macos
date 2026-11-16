/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Socket client of the CLI (T-M4-04): one request line, one reply line. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "cli.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int fail(char *err_text, size_t err_cap, const char *msg)
{
	snprintf(err_text, err_cap, "%s", msg);
	return -1;
}

static bool wait_fd(int fd, short events, long long deadline)
{
	long long left = deadline - now_ms();
	struct pollfd p;
	int r;

	if (left <= 0)
		return false;
	p.fd = fd;
	p.events = events;
	p.revents = 0;
	do
		r = poll(&p, 1, (int)left);
	while (r < 0 && errno == EINTR);
	return r > 0;
}

int ntfsmac_cli_exchange(const char *socket_path, const char *line, int timeout_ms, char *reply,
			 size_t cap, char *err_text, size_t err_cap)
{
	struct sockaddr_un sa;
	long long deadline = now_ms() + timeout_ms;
	size_t len = strlen(line), sent = 0, have = 0;
	int fd;

	if (cap < 2)
		return fail(err_text, err_cap, "internal error: reply buffer too small");
	if (strlen(socket_path) >= sizeof sa.sun_path)
		return fail(err_text, err_cap, "the socket path is too long");
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return fail(err_text, err_cap, "cannot create a socket");
	(void)fcntl(fd, F_SETFD, FD_CLOEXEC);
#if defined(SO_NOSIGPIPE)
	{
		int one = 1;

		(void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
	}
#endif
	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	memcpy(sa.sun_path, socket_path, strlen(socket_path) + 1);
	if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
		int e = errno;

		close(fd);
		if (e == ENOENT || e == ECONNREFUSED)
			return fail(err_text, err_cap,
				    "the ntfsmac daemon is not running (cannot connect to its socket)");
		if (e == EACCES)
			return fail(err_text, err_cap,
				    "no permission to use the ntfsmac daemon socket");
		return fail(err_text, err_cap, "cannot connect to the ntfsmac daemon");
	}
	(void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

	while (sent < len + 1) {
		const char *src = sent < len ? line + sent : "\n";
		size_t n = sent < len ? len - sent : 1;
		ssize_t w = send(fd, src, n, MSG_NOSIGNAL);

		if (w > 0) {
			sent += (size_t)w;
			continue;
		}
		if (w < 0 && errno == EINTR)
			continue;
		if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
		    wait_fd(fd, POLLOUT, deadline))
			continue;
		close(fd);
		return fail(err_text, err_cap, "cannot send the request to the daemon");
	}

	for (;;) {
		char *nl = memchr(reply, '\n', have);
		ssize_t r;

		if (nl != NULL) {
			*nl = '\0';
			close(fd);
			return 0;
		}
		if (have + 1 >= cap) {
			close(fd);
			return fail(err_text, err_cap, "the daemon's reply is too long");
		}
		if (!wait_fd(fd, POLLIN, deadline)) {
			close(fd);
			return fail(err_text, err_cap, "the daemon did not answer in time");
		}
		r = recv(fd, reply + have, cap - 1 - have, 0);
		if (r < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
			continue;
		if (r <= 0) {
			close(fd);
			return fail(err_text, err_cap, "the daemon closed the connection without a reply "
					    "(only the user at the console or an administrator may use ntfsmac)");
		}
		have += (size_t)r;
	}
}
