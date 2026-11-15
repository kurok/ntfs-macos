/* SPDX-License-Identifier: GPL-2.0-or-later */
/* In-memory image model of the NTFS fuzz harnesses; see image.h. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include "image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FZ_BASES_DIR
#define FZ_BASES_DIR "."
#endif

#define FZ_NBASES 3
#define FZ_SECTOR 512u
#define FZ_MAX_IMAGE (8u << 20)
#define FZ_MAX_FILE (FZ_MAX_IMAGE + (1u << 20))
#define FZ_LONG_RUN 32u /* a run of metadata sectors longer than this is the upcase table or similar */

static const char *const base_names[FZ_NBASES] = { "tree", "geom", "dirty" };

typedef struct fz_base {
	uint8_t *data;
	size_t size;
	uint32_t *hot; /* sector numbers */
	size_t nhot;
} fz_base;

static fz_base bases[FZ_NBASES];
static uint8_t *work;
static int loaded;

static void die(const char *base, const char *why)
{
	fprintf(stderr, "fuzz image: base %s: %s\n", base, why);
	abort();
}

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t rd64(const uint8_t *p)
{
	return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32;
}

static int uniform(const uint8_t *s)
{
	size_t i;

	for (i = 1; i < FZ_SECTOR; i++)
		if (s[i] != s[0])
			return 0;
	return 1;
}

/* The sectors that hold something other than zeros or one repeated byte. */
static void find_hot(fz_base *b, const char *name)
{
	size_t sectors = b->size / FZ_SECTOR, s, run = 0, start = 0;

	b->hot = malloc(sectors * sizeof(*b->hot));
	if (b->hot == NULL)
		die(name, "out of memory");
	for (s = 0; s <= sectors; s++) {
		int meta = s < sectors && !uniform(b->data + s * FZ_SECTOR);

		if (meta) {
			if (run == 0)
				start = s;
			run++;
			continue;
		}
		if (run) {
			size_t keep = run > FZ_LONG_RUN ? 2 : run, i;

			for (i = 0; i < keep; i++)
				b->hot[b->nhot++] = (uint32_t)(start + i);
		}
		run = 0;
	}
	if (b->nhot == 0)
		die(name, "no metadata sector");
}

static void load_base(fz_base *b, const char *dir, const char *name)
{
	char path[1024];
	uint8_t *file;
	size_t n, pos = 12, end = 0;
	uint64_t size;
	FILE *f;

	if (snprintf(path, sizeof(path), "%s/%s.nsb", dir, name) >= (int)sizeof(path))
		die(name, "path too long");
	f = fopen(path, "rb");
	if (f == NULL) {
		fprintf(stderr, "fuzz image: cannot open %s\n", path);
		abort();
	}
	file = malloc(FZ_MAX_FILE + 1);
	if (file == NULL)
		die(name, "out of memory");
	n = fread(file, 1, FZ_MAX_FILE + 1, f);
	fclose(f);
	if (n > FZ_MAX_FILE || n < 12 || memcmp(file, "NSB1", 4) != 0)
		die(name, "not an NSB1 file");
	size = rd64(file + 4);
	if (size < FZ_SECTOR || size > FZ_MAX_IMAGE || size % FZ_SECTOR != 0)
		die(name, "bad image size");
	b->size = (size_t)size;
	b->data = calloc(1, b->size);
	if (b->data == NULL)
		die(name, "out of memory");
	while (pos < n) {
		uint64_t off;
		uint32_t len;

		if (n - pos < 14)
			die(name, "truncated record");
		off = rd64(file + pos);
		len = rd32(file + pos + 8);
		if (off % FZ_SECTOR || len % FZ_SECTOR || len == 0 || off < end || off > size || len > size - off)
			die(name, "bad record");
		if (file[pos + 12] == 0) {
			if (n - pos - 14 < len)
				die(name, "truncated data");
			memcpy(b->data + off, file + pos + 14, len);
			pos += 14 + len;
		} else if (file[pos + 12] == 1) {
			memset(b->data + off, file[pos + 13], len);
			pos += 14;
		} else {
			die(name, "bad record kind");
		}
		end = off + len;
	}
	free(file);
	find_hot(b, name);
}

static void init(void)
{
	const char *dir = getenv("NTFSMAC_FUZZ_BASES");
	size_t i, max = 0;

	if (dir == NULL || dir[0] == '\0')
		dir = FZ_BASES_DIR;
	for (i = 0; i < FZ_NBASES; i++) {
		load_base(&bases[i], dir, base_names[i]);
		if (bases[i].size > max)
			max = bases[i].size;
	}
	work = malloc(max);
	if (work == NULL)
		die("work", "out of memory");
	loaded = 1;
}

static uint32_t rd16(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

const uint8_t *fz_image_build(const uint8_t *in, size_t n, size_t *size)
{
	const fz_base *b;
	size_t sz, pos = 3;
	uint32_t trunc = 0;

	if (!loaded)
		init();
	b = &bases[n > 0 ? in[0] % FZ_NBASES : 0];
	sz = b->size;
	memcpy(work, b->data, sz);
	if (n >= 3)
		trunc = rd16(in + 1);

	while (pos + 5 < n) {
		uint32_t a = rd16(in + pos), bb = rd16(in + pos + 2), c = in[pos + 4];
		size_t len = (c & 63u) + 1, avail, off, i;
		unsigned op = c >> 6;

		pos += 5;
		avail = n - pos;
		if (a & 0x8000u)
			off = (size_t)((((uint64_t)(a & 0x7fffu) << 16) | bb) % sz);
		else
			off = (size_t)b->hot[a % b->nhot] * FZ_SECTOR + bb % FZ_SECTOR;
		if (off + len > sz)
			len = sz - off;
		switch (op) {
		case 0:
		case 1:
			if (len > avail)
				len = avail;
			for (i = 0; i < len; i++)
				work[off + i] = op == 0 ? in[pos + i] : (uint8_t)(work[off + i] ^ in[pos + i]);
			pos += len;
			break;
		case 2:
			memset(work + off, in[pos], len);
			pos++;
			break;
		default: {
			size_t src = (size_t)b->hot[in[pos] % b->nhot] * FZ_SECTOR;

			if (src + len > sz)
				len = sz - src;
			memmove(work + off, work + src, len);
			pos++;
			break;
		}
		}
	}
	if ((trunc >> 12) == 0xF) {
		size_t sectors = sz / FZ_SECTOR, cut = (trunc & 0xFFFu) % sectors;

		sz -= cut * FZ_SECTOR;
	}
	*size = sz;
	return work;
}
