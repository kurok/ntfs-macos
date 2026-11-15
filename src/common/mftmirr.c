/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Strict stale-`$MFTMirr` rule, docs/ARCHITECTURE.md section 3.7, rules M1 to M5
 * (ADR-021). See mftmirr.h. The evidence is the observation of T-M3-14
 * (docs/spikes/M3-crashsweep-triage.md): `$MFT` is exactly one
 * ntfs_mft_record_init() step ahead of the mirror in record 0, nothing else
 * differs. Anything not shown by that evidence is "not split".
 *
 * Offsets are those of MFT_RECORD and ATTR_RECORD in NTFS-3G's layout.h, read
 * with explicit little-endian loads so that nothing depends on struct padding
 * or host byte order.
 */
#include "mftmirr.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define NTFS_BLOCK 512u

/* MFT_RECORD header */
#define R_USA_OFS 0x04
#define R_USA_COUNT 0x06
#define R_ATTRS_OFS 0x14
#define R_FLAGS 0x16
#define R_IN_USE_BYTES 0x18
#define R_ALLOC_BYTES 0x1C
#define R_USA_MIN_OFS 0x2A /* sizeof(MFT_RECORD_OLD): the update sequence array starts after it */
#define MFT_RECORD_IN_USE 0x0001

/* ATTR_RECORD */
#define A_TYPE 0x00
#define A_LENGTH 0x04
#define A_NON_RESIDENT 0x08
#define A_NAME_LENGTH 0x09
#define A_NAME_OFFSET 0x0A
#define A_FLAGS 0x0C
#define A_VALUE_LENGTH 0x10 /* resident */
#define A_VALUE_OFFSET 0x14 /* resident */
#define A_LOWEST_VCN 0x10   /* non-resident */
#define A_HIGHEST_VCN 0x18
#define A_MAPPING_OFS 0x20
#define A_COMPRESSION_UNIT 0x22
#define A_ALLOC_SIZE 0x28
#define A_DATA_SIZE 0x30
#define A_INIT_SIZE 0x38
#define A_RESIDENT_END 0x18
#define A_NONRES_END 0x40

#define AT_DATA 0x80u
#define AT_END 0xFFFFFFFFu

