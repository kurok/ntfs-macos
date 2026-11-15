/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Mount-point leaf creation with collision suffixes (ADR-016, "Collisions").
 * mkdirat() does not follow a symbolic link in the last component and fails with
 * EEXIST for every existing entry (directory, file or link), so an existing entry
 * is never reused.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include "sanitize.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

int ntfsmac_leaf_create(int dirfd, const char *base, mode_t mode, char *leaf, size_t cap,
			dev_t *dev, ino_t *ino)
{
	size_t blen;
	unsigned n;
	char name[NTFSMAC_LEAF_SIZE];

	if (leaf != NULL && cap > 0)
		leaf[0] = '\0';
	if (base == NULL || leaf == NULL || cap < NTFSMAC_LEAF_SIZE) {
		errno = EINVAL;
		return -1;
	}
	blen = strlen(base);
	if (!ntfsmac_label_part_ok(base, blen)) {
		errno = EINVAL;
		return -1;
	}
	memcpy(name, base, blen + 1);
	for (n = 1; n <= NTFSMAC_LEAF_SUFFIX_MAX; n++) {
		if (n >= NTFSMAC_LEAF_SUFFIX_MIN) {
			size_t k = blen;

			name[k++] = ' ';
			if (n >= 10)
				name[k++] = (char)('0' + n / 10);
			name[k++] = (char)('0' + n % 10);
			name[k] = '\0';
		}
		if (mkdirat(dirfd, name, mode) == 0) {
			struct stat st;

			memcpy(leaf, name, strlen(name) + 1);
			/* No unlink on failure: nothing proves the entry is still ours. */
			if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
				errno = ESTALE;
				return -1;
			}
			if (dev != NULL)
				*dev = st.st_dev;
			if (ino != NULL)
				*ino = st.st_ino;
			return 0;
		}
		if (errno != EEXIST)
			return -1;
	}
	errno = EEXIST;
	return -1;
}
