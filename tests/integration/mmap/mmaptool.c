// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * mmaptool - T-M3-11: test-only helper that holds one file descriptor and one MAP_SHARED mapping
 * and runs commands read from stdin, one per line, so a case can keep a mapping live across
 * mounts, unmounts and a worker kill. Built in the guest by the case (cc -Wall -Wextra -Werror).
 *
 * Every mapped access runs under a SIGBUS/SIGSEGV guard, so a fault is reported, not fatal.
 * Data moves through files: the case writes a file and the tool copies it into the mapping
 * (mput) or copies the mapping into a file (mget); the case hashes the files.
 *
 * Commands (reply is one line: "ok [info]", "sigbus", "sigsegv" or "err <errno> <name>"):
 *   open PATH rw|ro|create    open (create: O_CREAT|O_RDWR|O_TRUNC, mode 0644)
 *   close                     close(fd); reports errno of close
 *   map LEN rw|ro             mmap(NULL, LEN, PROT, MAP_SHARED, fd, 0)
 *   mput OFF FILE             copy FILE into the mapping at OFF (touches the pages)
 *   mget OFF N FILE           copy N bytes of the mapping at OFF into FILE
 *   mtouch OFF                read one byte of the mapping at OFF
 *   msync sync|async|inval    msync(map, len, MS_SYNC|MS_ASYNC|MS_INVALIDATE); reports errno
 *   munmap                    munmap
 *   pwrite OFF FILE           pwrite(fd, FILE content, OFF)
 *   pread OFF N FILE          pread N bytes into FILE
 *   fsync | fullfsync         fsync(fd) / fcntl(F_FULLFSYNC)
 *   ftruncate N               ftruncate(fd, N)
 *   fsize                     fstat size
 *   madvise dontneed|free     madvise on the mapping
 *   quit
 */
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int fd = -1;
static unsigned char *map;
static size_t maplen;
static sigjmp_buf jb;
static volatile sig_atomic_t guarded;
static volatile sig_atomic_t last_sig;

static void on_fault(int sig)
{
	last_sig = sig;
	if (guarded)
		siglongjmp(jb, 1);
	_exit(100 + sig);
}

static void reply_err(void)
{
	printf("err %d %s\n", errno, strerror(errno));
}

static void reply_fault(void)
{
	printf("%s\n", last_sig == SIGBUS ? "sigbus" : "sigsegv");
}

static int copy_in(const char *path, unsigned char **buf, size_t *n)
{
	struct stat st;
	int f = open(path, O_RDONLY);
	size_t got = 0;

	if (f < 0)
		return -1;
	if (fstat(f, &st) < 0) {
		close(f);
		return -1;
	}
	*n = (size_t)st.st_size;
	*buf = malloc(*n ? *n : 1);
	while (got < *n) {
		ssize_t r = read(f, *buf + got, *n - got);

		if (r <= 0)
			break;
		got += (size_t)r;
	}
	close(f);
	return got == *n ? 0 : -1;
}

static int copy_out(const char *path, const unsigned char *buf, size_t n)
{
	int f = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	size_t put = 0;

	if (f < 0)
		return -1;
	while (put < n) {
		ssize_t r = write(f, buf + put, n - put);

		if (r <= 0)
			break;
		put += (size_t)r;
	}
	close(f);
	return put == n ? 0 : -1;
}

