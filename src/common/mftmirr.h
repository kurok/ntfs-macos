/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_COMMON_MFTMIRR_H
#define NTFSMAC_COMMON_MFTMIRR_H

/*
 * The strict stale-`$MFTMirr` rule of docs/ARCHITECTURE.md section 3.7 (ADR-021),
 * rules M1 to M5, on the raw bytes of both copies of the first MFT records.
 * Shared by the probe (check 2a) and `ntfsmac-volmark sync-mirror` (T-M4-16):
 * one implementation, so the two cannot disagree. Rule M6 (the read-only
 * overlay mount) needs the volume library and is done by the caller with the
 * result of ntfsmac_mm_check().
 *
 * Pure C, no libntfs-3g, no allocation, no I/O of its own: bytes come through a
 * read callback, so the same code runs on a device descriptor and on an
 * in-memory copy (tests). Every on-disk number is untrusted.
 */

#include <stddef.h>
#include <stdint.h>

/* Records 0 to 3 are the only ones that may differ (rule M1). */
#define NTFSMAC_MM_DIFF_RECORDS 4u
/* The engine compares at most FILE_first_user (16) records. */
#define NTFSMAC_MM_MAX_COMPARED 16u

/* Read len bytes at byte offset off; 0 on success, -1 on error or short read. */
typedef int (*ntfsmac_mm_read_fn)(void *ctx, uint64_t off, void *buf, size_t len);

/* Geometry from the boot sector, as NTFS-3G's ntfs_boot_sector_parse() computes it. */
typedef struct ntfsmac_mm_geometry {
	uint32_t sector_size;   /* 512, 1024, 2048 or 4096 */
	uint32_t cluster_size;  /* power of two, 512 .. 2097152 */
	uint32_t record_size;   /* MFT record size, power of two */
	uint64_t total_sectors; /* number_of_sectors of the boot sector */
	uint64_t mft_lcn;
	uint64_t mftmirr_lcn;
	uint64_t serial;        /* volume_serial_number */
} ntfsmac_mm_geometry;

/*
 * Parse 512 bytes of boot sector. Returns 0, or -1 if the fields are not a
 * usable NTFS geometry (sector or cluster size out of range, cluster or record
 * size not a power of two, zero sectors, mft_lcn or mftmirr_lcn beyond the
 * cluster count). The OEM ID is not looked at (check 1 does that).
 */
int ntfsmac_mm_parse_boot(const uint8_t boot[512], ntfsmac_mm_geometry *g);

/* Byte offset of record k of `$MFT` (first extent) and of `$MFTMirr`. */
uint64_t ntfsmac_mm_mft_offset(const ntfsmac_mm_geometry *g, unsigned k);
uint64_t ntfsmac_mm_mirr_offset(const ntfsmac_mm_geometry *g, unsigned k);

typedef enum ntfsmac_mm_status {
	NTFSMAC_MM_SPLIT = 0,   /* M1 to M5 hold; diff_mask lists the differing records */
	NTFSMAC_MM_NOT_SPLIT,   /* some rule fails: the volume is `corrupt` */
	NTFSMAC_MM_IO_ERROR     /* a read failed: no verdict from this check */
} ntfsmac_mm_status;

typedef struct ntfsmac_mm_result {
	unsigned diff_mask;     /* bit k set: record k (0 to 3) differs; SPLIT only */
	unsigned failed_rule;   /* 1 to 5 for M1 to M5 when NOT_SPLIT, else 0 */
	unsigned records;       /* records the engine compares: min(mirror size, 16) */
	const char *why;        /* static text naming the failed condition, for logs and tests */
} ntfsmac_mm_result;

/*
 * Rules M1 to M5. The caller has checked that the volume fits the device and
 * that the boot sector passed check 1 (the first half of M1). Reads at most
 * 2 * 16 records. NOT_SPLIT with failed_rule 3 means nothing differs: the mount
 * failed for another reason.
 */
ntfsmac_mm_status ntfsmac_mm_check(ntfsmac_mm_read_fn rd, void *ctx,
				   const ntfsmac_mm_geometry *g, ntfsmac_mm_result *res);

#endif /* NTFSMAC_COMMON_MFTMIRR_H */
