/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * F-SANITIZE-01 (docs/TEST_STRATEGY.md section 4): libFuzzer on the strict probe-verdict
 * parser. Oracles, for every input:
 *   - no crash and no sanitizer report;
 *   - any error leaves exactly the fail-closed verdict;
 *   - every accepted verdict satisfies the section 3.1 grammar (stated again here, from the
 *     document, not from the parser) and the cross-field rules;
 *   - every accepted input re-serialises (the probe's own writer) to a document that parses
 *     to the same verdict, and that document is at most 64 KiB;
 *   - the daemon's consumers of the verdict never emit an unchecked byte: the journal name
 *     comes from the serial integer (16 upper-case hex digits), the label goes through the
 *     label sanitiser (output satisfies ADR-016), each path through the display sanitiser
 *     (lossless, allow-listed).
 */
#include "ntfsmac/probe.h"
#include "sanitize.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../unit/sanitize/oracle.h"

size_t ntfsmac_probe_json(const ntfsmac_probe_verdict *v, char *buf, size_t cap);

#define FAIL(msg)                                                                 \
	do {                                                                      \
		fprintf(stderr, "F-SANITIZE-01 oracle failure: %s (line %d)\n", msg, __LINE__); \
		abort();                                                          \
	} while (0)

static bool text_ok(const char *s, size_t max)
{
	size_t len = strlen(s), i = 0;

	if (len > max)
		return false;
	while (i < len) {
		uint32_t cp;
		int n = oracle_decode((const unsigned char *)s + i, len - i, &cp);

		if (n == 0)
			return false;
		i += (size_t)n;
	}
	return true;
}

static bool pow2(uint64_t v)
{
	return v != 0 && (v & (v - 1)) == 0;
}

static void check_grammar(const ntfsmac_probe_verdict *v)
{
	uint32_t i;
	unsigned reasons = 0, flags = 0, b;

	if ((unsigned)v->state >= (unsigned)NTFSMAC_PROBE_STATE_COUNT_ ||
	    (unsigned)v->logfile >= (unsigned)NTFSMAC_PROBE_LOGFILE_COUNT_ ||
	    (unsigned)v->hiberfile >= (unsigned)NTFSMAC_PROBE_HIBERFILE_COUNT_)
		FAIL("enum out of range");
	if (v->has_ntfs_version) {
		if (v->ntfs_version_major < 1 || v->ntfs_version_major > 3 || v->ntfs_version_minor > 9)
			FAIL("ntfs_version");
	} else if (v->ntfs_version_major != 0 || v->ntfs_version_minor != 0) {
		FAIL("null version with digits");
	}
	if (!text_ok(v->label, 512) || strlen(v->label) != v->label_len)
		FAIL("label");
	if (v->total_sectors < 1 || v->total_sectors > UINT64_C(0x7FFFFFFFFFFFFFFF))
		FAIL("total_sectors");
	if (!pow2(v->cluster_size) || v->cluster_size < 512 || v->cluster_size > 2097152)
		FAIL("cluster_size");
	if (v->sector_size != 512 && v->sector_size != 1024 && v->sector_size != 2048 &&
	    v->sector_size != 4096)
		FAIL("sector_size");
	for (b = 0; b < 32; b++) {
		if (v->volume_flags & (UINT32_C(1) << b))
			flags++;
		if (v->reason_codes & (UINT32_C(1) << b))
			reasons++;
	}
	if (flags > 9 || (v->volume_flags >> 9) != 0 || reasons > 18 || (v->reason_codes >> 18) != 0)
		FAIL("flag or reason set");
	if ((v->state == NTFSMAC_PROBE_STATE_CLEAN) != (reasons == 0))
		FAIL("state clean iff no reason");
	if (v->has_logfile_lsn == (v->logfile == NTFSMAC_PROBE_LOGFILE_UNREADABLE))
		FAIL("lsn null iff unreadable");
	if (v->nonnfc_path_count > 16 || v->nonnfc_path_count != (v->nonnfc_count < 16 ? v->nonnfc_count : 16))
		FAIL("path count");
	if ((v->nonnfc_count > 0) !=
	    ((v->reason_codes & NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_NAMES_NOT_NFC)) != 0))
		FAIL("count iff NAMES_NOT_NFC");
	for (i = 0; i < v->nonnfc_path_count; i++)
		if (!text_ok(v->nonnfc_paths[i], 512))
			FAIL("path text");
}

static bool same(const ntfsmac_probe_verdict *a, const ntfsmac_probe_verdict *b)
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

static bool is_unknown(const ntfsmac_probe_verdict *v)
{
	ntfsmac_probe_verdict u;

	ntfsmac_probe_verdict_unknown(&u);
	return memcmp(&u, v, sizeof(u)) == 0 && v->state == NTFSMAC_PROBE_STATE_UNKNOWN;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static ntfsmac_probe_verdict v, w;
	static char json[256 * 1024];
	static char disp[NTFSMAC_PATH_DISPLAY_SIZE];
	char leaf[NTFSMAC_LABEL_PART_SIZE];
	ntfsmac_probe_status st;
	size_t n, k;
	uint32_t i;

	memset(&v, 0x5A, sizeof(v));
	st = ntfsmac_probe_parse((const char *)data, size, &v);
	if (st != NTFSMAC_PROBE_OK) {
		if (!is_unknown(&v))
			FAIL("error without the fail-closed verdict");
		if (size > NTFSMAC_PROBE_MAX_JSON_BYTES && st != NTFSMAC_PROBE_ERR_TOO_LARGE)
			FAIL("oversize input not reported as such");
		return 0;
	}
	if (size > NTFSMAC_PROBE_MAX_JSON_BYTES)
		FAIL("input over 64 KiB accepted");
	if (ntfsmac_probe_verdict_validate(&v) != NTFSMAC_PROBE_OK)
		FAIL("accepted verdict fails validate");
	check_grammar(&v);

	/* re-serialise and parse again */
	n = ntfsmac_probe_json(&v, json, sizeof(json));
	if (n == 0 || n > NTFSMAC_PROBE_MAX_JSON_BYTES)
		FAIL("writer output missing or over 64 KiB");
	if (ntfsmac_probe_parse(json, n, &w) != NTFSMAC_PROBE_OK || !same(&v, &w))
		FAIL("round trip differs");

	/* consumers */
	{
		char name[17];
		int len = snprintf(name, sizeof(name), "%016llX", (unsigned long long)v.serial);

		if (len != 16)
			FAIL("serial formatting");
		for (k = 0; k < 16; k++)
			if (!((name[k] >= '0' && name[k] <= '9') || (name[k] >= 'A' && name[k] <= 'F')))
				FAIL("serial digits");
	}
	n = ntfsmac_label_sanitize(v.label, v.label_len, v.serial, leaf, sizeof(leaf));
	if (n == 0)
		FAIL("label sanitiser failed");
	if (oracle_label_violation(leaf, n, 64) != NULL)
		FAIL("label leaf violates ADR-016");
	for (i = 0; i < v.nonnfc_path_count; i++) {
		size_t dn = ntfsmac_path_display(v.nonnfc_paths[i], strlen(v.nonnfc_paths[i]), disp, sizeof(disp));

		if (dn != strlen(disp) || dn >= sizeof(disp))
			FAIL("path display length");
		for (k = 0; k < dn; k++) {
			unsigned char c = (unsigned char)disp[k];

			if (c < 0x20 || c == 0x7F)
				FAIL("control byte in path display");
		}
	}
	return 0;
}
