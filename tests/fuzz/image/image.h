/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_FUZZ_IMAGE_H
#define NTFSMAC_FUZZ_IMAGE_H

/*
 * In-memory NTFS image model shared by F-PROBE and F-MOUNT-WALK (T-M3-16).
 *
 * The fuzzer input is not an image but a recipe for one: a base image (a tiny volume made by
 * mkntfs and ntfs-3g, the .nsb files of tests/fuzz/image/bases) plus a list of byte-level edits. libFuzzer
 * input of a few dozen bytes then reaches the places where an NTFS driver parses (boot sector,
 * MFT records, index blocks, run lists) instead of spending its time on file data. Layout of
 * an input (anything is valid; a short input is a base with fewer edits):
 *
 *   byte 0        base: input[0] modulo the number of bases (tree, geom, dirty)
 *   bytes 1, 2    truncation (little endian): if the top four bits are 1111, the image
 *                 ends (low twelve bits modulo sectors) sectors early; otherwise full size
 *   from byte 3   edits of 5 bytes plus data:
 *                   u16 a, u16 b, u8 c
 *                   target: bit 15 of a clear: byte b modulo 512 of the (a modulo hot count)th
 *                           "hot" sector, a sector that holds metadata (non-zero, not a fill);
 *                           bit 15 of a set: offset ((a & 0x7fff) << 16 | b) modulo the size
 *                   length: (c & 63) + 1 bytes (cut at the end of the image)
 *                   operation c >> 6: 0 overwrite with the data that follows (length bytes),
 *                           1 xor with that data, 2 fill with the one data byte that follows,
 *                           3 copy from the start of the hot sector that the one data byte
 *                           that follows selects (moves a record or an index block around)
 *
 * Environment: NTFSMAC_FUZZ_BASES names the directory of the .nsb files (default: the
 * directory the harness was built from). A missing or malformed base aborts the process:
 * a harness without its bases tests nothing.
 */

#include <stddef.h>
#include <stdint.h>

/*
 * The image for one fuzz input. The pointer is to a buffer that the next call overwrites;
 * *size is the image size (at least 512).
 */
const uint8_t *fz_image_build(const uint8_t *in, size_t n, size_t *size);

#endif /* NTFSMAC_FUZZ_IMAGE_H */
