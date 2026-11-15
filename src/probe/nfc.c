/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Unicode NFC (UAX #15) over the tables of nfc_data.h: full canonical
 * decomposition, canonical ordering, canonical composition. Hangul syllables
 * are handled by the algorithm of UAX #15 section 16.
 */
#include "posix.h"
#include "nfc.h"

#include <stdbool.h>

#include "nfc_data.h"

#define SBASE 0xAC00u
#define LBASE 0x1100u
#define VBASE 0x1161u
#define TBASE 0x11A7u
#define LCOUNT 19u
#define VCOUNT 21u
#define TCOUNT 28u
#define NCOUNT (VCOUNT * TCOUNT)
#define SCOUNT (LCOUNT * NCOUNT)

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

const char *ntfsmac_nfc_unicode_version(void)
{
	return NTFSMAC_NFC_UNICODE_VERSION;
}

static unsigned ccc_of(uint32_t cp)
{
	size_t lo = 0, hi = ARRAY_LEN(nfc_ccc);

	if (cp < 0x300)
		return 0;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;

		if (nfc_ccc[mid].cp == cp)
			return nfc_ccc[mid].ccc;
		if (nfc_ccc[mid].cp < cp)
			lo = mid + 1;
		else
			hi = mid;
	}
	return 0;
}

/* Index into nfc_decomp, or -1. */
static long decomp_of(uint32_t cp)
{
	size_t lo = 0, hi = ARRAY_LEN(nfc_decomp);

	if (cp < 0xC0)
		return -1;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;

		if (nfc_decomp[mid].cp == cp)
			return (long)mid;
		if (nfc_decomp[mid].cp < cp)
			lo = mid + 1;
		else
			hi = mid;
	}
	return -1;
}

/*
 * Is the code point assigned in the Unicode version of the tables (nfc_data.h)? A code
 * point that is not (a character added by a newer Unicode version, a noncharacter, an
 * unassigned one) has no normalisation data here, so a name that holds one cannot be
 * called NFC: ntfsmac_nfc_is_normalized() answers 0 for it (fail closed). Private-use
 * code points count as assigned.
 */
static bool assigned_cp(uint32_t cp)
{
	size_t lo = 0, hi = ARRAY_LEN(nfc_assigned);

	if (cp < 0x300)
		return true;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;

		if (cp < nfc_assigned[mid].first)
			hi = mid;
		else if (cp > nfc_assigned[mid].last)
			lo = mid + 1;
		else
			return true;
	}
	return false;
}

/* Primary composite of (first, second), or 0 if none. */
static uint32_t compose_pair(uint32_t first, uint32_t second)
{
	size_t lo = 0, hi = ARRAY_LEN(nfc_comp);

	/* Hangul: L + V and LV + T. */
	if (first >= LBASE && first < LBASE + LCOUNT && second >= VBASE && second < VBASE + VCOUNT)
		return SBASE + ((first - LBASE) * VCOUNT + (second - VBASE)) * TCOUNT;
	if (first >= SBASE && first < SBASE + SCOUNT && (first - SBASE) % TCOUNT == 0 &&
	    second > TBASE && second < TBASE + TCOUNT)
		return first + (second - TBASE);

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;

		if (nfc_comp[mid].first == first && nfc_comp[mid].second == second)
			return nfc_comp[mid].composed;
		if (nfc_comp[mid].first < first ||
		    (nfc_comp[mid].first == first && nfc_comp[mid].second < second))
			lo = mid + 1;
		else
			hi = mid;
	}
	return 0;
}

/* UTF-16 to code points; a lone surrogate stays one code point. Returns the count. */
static size_t decode_utf16(const uint16_t *u, size_t n, uint32_t *cp)
{
	size_t i = 0, k = 0;

	while (i < n) {
		uint32_t c = u[i];

		if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF) {
			c = 0x10000 + ((c - 0xD800) << 10) + (u[i + 1] - 0xDC00);
			i += 2;
		} else {
			i++;
		}
		cp[k++] = c;
	}
	return k;
}

size_t ntfsmac_nfc_normalize(const uint16_t *u, size_t n, uint32_t *out, size_t cap)
{
	uint32_t src[NTFSMAC_NFC_MAX_UNITS];
	size_t nsrc, nd = 0, i, j, starter_pos, comp_pos;
	uint32_t starter;
	unsigned last_class;

	if (n > NTFSMAC_NFC_MAX_UNITS || cap < NTFSMAC_NFC_MAX_CODEPOINTS)
		return (size_t)-1;
	nsrc = decode_utf16(u, n, src);

	/* Full canonical decomposition into out. */
	for (i = 0; i < nsrc; i++) {
		uint32_t c = src[i];
		long d;

		if (c >= SBASE && c < SBASE + SCOUNT) {
			uint32_t s = c - SBASE;

			out[nd++] = LBASE + s / NCOUNT;
			out[nd++] = VBASE + (s % NCOUNT) / TCOUNT;
			if (s % TCOUNT)
				out[nd++] = TBASE + s % TCOUNT;
		} else if ((d = decomp_of(c)) >= 0) {
			for (j = 0; j < nfc_decomp[d].len; j++)
				out[nd++] = nfc_pool[nfc_decomp[d].off + j];
		} else {
			out[nd++] = c;
		}
	}

	/* Canonical ordering: insertion sort of each run of non-starters by class. */
	for (i = 1; i < nd; i++) {
		unsigned ci = ccc_of(out[i]);
		uint32_t c = out[i];

		if (ci == 0)
			continue;
		j = i;
		while (j > 0 && ccc_of(out[j - 1]) > ci) {
			out[j] = out[j - 1];
			j--;
		}
		out[j] = c;
	}

	/* Canonical composition (UAX #15 sample algorithm), in place. */
	if (nd == 0)
		return 0;
	starter_pos = 0;
	starter = out[0];
	last_class = ccc_of(starter);
	if (last_class != 0)
		last_class = 256; /* a string that starts with a non-starter composes nothing into it */
	comp_pos = 1;
	for (i = 1; i < nd; i++) {
		uint32_t c = out[i];
		unsigned cls = ccc_of(c);
		uint32_t composite = compose_pair(starter, c);

		if (composite != 0 && (last_class < cls || last_class == 0)) {
			out[starter_pos] = composite;
			starter = composite;
		} else {
			if (cls == 0) {
				starter_pos = comp_pos;
				starter = c;
			}
			last_class = cls;
			out[comp_pos++] = c;
		}
	}
	return comp_pos;
}

int ntfsmac_nfc_is_normalized(const uint16_t *u, size_t n)
{
	uint32_t src[NTFSMAC_NFC_MAX_UNITS];
	uint32_t out[NTFSMAC_NFC_MAX_CODEPOINTS];
	size_t i, nsrc, nout;
	bool simple = true;

	if (n > NTFSMAC_NFC_MAX_UNITS)
		return -1;
	for (i = 0; i < n; i++) {
		if (u[i] >= 0x300) {
			simple = false;
			break;
		}
	}
	if (simple)
		return 1;
	nsrc = decode_utf16(u, n, src);
	for (i = 0; i < nsrc; i++)
		if (!(src[i] >= 0xD800 && src[i] <= 0xDFFF) && !assigned_cp(src[i]))
			return 0; /* not known to the tables' Unicode version: not trusted as NFC */
	nout = ntfsmac_nfc_normalize(u, n, out, NTFSMAC_NFC_MAX_CODEPOINTS);
	if (nout == (size_t)-1)
		return -1;
	if (nout != nsrc)
		return 0;
	for (i = 0; i < nsrc; i++)
		if (out[i] != src[i])
			return 0;
	return 1;
}