int main(void)
{
	char line[4096];
	struct sigaction sa;

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_fault;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGSEGV, &sa, NULL);
	setvbuf(stdout, NULL, _IOLBF, 0);
	printf("ready pid=%d\n", (int)getpid());

	while (fgets(line, sizeof line, stdin)) {
		char *argv[6];
		int argc = 0;
		char *p = strtok(line, " \t\r\n");

		while (p && argc < 6) {
			argv[argc++] = p;
			p = strtok(NULL, " \t\r\n");
		}
		if (argc == 0)
			continue;
		if (!strcmp(argv[0], "quit"))
			break;
		if (!strcmp(argv[0], "open") && argc == 3) {
			int fl = !strcmp(argv[2], "ro") ? O_RDONLY : !strcmp(argv[2], "create") ? (O_CREAT | O_RDWR | O_TRUNC) : O_RDWR;

			fd = open(argv[1], fl, 0644);
			if (fd < 0)
				reply_err();
			else
				printf("ok fd=%d\n", fd);
		} else if (!strcmp(argv[0], "close")) {
			int r = close(fd);

			fd = -1;
			if (r < 0)
				reply_err();
			else
				printf("ok\n");
		} else if (!strcmp(argv[0], "map") && argc == 3) {
			int prot = !strcmp(argv[2], "ro") ? PROT_READ : (PROT_READ | PROT_WRITE);
			void *m;

			maplen = (size_t)strtoull(argv[1], NULL, 10);
			m = mmap(NULL, maplen, prot, MAP_SHARED, fd, 0);
			if (m == MAP_FAILED) {
				map = NULL;
				reply_err();
			} else {
				map = m;
				printf("ok\n");
			}
		} else if (!strcmp(argv[0], "mput") && argc == 3) {
			unsigned char *buf;
			size_t n, off = (size_t)strtoull(argv[1], NULL, 10);

			if (copy_in(argv[2], &buf, &n) < 0) {
				reply_err();
				continue;
			}
			guarded = 1;
			if (sigsetjmp(jb, 1) == 0) {
				memcpy(map + off, buf, n);
				guarded = 0;
				printf("ok %zu\n", n);
			} else {
				guarded = 0;
				reply_fault();
			}
			free(buf);
		} else if (!strcmp(argv[0], "mget") && argc == 4) {
			size_t off = (size_t)strtoull(argv[1], NULL, 10), n = (size_t)strtoull(argv[2], NULL, 10);
			unsigned char *buf = malloc(n ? n : 1);

			guarded = 1;
			if (sigsetjmp(jb, 1) == 0) {
				memcpy(buf, map + off, n);
				guarded = 0;
				if (copy_out(argv[3], buf, n) < 0)
					reply_err();
				else
					printf("ok %zu\n", n);
			} else {
				guarded = 0;
				reply_fault();
			}
			free(buf);
		} else if (!strcmp(argv[0], "mtouch") && argc == 2) {
			size_t off = (size_t)strtoull(argv[1], NULL, 10);
			volatile unsigned char c = 0;

			guarded = 1;
			if (sigsetjmp(jb, 1) == 0) {
				c = map[off];
				guarded = 0;
				printf("ok %u\n", (unsigned)c);
			} else {
				guarded = 0;
				reply_fault();
			}
		} else if (!strcmp(argv[0], "msync") && argc == 2) {
			int fl = !strcmp(argv[1], "sync") ? MS_SYNC : !strcmp(argv[1], "async") ? MS_ASYNC : MS_INVALIDATE;

			if (msync(map, maplen, fl) < 0)
				reply_err();
			else
				printf("ok\n");
		} else if (!strcmp(argv[0], "munmap")) {
			if (munmap(map, maplen) < 0)
				reply_err();
			else
				printf("ok\n");
			map = NULL;
		} else if (!strcmp(argv[0], "pwrite") && argc == 3) {
			unsigned char *buf;
			size_t n;
			off_t off = (off_t)strtoll(argv[1], NULL, 10);

			if (copy_in(argv[2], &buf, &n) < 0) {
				reply_err();
				continue;
			}
			ssize_t r = pwrite(fd, buf, n, off);

			if (r < 0)
				reply_err();
			else
				printf("ok %zd\n", r);
			free(buf);
		} else if (!strcmp(argv[0], "pread") && argc == 4) {
			off_t off = (off_t)strtoll(argv[1], NULL, 10);
			size_t n = (size_t)strtoull(argv[2], NULL, 10);
			unsigned char *buf = malloc(n ? n : 1);
			ssize_t r = pread(fd, buf, n, off);

			if (r < 0)
				reply_err();
			else if (copy_out(argv[3], buf, (size_t)r) < 0)
				reply_err();
			else
				printf("ok %zd\n", r);
			free(buf);
		} else if (!strcmp(argv[0], "fsync")) {
			if (fsync(fd) < 0)
				reply_err();
			else
				printf("ok\n");
		} else if (!strcmp(argv[0], "fullfsync")) {
			if (fcntl(fd, F_FULLFSYNC) < 0)
				reply_err();
			else
				printf("ok\n");
		} else if (!strcmp(argv[0], "ftruncate") && argc == 2) {
			if (ftruncate(fd, (off_t)strtoll(argv[1], NULL, 10)) < 0)
				reply_err();
			else
				printf("ok\n");
		} else if (!strcmp(argv[0], "fsize")) {
			struct stat st;

			if (fstat(fd, &st) < 0)
				reply_err();
			else
				printf("ok %lld\n", (long long)st.st_size);
		} else if (!strcmp(argv[0], "madvise") && argc == 2) {
			if (madvise(map, maplen, !strcmp(argv[1], "free") ? MADV_FREE : MADV_DONTNEED) < 0)
				reply_err();
			else
				printf("ok\n");
		} else {
			printf("err 22 bad command\n");
		}
	}
	return 0;
}
