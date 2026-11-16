/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Mount point lifecycle (ARCHITECTURE.md section 4.1 step 7 and section 4.5
 * step 3; ADR-016).
 *
 * The leaf name comes from the label sanitiser and is made with mkdirat()
 * relative to an O_NOFOLLOW descriptor of the parent, so no step follows a
 * link or reuses an existing entry. The daemon then works on a descriptor of
 * the directory it made, and removes it only when the name still has the
 * recorded device and inode.
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

#include "mountpoint_impl.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sanitize.h"

ntfsmac_dmn_status ntfsmac_mountpoint_create_in(const char *parent, uid_t parent_owner,
						const char *label, size_t label_len,
						uint64_t serial, uid_t console_uid,
						gid_t console_gid, ntfsmac_mountpoint *mp)
{
	char base[NTFSMAC_LABEL_PART_SIZE];
	char leaf[NTFSMAC_LEAF_SIZE];
	struct stat st;
	size_t plen;
	int pfd;
	int dfd;
	dev_t dev = 0;
	ino_t ino = 0;
	int n;

	if (mp != NULL) {
		mp->dirfd = -1;
		mp->dev = 0;
		mp->ino = 0;
		mp->path[0] = '\0';
	}
	if (mp == NULL || parent == NULL || parent[0] != '/' || (label == NULL && label_len != 0))
		return NTFSMAC_DMN_E_INVAL;
	plen = strlen(parent);
	while (plen > 1 && parent[plen - 1] == '/')
		plen--;
	/* "<parent>/<leaf>" and the NUL must fit the path field. */
	if (plen + 1 + (NTFSMAC_LEAF_SIZE - 1) + 1 > sizeof mp->path)
		return NTFSMAC_DMN_E_INVAL;

	if (ntfsmac_label_sanitize(label, label_len, serial, base, sizeof base) == 0)
		return NTFSMAC_DMN_E_INVAL;

	pfd = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (pfd < 0)
		return NTFSMAC_DMN_E_IO;
	if (fstat(pfd, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != parent_owner) {
		close(pfd);
		return NTFSMAC_DMN_E_DENIED;
	}

	if (ntfsmac_leaf_create(pfd, base, 0700, leaf, sizeof leaf, &dev, &ino) != 0) {
		int e = errno;

		close(pfd);
		/*
		 * EEXIST: all 98 names are taken. ESTALE: the directory was made but
		 * its identity could not be read; it is left in place, because
		 * removing it by name could remove somebody else's directory.
		 */
		return e == EEXIST ? NTFSMAC_DMN_E_BUSY : NTFSMAC_DMN_E_IO;
	}

	dfd = openat(pfd, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (dfd < 0) {
		close(pfd);
		return NTFSMAC_DMN_E_IO;
	}
	/* From here on only the descriptor is used. */
	if (fstat(dfd, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_dev != dev || st.st_ino != ino ||
	    st.st_uid != geteuid()) {
		/* Not what mkdirat made: do not touch it and do not remove it. */
		close(dfd);
		close(pfd);
		return NTFSMAC_DMN_E_DENIED;
	}
	if (fchmod(dfd, 0700) != 0 || fchown(dfd, console_uid, console_gid) != 0) {
		/* Proven ours by device and inode just above, so it is safe to take back. */
		if (fstatat(pfd, leaf, &st, AT_SYMLINK_NOFOLLOW) == 0 && st.st_dev == dev &&
		    st.st_ino == ino)
			(void)unlinkat(pfd, leaf, AT_REMOVEDIR);
		close(dfd);
		close(pfd);
		return NTFSMAC_DMN_E_IO;
	}
	close(pfd);

	n = snprintf(mp->path, sizeof mp->path, "%.*s/%s", (int)plen, parent, leaf);
	if (n < 0 || (size_t)n >= sizeof mp->path) {
		close(dfd); /* cannot happen after the length check */
		mp->path[0] = '\0';
		return NTFSMAC_DMN_E_INVAL;
	}
	mp->dirfd = dfd;
	mp->dev = dev;
	mp->ino = ino;
	return NTFSMAC_DMN_OK;
}

ntfsmac_dmn_status ntfsmac_mountpoint_create(const char *label, size_t label_len,
					     uint64_t serial, uid_t console_uid,
					     gid_t console_gid, ntfsmac_mountpoint *mp)
{
	return ntfsmac_mountpoint_create_in(NTFSMAC_VOLUMES_DIR, 0, label, label_len, serial,
					    console_uid, console_gid, mp);
}

/*
 * Called after the volume is unmounted and the worker is gone, so the name
 * no longer leads into a file system served by the worker. Even so, the
 * parent is opened with O_NOFOLLOW and the leaf is examined with
 * AT_SYMLINK_NOFOLLOW.
 *
 * Results: OK (removed, or already gone), E_BUSY (not empty or still a mount
 * point; the record stays so the call can be repeated), E_DENIED (the name
 * is not the directory we made; never removed), E_IO, E_INVAL.
 */
ntfsmac_dmn_status ntfsmac_mountpoint_remove(ntfsmac_mountpoint *mp)
{
	char parent[sizeof mp->path];
	const char *slash;
	const char *leaf;
	struct stat st;
	size_t plen;
	int pfd;
	ntfsmac_dmn_status res;

	if (mp == NULL || mp->path[0] != '/')
		return NTFSMAC_DMN_E_INVAL;
	slash = strrchr(mp->path, '/');
	leaf = slash + 1;
	if (leaf[0] == '\0' || strcmp(leaf, ".") == 0 || strcmp(leaf, "..") == 0)
		return NTFSMAC_DMN_E_INVAL;
	plen = (size_t)(slash - mp->path);
	if (plen == 0) {
		parent[0] = '/';
		parent[1] = '\0';
	} else {
		memcpy(parent, mp->path, plen);
		parent[plen] = '\0';
	}

	pfd = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (pfd < 0)
		return NTFSMAC_DMN_E_IO;

	if (fstatat(pfd, leaf, &st, AT_SYMLINK_NOFOLLOW) != 0) {
		res = errno == ENOENT ? NTFSMAC_DMN_OK : NTFSMAC_DMN_E_IO;
	} else if (!S_ISDIR(st.st_mode) || st.st_dev != mp->dev || st.st_ino != mp->ino) {
		res = NTFSMAC_DMN_E_DENIED;
	} else if (unlinkat(pfd, leaf, AT_REMOVEDIR) == 0) {
		res = NTFSMAC_DMN_OK;
	} else if (errno == ENOTEMPTY || errno == EBUSY || errno == EEXIST) {
		res = NTFSMAC_DMN_E_BUSY;
	} else if (errno == ENOENT) {
		res = NTFSMAC_DMN_OK;
	} else {
		res = NTFSMAC_DMN_E_IO;
	}
	close(pfd);

	if (res == NTFSMAC_DMN_OK || res == NTFSMAC_DMN_E_DENIED) {
		if (mp->dirfd >= 0)
			close(mp->dirfd);
		mp->dirfd = -1;
	}
	return res;
}
