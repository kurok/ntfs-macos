/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_PROBE_TEXT_H
#define NTFSMAC_PROBE_TEXT_H

#include <stddef.h>
#include <stdint.h>

/*
 * UTF-16 (host byte order, any unit sequence) to UTF-8 for the verdict. Always
 * valid UTF-8: a lone surrogate and U+0000 become U+FFFD, so the JSON never
 * needs `\u0000` and the strict parser (UTF-8 only) accepts it. Writes at most
 * cap - 1 bytes plus a NUL and never cuts a code point. Returns the length.
 */
size_t probe_utf16_to_utf8(const uint16_t *u, size_t n, char *out, size_t cap);

/* Append a UTF-8 string to out (cap as above, NUL kept), cut at a code point boundary. */
size_t probe_utf8_append(char *out, size_t len, size_t cap, const char *s, size_t n);

#endif /* NTFSMAC_PROBE_TEXT_H */
