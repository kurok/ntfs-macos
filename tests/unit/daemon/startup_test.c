/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-DMN-ST-01..02: two small pieces found by the first VM run of T-M4-08.
 *  - ntfsmac_fd_reset_flags(): clears the flags a child could set on the shared open file, and
 *    does not call F_SETFL when nothing is set (a disk device node answers ENOTTY to it).
 *  - ntfsmac_spawn_exec_as(): the `--exec-as UID GID PROGRAM ARGS` step of the worker's start;
 *    only its refusals can run here (the success path execs, and needs root).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "check.h"
#include "internal.h"

static void test_reset_flags(void)
{
	char tmpl[] = "/tmp/startup_test.XXXXXX";
	int fd = mkstemp(tmpl);
	int fl;

	CHECK(fd >= 0);
	if (fd < 0)
		return;
	(void)unlink(tmpl);
	CHECK_EQ_INT(ntfsmac_fd_reset_flags(fd), 0); /* nothing set */
	CHECK(fcntl(fd, F_SETFL, O_NONBLOCK | O_APPEND) == 0);
	fl = fcntl(fd, F_GETFL);
	CHECK((fl & (O_NONBLOCK | O_APPEND)) == (O_NONBLOCK | O_APPEND));
	CHECK_EQ_INT(ntfsmac_fd_reset_flags(fd), 0);
	fl = fcntl(fd, F_GETFL);
	CHECK((fl & (O_NONBLOCK | O_APPEND | O_ASYNC)) == 0);
	CHECK((fl & O_ACCMODE) == O_RDWR); /* the access mode stays */
	close(fd);
	CHECK(ntfsmac_fd_reset_flags(-1) < 0);
}

static void test_exec_as(void)
{
	char *ok[] = {"ntfsmacd", (char *)NTFSMAC_EXEC_AS_FLAG, "501", "20", "/usr/bin/true", NULL};
	char *few[] = {"ntfsmacd", (char *)NTFSMAC_EXEC_AS_FLAG, "501", "20", NULL};
	char *root[] = {"ntfsmacd", (char *)NTFSMAC_EXEC_AS_FLAG, "0", "20", "/usr/bin/true", NULL};
	char *junk[] = {"ntfsmacd", (char *)NTFSMAC_EXEC_AS_FLAG, "5x1", "20", "/usr/bin/true", NULL};
	char *empty[] = {"ntfsmacd", (char *)NTFSMAC_EXEC_AS_FLAG, "", "20", "/usr/bin/true", NULL};
	char *rel[] = {"ntfsmacd", (char *)NTFSMAC_EXEC_AS_FLAG, "501", "20", "true", NULL};
	char *flag[] = {"ntfsmacd", (char *)"--other", "501", "20", "/usr/bin/true", NULL};
	char *gid0[] = {"ntfsmacd", (char *)NTFSMAC_EXEC_AS_FLAG, "501", "0", "/usr/bin/true", NULL};
	char *big[] = {"ntfsmacd", (char *)NTFSMAC_EXEC_AS_FLAG, "99999999999", "20", "/usr/bin/true", NULL};

	CHECK_EQ_INT(ntfsmac_spawn_exec_as(4, few), 2);
	CHECK_EQ_INT(ntfsmac_spawn_exec_as(5, root), 2);
	CHECK_EQ_INT(ntfsmac_spawn_exec_as(5, junk), 2);
	CHECK_EQ_INT(ntfsmac_spawn_exec_as(5, empty), 2);
	CHECK_EQ_INT(ntfsmac_spawn_exec_as(5, rel), 2);
	CHECK_EQ_INT(ntfsmac_spawn_exec_as(5, flag), 2);
	CHECK_EQ_INT(ntfsmac_spawn_exec_as(5, big), 2);
	CHECK_EQ_INT(ntfsmac_spawn_exec_as(5, gid0), 2);
	if (geteuid() != 0)
		CHECK_EQ_INT(ntfsmac_spawn_exec_as(5, ok), 126); /* not root: refused before any exec */
}

int main(void)
{
	test_reset_flags();
	test_exec_as();
	if (g_failures != 0) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	return 0;
}
