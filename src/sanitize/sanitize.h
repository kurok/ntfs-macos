/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_SANITIZE_H
#define NTFSMAC_SANITIZE_H

/*
 * Disk-derived strings in the root daemon (ADR-016, ADR-023, docs/ARCHITECTURE.md
 * sections 3.1, 3.3, 3.4 and 6). The strict verdict parser, ntfsmac_probe_parse(),
 * is declared in include/ntfsmac/probe.h; this header holds the two sanitisers and
 * the helpers they share. Pure C apart from ntfsmac_leaf_create(), which calls
 * mkdirat().
 *
 * Both sanitisers are allow-lists: an output byte is either one the allow-list
 * names or it is produced by this code (a fallback name, a `\u{XXXX}` escape).
 * No input byte is copied without a check.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "ntfsmac/label_cp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Buffer size that always suffices for ntfsmac_label_sanitize(): 64 bytes plus NUL. */
#define NTFSMAC_LABEL_PART_SIZE (NTFSMAC_LABEL_PART_MAX_BYTES + 1u)

/* Collision suffixes of the leaf: " 2" to " 99" (ADR-016, "Collisions"). */
#define NTFSMAC_LEAF_SUFFIX_MIN 2u
#define NTFSMAC_LEAF_SUFFIX_MAX 99u

/* Buffer size for ntfsmac_leaf_create(): 64 bytes, a suffix of 3 bytes, NUL. */
#define NTFSMAC_LEAF_SIZE (NTFSMAC_LABEL_PART_MAX_BYTES + 3u + 1u)

/*
 * Path display (ADR-023 decision 4): the verdict holds at most 512 bytes per path;
 * every input byte becomes at most 8 output bytes (`\u{XXXX}` for a C0 control
 * or a 2- or 3-byte code point, `\x{XX}` for a byte of invalid UTF-8), a 4-byte
 * code point at most 10 (`\u{10FFFF}`). 512 * 8 plus NUL always suffices for
 * such a path.
 */
#define NTFSMAC_PATH_DISPLAY_SIZE (512u * 8u + 1u)

/*
 * Label sanitiser (ADR-016 decision 2). Maps the raw label label[0..len) to the leaf
 * label part and writes it NUL terminated to out. The rules, in the order of the
 * ADR: strict UTF-8 decoding (any invalid byte: the whole label is replaced by the
 * fallback); every code point that ntfsmac_label_cp_allowed() refuses becomes `_` (also NUL
 * and every other control byte); the result is cut at 64 bytes on a code point boundary;
 * leading spaces are removed; a leading `.` or a leading combining mark becomes `_`; trailing
 * spaces and dots are removed; a result that is empty, or consists only of `_` that replaced
 * something (no allowed original code point survived), becomes the fallback
 * `NTFS-<last 8 hex digits of serial>` (upper case, formatted from the integer). The result is
 * never empty, never `.` or `..`, and always satisfies ntfsmac_label_part_ok().
 *
 * The raw label is only read, never used otherwise. Returns the length written (1 to
 * 64, without the NUL; the fallback is 13 bytes). Returns 0 and writes an empty string
 * if out is NULL or cap is smaller than NTFSMAC_LABEL_PART_SIZE (fail closed: the
 * caller must treat 0 as an error, not as an empty name). `label` may be NULL only
 * with len 0.
 */
size_t ntfsmac_label_sanitize(const char *label, size_t len, uint64_t serial, char *out,
			      size_t cap);

/*
 * Path display sanitiser (ADR-023 decision 4). Writes the display form of a path
 * from `nonnfc_paths` into out, always NUL terminated, and returns the length it
 * needs (without the NUL, like snprintf; at most 8 per input byte). The path is split
 * at `/`; each `/` is kept. Each component goes through the code point rules of ADR-016
 * (strict UTF-8, allow-list of ntfsmac_label_cp_allowed()), except that a code point that
 * the label sanitiser would replace by `_` is shown as `\u{XXXX}` (upper-case hex, at
 * least 4 digits), so the user can find the file. A byte of invalid UTF-8 is shown as
 * `\x{XX}`. A backslash is not allowed, so it is itself shown as `\u{005C}`: every `\`
 * in the output starts an escape and the output is unambiguous. The component rules of
 * a created name (the 64-byte cut, leading and trailing spaces and dots) are not
 * applied: the path names an existing file and must be shown whole.
 *
 * The output is truncated, at an element boundary, if cap is too small; compare the
 * return value with cap - 1. With NTFSMAC_PATH_DISPLAY_SIZE bytes a path of at most
 * 512 bytes is never truncated. If cap is 0 nothing is written.
 */
size_t ntfsmac_path_display(const char *path, size_t len, char *out, size_t cap);

/*
 * Mount-point leaf (ADR-016, "Collisions"; U-LABEL-08). Creates the directory for
 * `base` (a sanitised label part: it must satisfy ntfsmac_label_part_ok()) with
 * mkdirat(dirfd, ..., mode), which does not follow a final symbolic link and fails with EEXIST
 * on any existing entry (the caller passes an O_DIRECTORY | O_NOFOLLOW descriptor of
 * /Volumes). On EEXIST it tries `<base> 2` to `<base> 99`. It never reuses an existing entry.
 *
 * On success returns 0 and writes the created leaf (at most 67 bytes) to leaf and,
 * if non-NULL, the device and inode of the directory it created to *dev and *ino, so
 * that the caller removes only that directory. Returns -1 with errno EEXIST if all
 * 99 names (`<base>` and `<base> 2` to `<base> 99`) are taken, EINVAL for an argument outside the contract (base not a label part,
 * cap smaller than NTFSMAC_LEAF_SIZE), or the errno of mkdirat() for any other failure; leaf
 * is empty in these cases. Returns -1 with errno ESTALE if the directory was created but its
 * device and inode could not be read: leaf then holds the created name, *dev and *ino are
 * untouched, and the caller must treat the entry as created but of unproven identity (it
 * must not remove it by name without checking).
 */
int ntfsmac_leaf_create(int dirfd, const char *base, mode_t mode, char *leaf, size_t cap,
			dev_t *dev, ino_t *ino);

#ifdef __cplusplus
}
#endif

#endif /* NTFSMAC_SANITIZE_H */
