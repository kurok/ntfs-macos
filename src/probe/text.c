/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "posix.h"
#include "text.h"

#include <string.h>

/* Encode one code point; returns the byte count (1 to 4). */
static size_t encode(uint32_t c, char *o)
{
	if (c < 0x80) {
		o[0] = (char)c;
		return 1;
	}
	if (c < 0x800) {
		o[0] = (char)(0xC0 | (c >> 6));
		o[1] = (char)(0x80 | (c & 0x3F));
		return 2;
	}
	if (c < 0x10000) {
		o[0] = (char)(0xE0 | (c >> 12));
		o[1] = (char)(0x80 | ((c >> 6) & 0x3F));
		o[2] = (char)(0x80 | (c & 0x3F));
		return 3;
	}
	o[0] = (char)(0xF0 | (c >> 18));
	o[1] = (char)(0x80 | ((c >> 12) & 0x3F));
	o[2] = (char)(0x80 | ((c >> 6) & 0x3F));
	o[3] = (char)(0x80 | (c & 0x3F));
	return 4;
}

size_t probe_utf16_to_utf8(const uint16_t *u, size_t n, char *out, size_t cap)
{
	size_t i = 0, len = 0;

	if (cap == 0)
		return 0;
	while (i < n) {
		uint32_t c = u[i];
		char tmp[4];
		size_t k;

		if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF) {
			c = 0x10000 + ((c - 0xD800) << 10) + (u[i + 1] - 0xDC00);
			i += 2;
		} else {
			i++;
			if (c == 0 || (c >= 0xD800 && c <= 0xDFFF))
				c = 0xFFFD;
		}
		k = encode(c, tmp);
		if (len + k > cap - 1)
			break;
		memcpy(out + len, tmp, k);
		len += k;
	}
	out[len] = '\0';
	return len;
}

size_t probe_utf8_append(char *out, size_t len, size_t cap, const char *s, size_t n)
{
	size_t take = n;

	if (cap == 0 || len >= cap)
		return len;
	if (take > cap - 1 - len)
		take = cap - 1 - len;
	/* Do not cut inside a code point: back up over continuation bytes and the lead byte. */
	if (take < n) {
		while (take > 0 && ((unsigned char)s[take] & 0xC0) == 0x80)
			take--;
	}
	memcpy(out + len, s, take);
	len += take;
	out[len] = '\0';
	return len;
}
