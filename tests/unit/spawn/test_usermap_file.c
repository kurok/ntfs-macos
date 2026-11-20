/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-UMP-10..: the I/O of the usermapping file check (src/daemon/usermap_file.c, issue 2) on real
 * files in a temporary directory: the daemon opens the file itself, checks the descriptor, and
 * compares the path with it once more before the spawn. The required owner is a parameter, so the
 * accepted case runs as the current user; the daemon passes 0. No device, no mount, no root.
 *
 * usage: test_usermap_file <scratch directory (created and removed by the test)>
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "usermap_file.h"

#define DOM "S-1-5-21-1111111111-2222222222-3333333333-"
#define GOOD "# mapping\n501:20:" DOM "1001\n::" DOM "1000\n"

static char g_dir[400];

static void path_of(char *out, size_t n, const char *name)
{
	if (snprintf(out, n, "%s/%s", g_dir, name) >= (int)n) {
		fprintf(stderr, "path_of: scratch path too long\n");
		exit(2);
	}
}

/* Removes the files of the scratch directory (flat), then the directory. */
static void cleanup(void)
{
	DIR *d = opendir(g_dir);
	struct dirent *e;
	char p[512];

	if (d == NULL)
		return;
	while ((e = readdir(d)) != NULL) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		path_of(p, sizeof(p), e->d_name);
		if (unlink(p) != 0)
			(void)rmdir(p);
	}
	closedir(d);
	(void)rmdir(g_dir);
}

static void put(const char *name, const char *text, size_t len, mode_t mode)
{
	char p[512];
	int fd;

	path_of(p, sizeof(p), name);
	(void)unlink(p);
	fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0 || (len != 0 && write(fd, text, len) != (ssize_t)len) || fchmod(fd, mode) != 0) {
		fprintf(stderr, "cannot write %s: %s\n", p, strerror(errno));
		exit(2);
	}
	close(fd);
}

static ntfsmac_usermap_verdict try_open(const char *name, uid_t owner, ntfsmac_usermap_file *f,
					unsigned *line)
{
	char p[512];

	path_of(p, sizeof(p), name);
	return ntfsmac_usermap_file_open(p, owner, f, line, NULL);
}

