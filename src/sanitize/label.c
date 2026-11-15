/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Label sanitiser (ADR-016 decision 2). The allow-list and the strict UTF-8 decoder are the
 * shared unit src/common/label_cp.c, the same one that the final argv validator uses for its
 * rule 2. A byte reaches the output because the rules name it, never because nothing forbids it.
 */
#include "sanitize.h"

#include <string.h>

static size_t fallback(char *out, uint64_t serial)
{
	static const char hex[] = "0123456789ABCDEF";
	int i;

	memcpy(out, "NTFS-", 5);
	for (i = 0; i < 8; i++)
		out[5 + i] = hex[(serial >> (28 - 4 * i)) & 0xF];
	out[13] = '\0';
	return 13;
}

size_t ntfsmac_label_sanitize(const char *label, size_t len, uint64_t serial, char *out,
			      size_t cap)
{
	unsigned char buf[NTFSMAC_LABEL_PART_MAX_BYTES];
	bool kept[NTFSMAC_LABEL_PART_MAX_BYTES]; /* byte comes from an allowed original code point */
	const unsigned char *in = (const unsigned char *)label;
	size_t n = 0, i = 0, start, end, k;

	if (out == NULL || cap == 0)
		return 0;
	out[0] = '\0';
	if (cap < NTFSMAC_LABEL_PART_SIZE || (label == NULL && len != 0))
		return 0;

	/* Decoding: any invalid byte replaces the whole label. */
	if (!ntfsmac_utf8_valid(in, len))
		return fallback(out, serial);

	/* Allow-list, then the cut at 64 bytes on a code point boundary. */
	while (i < len) {
		uint32_t cp = 0;
		size_t clen = ntfsmac_utf8_decode(in + i, len - i, &cp);
		bool ok;

		if (clen == 0)
			return fallback(out, serial); /* cannot happen after the check above */
		ok = ntfsmac_label_cp_allowed(cp);
		if (n + (ok ? clen : 1u) > sizeof(buf))
			break;
		if (ok) {
			for (k = 0; k < clen; k++)
				kept[n + k] = true;
			memcpy(buf + n, in + i, clen);
			n += clen;
		} else {
			kept[n] = false;
			buf[n++] = '_';
		}
		i += clen;
	}

	/* Path component: leading spaces, then a leading dot or combining mark becomes `_`. */
	start = 0;
	while (start < n && buf[start] == ' ')
		start++;
	if (start < n) {
		uint32_t cp = 0;
		size_t clen = ntfsmac_utf8_decode(buf + start, n - start, &cp);

		if (buf[start] == '.') {
			buf[start] = '_';
			kept[start] = false;
		} else if (clen > 0 && ntfsmac_label_cp_is_mark(cp)) {
			buf[start] = '_';
			kept[start] = false;
			memmove(buf + start + 1, buf + start + clen, n - start - clen);
			memmove(kept + start + 1, kept + start + clen, (n - start - clen) * sizeof(kept[0]));
			n -= clen - 1;
		}
	}
	/* Trailing spaces and dots. */
	end = n;
	while (end > start && (buf[end - 1] == ' ' || buf[end - 1] == '.'))
		end--;

	/* Nothing left, or nothing but replacements: no part of the label survived. */
	for (k = start; k < end; k++)
		if (kept[k])
			break;
	if (k == end)
		return fallback(out, serial);

	memcpy(out, buf + start, end - start);
	out[end - start] = '\0';
	return end - start;
}
