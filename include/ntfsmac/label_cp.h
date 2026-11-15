/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_LABEL_CP_H
#define NTFSMAC_LABEL_CP_H

/*
 * The label allow-list (ADR-016 decision 2), shared by the label sanitiser
 * (src/sanitize/) and by the final argv validator (src/options/argv_check.c, rule 2),
 * so that the rule that makes a leaf and the rule that accepts one cannot drift apart.
 * Pure C, no I/O. Implemented in src/common/label_cp.c; the non-ASCII table is generated
 * by src/common/gen_label_cp.py from Unicode 15.1.0 (the version of the probe's NFC tables).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ADR-016, "Length": the label part is at most 64 bytes of UTF-8. */
#define NTFSMAC_LABEL_PART_MAX_BYTES 64u

/*
 * Is code point `cp` allowed in a label?
 *  ASCII: `A-Z a-z 0-9`, space, `- _ . ( ) +`; every other ASCII value is not.
 *  Non-ASCII: every assigned code point (Unicode 15.1.0) except: general category Cc, Cf, Co,
 *  Cs, Zs, Zl, Zp; Default_Ignorable_Code_Points (including the Hangul fillers U+115F, U+1160,
 *  U+3164, U+FFA0); noncharacters; the slash look-alikes U+2044 U+2215 U+29F8 U+FF0F and the
 *  colon look-alike U+FF1A. A code point unassigned in 15.1.0 is not allowed (fail closed), nor
 *  is a value above U+10FFFF.
 */
bool ntfsmac_label_cp_allowed(uint32_t cp);

/* Is `cp` an allowed combining mark (Mn, Mc, Me)? Such a code point may not start a part. */
bool ntfsmac_label_cp_is_mark(uint32_t cp);

/*
 * Strict UTF-8 decoding of one code point at s[0..avail): shortest form only, no
 * surrogate, nothing above U+10FFFF. Returns the length 1..4 and stores the code
 * point, or returns 0 for invalid or truncated input (and stores nothing).
 */
size_t ntfsmac_utf8_decode(const unsigned char *s, size_t avail, uint32_t *cp);

/* Is s[0..len) strictly valid UTF-8? NUL bytes are valid here. */
bool ntfsmac_utf8_valid(const unsigned char *s, size_t len);

/*
 * Is s[0..len) a label part of ADR-016: 1 to 64 bytes, strict UTF-8, every code point allowed,
 * no leading or trailing space or dot, and no combining mark first? This is what the label
 * sanitiser produces (apart from the fallback, which also satisfies it).
 */
bool ntfsmac_label_part_ok(const char *s, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* NTFSMAC_LABEL_CP_H */
