// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * sync_probe - T-M2-06 (U4): which calls an application makes, and exactly when, so that the
 * log can be lined up with the FUSE debug log of the worker (tests/integration/smoke/sync_trace.sh).
 *
 * Runs in the macOS guest (Environment B), on a directory of a mounted NTFS image file that the
 * caller made. Every call is bracketed by marker lines on stdout:
 *
 *   P <CLOCK_MONOTONIC s> <CLOCK_REALTIME s> <event> [key=value ...]
 *
 * Modes:
 *   steps  DIR [GAP_MS]          for 4 KiB, 1 MiB and 64 MiB: create+write+close, reopen+write+fsync,
 *                                reopen+write+fcntl(F_FULLFSYNC), reopen+write+sync(), sync() with a clean fd
 *                                open and with nothing open, fsync on a clean fd,
 *                                fsync on the directory, rename, unlink. GAP_MS (default 1500) of sleep
 *                                between steps so that the worker's requests can be told apart.
 *   delay  DIR BYTES HOLD_MS     create, write BYTES in one write call, keep the descriptor open for HOLD_MS,
 *                                then close. The time between "write_end" and the first FUSE_WRITE in the log
 *                                is the delay.
 *   enospc DIR [CHUNK_BYTES]     write one file in chunks until the volume is full (no fsync, no close), then
 *                                fsync, close, and reopen to compare the size with what write() accepted; then
 *                                try a second small file (create, write, fsync, close). Records every errno.
 *
 * Exit 0 if the mode ran to the end (a failing call under test is a result, not an error), 2 on usage
 * or setup errors.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void mark(const char *ev, const char *fmt, ...)
{
	struct timespec m, r;
	va_list ap;

	clock_gettime(CLOCK_MONOTONIC, &m);
	clock_gettime(CLOCK_REALTIME, &r);
	printf("P %lld.%09ld %lld.%09ld %s", (long long)m.tv_sec, m.tv_nsec, (long long)r.tv_sec, r.tv_nsec, ev);
	if (fmt != NULL && *fmt != '\0') {
		putchar(' ');
		va_start(ap, fmt);
		vprintf(fmt, ap);
		va_end(ap);
	}
	putchar('\n');
	fflush(stdout);
}

static void gap(long ms)
{
	struct timespec ts;

	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (ms % 1000) * 1000000L;
	while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
	}
}

/* Report the result of a call: rc and errno (0 when rc >= 0). */
static void done(const char *ev, long long rc, const char *extra)
{
	int e = rc < 0 ? errno : 0;

	mark(ev, "rc=%lld errno=%d%s%s%s%s", rc, e, e ? " (" : "", e ? strerror(e) : "", e ? ")" : "",
	     extra ? extra : "");
}

static char *pattern(size_t n)
{
	char *b = malloc(n);
	size_t i;

	if (b == NULL) {
		perror("malloc");
		exit(2);
	}
	for (i = 0; i < n; i++)
		b[i] = (char)('A' + (i * 7 + i / 4096) % 26);
	return b;
}

/* write() until all of n is written (short writes are looped); bytes written, or -1 if nothing was. */
static long long write_all(int fd, const char *buf, size_t n)
{
	size_t off = 0;

	while (off < n) {
		ssize_t w = write(fd, buf + off, n - off);

		if (w < 0)
			return off ? (long long)off : -1;
		off += (size_t)w;
	}
	return (long long)off;
}

static int path_in(char *out, size_t sz, const char *dir, const char *name)
{
	return snprintf(out, sz, "%s/%s", dir, name) < (int)sz ? 0 : -1;
}

