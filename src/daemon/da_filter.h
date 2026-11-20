/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_DAEMON_DA_FILTER_H
#define NTFSMAC_DAEMON_DA_FILTER_H

/*
 * Pure helpers of the Disk Arbitration module (da.c, T-M4-02): the NTFS
 * filter of docs/ARCHITECTURE.md section 4.1 step 1, the BSD name check and
 * a small fixed-size table of BSD names. No Disk Arbitration, no
 * CoreFoundation, no allocation, no locking. They build and are tested on
 * every host.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "diskN" or "diskNsM", digits only, N and M at most 6 digits. */
bool ntfsmac_da_bsd_name_valid(const char *name);

/*
 * The filter of section 4.1 step 1. `content` is DAMediaContent (may be
 * NULL); `whole` is DAMediaWhole. A disk is a candidate if its content is
 * "Windows_NTFS" (MBR type 07) or the GPT basic-data GUID, compared without
 * regard to case, or if it is a whole disk whose content is empty or absent
 * (a raw file system without a partition map). A whole disk that carries a
 * partition map is not a candidate: its partitions are.
 */
bool ntfsmac_da_content_is_candidate(const char *content, bool whole);

/*
 * macFUSE's own device (T-M4-11). Every FSKit mount of the worker makes macFUSE attach an 8 MiB
 * disk image with a GUID partition map (its "virtual device"). When that disk appears its content
 * is still empty, so the filter above would take it for a raw NTFS disk and the daemon would
 * give a mount-approval dissent for it; the dissent refuses macFUSE's own mount ("Failed to
 * activate virtual device", reason `spawn` or `mount_timeout`) whenever the probe is slow. It is
 * recognised by what no physical disk has: a whole virtual disk image (DADeviceModel "Disk
 * Image", protocol "Virtual Interface") of exactly 8 MiB with no content yet. A raw NTFS disk
 * image of exactly 8 MiB is therefore not handled by this product (Apple's driver mounts it).
 */
#define NTFSMAC_DA_MACFUSE_BACKING_BYTES (UINT64_C(8) * 1024u * 1024u)
bool ntfsmac_da_is_macfuse_backing(uint64_t size_bytes, bool whole, const char *content,
				   const char *model, const char *protocol);

/* Table of BSD names, each with one flag. Fixed size; the caller locks. */
#define NTFSMAC_DA_TABLE_MAX 128
#define NTFSMAC_DA_NAME_MAX 32

typedef struct ntfsmac_da_table {
	unsigned count;
	char names[NTFSMAC_DA_TABLE_MAX][NTFSMAC_DA_NAME_MAX];
	bool flags[NTFSMAC_DA_TABLE_MAX];
} ntfsmac_da_table;

void ntfsmac_da_table_init(ntfsmac_da_table *t);
/*
 * Adds `name` with flag false. True if it is in the table afterwards, false
 * if the name is invalid or the table is full. Adding a name twice keeps one
 * entry and its flag.
 */
bool ntfsmac_da_table_add(ntfsmac_da_table *t, const char *name);
/* True if the name was present and is now removed; *flag gets its flag. */
bool ntfsmac_da_table_remove(ntfsmac_da_table *t, const char *name, bool *flag);
bool ntfsmac_da_table_contains(const ntfsmac_da_table *t, const char *name);
/* Sets the flag of a present name; false if the name is absent. */
bool ntfsmac_da_table_set_flag(ntfsmac_da_table *t, const char *name, bool flag);
/* Reads the flag of a present name; false if the name is absent. */
bool ntfsmac_da_table_get_flag(const ntfsmac_da_table *t, const char *name, bool *flag);

/*
 * macFUSE's disks of live mounts (kind "macfuse-local"): BSD name and the mount path that the
 * disk had when it appeared. The path is gone from the description by the time the disk
 * disappears, so it is kept here. Fixed size; the caller locks.
 */
#define NTFSMAC_DA_FUSE_PATH_MAX 128

typedef struct ntfsmac_da_fuse_table {
	unsigned count;
	char names[NTFSMAC_DA_TABLE_MAX][NTFSMAC_DA_NAME_MAX];
	char paths[NTFSMAC_DA_TABLE_MAX][NTFSMAC_DA_FUSE_PATH_MAX];
} ntfsmac_da_fuse_table;

void ntfsmac_da_fuse_init(ntfsmac_da_fuse_table *t);
/* Adds or updates `name` with `path`. False: invalid name, empty or too long path, or table full. */
bool ntfsmac_da_fuse_add(ntfsmac_da_fuse_table *t, const char *name, const char *path);
/* True if `name` was present and is removed; its path is copied to `path` (cap bytes). */
bool ntfsmac_da_fuse_take(ntfsmac_da_fuse_table *t, const char *name, char *path, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* NTFSMAC_DAEMON_DA_FILTER_H */
