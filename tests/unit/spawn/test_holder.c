/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-HLD-01..: the decision of the device sweep (src/daemon/spawn_holder.c, issue 1, audit M4 F-02),
 * driven with a fake holder list: no device, no mount, no process. U-HLD-06 checks that the macFUSE
 * helper path of spawn_config.h is the one process-exec rule of the worker sandbox profile.
 *
 * usage: test_holder <integration/sandbox directory>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "spawn_config.h"
#include "spawn_holder.h"

#define WORKER_PID 4000

static const ntfsmac_exe_id WORKER = { 7, 1001 };
static const ntfsmac_exe_id HELPER = { 7, 2002 };

typedef struct fake {
	const char *what;
	ntfsmac_holder h;
	bool kill;
} fake;

static ntfsmac_holder holder(pid_t pid, const char *exe, bool known, ntfsmac_exe_id id, unsigned n,
			     pid_t a0, pid_t a1)
{
	ntfsmac_holder h;

	memset(&h, 0, sizeof(h));
	h.pid = pid;
	h.exe = exe;
	h.exe_known = known;
	h.exe_id = id;
	h.nancestors = n;
	h.ancestors[0] = a0;
	h.ancestors[1] = a1;
	return h;
}

static void test_profile(const char *dir)
{
	char path[512], line[1024], want[512];
	FILE *f;
	bool found = false;

	snprintf(path, sizeof(path), "%s/worker.sb", dir);
	snprintf(want, sizeof(want), "(literal \"%s\")", NTFSMAC_PATH_MACFUSE_HELPER);
	f = fopen(path, "r");
	if (f == NULL) {
		fprintf(stderr, "cannot read %s\n", path);
		CHECK(f != NULL);
		return;
	}
	while (fgets(line, sizeof(line), f) != NULL)
		if (strstr(line, want) != NULL)
			found = true;
	fclose(f);
	CHECK(found);
}

int main(int argc, char **argv)
{
	const ntfsmac_exe_id known[2] = { WORKER, HELPER };
	const ntfsmac_exe_id other = { 7, 3003 };
	const ntfsmac_exe_id same_ino_other_dev = { 8, 1001 };
	fake list[16];
	size_t i, n = 0, killed = 0;
	ntfsmac_holder deep;

	/* U-HLD-01: processes of the console user that only opened the device are kept. */
	list[n++] = (fake){ "lsof", holder(5001, "/usr/sbin/lsof", true, other, 2, 600, 1), false };
	list[n++] = (fake){ "hdiutil", holder(5002, "/usr/bin/hdiutil", true, other, 1, 600, 0), false };
	list[n++] = (fake){ "exe unreadable", holder(5003, NULL, false, WORKER, 1, 600, 0), false };
	list[n++] = (fake){ "exe path known but file gone (replaced binary)",
			    holder(5004, "/x/worker (deleted)", false, WORKER, 0, 0, 0), false };
	list[n++] = (fake){ "same inode number on another device",
			    holder(5005, "/Volumes/x/ntfsmac-worker", true, same_ino_other_dev, 1, 600, 0), false };
	/* U-HLD-02: leftovers of the worker are killed, whatever the parent chain (worker already reaped). */
	list[n++] = (fake){ "orphan running the worker binary",
			    holder(5101, "/w/ntfsmac-worker", true, WORKER, 0, 0, 0), true };
	list[n++] = (fake){ "the macFUSE helper, orphan", holder(5102, "/h/macfuse", true, HELPER, 0, 0, 0), true };
	list[n++] = (fake){ "worker binary reached by another path (symlink): same inode",
			    holder(5103, "/other/link-target", true, WORKER, 1, 1, 0), true };
	for (i = 0; i < n; i++) {
		bool k = ntfsmac_holder_is_worker_leftover(&list[i].h, -1, known, 2);

		if (k != list[i].kill)
			fprintf(stderr, "holder \"%s\": kill=%d, expected %d\n", list[i].what, k, list[i].kill);
		CHECK(k == list[i].kill);
		killed += k;
	}
	CHECK_EQ_INT((int)killed, 3);

	/* U-HLD-03: the ancestor match counts only for a live worker (pid given), at any depth of
	 * the chain; with the worker reaped (-1) the same holder is kept. */
	deep = holder(5200, "/bin/sleep", true, other, NTFSMAC_HOLDER_ANC_MAX, 0, 0);
	for (i = 0; i < NTFSMAC_HOLDER_ANC_MAX; i++)
		deep.ancestors[i] = (pid_t)(700 + i);
	CHECK(!ntfsmac_holder_is_worker_leftover(&deep, WORKER_PID, known, 2));
	deep.ancestors[NTFSMAC_HOLDER_ANC_MAX - 1] = WORKER_PID;
	CHECK(ntfsmac_holder_is_worker_leftover(&deep, WORKER_PID, known, 2));
	CHECK(!ntfsmac_holder_is_worker_leftover(&deep, -1, known, 2));
	{
		ntfsmac_holder w = holder(WORKER_PID, NULL, false, other, 1, 1, 0);

		CHECK(ntfsmac_holder_is_worker_leftover(&w, WORKER_PID, known, 2));
		CHECK(!ntfsmac_holder_is_worker_leftover(&w, -1, known, 2));
	}

	/* U-HLD-04: nothing known matches nothing; a chain that ends in pid 1 never matches a
	 * worker pid of 0 or 1. */
	{
		ntfsmac_holder a = holder(5300, "/usr/sbin/lsof", true, other, 2, 1, 0);

		CHECK(!ntfsmac_holder_is_worker_leftover(&a, 1, NULL, 0));
		CHECK(!ntfsmac_holder_is_worker_leftover(&a, 0, known, 0));
		CHECK(!ntfsmac_holder_is_worker_leftover(NULL, WORKER_PID, known, 2));
		a.exe_id = WORKER;
		CHECK(ntfsmac_holder_is_worker_leftover(&a, -1, known, 2));
		CHECK(!ntfsmac_holder_is_worker_leftover(&a, -1, NULL, 0));
	}

	/* U-HLD-06: the helper the sweep may kill is the program the sandbox lets the worker exec. */
	CHECK(argc == 2);
	if (argc == 2)
		test_profile(argv[1]);

	if (g_failures != 0) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	printf("test_holder: ok\n");
	return 0;
}
