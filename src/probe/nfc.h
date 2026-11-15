/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_PROBE_NFC_H
#define NTFSMAC_PROBE_NFC_H

/*
 * Unicode NFC for the non-NFC name scan (docs/ARCHITECTURE.md section 3.1
 * check 5a, ADR-023). Pure C, no allocation, no I/O. The tables come from
 * nfc_data.h (src/probe/gen_nfc.py).
 *
 * Input is a UTF-16 string as NTFS stores a name: any 16-bit unit sequence,
 * lone surrogates included. A lone surrogate is an ordinary code point with no
 * decomposition and combining class 0, so it never makes a name non-NFC by
 * itself.
 */

#include <stddef.h>
#include <stdint.h>

/* A name has at most 255 UTF-16 units (NTFS FILE_NAME length is one byte). */
#define NTFSMAC_NFC_MAX_UNITS 255u

/* Largest full canonical decomposition of one code point is 18; room to spare. */
#define NTFSMAC_NFC_MAX_CODEPOINTS (NTFSMAC_NFC_MAX_UNITS * 20u)

/*
 * NFC of u[0..n) as code points. Writes at most cap code points to out (cap
 * must be at least NTFSMAC_NFC_MAX_CODEPOINTS for n up to the maximum) and
 * returns the number written, or (size_t)-1 if n or cap is too large.
 */
size_t ntfsmac_nfc_normalize(const uint16_t *u, size_t n, uint32_t *out, size_t cap);

/*
 * 1 if u[0..n) equals its NFC form, 0 if not, -1 if n exceeds
 * NTFSMAC_NFC_MAX_UNITS. Strings of units below U+0300 (no combining mark, no
 * code point that NFC changes) are answered without the tables. A string with a
 * code point that is unassigned in the tables' Unicode version (private use is
 * assigned; a lone surrogate is neutral) is answered 0: the tables cannot say how a
 * newer Unicode version normalises it, so it is not trusted (fail closed).
 */
int ntfsmac_nfc_is_normalized(const uint16_t *u, size_t n);

/* Unicode version of the tables, for the test log. */
const char *ntfsmac_nfc_unicode_version(void);

#endif /* NTFSMAC_PROBE_NFC_H */