#define MAX_EXTENTS 64

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p)
{
	return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static bool is_pow2(uint64_t v)
{
	return v != 0 && (v & (v - 1)) == 0;
}

int ntfsmac_mm_parse_boot(const uint8_t boot[512], ntfsmac_mm_geometry *g)
{
	uint32_t bps, spc, cluster, rs;
	int64_t sectors, mft_lcn, mirr_lcn, nr_clusters;
	int8_t c;
	uint8_t raw_spc;

	memset(g, 0, sizeof(*g));
	bps = rd16(boot + 0x0B);
	if (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096)
		return -1;
	raw_spc = boot[0x0D];
	if (raw_spc > 128) {
		unsigned shift = 256u - raw_spc;

		if (shift > 12) /* more than 2 MiB per cluster */
			return -1;
		spc = 1u << shift;
	} else {
		spc = raw_spc;
	}
	if (!is_pow2(spc))
		return -1;
	cluster = spc * bps;
	if (cluster < 512 || cluster > 2097152u || !is_pow2(cluster))
		return -1;
	sectors = (int64_t)rd64(boot + 0x28);
	if (sectors <= 0)
		return -1;
	mft_lcn = (int64_t)rd64(boot + 0x30);
	mirr_lcn = (int64_t)rd64(boot + 0x38);
	nr_clusters = sectors / (int64_t)spc;
	if (mft_lcn < 0 || mft_lcn > nr_clusters || mirr_lcn < 0 || mirr_lcn > nr_clusters)
		return -1;
	if ((uint64_t)mft_lcn > UINT64_MAX / cluster || (uint64_t)mirr_lcn > UINT64_MAX / cluster)
		return -1;
	c = (int8_t)boot[0x40];
	if (c < 0) {
		if (c < -16)
			return -1;
		rs = 1u << -c;
	} else {
		if (c == 0 || (uint32_t)c > 65536u / cluster)
			return -1;
		rs = (uint32_t)c * cluster;
	}
	if (rs < 512 || rs > 65536u || !is_pow2(rs))
		return -1;
	g->sector_size = bps;
	g->cluster_size = cluster;
	g->record_size = rs;
	g->total_sectors = (uint64_t)sectors;
	g->mft_lcn = (uint64_t)mft_lcn;
	g->mftmirr_lcn = (uint64_t)mirr_lcn;
	g->serial = rd64(boot + 0x48);
	return 0;
}

uint64_t ntfsmac_mm_mft_offset(const ntfsmac_mm_geometry *g, unsigned k)
{
	return g->mft_lcn * g->cluster_size + (uint64_t)k * g->record_size;
}

uint64_t ntfsmac_mm_mirr_offset(const ntfsmac_mm_geometry *g, unsigned k)
{
	return g->mftmirr_lcn * g->cluster_size + (uint64_t)k * g->record_size;
}

/* ---- records ------------------------------------------------------------ */

/*
 * Multi sector transfer check and removal, as ntfs_mst_post_read_fixup() does it:
 * the update sequence array has size/512 + 1 entries, and the last two bytes of
 * every 512-byte sector equal the update sequence number. On success the saved
 * bytes are put back in place (the update sequence array itself stays).
 */
static bool fixup(uint8_t *rec, uint32_t rs)
{
	uint32_t usa_ofs, usa_count, i;
	uint16_t usn;

	if (memcmp(rec, "FILE", 4) != 0)
		return false;
	usa_ofs = rd16(rec + R_USA_OFS);
	usa_count = rd16(rec + R_USA_COUNT);
	if ((usa_ofs & 1) || usa_ofs < R_USA_MIN_OFS || usa_count < 2 ||
	    (usa_count - 1) * NTFS_BLOCK != rs || usa_ofs + 2 * usa_count > rs)
		return false;
	usn = rd16(rec + usa_ofs);
	for (i = 1; i < usa_count; i++)
		if (rd16(rec + i * NTFS_BLOCK - 2) != usn)
			return false;
	for (i = 1; i < usa_count; i++)
		memcpy(rec + i * NTFS_BLOCK - 2, rec + usa_ofs + 2 * i, 2);
	return true;
}

/* One attribute inside the record: offset and length, or false if inconsistent. */
static bool attr_ok(const uint8_t *rec, uint32_t off, uint32_t len)
{
	const uint8_t *a = rec + off;

	if (a[A_NON_RESIDENT]) {
		if (len < A_NONRES_END)
			return false;
		if (rd16(a + A_MAPPING_OFS) < A_NONRES_END || rd16(a + A_MAPPING_OFS) > len)
			return false;
	} else {
		if (len < A_RESIDENT_END)
			return false;
		if ((uint64_t)rd16(a + A_VALUE_OFFSET) + rd32(a + A_VALUE_LENGTH) > len)
			return false;
	}
	if (a[A_NAME_LENGTH] &&
	    (uint32_t)rd16(a + A_NAME_OFFSET) + 2u * a[A_NAME_LENGTH] > len)
		return false;
	return true;
}

/*
 * Record structure as ntfs_mft_record_check() sees it, plus the in-use flag the
 * rule asks for. `rec` is after the fix-up. If data_off is not NULL it receives
 * the offset of the unnamed $DATA attribute, or 0 when there is none.
 */
static bool record_ok(const uint8_t *rec, uint32_t rs, uint32_t *data_off)
{
	uint32_t attrs, used, usa_end, off, prev_type = 0;

	if (data_off)
		*data_off = 0;
	used = rd32(rec + R_IN_USE_BYTES);
	attrs = rd16(rec + R_ATTRS_OFS);
	usa_end = (uint32_t)rd16(rec + R_USA_OFS) + 2u * rd16(rec + R_USA_COUNT);
	if (rd32(rec + R_ALLOC_BYTES) != rs || used > rs || used < attrs + 4 || (attrs & 7) ||
	    attrs < usa_end || attrs >= rs)
		return false;
	if (!(rd16(rec + R_FLAGS) & MFT_RECORD_IN_USE))
		return false;
	off = attrs;
	for (;;) {
		uint32_t type, len;

		if (used - off < 4)
			return false;
		type = rd32(rec + off + A_TYPE);
		if (type == AT_END)
			break;
		if (used - off < A_RESIDENT_END || type < prev_type)
			return false;
		len = rd32(rec + off + A_LENGTH);
		if (len > used - off || (len & 7) || !attr_ok(rec, off, len))
			return false;
		if (type == AT_DATA && rec[off + A_NAME_LENGTH] == 0 && data_off && *data_off == 0)
			*data_off = off;
		prev_type = type;
		off += len;
	}
	return true;
}

/* ---- run lists ---------------------------------------------------------- */

typedef struct extent {
	uint64_t vcn;
	uint64_t len;
	int64_t lcn; /* -1: sparse */
} extent;

/*
 * Decode the mapping pairs of the non-resident attribute at rec + off
 * (attribute length `alen`). Returns the extent count, or -1 if malformed,
 * longer than MAX_EXTENTS, or with a negative LCN.
 */
static int decode_runs(const uint8_t *rec, uint32_t off, uint32_t alen, extent *ex)
{
	const uint8_t *p = rec + off + rd16(rec + off + A_MAPPING_OFS);
	const uint8_t *end = rec + off + alen;
	uint64_t vcn = rd64(rec + off + A_LOWEST_VCN);
	int64_t lcn = 0;
	int n = 0;

	while (p < end && *p) {
		unsigned lsz = *p & 0x0F, osz = *p >> 4, i;
		uint64_t len = 0;
		int64_t delta = 0;

		if (lsz == 0 || lsz > 8 || osz > 8 || (size_t)(end - p) < 1u + lsz + osz)
			return -1;
		for (i = 0; i < lsz; i++)
			len |= (uint64_t)p[1 + i] << (8 * i);
		if (len == 0 || len > (uint64_t)INT64_MAX)
			return -1;
		if (osz) {
			uint64_t u = 0;

			for (i = 0; i < osz; i++)
				u |= (uint64_t)p[1 + lsz + i] << (8 * i);
			if (p[lsz + osz] & 0x80) /* sign extend */
				for (i = osz; i < 8; i++)
					u |= (uint64_t)0xFF << (8 * i);
			delta = (int64_t)u;
			if ((delta > 0 && lcn > INT64_MAX - delta) || (delta < 0 && lcn < INT64_MIN - delta))
				return -1;
			lcn += delta;
			if (lcn < 0)
				return -1;
		}
		if (n == MAX_EXTENTS)
			return -1;
		ex[n].vcn = vcn;
		ex[n].len = len;
		ex[n].lcn = osz ? lcn : -1;
		n++;
		if (vcn > UINT64_MAX - len)
			return -1;
		vcn += len;
		p += 1u + lsz + osz;
	}
	return n;
}

/* Sum of extent lengths; 0 on overflow (a run list never sums to 0). */
static uint64_t extents_total(const extent *ex, int n)
{
	uint64_t t = 0;
	int i;

	for (i = 0; i < n; i++) {
		if (t > UINT64_MAX - ex[i].len)
			return 0;
		t += ex[i].len;
	}
	return t;
}

/* Every extent of `inner` is mapped to the same LCNs by `outer` (no sparse parts). */
static bool extents_covered(const extent *inner, int ni, const extent *outer, int no)
{
	int i, j;

	for (i = 0; i < ni; i++) {
		uint64_t vcn = inner[i].vcn, left = inner[i].len;

		if (inner[i].lcn < 0)
			return false;
		while (left) {
			bool hit = false;

			for (j = 0; j < no; j++) {
				uint64_t skip;

				if (outer[j].lcn < 0 || vcn < outer[j].vcn || vcn - outer[j].vcn >= outer[j].len)
					continue;
				skip = vcn - outer[j].vcn;
				if ((uint64_t)outer[j].lcn + skip != (uint64_t)inner[i].lcn + (vcn - inner[i].vcn))
					return false;
				{
					uint64_t take = outer[j].len - skip;

					if (take > left)
						take = left;
					vcn += take;
					left -= take;
				}
				hit = true;
				break;
			}
			if (!hit)
				return false;
		}
	}
	return true;
}

/* ---- the rule ----------------------------------------------------------- */

#define MAX_ALLOWED 4

typedef struct range {
	uint32_t from, to; /* [from, to) */
} range;

static bool in_ranges(const range *r, int n, uint32_t pos)
{
	int i;

	for (i = 0; i < n; i++)
		if (pos >= r[i].from && pos < r[i].to)
			return true;
	return false;
}

/*
 * The record of `$MFT` and its mirror copy are equal in their first `used` bytes
 * except inside the allowed ranges.
 */
static bool equal_but(const uint8_t *m, const uint8_t *c, uint32_t used, const range *r, int n)
{
	uint32_t i;

	for (i = 0; i < used; i++)
		if (m[i] != c[i] && !in_ranges(r, n, i))
			return false;
	return true;
}

static ntfsmac_mm_status fail(ntfsmac_mm_result *res, unsigned rule, const char *why)
{
	res->failed_rule = rule;
	res->why = why;
	res->diff_mask = 0;
	return NTFSMAC_MM_NOT_SPLIT;
}

/*
 * Record 0: the difference must be one ntfs_mft_record_init() step in the
 * unnamed non-resident $DATA attribute (rule M4, k = 0).
 */
static const char *check_record0(const uint8_t *m, const uint8_t *c, uint32_t used,
				 uint32_t usa_pos, const ntfsmac_mm_geometry *g, uint32_t doff_m,
				 uint32_t doff_c)
{
	const uint8_t *dm, *dc;
	uint32_t alen, mp;
	range allow[MAX_ALLOWED];
	uint64_t alloc_m, alloc_c, size_m, size_c, init_m, init_c, hv_m, hv_c;
	extent em[MAX_EXTENTS], ec[MAX_EXTENTS];
	int nm, nc;

	if (doff_m == 0 || doff_m != doff_c)
		return "record 0 has no $DATA attribute at the same place in both copies";
	dm = m + doff_m;
	dc = c + doff_c;
	alen = rd32(dm + A_LENGTH);
	if (alen != rd32(dc + A_LENGTH) || !dm[A_NON_RESIDENT] || !dc[A_NON_RESIDENT])
		return "$DATA of record 0 is resident or of another length";
	if (rd16(dm + A_FLAGS) != 0 || dm[A_COMPRESSION_UNIT] != 0 || rd64(dm + A_LOWEST_VCN) != 0)
		return "$DATA of record 0 is compressed, sparse or does not start at VCN 0";
	mp = rd16(dm + A_MAPPING_OFS);
	allow[0].from = usa_pos;
	allow[0].to = usa_pos + 2;
	allow[1].from = doff_m + A_HIGHEST_VCN;
	allow[1].to = doff_m + A_HIGHEST_VCN + 8;
	allow[2].from = doff_m + A_ALLOC_SIZE;
	allow[2].to = doff_m + A_INIT_SIZE + 8;
	allow[3].from = doff_m + mp;
	allow[3].to = doff_m + alen;
	if (!equal_but(m, c, used, allow, MAX_ALLOWED))
		return "record 0 differs outside the allowed $DATA fields and the USN";

	alloc_m = rd64(dm + A_ALLOC_SIZE);
	alloc_c = rd64(dc + A_ALLOC_SIZE);
	size_m = rd64(dm + A_DATA_SIZE);
	size_c = rd64(dc + A_DATA_SIZE);
	init_m = rd64(dm + A_INIT_SIZE);
	init_c = rd64(dc + A_INIT_SIZE);
	hv_m = rd64(dm + A_HIGHEST_VCN);
	hv_c = rd64(dc + A_HIGHEST_VCN);
	if (init_c > UINT64_MAX - g->record_size || init_m != init_c + g->record_size)
		return "$MFT is not exactly one record ahead of the mirror";
	if (size_m != init_m || size_c != init_c)
		return "data size differs from initialized size";
	if (alloc_c == 0 || alloc_c % g->cluster_size || alloc_m < alloc_c ||
	    (alloc_m - alloc_c) % g->cluster_size || init_m > alloc_m)
		return "allocated size is not larger by whole clusters";
	if (hv_m != alloc_m / g->cluster_size - 1 || hv_c != alloc_c / g->cluster_size - 1)
		return "highest VCN does not match the allocated size";
	nm = decode_runs(m, doff_m, alen, em);
	nc = decode_runs(c, doff_c, alen, ec);
	if (nm <= 0 || nc <= 0)
		return "run list does not decode";
	if (extents_total(em, nm) != hv_m + 1 || extents_total(ec, nc) != hv_c + 1)
		return "run list length does not match the highest VCN";
	if (em[0].lcn != ec[0].lcn || em[0].lcn < 0 || (uint64_t)em[0].lcn != g->mft_lcn)
		return "run lists do not start at the LCN of the boot sector";
	if (!extents_covered(ec, nc, em, nm))
		return "the mirror maps a VCN that $MFT maps elsewhere";
	return NULL;
}

ntfsmac_mm_status ntfsmac_mm_check(ntfsmac_mm_read_fn rd, void *ctx,
				   const ntfsmac_mm_geometry *g, ntfsmac_mm_result *res)
{
	const uint32_t rs = g->record_size;
	uint32_t n_cap, n, i, k;
	uint32_t d1_off = 0, d0_off = 0, d0c_off = 0;
	uint8_t *buf = NULL, *mft, *mir;
	uint64_t mirr_size;
	extent e0[MAX_EXTENTS], e1[MAX_EXTENTS];
	int n0, n1;
	unsigned mask = 0;
	ntfsmac_mm_status st = NTFSMAC_MM_NOT_SPLIT;

	memset(res, 0, sizeof(*res));
	res->why = "";
	if (rs < 1024 || rs % NTFS_BLOCK || rs > 65536u || g->cluster_size == 0)
		return fail(res, 1, "record size not usable");

	/* The engine's initial mirror size (ntfs_boot_sector_parse), capped at 16. */
	n_cap = g->cluster_size <= 4u * rs ? 4u : g->cluster_size / rs;
	if (n_cap > NTFSMAC_MM_MAX_COMPARED)
		n_cap = NTFSMAC_MM_MAX_COMPARED;

	buf = malloc((size_t)n_cap * rs * 2);
	if (!buf)
		return NTFSMAC_MM_IO_ERROR;
	mft = buf;
	mir = buf + (size_t)n_cap * rs;

	/* $MFT records 0 .. n_cap-1 first: record 1 says how big the mirror is. */
	if (rd(ctx, ntfsmac_mm_mft_offset(g, 0), mft, (size_t)n_cap * rs) != 0) {
		st = NTFSMAC_MM_IO_ERROR;
		goto out;
	}
	for (i = 0; i < n_cap; i++)
		if (!fixup(mft + (size_t)i * rs, rs) ||
		    !record_ok(mft + (size_t)i * rs, rs, i == 0 ? &d0_off : i == 1 ? &d1_off : NULL)) {
			st = fail(res, 2, "a $MFT record is invalid");
			goto out;
		}
	/* Record 1 ($MFTMirr): its unnamed $DATA gives the mirror size and where it is. */
	if (d1_off == 0 || !mft[(size_t)rs + d1_off + A_NON_RESIDENT]) {
		st = fail(res, 1, "$MFTMirr has no non-resident $DATA");
		goto out;
	}
	{
		const uint8_t *r1 = mft + rs;

		mirr_size = rd64(r1 + d1_off + A_DATA_SIZE);
		n1 = decode_runs(r1, d1_off, rd32(r1 + d1_off + A_LENGTH), e1);
	}
	n = (uint32_t)(mirr_size / rs < n_cap ? mirr_size / rs : n_cap);
	res->records = n;
	if (n < NTFSMAC_MM_DIFF_RECORDS) {
		st = fail(res, 1, "the mirror holds fewer than four records");
		goto out;
	}
	if (n1 <= 0 || e1[0].lcn < 0 || (uint64_t)e1[0].lcn != g->mftmirr_lcn ||
	    e1[0].len > UINT64_MAX / g->cluster_size ||
	    e1[0].len * g->cluster_size < (uint64_t)n * rs) {
		st = fail(res, 1, "the mirror is not contiguous at the LCN of the boot sector");
		goto out;
	}
	n0 = -1;
	if (d0_off != 0 && mft[d0_off + A_NON_RESIDENT])
		n0 = decode_runs(mft, d0_off, rd32(mft + d0_off + A_LENGTH), e0);
	if (n0 <= 0 || e0[0].lcn < 0 || (uint64_t)e0[0].lcn != g->mft_lcn ||
	    e0[0].len > UINT64_MAX / g->cluster_size ||
	    e0[0].len * g->cluster_size < (uint64_t)n_cap * rs) {
		st = fail(res, 1, "$MFT is not contiguous at the LCN of the boot sector");
		goto out;
	}

	if (rd(ctx, ntfsmac_mm_mirr_offset(g, 0), mir, (size_t)n * rs) != 0) {
		st = NTFSMAC_MM_IO_ERROR;
		goto out;
	}
	/* M2: every compared record of both copies is valid. */
	for (i = 0; i < n; i++)
		if (!fixup(mir + (size_t)i * rs, rs) ||
		    !record_ok(mir + (size_t)i * rs, rs, i == 0 ? &d0c_off : NULL)) {
			st = fail(res, 2, "a $MFTMirr record is invalid");
			goto out;
		}

	/* M3 and M1: which records differ, as the engine compares them. */
	for (i = 0; i < n; i++) {
		const uint8_t *m = mft + (size_t)i * rs, *c = mir + (size_t)i * rs;
		uint32_t used = rd32(m + R_IN_USE_BYTES);

		if (memcmp(m, c, used) != 0) {
			if (i >= NTFSMAC_MM_DIFF_RECORDS) {
				st = fail(res, 1, "a record at index 4 or above differs");
				goto out;
			}
			mask |= 1u << i;
		}
	}
	if (mask == 0) {
		st = fail(res, 3, "the copies do not differ");
		goto out;
	}

	/* M4 and M5 for each differing record. */
	for (k = 0; k < NTFSMAC_MM_DIFF_RECORDS; k++) {
		const uint8_t *m = mft + (size_t)k * rs, *c = mir + (size_t)k * rs;
		uint32_t used = rd32(m + R_IN_USE_BYTES), usa_pos = rd16(m + R_USA_OFS);
		uint16_t usn_m, usn_c, next;

		if (!(mask & (1u << k)))
			continue;
		if (k == 3) {
			/*
			 * The VOLUME_IS_DIRTY difference of volmark's write is accepted
			 * only after T-M3-12 has recorded it (section 3.7, M4, k = 3).
			 */
			st = fail(res, 4, "a differing record 3 is not accepted yet");
			goto out;
		}
		if (k == 0) {
			const char *why = check_record0(m, c, used, usa_pos, g, d0_off, d0c_off);

			if (why) {
				st = fail(res, 4, why);
				goto out;
			}
		} else {
			range allow[1];

			allow[0].from = usa_pos;
			allow[0].to = usa_pos + 2;
			if (!equal_but(m, c, used, allow, 1)) {
				st = fail(res, 4, "record 1 or 2 differs in more than the USN");
				goto out;
			}
		}
		/* M5: the $MFT USN is the successor of the mirror USN. */
		usn_m = rd16(m + usa_pos);
		usn_c = rd16(c + usa_pos);
		next = (uint16_t)(usn_c + 1);
		if (next == 0xFFFF || next == 0)
			next = 1;
		if (usn_m != next) {
			st = fail(res, 5, "the $MFT USN is not the successor of the mirror USN");
			goto out;
		}
	}
	res->diff_mask = mask;
	res->failed_rule = 0;
	res->why = "";
	st = NTFSMAC_MM_SPLIT;
out:
	free(buf);
	return st;
}
