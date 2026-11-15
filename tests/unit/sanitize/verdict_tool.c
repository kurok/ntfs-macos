/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Helper for test_verdict_schema.py: the strict verdict parser as a filter. Each line of
 * stdin is the hex of one input (`-` for an empty one); each line of stdout is the status
 * name, or for an accepted input `ok` followed by every parsed field in a fixed form, and
 * `rt=same` if the probe's JSON writer and the parser reproduce the verdict (the
 * re-serialisation oracle of F-SANITIZE-01), `rt=DIFF` otherwise.
 */
#include "ntfsmac/probe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t ntfsmac_probe_json(const ntfsmac_probe_verdict *v, char *buf, size_t cap);

static const char *const status_names[] = {
	"ok", "too_large", "utf8", "nul", "syntax", "depth", "dup_key", "unknown_key",
	"missing_key", "type", "range", "enum", "grammar", "cross_field",
};

static int hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static void put_hex(const char *s, size_t n)
{
	size_t i;

	if (n == 0) {
		putchar('-');
		return;
	}
	for (i = 0; i < n; i++)
		printf("%02x", (unsigned)(unsigned char)s[i]);
}

static void put_bits(uint32_t bits, unsigned count, const char *(*name)(int))
{
	unsigned i, n = 0;

	for (i = 0; i < count; i++) {
		if (!(bits & (UINT32_C(1) << i)))
			continue;
		printf("%s%s", n++ ? "," : "", name((int)i));
	}
	if (n == 0)
		putchar('-');
}

static const char *flag_name(int i) { return ntfsmac_probe_volflag_name((ntfsmac_probe_volflag)i); }
static const char *reason_name(int i) { return ntfsmac_probe_reason_name((ntfsmac_probe_reason)i); }

static bool same_verdict(const ntfsmac_probe_verdict *a, const ntfsmac_probe_verdict *b)
{
	uint32_t i;

	if (a->is_ntfs != b->is_ntfs || a->bitlocker != b->bitlocker ||
	    a->has_ntfs_version != b->has_ntfs_version ||
	    a->ntfs_version_major != b->ntfs_version_major ||
	    a->ntfs_version_minor != b->ntfs_version_minor || a->label_len != b->label_len ||
	    strcmp(a->label, b->label) != 0 || a->serial != b->serial ||
	    a->total_sectors != b->total_sectors || a->cluster_size != b->cluster_size ||
	    a->sector_size != b->sector_size || a->volume_flags != b->volume_flags ||
	    a->logfile != b->logfile || a->has_logfile_lsn != b->has_logfile_lsn ||
	    a->logfile_lsn != b->logfile_lsn || a->hiberfile != b->hiberfile || a->state != b->state ||
	    a->reason_codes != b->reason_codes || a->nonnfc_count != b->nonnfc_count ||
	    a->nonnfc_path_count != b->nonnfc_path_count)
		return false;
	for (i = 0; i < a->nonnfc_path_count; i++)
		if (strcmp(a->nonnfc_paths[i], b->nonnfc_paths[i]) != 0)
			return false;
	return true;
}

int main(void)
{
	size_t cap = 2u * (NTFSMAC_PROBE_MAX_JSON_BYTES + 64u) + 16u;
	char *line = malloc(cap);
	char *buf = malloc(NTFSMAC_PROBE_MAX_JSON_BYTES + 64u);
	char *json = malloc(256u * 1024u);

	if (line == NULL || buf == NULL || json == NULL)
		return 1;
	while (fgets(line, (int)cap, stdin) != NULL) {
		size_t len = strlen(line), n = 0, i;
		ntfsmac_probe_verdict v, w;
		ntfsmac_probe_status st;
		bool good = true;

		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = '\0';
		if (strcmp(line, "-") != 0) {
			for (i = 0; i < len; i += 2) {
				int h = hexval(line[i]), l = i + 1 < len ? hexval(line[i + 1]) : -1;

				if (h < 0 || l < 0 || n >= NTFSMAC_PROBE_MAX_JSON_BYTES + 64u) {
					good = false;
					break;
				}
				buf[n++] = (char)(h * 16 + l);
			}
		}
		if (!good) {
			puts("bad_input");
			continue;
		}
		st = ntfsmac_probe_parse(buf, n, &v);
		if (st != NTFSMAC_PROBE_OK) {
			puts((unsigned)st < sizeof(status_names) / sizeof(status_names[0]) ? status_names[st]
											  : "?");
			continue;
		}
		printf("ok is_ntfs=%d bitlocker=%d version=", v.is_ntfs, v.bitlocker);
		if (v.has_ntfs_version)
			printf("%u.%u", (unsigned)v.ntfs_version_major, (unsigned)v.ntfs_version_minor);
		else
			putchar('-');
		printf(" label=");
		put_hex(v.label, v.label_len);
		printf(" serial=%016llX total=%llu cluster=%u sector=%u flags=", (unsigned long long)v.serial,
		       (unsigned long long)v.total_sectors, (unsigned)v.cluster_size, (unsigned)v.sector_size);
		put_bits(v.volume_flags, NTFSMAC_PROBE_VOLFLAG_COUNT_, flag_name);
		printf(" logfile=%s lsn=", ntfsmac_probe_logfile_name(v.logfile));
		if (v.has_logfile_lsn)
			printf("%016llX", (unsigned long long)v.logfile_lsn);
		else
			putchar('-');
		printf(" hiberfile=%s state=%s reasons=", ntfsmac_probe_hiberfile_name(v.hiberfile),
		       ntfsmac_probe_state_name(v.state));
		put_bits(v.reason_codes, NTFSMAC_PROBE_REASON_COUNT_, reason_name);
		printf(" count=%u paths=", (unsigned)v.nonnfc_count);
		for (i = 0; i < v.nonnfc_path_count; i++) {
			if (i)
				putchar(',');
			put_hex(v.nonnfc_paths[i], strlen(v.nonnfc_paths[i]));
		}
		if (v.nonnfc_path_count == 0)
			putchar('-');
		/* re-serialise with the probe's writer and parse the result again */
		{
			size_t jn = ntfsmac_probe_json(&v, json, 256u * 1024u);

			if (jn > 0 && ntfsmac_probe_parse(json, jn, &w) == NTFSMAC_PROBE_OK && same_verdict(&v, &w))
				printf(" rt=same\n");
			else
				printf(" rt=DIFF\n");
		}
	}
	free(line);
	free(buf);
	free(json);
	return 0;
}
