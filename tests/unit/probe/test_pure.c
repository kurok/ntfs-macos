/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The pure parts of the probe, on every host: UTF-8 text (src/probe/text.c), the state derived
 * from reason codes (state.c), the NFC fast paths (nfc.c) and the JSON writer (json.c).
 */
#include "../../../src/probe/posix.h"

#include <stdio.h>
#include <string.h>

#include "nfc.h"
#include "probe_core.h"
#include "state.h"
#include "text.h"

static int failures;

#define CHECK(cond)                                                              \
	do {                                                                     \
		if (!(cond)) {                                                   \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                              \
		}                                                                \
	} while (0)

static void test_text(void)
{
	char out[64];
	const uint16_t ascii[] = { 'a', 'b', 'c' };
	const uint16_t latin[] = { 0x00E9, 0x20AC };               /* e acute, euro sign */
	const uint16_t pair[] = { 0xD83D, 0xDE00 };                /* U+1F600 */
	const uint16_t lone_hi[] = { 'x', 0xD83D, 'y' };
	const uint16_t lone_lo[] = { 0xDE00 };
	const uint16_t with_nul[] = { 'a', 0, 'b' };
	const uint16_t reversed[] = { 0xDE00, 0xD83D };
	size_t n;

	n = probe_utf16_to_utf8(ascii, 3, out, sizeof(out));
	CHECK(n == 3 && strcmp(out, "abc") == 0);
	n = probe_utf16_to_utf8(latin, 2, out, sizeof(out));
	CHECK(n == 5 && memcmp(out, "\xC3\xA9\xE2\x82\xAC", 6) == 0);
	n = probe_utf16_to_utf8(pair, 2, out, sizeof(out));
	CHECK(n == 4 && memcmp(out, "\xF0\x9F\x98\x80", 5) == 0);
	n = probe_utf16_to_utf8(lone_hi, 3, out, sizeof(out));
	CHECK(n == 5 && memcmp(out, "x\xEF\xBF\xBDy", 6) == 0);
	n = probe_utf16_to_utf8(lone_lo, 1, out, sizeof(out));
	CHECK(n == 3 && memcmp(out, "\xEF\xBF\xBD", 4) == 0);
	n = probe_utf16_to_utf8(with_nul, 3, out, sizeof(out));
	CHECK(n == 5 && memcmp(out, "a\xEF\xBF\xBD" "b", 6) == 0); /* no NUL inside */
	n = probe_utf16_to_utf8(reversed, 2, out, sizeof(out));
	CHECK(n == 6);
	/* a cut never falls inside a code point */
	n = probe_utf16_to_utf8(latin, 2, out, 4); /* room for 3 bytes: e acute (2), euro (3) does not fit */
	CHECK(n == 2 && memcmp(out, "\xC3\xA9", 3) == 0);
	n = probe_utf16_to_utf8(pair, 2, out, 4); /* 3 bytes of room, the code point needs 4 */
	CHECK(n == 0 && out[0] == '\0');
	n = probe_utf16_to_utf8(ascii, 3, out, 1);
	CHECK(n == 0 && out[0] == '\0');
	CHECK(probe_utf16_to_utf8(ascii, 3, out, 0) == 0);

	/* appending UTF-8 */
	out[0] = '\0';
	n = probe_utf8_append(out, 0, 8, "/ab", 3);
	CHECK(n == 3 && strcmp(out, "/ab") == 0);
	n = probe_utf8_append(out, n, 8, "\xC3\xA9\xC3\xA9", 4); /* 5 bytes of room: two characters need 4 */
	CHECK(n == 7 && strcmp(out, "/ab\xC3\xA9\xC3\xA9") == 0);
	n = probe_utf8_append(out, n, 8, "z", 1);
	CHECK(n == 7);
	out[0] = '\0';
	n = probe_utf8_append(out, 0, 5, "ab\xE2\x82\xAC", 5); /* euro needs 3, room for 2 after "ab" */
	CHECK(n == 2 && strcmp(out, "ab") == 0);
	n = probe_utf8_append(out, 0, 6, "ab\xE2\x82\xAC", 5); /* 5 bytes of room: fits exactly */
	CHECK(n == 5);
}

