/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Control for S-LEAK-01: a program that leaks 8 blocks from a known function. leaks_guest.sh
 * runs it with MallocStackLogging=1 and scans it with `leaks` the way it scans the worker; the
 * run only counts if `leaks` finds these blocks with this function in their stack, and
 * classify.py turns them into a FAIL when a rule calls this file project code. Guest only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

__attribute__((noinline)) static void scrub_stack(void)
{
	volatile char b[16384];
	memset((void *)b, 0, sizeof(b));
}

__attribute__((noinline)) void ctl_leak_site(void)
{
	int i;

	for (i = 0; i < 8; i++) {
		void *volatile p = malloc(256 + (size_t)i * 16);
		if (p != NULL)
			memset(p, 0x5a, 256);
		p = NULL;
	}
}

int main(int argc, char **argv)
{
	FILE *f;

	ctl_leak_site();
	scrub_stack();
	if (argc > 1 && (f = fopen(argv[1], "w")) != NULL) {
		fputs("ready\n", f);
		fclose(f);
	}
	sleep(300);
	return 0;
}
