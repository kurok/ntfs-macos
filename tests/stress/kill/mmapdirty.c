/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * mmapdirty FILE - map the first page of FILE shared and read/write, dirty it with 'B', and wait.
 * msync(2) is never called. Prints "dirty" once the page is written. The caller kills it.
 * Used by tests/stress/kill (S-KILL-03 measurement: does a dirty mmap page survive the kill of the module).
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	int fd = open(argv[1], O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	memset(p, 'B', 4096);
	printf("dirty\n");
	fflush(stdout);
	for (;;)
		pause();
}