static void test_state(void)
{
	/* The documented order, from most to least severe. */
	static const ntfsmac_probe_state order[] = {
		NTFSMAC_PROBE_STATE_UNSUPPORTED, NTFSMAC_PROBE_STATE_CORRUPT, NTFSMAC_PROBE_STATE_MIRROR_SPLIT,
		NTFSMAC_PROBE_STATE_HIBERNATED, NTFSMAC_PROBE_STATE_FAST_STARTUP, NTFSMAC_PROBE_STATE_UNCLEAN_LOG,
		NTFSMAC_PROBE_STATE_DIRTY_FLAG, NTFSMAC_PROBE_STATE_NAMES_NOT_NFC, NTFSMAC_PROBE_STATE_UNKNOWN,
		NTFSMAC_PROBE_STATE_CLEAN,
	};
	unsigned i;
	uint32_t set;

	CHECK(sizeof(order) / sizeof(order[0]) == (size_t)NTFSMAC_PROBE_STATE_COUNT_);
	for (i = 0; i + 1 < sizeof(order) / sizeof(order[0]); i++)
		CHECK(probe_state_rank(order[i]) == probe_state_rank(order[i + 1]) + 1);
	CHECK(probe_state_rank(NTFSMAC_PROBE_STATE_CLEAN) == 0);
	CHECK(probe_state_rank(NTFSMAC_PROBE_STATE_COUNT_) == -1);
	CHECK(probe_state_rank((ntfsmac_probe_state)-1) == -1);

	/* Every reason code stands for one state, never clean. */
	for (i = 0; i < (unsigned)NTFSMAC_PROBE_REASON_COUNT_; i++)
		CHECK(probe_state_of_reason((ntfsmac_probe_reason)i) != NTFSMAC_PROBE_STATE_CLEAN);
	CHECK(probe_state_of_reasons(0) == NTFSMAC_PROBE_STATE_CLEAN);

	/* Exhaustive over all 2^18 sets: the most severe state of the members, clean iff empty. */
	for (set = 0; set < (UINT32_C(1) << NTFSMAC_PROBE_REASON_COUNT_); set++) {
		int best = 0;
		ntfsmac_probe_state st = probe_state_of_reasons(set);

		for (i = 0; i < (unsigned)NTFSMAC_PROBE_REASON_COUNT_; i++) {
			int r;

			if (!(set & (UINT32_C(1) << i)))
				continue;
			r = probe_state_rank(probe_state_of_reason((ntfsmac_probe_reason)i));
			if (r > best)
				best = r;
		}
		if (probe_state_rank(st) != best || ((st == NTFSMAC_PROBE_STATE_CLEAN) != (set == 0))) {
			CHECK(0 && "state is the most severe member");
			break;
		}
	}
	/* NAMES_NOT_NFC beside each other code: only unknown and clean rank below it. */
	CHECK(probe_state_of_reasons(NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_NAMES_NOT_NFC)) ==
	      NTFSMAC_PROBE_STATE_NAMES_NOT_NFC);
	CHECK(probe_state_of_reasons(NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_NAMES_NOT_NFC) |
				     NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_CHECK_FAILED)) ==
	      NTFSMAC_PROBE_STATE_NAMES_NOT_NFC);
	CHECK(probe_state_of_reasons(NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_NAMES_NOT_NFC) |
				     NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_FLAG_DIRTY)) ==
	      NTFSMAC_PROBE_STATE_DIRTY_FLAG);
	CHECK(probe_state_of_reasons(NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_NAMES_NOT_NFC) |
				     NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_MFTMIRR_STALE)) ==
	      NTFSMAC_PROBE_STATE_MIRROR_SPLIT);
	/* mirror_split ranks above the findings of checks 3 to 5 and below corrupt. */
	CHECK(probe_state_of_reasons(NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_MFTMIRR_STALE) |
				     NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_HIBER_SIGNATURE)) ==
	      NTFSMAC_PROBE_STATE_MIRROR_SPLIT);
	CHECK(probe_state_of_reasons(NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_MFTMIRR_STALE) |
				     NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_MOUNT_FAILED)) ==
	      NTFSMAC_PROBE_STATE_CORRUPT);
}

