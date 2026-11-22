/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * dirtests - directory iteration cases (I-RDIR-*, patch 0014; macOS and Linux).
 *   dirtests unlink DIR            opendir; readdir + unlink each entry; closedir; rmdir.
 *                                  Prints the unlink failures grouped by errno, the readdir error, the
 *                                  names left (a full listing on a new descriptor, and its result) and
 *                                  the rmdir result.
 *   dirtests seekeof DIR K         opendir; telldir after K entries; readdir to the end; seekdir back;
 *                                  readdir to the end again. The two tails must be the same names.
 *   dirtests handoff DIR BUFSIZE   raw reads (getdirentries64 / getdents64) on two descriptors of one
 *                                  directory: A reads one buffer, B reads to the end, A reads to the
 *                                  end; then interleaved, one buffer each in turn. A's and B's sets
 *                                  must each be the full listing, without duplicates.
 *   dirtests grow DIR BUFSIZE N    raw: open, read one buffer, create N files named grow-*, read to
 *                                  the end. Every name present before must come exactly once; no error.
 *   dirtests bulk DIR              macOS: one listing with getattrlistbulk (as Finder lists); prints the
 *                                  number of names, exit 1 on an error. Used as a concurrent lister.
 * Exit 0 when the case holds, 1 when not, 2 on a usage or setup error.
 * Build: cc -O2 -Wall -o dirtests dirtests.c
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/attr.h>
#ifndef SYS_getdirentries64
#define SYS_getdirentries64 344
#endif
static long raw_read(int fd, char *buf, int bs)
{
	int64_t base = 0;
	return syscall(SYS_getdirentries64, fd, buf, bs, &base);
}
#define NAME_OFF 21
#else
static long raw_read(int fd, char *buf, int bs) { return syscall(SYS_getdents64, fd, buf, bs); }
#define NAME_OFF 19
#endif

struct set { char **v; long n, cap; };

static void add(struct set *s, const char *nm)
{
	if (s->n == s->cap) {
		s->cap = s->cap ? 2 * s->cap : 256;
		s->v = realloc(s->v, s->cap * sizeof(char *));
	}
	s->v[s->n++] = strdup(nm);
}

static int dot(const char *n) { return !strcmp(n, ".") || !strcmp(n, ".."); }
static int cmp(const void *a, const void *b) { return strcmp(*(char **)a, *(char **)b); }

/* one raw buffer into s; returns bytes, 0 at the end, -errno */
static long raw_one(int fd, char *buf, int bs, struct set *s)
{
	long n = raw_read(fd, buf, bs);
	if (n < 0)
		return -errno;
	for (long off = 0; off < n;) {
		unsigned short reclen;
		memcpy(&reclen, buf + off + 16, 2);
		if (!reclen)
			break;
		const char *nm = buf + off + NAME_OFF;
		if (!dot(nm))
			add(s, nm);
		off += reclen;
	}
	return n;
}

static long raw_rest(int fd, char *buf, int bs, struct set *s)
{
	long n;
	while ((n = raw_one(fd, buf, bs, s)) > 0)
		;
	return n;
}

/* compare got with want: missing and duplicate counts */
static void compare(struct set *got, struct set *want, long *missing, long *dup)
{
	qsort(got->v, got->n, sizeof(char *), cmp);
	qsort(want->v, want->n, sizeof(char *), cmp);
	*dup = 0;
	for (long i = 1; i < got->n; i++)
		if (!strcmp(got->v[i], got->v[i - 1]))
			(*dup)++;
	*missing = 0;
	for (long i = 0, j = 0; i < want->n; i++) {
		while (j < got->n && strcmp(got->v[j], want->v[i]) < 0)
			j++;
		if (j >= got->n || strcmp(got->v[j], want->v[i]))
			(*missing)++;
	}
}

static int full(const char *dir, struct set *s)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	if (!d)
		return -errno;
	errno = 0;
	while ((e = readdir(d)))
		if (!dot(e->d_name))
			add(s, e->d_name);
	int r = errno ? -errno : 0;
	closedir(d);
	return r;
}

