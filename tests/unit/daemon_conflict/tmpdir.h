/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_TEST_DAEMON_TMPDIR_H
#define NTFSMAC_TEST_DAEMON_TMPDIR_H

#ifdef __linux__
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Create a fresh directory under $TMPDIR (or /tmp). Exits on failure. */
static void tmp_make(char *out, size_t cap)
{
	const char *base = getenv("TMPDIR");
	int n;

	if (base == NULL || base[0] == '\0')
		base = "/tmp";
	n = snprintf(out, cap, "%s/ntfsmac-dmn-XXXXXX", base);
	if (n < 0 || (size_t)n >= cap || mkdtemp(out) == NULL) {
		perror("mkdtemp");
		exit(2);
	}
}

static int tmp_rm_cb(const char *p, const struct stat *sb, int flag, struct FTW *f)
{
	(void)sb;
	(void)flag;
	(void)f;
	return remove(p);
}

static void tmp_rmrf(const char *dir)
{
	(void)nftw(dir, tmp_rm_cb, 16, FTW_DEPTH | FTW_PHYS);
}

/* One directory below an existing directory. */
static void tmp_mkdir(const char *base, const char *rel)
{
	char p[PATH_MAX];

	snprintf(p, sizeof p, "%s/%s", base, rel);
	if (mkdir(p, 0755) != 0 && errno != EEXIST) {
		perror(p);
		exit(2);
	}
}

static void tmp_write(const char *base, const char *rel, const char *text)
{
	char p[PATH_MAX];
	FILE *f;

	snprintf(p, sizeof p, "%s/%s", base, rel);
	f = fopen(p, "w");
	if (f == NULL) {
		perror(p);
		exit(2);
	}
	fputs(text, f);
	fclose(f);
}

#endif