static void test_nfc_basics(void)
{
	const uint16_t plain[] = { 'a', 'b' };
	const uint16_t composed[] = { 0x00E9 };
	const uint16_t decomposed[] = { 'e', 0x0301 };
	const uint16_t hangul_nfc[] = { 0xD55C, 0xAE00 };
	const uint16_t hangul_jamo[] = { 0x1112, 0x1161, 0x11AB };
	const uint16_t lone[] = { 0xD800, 0x0301 };
	uint32_t cp[NTFSMAC_NFC_MAX_CODEPOINTS];
	size_t n;

	CHECK(ntfsmac_nfc_is_normalized(plain, 2) == 1);
	CHECK(ntfsmac_nfc_is_normalized(composed, 1) == 1);
	CHECK(ntfsmac_nfc_is_normalized(decomposed, 2) == 0);
	CHECK(ntfsmac_nfc_is_normalized(hangul_nfc, 2) == 1);
	CHECK(ntfsmac_nfc_is_normalized(hangul_jamo, 3) == 0);
	CHECK(ntfsmac_nfc_is_normalized(lone, 2) == 1); /* a lone surrogate composes with nothing */
	CHECK(ntfsmac_nfc_is_normalized(plain, 0) == 1);
	n = ntfsmac_nfc_normalize(decomposed, 2, cp, NTFSMAC_NFC_MAX_CODEPOINTS);
	CHECK(n == 1 && cp[0] == 0xE9);
	n = ntfsmac_nfc_normalize(hangul_jamo, 3, cp, NTFSMAC_NFC_MAX_CODEPOINTS);
	CHECK(n == 1 && cp[0] == 0xD55C);
	CHECK(ntfsmac_nfc_normalize(plain, NTFSMAC_NFC_MAX_UNITS + 1, cp, NTFSMAC_NFC_MAX_CODEPOINTS) == (size_t)-1);
	CHECK(ntfsmac_nfc_normalize(plain, 2, cp, 4) == (size_t)-1);
	CHECK(ntfsmac_nfc_is_normalized(plain, NTFSMAC_NFC_MAX_UNITS + 1) == -1);
}

static ntfsmac_probe_verdict base_verdict(void)
{
	ntfsmac_probe_verdict v;

	memset(&v, 0, sizeof(v));
	v.is_ntfs = true;
	v.has_ntfs_version = true;
	v.ntfs_version_major = 3;
	v.ntfs_version_minor = 1;
	strcpy(v.label, "DATA");
	v.label_len = 4;
	v.serial = UINT64_C(0x1A2B3C4D5E6F7788);
	v.total_sectors = 131071;
	v.cluster_size = 4096;
	v.sector_size = 512;
	v.logfile = NTFSMAC_PROBE_LOGFILE_CLEAN;
	v.has_logfile_lsn = true;
	v.logfile_lsn = 0x1A2B3C4;
	v.hiberfile = NTFSMAC_PROBE_HIBERFILE_ABSENT;
	return v;
}

