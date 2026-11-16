/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Session channel: the pipe and the reading (T-M4-13). Plain POSIX.
 * The parser is in errwatch.c. See docs/ARCHITECTURE.md section 7, "Bounds
 * and descriptor hygiene".
 *
 * The daemon never closes the read end while the worker runs, so the worker
 * never gets SIGPIPE from the daemon. Whatever the worker sends is read into
 * a fixed buffer and fed to the parser, which discards it after a protocol
 * error, so memory stays bounded for any stream.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

#include "internal.h"

#define PUMP_BUF 4096
#define PUMP_READS_MAX 16 /* one call reads at most 64 KiB, then returns to the loop */

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* A descriptor at 3 or above, with FD_CLOEXEC; closes the old one. */
static int fd_high_cloexec(int fd)
{
	int n = fcntl(fd, F_DUPFD_CLOEXEC, 3);

	close(fd);
	return n;
}

ntfsmac_dmn_status ntfsmac_errwatch_pipe_open(int *rd, int *wr)
{
	int p[2], fl;

	if (rd == NULL || wr == NULL)
		return NTFSMAC_DMN_E_INVAL;
	*rd = -1;
	*wr = -1;
	if (pipe(p) != 0)
		return NTFSMAC_DMN_E_IO;
	/*
	 * No pipe2() on macOS. A fork in another thread between pipe() and the
	 * next call could hold a copy until its exec; every spawn uses
	 * POSIX_SPAWN_CLOEXEC_DEFAULT, so no child ever keeps one.
	 */
	if (fcntl(p[0], F_SETFD, FD_CLOEXEC) != 0 || fcntl(p[1], F_SETFD, FD_CLOEXEC) != 0)
		goto fail;
	p[0] = fd_high_cloexec(p[0]);
	if (p[0] < 0) {
		close(p[1]);
		return NTFSMAC_DMN_E_IO;
	}
	p[1] = fd_high_cloexec(p[1]);
	if (p[1] < 0) {
		close(p[0]);
		return NTFSMAC_DMN_E_IO;
	}
	fl = fcntl(p[0], F_GETFL);
	if (fl == -1 || fcntl(p[0], F_SETFL, fl | O_NONBLOCK) == -1)
		goto fail;
	*rd = p[0];
	*wr = p[1];
	return NTFSMAC_DMN_OK;
fail:
	close(p[0]);
	close(p[1]);
	return NTFSMAC_DMN_E_IO;
}

ntfsmac_dmn_status ntfsmac_errwatch_pump(ntfsmac_errwatch *w, int rd)
{
	char buf[PUMP_BUF];
	int i;

	if (w == NULL || rd < 0)
		return NTFSMAC_DMN_E_INVAL;
	for (i = 0; i < PUMP_READS_MAX; i++) {
		ssize_t n = read(rd, buf, sizeof(buf));

		if (n > 0) {
			(void)ntfsmac_errwatch_feed(w, buf, (size_t)n);
			continue;
		}
		if (n == 0) {
			ntfsmac_errwatch_eof(w);
			return NTFSMAC_DMN_OK;
		}
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return NTFSMAC_DMN_OK;
		return NTFSMAC_DMN_E_IO;
	}
	return NTFSMAC_DMN_OK;
}

ntfsmac_dmn_status ntfsmac_errwatch_drain(ntfsmac_errwatch *w, int rd, unsigned timeout_ms)
{
	uint64_t deadline = now_ms() + timeout_ms;

	if (w == NULL || rd < 0)
		return NTFSMAC_DMN_E_INVAL;
	for (;;) {
		struct pollfd p;
		uint64_t t;
		ntfsmac_dmn_status st = ntfsmac_errwatch_pump(w, rd);

		if (st != NTFSMAC_DMN_OK)
			return st;
		if ((w->flags & NTFSMAC_EW_EOF) != 0u)
			return NTFSMAC_DMN_OK;
		t = now_ms();
		if (t >= deadline)
			return NTFSMAC_DMN_E_TIMEOUT; /* some process still holds the write end */
		p.fd = rd;
		p.events = POLLIN;
		p.revents = 0;
		(void)poll(&p, 1, (int)(deadline - t > 1000u ? 1000u : deadline - t));
	}
}
