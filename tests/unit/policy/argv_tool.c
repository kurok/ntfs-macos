/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Helper for test_mount_options_json.py (U-ARGV-06). Reads tab separated
 * commands on stdin, writes one line per command on stdout:
 *
 *   build <TAB> rw|ro <TAB> permissions 0|1 <TAB> usermapping 0|1 <TAB> hide 0|1
 *       -> the builder's option string, or "ERR <n>"
 *   check <TAB> rw|ro <TAB> <option string, may be empty>
 *       -> argv_check_text() status name for the fixed vector
 *          WORKER /dev/fd/5 /Volumes/Test Vol -o <option string>
 */
#include "ntfsmac/options.h"

#include <stdio.h>
#include <string.h>

#define WORKER "/usr/local/libexec/ntfs-macos/ntfsmac-worker"
#define LEAF "Test Vol"

static int parse_dec(const char *s)
{
	return strcmp(s, "ro") == 0 ? NTFSMAC_POLICY_RO : NTFSMAC_POLICY_RW;
}

int main(void)
{
	char line[4096];

	while (fgets(line, sizeof(line), stdin) != NULL) {
		char *f[6] = { 0 };
		size_t n = 0, len = strlen(line);
		char *p = line;

		if (len > 0 && line[len - 1] == '\n')
			line[len - 1] = '\0';
		while (n < 6) {
			f[n++] = p;
			p = strchr(p, '\t');
			if (p == NULL)
				break;
			*p++ = '\0';
		}
		if (strcmp(f[0], "build") == 0 && n == 5) {
			ntfsmac_options_input in;
			ntfsmac_worker_argv w;
			ntfsmac_options_status st;

			memset(&in, 0, sizeof(in));
			in.worker_path = WORKER;
			in.device_fd = 5;
			in.leaf = LEAF;
			in.decision = parse_dec(f[1]);
			in.console_uid = 501;
			in.console_gid = 20;
			in.permissions = f[2][0] == '1';
			in.usermapping = f[3][0] == '1';
			in.hide_hid_files = f[4][0] == '1';
			st = ntfsmac_options_build(&in, &w);
			if (st == NTFSMAC_OPTIONS_OK)
				printf("%s\n", w.optstr);
			else
				printf("ERR %d\n", (int)st);
		} else if (strcmp(f[0], "check") == 0 && n == 3) {
			ntfsmac_argv_expect ex;
			const char *v[5];
			ntfsmac_argv_status st;

			ex.worker_path = WORKER;
			ex.leaf = LEAF;
			ex.decision = parse_dec(f[1]);
			ex.console_uid = 501;
			ex.console_gid = 20;
			v[0] = WORKER;
			v[1] = "/dev/fd/5";
			v[2] = "/Volumes/" LEAF;
			v[3] = "-o";
			v[4] = f[2];
			st = ntfsmac_argv_check_text(v, 5, &ex);
			printf("%s\n", ntfsmac_argv_status_name(st));
		} else {
			printf("BAD\n");
		}
	}
	return 0;
}
