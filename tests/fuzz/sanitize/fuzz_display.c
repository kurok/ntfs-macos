/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * F-SANITIZE-03 (added by T-M3-19, not in TEST_STRATEGY.md section 4): libFuzzer on the path
 * display sanitiser of ADR-023. Input: 2 bytes (output capacity), then the path bytes. Oracles:
 *   - no crash, no sanitizer report, nothing written beyond the capacity;
 *   - the output is NUL terminated and its length equals what the call returned, unless the
 *     capacity was too small (then it is shorter, cut at an element boundary);
 *   - with enough room the output is lossless (undoing the escapes gives the input), every escape
 *     stands for something the allow-list refuses, and every other output byte belongs to an
 *     allowed code point or is `/`; so no control byte, bidirectional control or ESC survives.
 */
#include "sanitize.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../unit/sanitize/oracle.h"

#define FAIL(msg)                                                                 \
	do {                                                                      \
		fprintf(stderr, "F-SANITIZE-03 oracle failure: %s (line %d)\n", msg, __LINE__); \
		abort();                                                          \
	} while (0)

/* Undo the escapes; false if the text is not a well-formed display text. */
static bool unescape(const char *out, size_t len, const unsigned char *in, size_t inlen,
		     unsigned char *back, size_t *backlen)
{
	size_t i = 0, n = 0;

	while (i < len) {
		unsigned char c = (unsigned char)out[i];

		if (c == '\\') {
			char kind;
			uint32_t v = 0;
			size_t digits = 0;

			if (i + 2 >= len || (out[i + 1] != 'u' && out[i + 1] != 'x') || out[i + 2] != '{')
				return false;
			kind = out[i + 1];
			i += 3;
			while (i < len && out[i] != '}') {
				char d = out[i];

				if (d >= '0' && d <= '9')
					v = v * 16 + (uint32_t)(d - '0');
				else if (d >= 'A' && d <= 'F')
					v = v * 16 + (uint32_t)(d - 'A' + 10);
				else
					return false;
				if (++digits > 6)
					return false;
				i++;
			}
			if (i >= len || digits == 0)
				return false;
			i++;
			if (kind == 'x') {
				uint32_t other;

				/* `\x{..}` stands for a byte that really is invalid UTF-8 at this position */
				if (n >= inlen || oracle_decode(in + n, inlen - n, &other) != 0)
					return false;
				if (digits != 2)
					return false;
				back[n++] = (unsigned char)v;
			} else {
				if (digits < 4 || (digits > 4 && (v >> (4 * (digits - 1))) == 0) ||
				    oracle_cp_allowed(v) || v == '/' || (v >= 0xD800 && v <= 0xDFFF) || v > 0x10FFFF)
					return false;
				n += oracle_encode(v, back + n);
			}
		} else {
			uint32_t cp;
			int k = oracle_decode((const unsigned char *)out + i, len - i, &cp);

			if (k == 0 || (cp != '/' && !oracle_cp_allowed(cp)))
				return false;
			memcpy(back + n, out + i, (size_t)k);
			n += (size_t)k;
			i += (size_t)k;
		}
	}
	*backlen = n;
	return true;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static char big[NTFSMAC_PATH_DISPLAY_SIZE * 2];
	static unsigned char back[NTFSMAC_PATH_DISPLAY_SIZE * 2];
	size_t cap, len, need, backlen = 0, i;
	const char *path;

	if (size < 2)
		return 0;
	cap = ((size_t)data[0] << 8 | data[1]) % (NTFSMAC_PATH_DISPLAY_SIZE + 64);
	path = (const char *)data + 2;
	len = size - 2;
	if (len > 1024)
		len = 1024; /* the verdict allows 512; longer inputs only test the cut */

	/* small capacity: guard bytes behind the buffer stay intact */
	memset(big, 0x7E, sizeof(big));
	need = ntfsmac_path_display(path, len, big, cap);
	if (cap > 0) {
		if (memchr(big, '\0', cap) == NULL)
			FAIL("not NUL terminated");
		if (strlen(big) > need || strlen(big) >= cap)
			FAIL("output longer than reported or than the capacity");
		if (need < cap && strlen(big) != need)
			FAIL("output shorter though it fits");
	}
	for (i = cap; i < sizeof(big); i++)
		if (big[i] != 0x7E)
			FAIL("wrote behind the capacity");

	/* full room: lossless and allow-listed */
	need = ntfsmac_path_display(path, len, big, sizeof(big));
	if (need != strlen(big) || need >= sizeof(big))
		FAIL("length with room");
	if (len <= 512 && need >= NTFSMAC_PATH_DISPLAY_SIZE)
		FAIL("NTFSMAC_PATH_DISPLAY_SIZE too small for a 512 byte path");
	if (!unescape(big, need, (const unsigned char *)path, len, back, &backlen) || backlen != len || memcmp(back, path, len) != 0)
		FAIL("display is not lossless or not allow-listed");
	return 0;
}
