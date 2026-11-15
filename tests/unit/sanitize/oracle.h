/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_SANITIZE_ORACLE_H
#define NTFSMAC_SANITIZE_ORACLE_H

/*
 * Independent statement of the ADR-016 rules for the tests and the fuzz harnesses. It shares
 * no code with src/sanitize/ or src/common/label_cp.c. The allow-list is written here as rules
 * over general categories (the run table oracle_data.h, made by tests/unit/sanitize/gen_oracle.py)
 * and over the lists below; the production table is made by different code as denied ranges
 * (src/common/gen_label_cp.py). The exhaustive test compares the two for every code point.
 * Used by test_label.c, test_display.c and tests/fuzz/sanitize/.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "oracle_data.h"

/* Default_Ignorable_Code_Point of Unicode 15.1.0, written out again (not shared with the generator). */
static const uint32_t oracle_di[][2] = {
	{ 0x00AD, 0x00AD }, { 0x034F, 0x034F }, { 0x061C, 0x061C }, { 0x115F, 0x1160 },
	{ 0x17B4, 0x17B5 }, { 0x180B, 0x180F }, { 0x200B, 0x200F }, { 0x202A, 0x202E },
	{ 0x2060, 0x206F }, { 0x3164, 0x3164 }, { 0xFE00, 0xFE0F }, { 0xFEFF, 0xFEFF },
	{ 0xFFA0, 0xFFA0 }, { 0xFFF0, 0xFFF8 }, { 0x1BCA0, 0x1BCA3 }, { 0x1D173, 0x1D17A },
	{ 0xE0000, 0xE0FFF },
};

/* General category of cp >= 0x80 from the run table. */
static inline const char *oracle_cat(uint32_t cp)
{
	size_t lo = 0, hi = sizeof(oracle_gc) / sizeof(oracle_gc[0]);

	while (hi - lo > 1) {
		size_t mid = (lo + hi) / 2;

		if (oracle_gc[mid].first <= cp)
			lo = mid;
		else
			hi = mid;
	}
	return oracle_gc[lo].cat;
}

static inline bool oracle_cat_in(const char *cat, const char *list)
{
	/* list: two-letter categories back to back */
	size_t i;

	for (i = 0; list[i] != '\0'; i += 2)
		if (cat[0] == list[i] && cat[1] == list[i + 1])
			return true;
	return false;
}

static inline bool oracle_cp_allowed(uint32_t cp)
{
	size_t i;

	if (cp < 0x80) {
		const char *ok = "-_.()+ ";

		return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
		       (cp >= '0' && cp <= '9') || (cp != 0 && strchr(ok, (int)cp) != NULL);
	}
	if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
		return false;
	if (oracle_cat_in(oracle_cat(cp), "CnCcCfCsCoZsZlZp"))
		return false; /* unassigned (noncharacters too), controls, format, surrogate, private use, spaces */
	for (i = 0; i < sizeof(oracle_di) / sizeof(oracle_di[0]); i++)
		if (cp >= oracle_di[i][0] && cp <= oracle_di[i][1])
			return false;
	if (cp == 0x2044 || cp == 0x2215 || cp == 0x29F8 || cp == 0xFF0F || cp == 0xFF1A)
		return false; /* slash and colon look-alikes */
	return true;
}

/* An allowed combining mark (Mn, Mc, Me). */
static inline bool oracle_is_mark(uint32_t cp)
{
	return cp >= 0x80 && oracle_cp_allowed(cp) && oracle_cat_in(oracle_cat(cp), "MnMcMe");
}

