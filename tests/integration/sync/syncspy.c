// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * syncspy - T-M3-10: test-only DYLD interpose library that records the device-level sync calls of
 * the NTFS-3G worker (libntfs-3g/unix_io.c ntfs_fsync(): fcntl(F_FULLFSYNC), then fsync() as the
 * fallback). It is loaded with DYLD_INSERT_LIBRARIES into a worker that the case built itself;
 * nothing of it is part of the shipped build and no patch is involved.
 *
 * It interposes fcntl, fsync, open, pread, pwrite and write. Reads and successful writes are not
 * logged; successful writes are counted between sync calls.
 *
 * Environment:
 *   NTFSMAC_SYNCSPY_LOG     file that receives the records (appended, one write(2) per line); unset: no output
 *   NTFSMAC_SYNCSPY_FAIL    "fullfsync": F_FULLFSYNC is not issued and fails with ENOTTY, to show the
 *                           fallback path (upstream then calls fsync()); anything else: no injection
 *   NTFSMAC_SYNCSPY_WRITES  set: also one record per successful device write (debugging)
 *
 * Record (CLOCK_REALTIME, the clock the case driver uses too):
 *   <sec.usec> <event> pid=<n> fd=<n> path=<path of fd> [rc=<n> errno=<n>] [us=<n>] [writes=<n> bytes=<n>]
 * Events:
 *   open                  open() of a /dev node or of an .img file
 *   fullfsync, fsync      the sync call; us is its duration; writes and bytes count the write calls on any
 *                         device or image descriptor since the last sync record
 *   pread-fail            a failed pread; the path field holds off and len
 *   pwrite-fail           a failed pwrite; same
 *   write-fail            a failed write; same (off is the current file offset)
 *   pwrite, write         only with NTFSMAC_SYNCSPY_WRITES
 *
 * The library is built for arm64 and arm64e: the macFUSE mount helper that libfuse starts inherits
 * DYLD_INSERT_LIBRARIES and is arm64e. Records carry the pid, so the helper's are told apart.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define INTERPOSE(repl, orig) \
	__attribute__((used)) static struct { const void *r; const void *o; } \
	interpose_##orig __attribute__((section("__DATA,__interpose"))) = { (const void *)(repl), (const void *)(orig) }

static int logfd = -2;
static unsigned long long nwrites, nbytes;

static long long now_us(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (long long)tv.tv_sec * 1000000LL + tv.tv_usec;
}

/* rc and errno are printed when with_rc; us when us >= 0; the write counters when with_cnt. */
static void rec(const char *ev, int fd, const char *path, int rc, int err, int with_rc, long long us, int with_cnt)
{
	char line[1200], p[1024];
	long long t = now_us();
	int n, saved = errno;

	if (logfd == -2) {
		const char *f = getenv("NTFSMAC_SYNCSPY_LOG");

		logfd = f != NULL ? open(f, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644) : -1;
	}
	if (logfd < 0) {
		errno = saved;
		return;
	}
	if (path == NULL) {
		p[0] = '\0';
		if (fcntl(fd, F_GETPATH, p) != 0)
			strcpy(p, "?");
		path = p;
	}
	n = snprintf(line, sizeof line, "%lld.%06d %s pid=%d fd=%d path=%s", t / 1000000, (int)(t % 1000000), ev,
		     (int)getpid(), fd, path);
	if (with_rc)
		n += snprintf(line + n, sizeof line - (size_t)n, " rc=%d errno=%d", rc, err);
	if (us >= 0)
		n += snprintf(line + n, sizeof line - (size_t)n, " us=%lld", us);
	if (with_cnt)
		n += snprintf(line + n, sizeof line - (size_t)n, " writes=%llu bytes=%llu", nwrites, nbytes);
	line[n++] = '\n';
	if (write(logfd, line, (size_t)n) < 0) {
	}
	errno = saved;
}

static int is_dev(int fd)
{
	char p[1024];

	return fcntl(fd, F_GETPATH, p) == 0 && (strncmp(p, "/dev/", 5) == 0 || strstr(p, ".img") != NULL);
}

static int my_fcntl(int fd, int cmd, ...)
{
	va_list ap;
	long arg;
	int rc, err;
	long long t0, us;

	va_start(ap, cmd);
	arg = va_arg(ap, long);
	va_end(ap);
	if (cmd == F_FULLFSYNC) {
		const char *f = getenv("NTFSMAC_SYNCSPY_FAIL");

		t0 = now_us();
		if (f != NULL && strcmp(f, "fullfsync") == 0) {
			rc = -1;
			err = ENOTTY;
		} else {
			rc = fcntl(fd, cmd, arg);
			err = rc < 0 ? errno : 0;
		}
		us = now_us() - t0;
		rec("fullfsync", fd, NULL, rc, err, 1, us, 1);
		nwrites = nbytes = 0;
		errno = err;
		return rc;
	}
	return fcntl(fd, cmd, arg);
}

static int my_fsync(int fd)
{
	long long t0 = now_us();
	int rc = fsync(fd), err = rc < 0 ? errno : 0;

	rec("fsync", fd, NULL, rc, err, 1, now_us() - t0, 1);
	nwrites = nbytes = 0;
	errno = err;
	return rc;
}

static int my_open(const char *path, int flags, ...)
{
	va_list ap;
	int mode, fd;

	va_start(ap, flags);
	mode = va_arg(ap, int);
	va_end(ap);
	fd = open(path, flags, mode);
	if (fd >= 0 && (strncmp(path, "/dev/", 5) == 0 || strstr(path, ".img") != NULL))
		rec("open", fd, path, 0, 0, 0, -1, 0);
	return fd;
}

static void fail_rec(const char *ev, int fd, off_t off, size_t n, int e)
{
	char t[96];

	snprintf(t, sizeof t, "off=%lld len=%zu", (long long)off, n);
	rec(ev, fd, t, -1, e, 1, -1, 0);
}

static void count_write(const char *ev, int fd, off_t off, ssize_t r)
{
	nwrites++;
	nbytes += (unsigned long long)r;
	if (getenv("NTFSMAC_SYNCSPY_WRITES") != NULL) {
		char t[96];

		snprintf(t, sizeof t, "off=%lld len=%zd", (long long)off, r);
		rec(ev, fd, t, 0, 0, 0, -1, 0);
	}
}

static ssize_t my_pwrite(int fd, const void *buf, size_t n, off_t off)
{
	ssize_t r = pwrite(fd, buf, n, off);
	int e = errno;

	if (r < 0)
		fail_rec("pwrite-fail", fd, off, n, e);
	else if (r > 0)
		count_write("pwrite", fd, off, r);
	errno = e;
	return r;
}

static ssize_t my_write(int fd, const void *buf, size_t n)
{
	ssize_t r = write(fd, buf, n);
	int e = errno;

	if (fd != logfd && (r != 0) && is_dev(fd)) {
		off_t cur = lseek(fd, 0, SEEK_CUR);

		if (r < 0)
			fail_rec("write-fail", fd, cur, n, e);
		else
			count_write("write", fd, cur - r, r);
	}
	errno = e;
	return r;
}

static ssize_t my_pread(int fd, void *buf, size_t n, off_t off)
{
	ssize_t r = pread(fd, buf, n, off);
	int e = errno;

	if (r < 0)
		fail_rec("pread-fail", fd, off, n, e);
	errno = e;
	return r;
}

INTERPOSE(my_fcntl, fcntl);
INTERPOSE(my_fsync, fsync);
INTERPOSE(my_open, open);
INTERPOSE(my_pwrite, pwrite);
INTERPOSE(my_pread, pread);
INTERPOSE(my_write, write);
