/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Conflict detection and passive mode (docs/ARCHITECTURE.md section 8.4 and
 * section 4.1 step 2).
 *
 * If another non-Apple NTFS driver is installed, the daemon is in passive
 * mode: it issues no mount-approval dissent and no automount. Passive mode
 * is not a volume state; the daemon does not start a volume's state machine.
 *
 * The rule is pure (a directory entry name in, a kind out). The only file
 * system access is ntfsmac_conflict_scan(), which takes the directory as an
 * argument so that tests can point it at a temporary directory.
 */
#include "internal.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

/* Case-insensitive, ASCII only (bundle names on APFS ignore case). */
static bool ci_equal(const char *a, const char *b)
{
	for (; *a != '\0' && *b != '\0'; a++, b++)
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return false;
	return *a == *b;
}

static bool ci_prefix(const char *s, const char *prefix)
{
	for (; *prefix != '\0'; s++, prefix++)
		if (*s == '\0' ||
		    tolower((unsigned char)*s) != tolower((unsigned char)*prefix))
			return false;
	return true;
}

static bool ci_contains(const char *s, const char *needle)
{
	for (; *s != '\0'; s++)
		if (ci_prefix(s, needle))
			return true;
	return false;
}

static bool ci_suffix(const char *s, const char *suffix)
{
	size_t ls = strlen(s), lx = strlen(suffix);

	return ls >= lx && ci_equal(s + ls - lx, suffix);
}

ntfsmac_conflict_kind ntfsmac_conflict_classify(const char *name)
{
	if (name == NULL || name[0] == '\0')
		return NTFSMAC_CONFLICT_NONE;
	if (ci_equal(name, "ufsd_NTFS.fs"))
		return NTFSMAC_CONFLICT_PARAGON;
	if (ci_equal(name, "tuxera_ntfs.fs"))
		return NTFSMAC_CONFLICT_TUXERA;
	/* Not conflicts: Apple's own bundle and our own names. */
	if (ci_equal(name, "ntfs.fs") || ci_prefix(name, "ntfsmac"))
		return NTFSMAC_CONFLICT_NONE;
	/* macFUSE and fuse-t have no "ntfs" in the name and fall through. */
	if (ci_suffix(name, ".fs") && ci_contains(name, "ntfs"))
		return NTFSMAC_CONFLICT_OTHER;
	return NTFSMAC_CONFLICT_NONE;
}

const char *ntfsmac_conflict_kind_name(ntfsmac_conflict_kind k)
{
	switch (k) {
	case NTFSMAC_CONFLICT_NONE: return "none";
	case NTFSMAC_CONFLICT_PARAGON: return "Paragon NTFS (ufsd_NTFS.fs)";
	case NTFSMAC_CONFLICT_TUXERA: return "Tuxera NTFS (tuxera_ntfs.fs)";
	case NTFSMAC_CONFLICT_OTHER: return "another NTFS driver";
	case NTFSMAC_CONFLICT_KIND_COUNT_: break;
	}
	return "?";
}

ntfsmac_dmn_status ntfsmac_conflict_scan(const char *fs_dir, ntfsmac_conflict_report *out)
{
	DIR *d;
	struct dirent *de;
	int saved;

	if (out == NULL)
		return NTFSMAC_DMN_E_INVAL;
	out->passive = false;
	out->drivers_found = 0;
	out->kind_mask = 0;
	if (fs_dir == NULL)
		return NTFSMAC_DMN_E_INVAL;

	d = opendir(fs_dir);
	if (d == NULL) {
		if (errno == ENOENT)
			return NTFSMAC_DMN_OK; /* no bundle directory: no driver */
		out->passive = true;           /* cannot tell: stay out of the way */
		return NTFSMAC_DMN_E_IO;
	}
	errno = 0;
	while ((de = readdir(d)) != NULL) {
		ntfsmac_conflict_kind k = ntfsmac_conflict_classify(de->d_name);
		struct stat st;

		if (k != NTFSMAC_CONFLICT_NONE &&
		    /* A bundle is a directory; a stray file or dangling link is not. */
		    fstatat(dirfd(d), de->d_name, &st, 0) == 0 && S_ISDIR(st.st_mode)) {
			out->drivers_found++;
			out->kind_mask |= 1u << (unsigned)k;
		}
		errno = 0;
	}
	saved = errno;
	closedir(d);
	if (saved != 0) {
		out->passive = true;
		return NTFSMAC_DMN_E_IO;
	}
	out->passive = out->drivers_found > 0;
	return NTFSMAC_DMN_OK;
}

ntfsmac_dmn_status ntfsmac_conflict_detect(ntfsmac_conflict_report *out)
{
	return ntfsmac_conflict_scan(NTFSMAC_FS_DIR_DEFAULT, out);
}

bool ntfsmac_passive_allows_dissent(const ntfsmac_conflict_report *r)
{
	return r != NULL && !r->passive;
}

bool ntfsmac_passive_allows_automount(const ntfsmac_conflict_report *r)
{
	return r != NULL && !r->passive;
}

ntfsmac_dmn_status ntfsmac_passive_check_manual_mount(const ntfsmac_conflict_report *r,
						      bool volume_mounted_by_other)
{
	if ((r == NULL || r->passive) && volume_mounted_by_other)
		return NTFSMAC_DMN_E_DENIED;
	return NTFSMAC_DMN_OK;
}