static int steps_for(const char *dir, size_t n, const char *label, long gap_ms)
{
	char f[1024], g[1024], tag[64];
	char *buf = pattern(n > 4096 ? n : 4096);
	char *small = pattern(4096);
	long long rc;
	int fd;

	if (path_in(f, sizeof f, dir, label) != 0)
		return -1;
	snprintf(g, sizeof g, "%s.renamed", f);
	snprintf(tag, sizeof tag, "size=%zu", n);
	mark("section", "%s file=%s", tag, label);

	/* 1. create, write, close */
	mark("open_create_begin", "%s", tag);
	fd = open(f, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	done("open_create_end", fd, NULL);
	if (fd < 0)
		return -1;
	mark("write_begin", "%s", tag);
	rc = write_all(fd, buf, n);
	done("write_end", rc, NULL);
	mark("close_begin", "%s", tag);
	rc = close(fd);
	done("close_end", rc, NULL);
	gap(gap_ms);

	/* 2. reopen, write, fsync */
	fd = open(f, O_WRONLY | O_APPEND);
	done("reopen_end", fd, " for=fsync");
	if (fd < 0)
		return -1;
	rc = write_all(fd, small, 4096);
	done("write2_end", rc, NULL);
	mark("fsync_begin", "%s", tag);
	rc = fsync(fd);
	done("fsync_end", rc, NULL);
	gap(gap_ms);
	rc = close(fd);
	done("close2_end", rc, NULL);
	gap(gap_ms);

	/* 3. reopen, write, fcntl(F_FULLFSYNC) */
	fd = open(f, O_WRONLY | O_APPEND);
	done("reopen_end", fd, " for=fullfsync");
	if (fd < 0)
		return -1;
	rc = write_all(fd, small, 4096);
	done("write3_end", rc, NULL);
	mark("fullfsync_begin", "%s", tag);
	rc = fcntl(fd, F_FULLFSYNC);
	done("fullfsync_end", rc, NULL);
	gap(gap_ms);
	rc = close(fd);
	done("close3_end", rc, NULL);
	gap(gap_ms);

	/* 4. reopen, write, sync(2) with the descriptor still open, then close */
	fd = open(f, O_WRONLY | O_APPEND);
	done("reopen_end", fd, " for=sync");
	if (fd < 0)
		return -1;
	rc = write_all(fd, small, 4096);
	done("write4_end", rc, NULL);
	mark("sync_begin", "%s", tag);
	sync();
	mark("sync_end", "%s", tag);
	gap(gap_ms);
	rc = close(fd);
	done("close4_end", rc, NULL);
	gap(gap_ms);

	/* 4b. sync(2) while a clean descriptor is open */
	fd = open(f, O_RDONLY);
	done("reopen_end", fd, " for=sync_clean_fd");
	if (fd >= 0) {
		mark("sync_clean_fd_begin", "%s", tag);
		sync();
		mark("sync_clean_fd_end", "%s", tag);
		gap(gap_ms);
		close(fd);
		gap(gap_ms);
	}

	/* 4c. sync(2) after a write and close, with nothing open */
	fd = open(f, O_WRONLY | O_APPEND);
	done("reopen_end", fd, " for=sync_closed");
	if (fd >= 0) {
		rc = write_all(fd, small, 4096);
		done("write5_end", rc, NULL);
		rc = close(fd);
		done("close5_end", rc, NULL);
		gap(300);
		mark("sync_closed_begin", "%s", tag);
		sync();
		mark("sync_closed_end", "%s", tag);
		gap(gap_ms);
	}

	/* 5. fsync and F_FULLFSYNC on a descriptor with nothing dirty */
	fd = open(f, O_RDONLY);
	done("reopen_end", fd, " for=clean_fsync");
	if (fd >= 0) {
		mark("clean_fsync_begin", "%s", tag);
		rc = fsync(fd);
		done("clean_fsync_end", rc, NULL);
		mark("clean_fullfsync_begin", "%s", tag);
		rc = fcntl(fd, F_FULLFSYNC);
		done("clean_fullfsync_end", rc, NULL);
		close(fd);
	}
	gap(gap_ms);

	/* 6. fsync on the directory */
	fd = open(dir, O_RDONLY);
	done("dir_open_end", fd, NULL);
	if (fd >= 0) {
		mark("dir_fsync_begin", "%s", tag);
		rc = fsync(fd);
		done("dir_fsync_end", rc, NULL);
		mark("dir_fullfsync_begin", "%s", tag);
		rc = fcntl(fd, F_FULLFSYNC);
		done("dir_fullfsync_end", rc, NULL);
		close(fd);
	}
	gap(gap_ms);

	/* 7. rename, 8. unlink */
	mark("rename_begin", "%s", tag);
	rc = rename(f, g);
	done("rename_end", rc, NULL);
	gap(gap_ms);
	mark("unlink_begin", "%s", tag);
	rc = unlink(g);
	done("unlink_end", rc, NULL);
	gap(gap_ms);
	free(buf);
	free(small);
	return 0;
}

static int mode_steps(const char *dir, long gap_ms)
{
	static const struct {
		size_t n;
		const char *label;
	} sizes[] = {{4096, "p4k"}, {1048576, "p1m"}, {67108864, "p64m"}};
	size_t i;

	for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
		if (steps_for(dir, sizes[i].n, sizes[i].label, gap_ms) != 0)
			return 2;
	mark("steps_done", "");
	return 0;
}

static int mode_delay(const char *dir, size_t n, long hold_ms)
{
	char f[1024];
	char *buf = pattern(n);
	long long rc;
	int fd;

	if (path_in(f, sizeof f, dir, "delay.dat") != 0)
		return 2;
	mark("section", "delay size=%zu hold_ms=%ld", n, hold_ms);
	fd = open(f, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	done("open_create_end", fd, NULL);
	if (fd < 0)
		return 2;
	mark("write_begin", "size=%zu", n);
	rc = write_all(fd, buf, n);
	done("write_end", rc, NULL);
	gap(hold_ms);
	mark("close_begin", "");
	rc = close(fd);
	done("close_end", rc, NULL);
	gap(1000);
	mark("unlink_begin", "");
	rc = unlink(f);
	done("unlink_end", rc, NULL);
	free(buf);
	return 0;
}

static int mode_enospc(const char *dir, size_t chunk)
{
	char f[1024], f2[1024];
	char *buf = pattern(chunk);
	long long total = 0, rc;
	long nchunks = 0;
	struct stat st;
	int fd, first_err = 0;

	if (path_in(f, sizeof f, dir, "fill.dat") != 0 || path_in(f2, sizeof f2, dir, "after.dat") != 0)
		return 2;
	mark("section", "enospc chunk=%zu", chunk);
	fd = open(f, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	done("open_create_end", fd, NULL);
	if (fd < 0)
		return 2;
	for (;;) {
		ssize_t w = write(fd, buf, chunk);

		if (w < 0) {
			first_err = errno;
			done("fill_write_failed", -1, NULL);
			mark("fill_summary", "accepted_bytes=%lld chunks_ok=%ld first_errno=%d", total, nchunks,
			     first_err);
			break;
		}
		total += w;
		nchunks++;
		if (w != (ssize_t)chunk) {
			mark("fill_short_write", "wrote=%zd of=%zu", w, chunk);
		}
	}
	/* The write that failed: does a retry fail the same way, and is a 1-byte write accepted? */
	rc = write(fd, buf, 1);
	done("retry_write1_end", rc, NULL);
	mark("fsync_begin", "after_full");
	rc = fsync(fd);
	done("fsync_end", rc, NULL);
	mark("fullfsync_begin", "after_full");
	rc = fcntl(fd, F_FULLFSYNC);
	done("fullfsync_end", rc, NULL);
	mark("close_begin", "after_full");
	rc = close(fd);
	done("close_end", rc, NULL);
	gap(2000);
	if (stat(f, &st) == 0)
		mark("stat_after_close", "st_size=%lld accepted_bytes=%lld lost=%lld", (long long)st.st_size, total,
		     total - (long long)st.st_size);
	else
		done("stat_after_close", -1, NULL);
	mark("sync_begin", "after_full");
	sync();
	mark("sync_end", "after_full");
	gap(1000);

	/* A second, small file on the full volume: where does the error show up? */
	mark("second_file", "begin");
	fd = open(f2, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	done("open_create2_end", fd, NULL);
	if (fd >= 0) {
		rc = write_all(fd, buf, chunk < 65536 ? chunk : 65536);
		done("write2_end", rc, NULL);
		rc = fsync(fd);
		done("fsync2_end", rc, NULL);
		rc = close(fd);
		done("close2_end", rc, NULL);
		gap(1500);
		if (stat(f2, &st) == 0)
			mark("stat2_after_close", "st_size=%lld", (long long)st.st_size);
		else
			done("stat2_after_close", -1, NULL);
	}
	/* Free the space again, then write the same small file: proves the volume works after the error. */
	mark("unlink_begin", "fill");
	rc = unlink(f);
	done("unlink_end", rc, NULL);
	gap(1500);
	fd = open(f2, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	done("open_create3_end", fd, NULL);
	if (fd >= 0) {
		rc = write_all(fd, buf, chunk < 65536 ? chunk : 65536);
		done("write3_end", rc, NULL);
		rc = fsync(fd);
		done("fsync3_end", rc, NULL);
		rc = close(fd);
		done("close3_end", rc, NULL);
		unlink(f2);
	}
	free(buf);
	mark("enospc_done", "");
	return 0;
}

int main(int argc, char **argv)
{
	long gap_ms = 1500;

	if (argc < 3) {
		fprintf(stderr, "usage: sync_probe steps DIR [GAP_MS] | delay DIR BYTES HOLD_MS | enospc DIR [CHUNK]\n");
		return 2;
	}
	if (strcmp(argv[1], "steps") == 0) {
		if (argc > 3)
			gap_ms = atol(argv[3]);
		return mode_steps(argv[2], gap_ms);
	}
	if (strcmp(argv[1], "delay") == 0 && argc == 5)
		return mode_delay(argv[2], (size_t)atoll(argv[3]), atol(argv[4]));
	if (strcmp(argv[1], "enospc") == 0)
		return mode_enospc(argv[2], argc > 3 ? (size_t)atoll(argv[3]) : 1048576);
	fprintf(stderr, "sync_probe: bad mode or arguments\n");
	return 2;
}