static void test_json(void)
{
	static char buf[NTFSMAC_PROBE_MAX_JSON_BYTES];
	ntfsmac_probe_verdict v = base_verdict();
	size_t n, i;

	/* The example of ARCHITECTURE.md section 3.1. */
	v.volume_flags = NTFSMAC_PROBE_VOLFLAG_BIT(NTFSMAC_PROBE_VOLFLAG_DIRTY);
	v.reason_codes = NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_FLAG_DIRTY);
	v.state = NTFSMAC_PROBE_STATE_DIRTY_FLAG;
	n = ntfsmac_probe_json(&v, buf, sizeof(buf));
	CHECK(n > 0 && n == strlen(buf));
	CHECK(strstr(buf, "\"schema\": 1,") != NULL);
	CHECK(strstr(buf, "\"serial\": \"0x1A2B3C4D5E6F7788\",") != NULL);
	CHECK(strstr(buf, "\"logfile_lsn\": \"0x0000000001A2B3C4\",") != NULL);
	CHECK(strstr(buf, "\"volume_flags\": [\"DIRTY\"],") != NULL);
	CHECK(strstr(buf, "\"reason_codes\": [\"FLAG_DIRTY\"],") != NULL);
	CHECK(strstr(buf, "\"nonnfc_count\": 0,") != NULL);
	CHECK(strstr(buf, "\"nonnfc_paths\": []\n}\n") != NULL);
	CHECK(buf[n - 1] == '\n');

	/* null version and null LSN */
	v.has_ntfs_version = false;
	v.logfile = NTFSMAC_PROBE_LOGFILE_UNREADABLE;
	v.has_logfile_lsn = false;
	n = ntfsmac_probe_json(&v, buf, sizeof(buf));
	CHECK(n > 0 && strstr(buf, "\"ntfs_version\": null,") && strstr(buf, "\"logfile_lsn\": null,"));

	/* a cap too small is refused, not truncated */
	CHECK(ntfsmac_probe_json(&v, buf, 40) == 0);
	CHECK(ntfsmac_probe_json(&v, buf, 0) == 0);

	/* worst case: all flags and codes, a 512-byte label and 16 paths of 512 control characters */
	v = base_verdict();
	v.volume_flags = (UINT32_C(1) << NTFSMAC_PROBE_VOLFLAG_COUNT_) - 1;
	v.reason_codes = (UINT32_C(1) << NTFSMAC_PROBE_REASON_COUNT_) - 1;
	v.state = NTFSMAC_PROBE_STATE_UNSUPPORTED;
	memset(v.label, 0x01, NTFSMAC_PROBE_LABEL_MAX_BYTES);
	v.label[NTFSMAC_PROBE_LABEL_MAX_BYTES] = '\0';
	v.label_len = NTFSMAC_PROBE_LABEL_MAX_BYTES;
	v.nonnfc_count = UINT32_MAX;
	v.nonnfc_path_count = 16;
	for (i = 0; i < 16; i++) {
		memset(v.nonnfc_paths[i], 0x1F, NTFSMAC_PROBE_NONNFC_PATH_MAX_BYTES);
		v.nonnfc_paths[i][0] = '/';
		v.nonnfc_paths[i][NTFSMAC_PROBE_NONNFC_PATH_MAX_BYTES] = '\0';
	}
	n = ntfsmac_probe_json(&v, buf, sizeof(buf));
	CHECK(n > 0 && n < NTFSMAC_PROBE_MAX_JSON_BYTES);
	CHECK(strstr(buf, "\\u0000") == NULL);
	CHECK(strstr(buf, "\"nonnfc_count\": 4294967295,") != NULL);
	fprintf(stderr, "worst case verdict: %zu bytes (limit %u)\n", n, NTFSMAC_PROBE_MAX_JSON_BYTES);

	/* escapes */
	v = base_verdict();
	v.state = NTFSMAC_PROBE_STATE_CLEAN;
	strcpy(v.label, "q\"u\\o\ttes\n\x7f");
	n = ntfsmac_probe_json(&v, buf, sizeof(buf));
	CHECK(n > 0 && strstr(buf, "\"label\": \"q\\\"u\\\\o\\u0009tes\\u000A\x7f\",") != NULL);

	/* an out-of-range enum is refused */
	v = base_verdict();
	v.state = NTFSMAC_PROBE_STATE_COUNT_;
	CHECK(ntfsmac_probe_json(&v, buf, sizeof(buf)) == 0);
}

int main(void)
{
	test_text();
	test_state();
	test_nfc_basics();
	test_json();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	puts("probe pure: all checks passed");
	return 0;
}
