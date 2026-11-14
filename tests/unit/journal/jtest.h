/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_JTEST_H
#define NTFSMAC_JTEST_H

/* Shared helpers of the journal tests. Test code only: paths are built with
 * snprintf here, the library under test never does that. */

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include "ntfsmac/journal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define CHECK(cond)                                                          \
	do {                                                                 \
		if (!(cond)) {                                               \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
				#cond);                                      \
			exit(1);                                             \
		}                                                            \
	} while (0)

#define CHECK_ST(call, want)                                                 \
	do {                                                                 \
		ntfsmac_journal_status st_ = (call);                         \
		if (st_ != (want)) {                                         \
			fprintf(stderr, "FAIL %s:%d: %s returned %d, want %s\n", \
				__FILE__, __LINE__, #call, (int)st_, #want); \
			exit(1);                                             \
		}                                                            \
	} while (0)

#define PATH_MAX_T 1024

static inline void jt_tmpdir(char *out, size_t size)
{
	const char *base = getenv("TMPDIR");
	int n;

	if (base == NULL || base[0] == '\0')
		base = "/tmp";
	n = snprintf(out, size, "%s/ntfsmac-jrn-XXXXXX", base);
	CHECK(n > 0 && (size_t)n < size);
	CHECK(mkdtemp(out) != NULL);
}

static inline void jt_path(char *out, size_t size, const char *dir, const char *a,
			   const char *b)
{
	int n;

	if (b != NULL)
		n = snprintf(out, size, "%s/%s/%s", dir, a, b);
	else
		n = snprintf(out, size, "%s/%s", dir, a);
	CHECK(n > 0 && (size_t)n < size);
}

static inline void jt_rmrf(const char *path)
{
	struct stat st;

	if (lstat(path, &st) != 0)
		return;
	if (S_ISDIR(st.st_mode)) {
		DIR *d = opendir(path);
		struct dirent *de;

		if (d != NULL) {
			while ((de = readdir(d)) != NULL) {
				char sub[PATH_MAX_T];

				if (strcmp(de->d_name, ".") == 0 ||
				    strcmp(de->d_name, "..") == 0)
					continue;
				jt_path(sub, sizeof(sub), path, de->d_name, NULL);
				jt_rmrf(sub);
			}
			closedir(d);
		}
		(void)chmod(path, 0700);
		(void)rmdir(path);
	} else {
		(void)unlink(path);
	}
}

/* Journal directory under a fresh temp base: <base>/j (created by open). */
static inline void jt_jdir(char *base, size_t bsize, char *jdir, size_t jsize)
{
	jt_tmpdir(base, bsize);
	jt_path(jdir, jsize, base, "j", NULL);
}

static inline void jt_write_file(const char *path, const void *data, size_t len,
				 mode_t mode)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);

	CHECK(fd >= 0);
	CHECK(write(fd, data, len) == (ssize_t)len);
	CHECK(fchmod(fd, mode) == 0);
	CHECK(close(fd) == 0);
}

/* Reads up to size bytes; returns the length. */
static inline size_t jt_read_file(const char *path, void *buf, size_t size)
{
	int fd = open(path, O_RDONLY);
	ssize_t n;

	CHECK(fd >= 0);
	n = read(fd, buf, size);
	CHECK(n >= 0);
	close(fd);
	return (size_t)n;
}

static inline ntfsmac_journal_entry jt_entry(uint64_t serial)
{
	ntfsmac_journal_entry e;

	memset(&e, 0, sizeof(e));
	e.serial = serial;
	snprintf(e.label, sizeof(e.label), "TESTVOL");
	e.total_sectors = 976773167u;
	e.session_id = 0x1122334455667788ull;
	e.created = 1700000000;
	e.updated = 1700000000;
	e.logfile_lsn_at_start = 0x00000001ABCDEF01ull;
	e.marker = NTFSMAC_JOURNAL_MARKER_PENDING;
	e.state = NTFSMAC_JOURNAL_STATE_OPEN;
	return e;
}

static inline int jt_entry_eq(const ntfsmac_journal_entry *a,
			      const ntfsmac_journal_entry *b)
{
	return a->serial == b->serial && strcmp(a->label, b->label) == 0 &&
	       a->total_sectors == b->total_sectors &&
	       a->session_id == b->session_id && a->created == b->created &&
	       a->updated == b->updated &&
	       a->logfile_lsn_at_start == b->logfile_lsn_at_start &&
	       a->marker == b->marker && a->state == b->state;
}

/* Number of names in `dir` for which pred(name) is true. */
static inline int jt_count_names(const char *dir, int (*pred)(const char *))
{
	DIR *d = opendir(dir);
	struct dirent *de;
	int n = 0;

	CHECK(d != NULL);
	while ((de = readdir(d)) != NULL)
		if (pred(de->d_name))
			n++;
	closedir(d);
	return n;
}

static inline int jt_is_tmp(const char *name)
{
	return name[0] == '.' && strlen(name) > 4 &&
	       strcmp(name + strlen(name) - 4, ".tmp") == 0;
}

static inline int jt_is_any(const char *name)
{
	return strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

#endif /* NTFSMAC_JTEST_H */
