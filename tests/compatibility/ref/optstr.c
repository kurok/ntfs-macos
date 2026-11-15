/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * optstr - print the worker option string that the product's builder (src/options/options.c)
 * produces, so that the reference comparison mounts Environment B with exactly that string.
 *
 * Usage: optstr UID GID rw|ro
 * Build: cc -std=c17 -I REPO/include -o optstr optstr.c REPO/src/options/options.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ntfsmac/options.h"

int main(int argc, char **argv)
{
	ntfsmac_options_input in;
	static ntfsmac_worker_argv out;
	char *end;

	if (argc != 4 || (strcmp(argv[3], "rw") != 0 && strcmp(argv[3], "ro") != 0)) {
		fprintf(stderr, "usage: optstr UID GID rw|ro\n");
		return 2;
	}
	memset(&in, 0, sizeof(in));
	in.worker_path = "/usr/local/libexec/ntfs-3g";
	in.device_fd = 3;
	in.leaf = "ref";
	in.decision = strcmp(argv[3], "ro") == 0 ? NTFSMAC_POLICY_RO : NTFSMAC_POLICY_RW;
	in.console_uid = (uint32_t)strtoul(argv[1], &end, 10);
	if (*end != '\0')
		return 2;
	in.console_gid = (uint32_t)strtoul(argv[2], &end, 10);
	if (*end != '\0')
		return 2;
	if (ntfsmac_options_build(&in, &out) != NTFSMAC_OPTIONS_OK) {
		fprintf(stderr, "optstr: builder refused the input\n");
		return 1;
	}
	puts(out.optstr);
	return 0;
}
