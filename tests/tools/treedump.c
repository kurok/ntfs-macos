/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * treedump - print a stable, sorted description of a directory tree.
 *
 * Usage: treedump [-a] [-m] DIR
 *
 * One line per entry, fields separated by TAB, lines sorted bytewise by the
 * path relative to DIR:
 *
 *   d  PATH
 *   f  PATH  SIZE  SHA256-of-content
 *   l  PATH  TARGET
 *   o  PATH                          (fifo, socket, device node, ...)
 *
 * Bytes below 0x20, 0x7f and backslash in PATH and TARGET are escaped as
 * \t, \n, \\ or \xHH. With -m every line gets a last field with the
 * permission bits (octal, mode & 0777). Timestamps, inode numbers, link counts
 * and owners are never printed: they are volatile across generations.
 *
 * Unless -a is given, the top level skips NTFS metafiles ("$MFT", "$Extend",
 * ...) and "System Volume Information", which a volume may expose.
 *
 * Portable C: builds on macOS and Linux, no external crypto library.
 */
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ---- SHA-256 (FIPS 180-4) ---- */

struct sha256 {
	uint32_t h[8];
	uint64_t len;
	unsigned char buf[64];
	size_t fill;
};

static const uint32_t K256[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
	0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
	0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
	0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
	0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
	0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(struct sha256 *s, const unsigned char *p)
{
	uint32_t w[64], a, b, c, d, e, f, g, h, t1, t2;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
		       ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
	for (i = 16; i < 64; i++) {
		uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);

		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
	e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
	for (i = 0; i < 64; i++) {
		t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) +
		     K256[i] + w[i];
		t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) +
		     ((a & b) ^ (a & c) ^ (b & c));
		h = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}
	s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
	s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha256_init(struct sha256 *s)
{
	static const uint32_t iv[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
	};

	memcpy(s->h, iv, sizeof(iv));
	s->len = 0;
	s->fill = 0;
}

static void sha256_update(struct sha256 *s, const unsigned char *p, size_t n)
{
	s->len += n;
	while (n > 0) {
		size_t take = 64 - s->fill;

		if (take > n)
			take = n;
		memcpy(s->buf + s->fill, p, take);
		s->fill += take;
		p += take;
		n -= take;
		if (s->fill == 64) {
			sha256_block(s, s->buf);
			s->fill = 0;
		}
	}
}

static void sha256_final_hex(struct sha256 *s, char out[65])
{
	static const char hex[] = "0123456789abcdef";
	uint64_t bits = s->len * 8;
	unsigned char pad = 0x80, zero = 0, lenb[8];
	int i;

	sha256_update(s, &pad, 1);
	while (s->fill != 56)
		sha256_update(s, &zero, 1);
	for (i = 0; i < 8; i++)
		lenb[i] = (unsigned char)(bits >> (56 - 8 * i));
	sha256_update(s, lenb, 8);
	for (i = 0; i < 32; i++) {
		unsigned char byte = (unsigned char)(s->h[i / 4] >> (24 - 8 * (i % 4)));

		out[2 * i] = hex[byte >> 4];
		out[2 * i + 1] = hex[byte & 15];
	}
	out[64] = '\0';
}

/* ---- tree walk ---- */

struct entry {
	char *path;   /* relative, raw bytes */
	char *target; /* symlink target or NULL */
	char type;    /* d f l o */
	off_t size;
	unsigned mode;
	char sha[65];
};

static struct entry *g_ent;
static size_t g_n, g_cap;
static int g_all, g_mode;

static void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);

	if (!p) {
		fprintf(stderr, "treedump: out of memory\n");
		exit(2);
	}
	return p;
}

static char *xstrdup(const char *s)
{
	size_t n = strlen(s) + 1;

	return memcpy(xmalloc(n), s, n);
}

static struct entry *add_entry(void)
{
	if (g_n == g_cap) {
		size_t cap = g_cap ? g_cap * 2 : 256;
		struct entry *ne = xmalloc(cap * sizeof(*ne));

		if (g_n)
			memcpy(ne, g_ent, g_n * sizeof(*ne));
		free(g_ent);
		g_ent = ne;
		g_cap = cap;
	}
	memset(&g_ent[g_n], 0, sizeof(g_ent[g_n]));
	return &g_ent[g_n++];
}

