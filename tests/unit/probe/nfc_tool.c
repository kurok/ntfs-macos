/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Test helper for test_nfc.py: reads one string per line as space separated
 * hexadecimal UTF-16 units, prints "<normalized 0|1> <code points of the NFC form in hex>".
 * With --version prints the Unicode version of the tables.
 */
#include "../../../src/probe/posix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nfc.h"

int main(int argc, char **argv)
{
	char line[8192];
	uint32_t cp[NTFSMAC_NFC_MAX_CODEPOINTS];

	if (argc == 2 && strcmp(argv[1], "--version") == 0) {
		puts(ntfsmac_nfc_unicode_version());
		return 0;
	}
	while (fgets(line, sizeof(line), stdin)) {
		uint16_t u[NTFSMAC_NFC_MAX_UNITS];
		size_t n = 0, i, k;
		char *p = line, *end;
		int norm;

		while (*p) {
			unsigned long v = strtoul(p, &end, 16);

			if (end == p)
				break;
			if (n == NTFSMAC_NFC_MAX_UNITS)
				return 2;
			u[n++] = (uint16_t)v;
			p = end;
		}
		k = ntfsmac_nfc_normalize(u, n, cp, NTFSMAC_NFC_MAX_CODEPOINTS);
		norm = ntfsmac_nfc_is_normalized(u, n);
		if (k == (size_t)-1 || norm < 0)
			return 2;
		printf("%d", norm);
		for (i = 0; i < k; i++)
			printf(" %X", (unsigned)cp[i]);
		putchar('\n');
	}
	return 0;
}
