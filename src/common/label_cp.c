/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The label allow-list (ADR-016 decision 2) and strict UTF-8 decoding; see
 * include/ntfsmac/label_cp.h. Allow-list only: a code point is allowed because the rules
 * name it, never because nothing forbids it.
 */
#include "ntfsmac/label_cp.h"

#include "label_cp_data.h"

size_t ntfsmac_utf8_decode(const unsigned char *s, size_t avail, uint32_t *cp)
{
	unsigned char c;

	if (avail == 0)
		return 0;
	c = s[0];
	if (c < 0x80) {
		*cp = c;
		return 1;
	}
	if (c >= 0xC2 && c <= 0xDF) {
		if (avail < 2 || (s[1] & 0xC0) != 0x80)
			return 0;
		*cp = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
		return 2;
	}
	if (c >= 0xE0 && c <= 0xEF) {
		uint32_t v;

		if (avail < 3 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80)
			return 0;
		v = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) |
		    (uint32_t)(s[2] & 0x3F);
		if (v < 0x800 || (v >= 0xD800 && v <= 0xDFFF))
			return 0; /* overlong, surrogate */
		*cp = v;
		return 3;
	}
	if (c >= 0xF0 && c <= 0xF4) {
		uint32_t v;

		if (avail < 4 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 ||
		    (s[3] & 0xC0) != 0x80)
			return 0;
		v = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
		    ((uint32_t)(s[2] & 0x3F) << 6) | (uint32_t)(s[3] & 0x3F);
		if (v < 0x10000 || v > 0x10FFFF)
			return 0; /* overlong, above U+10FFFF */
		*cp = v;
		return 4;
	}
	return 0; /* continuation byte, C0, C1, F5..FF */
}

bool ntfsmac_utf8_valid(const unsigned char *s, size_t len)
{
	size_t i = 0;

	while (i < len) {
		uint32_t cp;
		size_t n = ntfsmac_utf8_decode(s + i, len - i, &cp);

		if (n == 0)
			return false;
		i += n;
	}
	return true;
}

/* Is cp inside one of the n sorted, disjoint ranges? */
static bool in_ranges(const uint32_t (*r)[2], size_t n, uint32_t cp)
{
	size_t lo = 0, hi = n;

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;

		if (cp < r[mid][0])
			hi = mid;
		else if (cp > r[mid][1])
			lo = mid + 1;
		else
			return true;
	}
	return false;
}

bool ntfsmac_label_cp_allowed(uint32_t cp)
{
	if (cp < 0x80) {
		return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
		       (cp >= '0' && cp <= '9') || cp == ' ' || cp == '-' || cp == '_' ||
		       cp == '.' || cp == '(' || cp == ')' || cp == '+';
	}
	if (cp > 0x10FFFF)
		return false;
	return !in_ranges(label_cp_denied, sizeof(label_cp_denied) / sizeof(label_cp_denied[0]), cp);
}

bool ntfsmac_label_cp_is_mark(uint32_t cp)
{
	return cp >= 0x80 && cp <= 0x10FFFF &&
	       in_ranges(label_cp_marks, sizeof(label_cp_marks) / sizeof(label_cp_marks[0]), cp);
}

bool ntfsmac_label_part_ok(const char *s, size_t len)
{
	const unsigned char *u = (const unsigned char *)s;
	size_t i = 0;

	if (s == NULL || len == 0 || len > NTFSMAC_LABEL_PART_MAX_BYTES)
		return false;
	if (s[0] == ' ' || s[0] == '.' || s[len - 1] == ' ' || s[len - 1] == '.')
		return false;
	while (i < len) {
		uint32_t cp;
		size_t n = ntfsmac_utf8_decode(u + i, len - i, &cp);

		if (n == 0 || !ntfsmac_label_cp_allowed(cp))
			return false;
		if (i == 0 && ntfsmac_label_cp_is_mark(cp))
			return false;
		i += n;
	}
	return true;
}
