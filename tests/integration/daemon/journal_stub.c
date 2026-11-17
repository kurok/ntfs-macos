/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * T-M4-14: stand-in for ntfsmac-volmark in journal_vm.sh. The kind comes from the base name of
 * the path it is started under (a link in the work directory):
 *
 *   jstub-exit1    exit 1 without a word (I-DMN-22: volmark replaced by a test binary that
 *                  exits 1)
 *   jstub-sector0  print `w`, change one byte of the boot code area of sector 0 through the
 *                  inherited descriptor 3, print `d`, exit 0 (I-DMN-25 c: volmark that changes
 *                  sector 0)
 *   jstub-hang     print nothing and wait (the 20 s limit before `w`)
 *
 * It takes the arguments of the real tool (`set --fd 3 --expect-serial S --expect-sectors N`) and
 * checks nothing about them.
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	char *copy;
	const char *base;
	unsigned char b = 0;
	int fd;

	if (argc < 4)
		return 2;
	copy = strdup(argv[0]);
	if (copy == NULL)
		return 2;
	base = basename(copy);
	fd = atoi(argv[3]);
	if (strcmp(base, "jstub-exit1") == 0)
		return 1;
	if (strcmp(base, "jstub-hang") == 0) {
		for (;;)
			sleep(60);
	}
	if (strcmp(base, "jstub-sector0") == 0) {
		printf("w\n");
		fflush(stdout);
		if (pread(fd, &b, 1, 0x70) != 1)
			return 3;
		b ^= 0xFF;
		if (pwrite(fd, &b, 1, 0x70) != 1)
			return 4;
		printf("d\n");
		fflush(stdout);
		return 0;
	}
	return 2;
}