static int hash_file(const char *full, off_t *size, char sha[65])
{
	unsigned char buf[65536];
	struct sha256 s;
	FILE *f = fopen(full, "rb");
	size_t n;
	off_t total = 0;

	if (!f)
		return -1;
	sha256_init(&s);
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		sha256_update(&s, buf, n);
		total += (off_t)n;
	}
	if (ferror(f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	sha256_final_hex(&s, sha);
	*size = total;
	return 0;
}

static int skip_top(const char *name)
{
	if (g_all)
		return 0;
	return name[0] == '$' || strcasecmp(name, "System Volume Information") == 0;
}

static int walk(const char *full, const char *rel, int top)
{
	DIR *d = opendir(full);
	struct dirent *de;
	int rc = 0;

	if (!d) {
		fprintf(stderr, "treedump: %s: %s\n", full, strerror(errno));
		return -1;
	}
	while ((de = readdir(d)) != NULL) {
		const char *nm = de->d_name;
		char *cfull, *crel;
		struct stat st;
		struct entry *e;
		size_t lf, lr;

		if (!strcmp(nm, ".") || !strcmp(nm, ".."))
			continue;
		if (top && skip_top(nm))
			continue;
		lf = strlen(full) + strlen(nm) + 2;
		lr = strlen(rel) + strlen(nm) + 2;
		cfull = xmalloc(lf);
		crel = xmalloc(lr);
		snprintf(cfull, lf, "%s/%s", full, nm);
		if (rel[0])
			snprintf(crel, lr, "%s/%s", rel, nm);
		else
			snprintf(crel, lr, "%s", nm);
		if (lstat(cfull, &st) != 0) {
			fprintf(stderr, "treedump: %s: %s\n", cfull, strerror(errno));
			rc = -1;
			free(cfull);
			free(crel);
			continue;
		}
		e = add_entry();
		e->path = crel;
		e->mode = (unsigned)(st.st_mode & 0777);
		if (S_ISDIR(st.st_mode)) {
			e->type = 'd';
			if (walk(cfull, crel, 0) != 0)
				rc = -1;
		} else if (S_ISREG(st.st_mode)) {
			e->type = 'f';
			if (hash_file(cfull, &e->size, e->sha) != 0) {
				fprintf(stderr, "treedump: %s: %s\n", cfull, strerror(errno));
				rc = -1;
			}
		} else if (S_ISLNK(st.st_mode)) {
			char tb[4096];
			ssize_t n = readlink(cfull, tb, sizeof(tb) - 1);

			e->type = 'l';
			if (n < 0) {
				fprintf(stderr, "treedump: %s: %s\n", cfull, strerror(errno));
				rc = -1;
				n = 0;
			}
			tb[n] = '\0';
			e->target = xstrdup(tb);
		} else {
			e->type = 'o';
		}
		free(cfull);
	}
	closedir(d);
	return rc;
}

static int cmp_entry(const void *a, const void *b)
{
	return strcmp(((const struct entry *)a)->path, ((const struct entry *)b)->path);
}

static void put_escaped(const char *s)
{
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;

		if (c == '\\')
			fputs("\\\\", stdout);
		else if (c == '\t')
			fputs("\\t", stdout);
		else if (c == '\n')
			fputs("\\n", stdout);
		else if (c < 0x20 || c == 0x7f)
			printf("\\x%02x", c);
		else
			putchar(c);
	}
}

int main(int argc, char **argv)
{
	size_t i;
	int rc, opt;

	while ((opt = getopt(argc, argv, "am")) != -1) {
		if (opt == 'a')
			g_all = 1;
		else if (opt == 'm')
			g_mode = 1;
		else
			goto usage;
	}
	if (argc - optind != 1) {
usage:
		fprintf(stderr, "usage: treedump [-a] [-m] DIR\n");
		return 2;
	}
	rc = walk(argv[optind], "", 1);
	qsort(g_ent, g_n, sizeof(*g_ent), cmp_entry);
	for (i = 0; i < g_n; i++) {
		struct entry *e = &g_ent[i];

		printf("%c\t", e->type);
		put_escaped(e->path);
		if (e->type == 'f')
			printf("\t%lld\t%s", (long long)e->size, e->sha);
		else if (e->type == 'l') {
			putchar('\t');
			put_escaped(e->target);
		}
		if (g_mode)
			printf("\t%03o", e->mode);
		putchar('\n');
	}
	if (fflush(stdout) != 0 || ferror(stdout)) {
		fprintf(stderr, "treedump: write error\n");
		return 2;
	}
	return rc == 0 ? 0 : 1;
}