int main(int argc, char **argv)
{
	uid_t me = geteuid();
	ntfsmac_usermap_file f;
	unsigned line = 99;
	int first_free;
	ntfsmac_usermap_verdict v;
	char p[512], q[512];
	char *big;
	size_t i;

	if (argc != 2 || strlen(argv[1]) > 300) {
		fprintf(stderr, "usage: test_usermap_file <scratch dir>\n");
		return 2;
	}
	snprintf(g_dir, sizeof(g_dir), "%s", argv[1]);
	{
		int fd = open("/dev/null", O_RDONLY);

		first_free = fd; /* the lowest free descriptor now; the same at the end means no leak */
		close(fd);
	}
	cleanup(); /* a leftover of an earlier failed run */
	if (mkdir(g_dir, 0700) != 0) {
		fprintf(stderr, "cannot create %s: %s\n", g_dir, strerror(errno));
		return 2;
	}
	ntfsmac_usermap_file_init(&f);
	CHECK_EQ_INT(f.fd, -1);

	/* U-UMP-10: a good file is accepted, the descriptor stays open and is non-inheritable. */
	put("good", GOOD, strlen(GOOD), 0644);
	CHECK_EQ_INT(try_open("good", me, &f, &line), NTFSMAC_USERMAP_V_ENABLE);
	CHECK(f.fd >= 0);
	if (f.fd >= 0) {
		CHECK((fcntl(f.fd, F_GETFD) & FD_CLOEXEC) != 0);
		/* U-UMP-11: nothing changed: the re-check just before the spawn agrees. */
		path_of(p, sizeof(p), "good");
		CHECK_EQ_INT(ntfsmac_usermap_file_reverify(p, me, &f), NTFSMAC_USERMAP_V_ENABLE);

		/* U-UMP-12: the name is made to lead to another file (a replacement of the same content,
		 * mode and owner): the descriptor is no longer the file the name leads to. */
		put("other", GOOD, strlen(GOOD), 0644);
		path_of(q, sizeof(q), "other");
		CHECK_EQ_INT(rename(q, p), 0);
		CHECK_EQ_INT(ntfsmac_usermap_file_reverify(p, me, &f), NTFSMAC_USERMAP_V_CHANGED);
		/* the descriptor still names the old file, which itself did not change */
		ntfsmac_usermap_file_close(&f);
		CHECK_EQ_INT(f.fd, -1);
		CHECK_EQ_INT(ntfsmac_usermap_file_reverify(p, me, &f), NTFSMAC_USERMAP_V_UNREADABLE);
	}

	/* U-UMP-13: the name is replaced by a link, or removed. */
	put("good", GOOD, strlen(GOOD), 0644);
	CHECK_EQ_INT(try_open("good", me, &f, &line), NTFSMAC_USERMAP_V_ENABLE);
	path_of(p, sizeof(p), "good");
	path_of(q, sizeof(q), "target");
	put("target", GOOD, strlen(GOOD), 0644);
	CHECK_EQ_INT(unlink(p), 0);
	CHECK_EQ_INT(symlink(q, p), 0);
	/* (unlinking the file also changes its ctime, which the descriptor shows: CHANGED comes first) */
	v = ntfsmac_usermap_file_reverify(p, me, &f);
	CHECK(v == NTFSMAC_USERMAP_V_GONE || v == NTFSMAC_USERMAP_V_CHANGED);
	CHECK_EQ_INT(unlink(p), 0);
	v = ntfsmac_usermap_file_reverify(p, me, &f);
	CHECK(v == NTFSMAC_USERMAP_V_GONE || v == NTFSMAC_USERMAP_V_CHANGED);
	ntfsmac_usermap_file_close(&f);

	/* U-UMP-14: the file is changed in place (append, chmod) after it was read. */
	put("good", GOOD, strlen(GOOD), 0644);
	CHECK_EQ_INT(try_open("good", me, &f, &line), NTFSMAC_USERMAP_V_ENABLE);
	path_of(p, sizeof(p), "good");
	{
		int fd = open(p, O_WRONLY | O_APPEND);

		CHECK(fd >= 0);
		CHECK_EQ_INT(write(fd, "# x\n", 4), 4);
		close(fd);
	}
	CHECK_EQ_INT(ntfsmac_usermap_file_reverify(p, me, &f), NTFSMAC_USERMAP_V_CHANGED);
	ntfsmac_usermap_file_close(&f);
	put("good", GOOD, strlen(GOOD), 0644);
	CHECK_EQ_INT(try_open("good", me, &f, &line), NTFSMAC_USERMAP_V_ENABLE);
	CHECK_EQ_INT(chmod(p, 0664), 0);
	CHECK_EQ_INT(ntfsmac_usermap_file_reverify(p, me, &f), NTFSMAC_USERMAP_V_CHANGED);
	ntfsmac_usermap_file_close(&f);

	/* U-UMP-15: refused at open, each with its own verdict and no descriptor left. */
	put("owner", GOOD, strlen(GOOD), 0644);
	CHECK_EQ_INT(try_open("owner", me + 1, &f, &line), NTFSMAC_USERMAP_V_NOT_OWNER); /* "not root" */
	CHECK_EQ_INT(f.fd, -1);
	{
		static const mode_t bad[] = { 0600, 0640, 0664, 0666, 0755, 04644 };

		for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
			put("mode", GOOD, strlen(GOOD), bad[i]);
			CHECK_EQ_INT(try_open("mode", me, &f, &line), NTFSMAC_USERMAP_V_BAD_MODE);
			CHECK_EQ_INT(f.fd, -1);
		}
	}
	path_of(p, sizeof(p), "dir");
	CHECK_EQ_INT(mkdir(p, 0755), 0);
	CHECK_EQ_INT(chmod(p, 0644), 0);
	CHECK_EQ_INT(try_open("dir", me, &f, &line), NTFSMAC_USERMAP_V_NOT_REGULAR);
	path_of(p, sizeof(p), "fifo");
	CHECK_EQ_INT(mkfifo(p, 0644), 0);
	CHECK_EQ_INT(chmod(p, 0644), 0);
	CHECK_EQ_INT(try_open("fifo", me, &f, &line), NTFSMAC_USERMAP_V_NOT_REGULAR); /* and does not block */
	path_of(p, sizeof(p), "link");
	path_of(q, sizeof(q), "target");
	CHECK_EQ_INT(symlink(q, p), 0);
	CHECK_EQ_INT(try_open("link", me, &f, &line), NTFSMAC_USERMAP_V_UNREADABLE); /* O_NOFOLLOW */
	CHECK_EQ_INT(try_open("missing", me, &f, &line), NTFSMAC_USERMAP_V_UNREADABLE);
	CHECK_EQ_INT(f.fd, -1);

	/* U-UMP-16: larger than 1 MiB is refused without being read; exactly 1 MiB is read. */
	big = malloc(NTFSMAC_USERMAP_MAX_SIZE + 1u);
	CHECK(big != NULL);
	if (big != NULL) {
		size_t pos = 22;

		memcpy(big, "::S-1-5-21-1-2-3-1000\n", 22);
		while (pos < NTFSMAC_USERMAP_MAX_SIZE) {
			size_t l = NTFSMAC_USERMAP_MAX_SIZE - pos < 100u ? NTFSMAC_USERMAP_MAX_SIZE - pos : 100u;

			memset(big + pos, '#', l);
			big[pos + l - 1u] = '\n';
			pos += l;
		}
		put("max", big, NTFSMAC_USERMAP_MAX_SIZE, 0644);
		CHECK_EQ_INT(try_open("max", me, &f, &line), NTFSMAC_USERMAP_V_ENABLE);
		ntfsmac_usermap_file_close(&f);
		big[NTFSMAC_USERMAP_MAX_SIZE] = '\n';
		put("toobig", big, NTFSMAC_USERMAP_MAX_SIZE + 1u, 0644);
		CHECK_EQ_INT(try_open("toobig", me, &f, &line), NTFSMAC_USERMAP_V_TOO_BIG);
		CHECK_EQ_INT(f.fd, -1);
		free(big);
	}

	/* U-UMP-17: content the parser refuses: no option, with the line number. */
	{
		static const char bad[] = "# c\n501:20:" DOM "1001\n::" DOM "999\n";
		static const char junk[] = "this is not a mapping\n";
		static const char empty[] = "";

		put("pat", bad, sizeof(bad) - 1u, 0644);
		line = 99;
		CHECK_EQ_INT(try_open("pat", me, &f, &line), NTFSMAC_USERMAP_V_PARSE);
		CHECK_EQ_INT(line, 3);
		CHECK_EQ_INT(f.fd, -1);
		put("junk", junk, sizeof(junk) - 1u, 0644);
		line = 99;
		CHECK_EQ_INT(try_open("junk", me, &f, &line), NTFSMAC_USERMAP_V_PARSE);
		CHECK_EQ_INT(line, 1);
		put("empty", empty, 0, 0644);
		CHECK_EQ_INT(try_open("empty", me, &f, &line), NTFSMAC_USERMAP_V_PARSE);
		CHECK_EQ_INT(f.fd, -1);
	}

	/* No descriptor is left open by the refusals: the lowest free one is what it was at the start. */
	{
		int fd = open("/dev/null", O_RDONLY);

		CHECK_EQ_INT(fd, first_free);
		close(fd);
	}

	cleanup();
	if (g_failures != 0) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	printf("test_usermap_file: ok\n");
	return 0;
}