/* Strict UTF-8 decode, written separately from src/common/label_cp.c (RFC 3629 table). */
static inline int oracle_decode(const unsigned char *s, size_t n, uint32_t *cp)
{
	if (n == 0)
		return 0;
	if (s[0] < 0x80) {
		*cp = s[0];
		return 1;
	}
	if (s[0] >= 0xC2 && s[0] <= 0xDF && n >= 2 && (s[1] & 0xC0) == 0x80) {
		*cp = (uint32_t)(s[0] & 0x1F) << 6 | (s[1] & 0x3F);
		return 2;
	}
	if (s[0] >= 0xE0 && s[0] <= 0xEF && n >= 3 && (s[1] & 0xC0) == 0x80 &&
	    (s[2] & 0xC0) == 0x80) {
		uint32_t v = (uint32_t)(s[0] & 0x0F) << 12 | (uint32_t)(s[1] & 0x3F) << 6 | (s[2] & 0x3F);

		if (v < 0x800 || (v >= 0xD800 && v <= 0xDFFF))
			return 0;
		*cp = v;
		return 3;
	}
	if (s[0] >= 0xF0 && s[0] <= 0xF4 && n >= 4 && (s[1] & 0xC0) == 0x80 &&
	    (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
		uint32_t v = (uint32_t)(s[0] & 0x07) << 18 | (uint32_t)(s[1] & 0x3F) << 12 |
			     (uint32_t)(s[2] & 0x3F) << 6 | (s[3] & 0x3F);

		if (v < 0x10000 || v > 0x10FFFF)
			return 0;
		*cp = v;
		return 4;
	}
	return 0;
}

static inline size_t oracle_encode(uint32_t cp, unsigned char *u)
{
	if (cp < 0x80) {
		u[0] = (unsigned char)cp;
		return 1;
	}
	if (cp < 0x800) {
		u[0] = (unsigned char)(0xC0 | (cp >> 6));
		u[1] = (unsigned char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		u[0] = (unsigned char)(0xE0 | (cp >> 12));
		u[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
		u[2] = (unsigned char)(0x80 | (cp & 0x3F));
		return 3;
	}
	u[0] = (unsigned char)(0xF0 | (cp >> 18));
	u[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
	u[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
	u[3] = (unsigned char)(0x80 | (cp & 0x3F));
	return 4;
}

/*
 * Do the ADR-016 properties of a label part hold for s[0..len)? Returns NULL if yes, else a
 * short reason. `max` is the length cap (64 for a part).
 */
static inline const char *oracle_label_violation(const char *s, size_t len, size_t max)
{
	size_t i = 0;

	if (len == 0)
		return "empty";
	if (len > max)
		return "too long";
	if (memchr(s, '\0', len) != NULL)
		return "NUL inside";
	if (s[0] == '.' || s[0] == ' ')
		return "leading dot or space";
	if (s[len - 1] == '.' || s[len - 1] == ' ')
		return "trailing dot or space";
	while (i < len) {
		uint32_t cp;
		int n = oracle_decode((const unsigned char *)s + i, len - i, &cp);

		if (n == 0)
			return "invalid UTF-8";
		if (!oracle_cp_allowed(cp))
			return "code point outside the allow-list";
		if (i == 0 && oracle_is_mark(cp))
			return "combining mark first";
		i += (size_t)n;
	}
	return NULL;
}

/*
 * The leaf rule of the validator (ARCHITECTURE.md section 3.3 rule 2): a label part, or a label
 * part and " 2".." 9" or " 10".." 99". Returns NULL if s[0..len) is such a leaf, else a reason.
 */
static inline const char *oracle_leaf_violation(const char *s, size_t len)
{
	const char *why = oracle_label_violation(s, len, 64);
	size_t i;

	if (why == NULL)
		return NULL;
	if (len > 67)
		return "over 67 bytes";
	for (i = 2; i <= 3; i++) {
		if (len > i && s[len - i] == ' ') {
			const char *d = s + len - i + 1;
			bool digits = i == 2 ? (d[0] >= '2' && d[0] <= '9')
					     : (d[0] >= '1' && d[0] <= '9' && d[1] >= '0' && d[1] <= '9');

			if (digits && oracle_label_violation(s, len - i, 64) == NULL)
				return NULL;
		}
	}
	return why;
}

/*
 * Reference label sanitiser: ADR-016 decision 2 written again from the text, over arrays of code
 * points (src/sanitize/label.c works on bytes). `out` holds at least 65 bytes. Returns the length.
 */
static inline size_t oracle_label_sanitize(const char *label, size_t len, uint64_t serial, char *out)
{
	uint32_t cps[64];
	bool kept[64];
	size_t i = 0, n = 0, first, last, k, pos = 0, bytes = 0;
	unsigned char enc[4];
	const unsigned char *in = (const unsigned char *)label;

	/* decoding: one bad byte replaces the whole label */
	while (i < len) {
		uint32_t cp = 0;
		int d = oracle_decode(in + i, len - i, &cp);

		if (d == 0)
			goto fallback;
		i += (size_t)d;
	}
	/* allow-list and the 64-byte cut */
	i = 0;
	while (i < len) {
		uint32_t cp = 0;
		int d = oracle_decode(in + i, len - i, &cp);
		bool ok = oracle_cp_allowed(cp);
		size_t w = ok ? oracle_encode(cp, enc) : 1;

		if (bytes + w > 64)
			break;
		cps[n] = ok ? cp : '_';
		kept[n] = ok;
		n++;
		bytes += w;
		i += (size_t)d;
	}
	/* path component */
	first = 0;
	while (first < n && cps[first] == ' ')
		first++;
	if (first < n && (cps[first] == '.' || oracle_is_mark(cps[first]))) {
		cps[first] = '_';
		kept[first] = false;
	}
	last = n;
	while (last > first && (cps[last - 1] == ' ' || cps[last - 1] == '.'))
		last--;
	for (k = first; k < last; k++)
		if (kept[k])
			break;
	if (k == last)
		goto fallback;
	for (k = first; k < last; k++)
		pos += oracle_encode(cps[k], (unsigned char *)out + pos);
	out[pos] = '\0';
	return pos;
fallback:
	{
		static const char hex[] = "0123456789ABCDEF";

		memcpy(out, "NTFS-", 5);
		for (k = 0; k < 8; k++)
			out[5 + k] = hex[(serial >> (4 * (7 - k))) & 0xF];
		out[13] = '\0';
		return 13;
	}
}

#endif /* NTFSMAC_SANITIZE_ORACLE_H */