int main(int argc, char **argv)
{
	if (argc < 3)
		return 2;
	const char *cmd = argv[1], *dir = argv[2];

	if (!strcmp(cmd, "unlink")) {
		DIR *d = opendir(dir);
		struct dirent *e;
		long removed = 0, failed = 0, byerr[256] = {0};
		char p[4096];
		if (!d)
			return 2;
		errno = 0;
		while ((e = readdir(d))) {
			if (dot(e->d_name))
				continue;
			snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
			if (unlink(p)) {
				failed++;
				byerr[errno > 0 && errno < 256 ? errno : 255]++;
			} else
				removed++;
			errno = 0;
		}
		int rerr = errno;
		closedir(d);
		struct set left = {0};
		int lerr = full(dir, &left);	/* the names left, read on a new descriptor */
		int rr = rmdir(dir) ? errno : 0;
		/* unlink failures grouped by errno, as "ENOENT:8,EACCES:1" (errno names for the common ones) */
		char fe[512] = "";
		size_t fl = 0;
		for (int i = 1; i < 256; i++) {
			const char *nm = i == ENOENT ? "ENOENT" : i == EACCES ? "EACCES" : i == EPERM ? "EPERM"
				       : i == EBUSY ? "EBUSY" : i == EINVAL ? "EINVAL" : i == EIO ? "EIO" : NULL;
			if (byerr[i] && fl < sizeof(fe) - 32) {
				if (nm)
					fl += (size_t)snprintf(fe + fl, sizeof(fe) - fl, "%s%s:%ld", fl ? "," : "", nm, byerr[i]);
				else
					fl += (size_t)snprintf(fe + fl, sizeof(fe) - fl, "%serrno%d:%ld", fl ? "," : "", i, byerr[i]);
			}
		}
		printf("unlink: removed %ld, unlink failures %ld (%s), readdir error %s, left %ld, final listing %s, rmdir %s\n",
		       removed, failed, fl ? fe : "none", rerr ? strerror(rerr) : "none", left.n,
		       lerr ? strerror(-lerr) : "ok", rr ? strerror(rr) : "ok");
		return (left.n || rr || rerr || failed || lerr) ? 1 : 0;
	}
	if (!strcmp(cmd, "seekeof") && argc == 4) {
		long k = atol(argv[3]), i = 0;
		DIR *d = opendir(dir);
		struct dirent *e;
		struct set t1 = {0}, t2 = {0};
		long cookie = -1;
		if (!d)
			return 2;
		while ((e = readdir(d))) {
			if (i == k)
				cookie = telldir(d);
			if (i >= k && !dot(e->d_name))
				add(&t1, e->d_name);
			i++;
		}
		/* the entry read when i == k was read before telldir: telldir gives the position after it */
		if (cookie < 0) {
			printf("seekeof: fewer than %ld entries\n", k);
			closedir(d);
			return 2;
		}
		seekdir(d, cookie);
		errno = 0;
		while ((e = readdir(d)))
			if (!dot(e->d_name))
				add(&t2, e->d_name);
		int r = errno;
		closedir(d);
		/* t1 holds the entry at k plus the tail; t2 must hold the tail after it */
		long missing, dup, extra = t1.n - 1 - t2.n;
		compare(&t2, &t1, &missing, &dup);
		/* exactly one name of t1 (the entry at k) is absent from t2 */
		printf("seekeof: tail before %ld, after seekdir %ld (expected %ld), missing %ld (expected 1), duplicates %ld, error %s\n",
		       t1.n - 1, t2.n, t1.n - 1, missing, dup, r ? strerror(r) : "none");
		return (extra || dup || r || t2.n == 0 || missing != 1) ? 1 : 0;
	}
	if (!strcmp(cmd, "handoff") && argc == 4) {
		int bs = atoi(argv[3]);
		char *ba = malloc(bs), *bb = malloc(bs);
		struct set want = {0};
		int rc = 0;
		if (full(dir, &want))
			return 2;
		for (int mode = 0; mode < 2; mode++) {
			struct set a = {0}, b = {0};
			int fa = open(dir, O_RDONLY | O_DIRECTORY), fb = open(dir, O_RDONLY | O_DIRECTORY);
			long ra = 0, rb = 0;
			if (fa < 0 || fb < 0)
				return 2;
			if (mode == 0) {
				ra = raw_one(fa, ba, bs, &a);
				if (ra > 0) {
					rb = raw_rest(fb, bb, bs, &b);
					ra = raw_rest(fa, ba, bs, &a);
				}
			} else {
				long xa = 1, xb = 1;
				while (xa > 0 || xb > 0) {
					if (xa > 0)
						xa = raw_one(fa, ba, bs, &a);
					if (xb > 0)
						xb = raw_one(fb, bb, bs, &b);
				}
				ra = xa;
				rb = xb;
			}
			long ma, da, mb, db;
			struct set w2 = {0};
			for (long i = 0; i < want.n; i++)
				add(&w2, want.v[i]);
			compare(&a, &want, &ma, &da);
			compare(&b, &w2, &mb, &db);
			printf("handoff %s: A %ld names (missing %ld, dup %ld, end %s), B %ld names (missing %ld, dup %ld, end %s)\n",
			       mode ? "interleaved" : "A1-Ball-Arest", a.n, ma, da, ra < 0 ? strerror((int)-ra) : "ok", b.n, mb,
			       db, rb < 0 ? strerror((int)-rb) : "ok");
			if (ma || da || mb || db || ra < 0 || rb < 0)
				rc = 1;
			close(fa);
			close(fb);
		}
		return rc;
	}
#if defined(__APPLE__)
	if (!strcmp(cmd, "bulk")) {
		struct attrlist al;
		char *buf = malloc(65536);
		long names = 0;
		int fd = open(dir, O_RDONLY | O_DIRECTORY), n;
		if (fd < 0 || !buf)
			return 2;
		memset(&al, 0, sizeof(al));
		al.bitmapcount = ATTR_BIT_MAP_COUNT;
		al.commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME; /* RETURNED_ATTRS is required */
		while ((n = getattrlistbulk(fd, &al, buf, 65536, 0)) > 0)
			names += n;
		close(fd);
		printf("bulk: %ld names%s%s\n", names, n < 0 ? ", error " : "", n < 0 ? strerror(errno) : "");
		return n < 0 ? 1 : 0;
	}
#endif
	if (!strcmp(cmd, "grow") && argc == 5) {
		int bs = atoi(argv[3]), n = atoi(argv[4]);
		char *buf = malloc(bs), p[4096];
		struct set want = {0}, got = {0};
		if (full(dir, &want))
			return 2;
		int fd = open(dir, O_RDONLY | O_DIRECTORY);
		if (fd < 0)
			return 2;
		long r = raw_one(fd, buf, bs, &got);
		for (int i = 0; i < n; i++) {
			snprintf(p, sizeof(p), "%s/grow-%05d-%s", dir, i, "gggggggggggggggggggggggggggggggggggggggg");
			int f = open(p, O_CREAT | O_WRONLY, 0644);
			if (f >= 0)
				close(f);
		}
		if (r > 0)
			r = raw_rest(fd, buf, bs, &got);
		close(fd);
		struct set orig = {0};
		long newseen = 0;
		for (long i = 0; i < got.n; i++)
			if (strncmp(got.v[i], "grow-", 5))
				add(&orig, got.v[i]);
			else
				newseen++;
		long missing, dup;
		compare(&orig, &want, &missing, &dup);
		printf("grow: before %ld names; listed %ld of them (missing %ld, duplicates %ld), new names seen %ld, end %s\n",
		       want.n, orig.n, missing, dup, newseen, r < 0 ? strerror((int)-r) : "ok");
		return (missing || dup || r < 0) ? 1 : 0;
	}
	return 2;
}
