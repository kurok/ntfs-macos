/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Test helper for test_mftmirr.py: runs the rule of src/common/mftmirr.c on in-memory
 * copies of `$MFT` and `$MFTMirr`.
 *
 *   mm_tool rule --sector-size S --cluster-size C --record-size R --mft-lcn N --mirr-lcn N
 *                --mft FILE --mir FILE [--fail-read K]
 *   mm_tool boot FILE        (512 bytes of boot sector: prints the geometry or "invalid")
 *
 * FILE of --mft holds `$MFT` from its first byte (the bytes at mft_lcn * C), FILE of --mir the bytes
 * at mirr_lcn * C. A read outside them fails, as does the K-th read with --fail-read K (1 based).
 * Output of `rule`: "SPLIT mask=M records=N" or "NOT_SPLIT rule=K why=TEXT" or "IO_ERROR".
 */
#include "../../../src/probe/posix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mftmirr.h"

typedef struct mem {
	uint8_t *mft, *mir;
	size_t mft_len, mir_len;
	uint64_t mft_off, mir_off;
	int reads, fail_read;
} mem;

static int rd(void *ctx, uint64_t off, void *buf, size_t len)
{
	mem *m = ctx;

	m->reads++;
	if (m->fail_read && m->reads == m->fail_read)
		return -1;
	if (off >= m->mft_off && off + len <= m->mft_off + m->mft_len) {
		memcpy(buf, m->mft + (off - m->mft_off), len);
		return 0;
	}
	if (off >= m->mir_off && off + len <= m->mir_off + m->mir_len) {
		memcpy(buf, m->mir + (off - m->mir_off), len);
		return 0;
	}
	return -1;
}

static uint8_t *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	uint8_t *b;
	long n;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = malloc((size_t)n ? (size_t)n : 1);
	if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
		free(b);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*len = (size_t)n;
	return b;
}

int main(int argc, char **argv)
{
	ntfsmac_mm_geometry g;
	ntfsmac_mm_result res;
	mem m;
	int i;

	memset(&g, 0, sizeof(g));
	memset(&m, 0, sizeof(m));
	if (argc == 3 && strcmp(argv[1], "boot") == 0) {
		size_t n;
		uint8_t *b = slurp(argv[2], &n);

		if (!b || n != 512 || ntfsmac_mm_parse_boot(b, &g) != 0) {
			puts("invalid");
		} else {
			printf("sector=%u cluster=%u record=%u sectors=%llu mft=%llu mirr=%llu serial=%llX\n",
			       g.sector_size, g.cluster_size, g.record_size, (unsigned long long)g.total_sectors,
			       (unsigned long long)g.mft_lcn, (unsigned long long)g.mftmirr_lcn,
			       (unsigned long long)g.serial);
		}
		free(b);
		return 0;
	}
	if (argc < 2 || strcmp(argv[1], "rule") != 0)
		return 2;
	for (i = 2; i + 1 < argc; i += 2) {
		const char *k = argv[i], *v = argv[i + 1];

		if (!strcmp(k, "--sector-size"))
			g.sector_size = (uint32_t)strtoul(v, NULL, 10);
		else if (!strcmp(k, "--cluster-size"))
			g.cluster_size = (uint32_t)strtoul(v, NULL, 10);
		else if (!strcmp(k, "--record-size"))
			g.record_size = (uint32_t)strtoul(v, NULL, 10);
		else if (!strcmp(k, "--mft-lcn"))
			g.mft_lcn = strtoull(v, NULL, 10);
		else if (!strcmp(k, "--mirr-lcn"))
			g.mftmirr_lcn = strtoull(v, NULL, 10);
		else if (!strcmp(k, "--mft"))
			m.mft = slurp(v, &m.mft_len);
		else if (!strcmp(k, "--mir"))
			m.mir = slurp(v, &m.mir_len);
		else if (!strcmp(k, "--fail-read"))
			m.fail_read = atoi(v);
		else
			return 2;
	}
	if (!m.mft || !m.mir || !g.cluster_size || !g.record_size) {
		free(m.mft);
		free(m.mir);
		return 2;
	}
	m.mft_off = ntfsmac_mm_mft_offset(&g, 0);
	m.mir_off = ntfsmac_mm_mirr_offset(&g, 0);
	switch (ntfsmac_mm_check(rd, &m, &g, &res)) {
	case NTFSMAC_MM_SPLIT:
		printf("SPLIT mask=%u records=%u\n", res.diff_mask, res.records);
		break;
	case NTFSMAC_MM_NOT_SPLIT:
		printf("NOT_SPLIT rule=%u why=%s\n", res.failed_rule, res.why);
		break;
	default:
		puts("IO_ERROR");
	}
	free(m.mft);
	free(m.mir);
	return 0;
}
