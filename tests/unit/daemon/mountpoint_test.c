/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-DMN-MP-01..08: mount point creation and removal (src/daemon/mountpoint.c)
 * in a temporary directory. The parent is owned by the test user, so no root
 * is needed; production passes /Volumes and uid 0.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "mountpoint_impl.h"

static char g_root[128];

static void make_root(void)
{
	/*
	 * A short fixed parent: the mount point path field is 128 bytes and the
	 * module reserves room for the longest leaf (67 bytes).
	 */
	char tmpl[] = "/tmp/ntfsmac-mp.XXXXXX";

	if (mkdtemp(tmpl) == NULL) {
		perror("mkdtemp");
		exit(2);
	}
	snprintf(g_root, sizeof g_root, "%s", tmpl);
}

static bool is_dir(const char *p)
{
	struct stat st;

	return lstat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool exists(const char *p)
{
	struct stat st;

	return lstat(p, &st) == 0;
}

static int count_entries(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	int n = 0;

	if (d == NULL)
		return -1;
	while ((e = readdir(d)) != NULL)
		if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
			n++;
	closedir(d);
	return n;
}

static ntfsmac_dmn_status create(const char *label, uint64_t serial, ntfsmac_mountpoint *mp)
{
	return ntfsmac_mountpoint_create_in(g_root, geteuid(), label, label ? strlen(label) : 0,
					    serial, geteuid(), getegid(), mp);
}

static void expect_leaf(const ntfsmac_mountpoint *mp, const char *leaf)
{
	char want[256];

	snprintf(want, sizeof want, "%s/%s", g_root, leaf);
	CHECK(strcmp(mp->path, want) == 0);
	if (strcmp(mp->path, want) != 0)
		fprintf(stderr, "  got  %s\n  want %s\n", mp->path, want);
}

/* U-DMN-MP-01: plain label, directory made, mode 0700, owner, identity recorded. */
static void test_basic(void)
{
	ntfsmac_mountpoint mp;
	struct stat st, fst;

	CHECK_EQ_INT(create("Backup", 0x1234, &mp), NTFSMAC_DMN_OK);
	expect_leaf(&mp, "Backup");
	CHECK(mp.dirfd >= 0);
	CHECK(is_dir(mp.path));
	CHECK_EQ_INT(stat(mp.path, &st), 0);
	CHECK_EQ_INT(fstat(mp.dirfd, &fst), 0);
	CHECK_EQ_INT(st.st_mode & 07777, 0700);
	CHECK(st.st_uid == geteuid());
	CHECK(mp.dev == st.st_dev && mp.ino == st.st_ino);
	CHECK(fst.st_ino == mp.ino);
	CHECK((fcntl(mp.dirfd, F_GETFD) & FD_CLOEXEC) != 0);
	CHECK_EQ_INT(ntfsmac_mountpoint_remove(&mp), NTFSMAC_DMN_OK);
	CHECK(mp.dirfd == -1);
	CHECK(!exists(g_root) || count_entries(g_root) == 0);
}

/* U-DMN-MP-02: hostile and odd labels give the sanitised leaf and stay inside the parent. */
static void test_sanitised(void)
{
	static const struct {
		const char *label;
		const char *leaf;
	} t[] = {
		{ "X,remove_hiberfile", "X_remove_hiberfile" },
		{ "../../x", "_._.._x" },
		{ "a/b", "a_b" },
		{ "..", "NTFS-0000ABCD" },
		{ "", "NTFS-0000ABCD" },
		{ ".hidden", "_hidden" },
		{ "  Name  ", "Name" },
		{ "a:b", "a_b" },
		{ "a\\b", "a_b" },
	};
	size_t i;

	for (i = 0; i < sizeof t / sizeof t[0]; i++) {
		ntfsmac_mountpoint mp;

		CHECK_EQ_INT(create(t[i].label, 0xABCD, &mp), NTFSMAC_DMN_OK);
		expect_leaf(&mp, t[i].leaf);
		CHECK_EQ_INT(ntfsmac_mountpoint_remove(&mp), NTFSMAC_DMN_OK);
	}
	CHECK_EQ_INT(count_entries(g_root), 0);
}

/* Invalid UTF-8 and an embedded NUL fall back or become `_`; the label is length-bounded. */
static void test_label_bytes(void)
{
	ntfsmac_mountpoint mp;
	char big[300];

	CHECK_EQ_INT(ntfsmac_mountpoint_create_in(g_root, geteuid(), "a\0b", 3, 1, geteuid(),
						 getegid(), &mp), NTFSMAC_DMN_OK);
	expect_leaf(&mp, "a_b");
	ntfsmac_mountpoint_remove(&mp);

	CHECK_EQ_INT(create("\xff\xfe", 0xAB, &mp), NTFSMAC_DMN_OK);
	expect_leaf(&mp, "NTFS-000000AB");
	ntfsmac_mountpoint_remove(&mp);

	memset(big, 'k', sizeof big - 1);
	big[sizeof big - 1] = '\0';
	CHECK_EQ_INT(create(big, 1, &mp), NTFSMAC_DMN_OK);
	CHECK(strlen(mp.path) == strlen(g_root) + 1 + 64);
	ntfsmac_mountpoint_remove(&mp);
	CHECK_EQ_INT(count_entries(g_root), 0);
}

/* U-DMN-MP-03: the base name and " 2" to " 99" (99 names), then Busy; nothing existing is reused. */
static void test_collisions(void)
{
	ntfsmac_mountpoint mp[99];
	ntfsmac_mountpoint extra;
	char leaf[80];
	char other[256];
	int i;

	for (i = 0; i < 99; i++) {
		CHECK_EQ_INT(create("Disk", 7, &mp[i]), NTFSMAC_DMN_OK);
		if (i == 0)
			snprintf(leaf, sizeof leaf, "Disk");
		else
			snprintf(leaf, sizeof leaf, "Disk %d", i + 1);
		expect_leaf(&mp[i], leaf);
	}
	CHECK_EQ_INT(create("Disk", 7, &extra), NTFSMAC_DMN_E_BUSY);
	CHECK(extra.dirfd == -1);
	CHECK(extra.path[0] == '\0');
	CHECK_EQ_INT(count_entries(g_root), 99);
	/* A different label is unaffected. */
	CHECK_EQ_INT(create("Other", 7, &extra), NTFSMAC_DMN_OK);
	ntfsmac_mountpoint_remove(&extra);
	/* Free the middle one: the next create takes the lowest free name. */
	CHECK_EQ_INT(ntfsmac_mountpoint_remove(&mp[4]), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(create("Disk", 7, &extra), NTFSMAC_DMN_OK);
	expect_leaf(&extra, "Disk 5");
	ntfsmac_mountpoint_remove(&extra);
	for (i = 0; i < 99; i++)
		if (i != 4)
			CHECK_EQ_INT(ntfsmac_mountpoint_remove(&mp[i]), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(count_entries(g_root), 0);

	/* A pre-existing file and a dangling symbolic link are never reused. */
	snprintf(other, sizeof other, "%s/File", g_root);
	CHECK_EQ_INT(close(open(other, O_CREAT | O_WRONLY, 0600)), 0);
	CHECK_EQ_INT(create("File", 1, &extra), NTFSMAC_DMN_OK);
	expect_leaf(&extra, "File 2");
	ntfsmac_mountpoint_remove(&extra);
	snprintf(other, sizeof other, "%s/Link", g_root);
	CHECK_EQ_INT(symlink("/nonexistent-ntfsmac-target", other), 0);
	CHECK_EQ_INT(create("Link", 1, &extra), NTFSMAC_DMN_OK);
	expect_leaf(&extra, "Link 2");
	ntfsmac_mountpoint_remove(&extra);
	CHECK(exists(other)); /* the link is untouched */
	unlink(other);
	snprintf(other, sizeof other, "%s/File", g_root);
	unlink(other);
}

/* U-DMN-MP-04: a pre-existing directory of the same name is not adopted or changed. */
static void test_existing_dir_untouched(void)
{
	ntfsmac_mountpoint mp;
	char pre[256];
	struct stat before, after;

	snprintf(pre, sizeof pre, "%s/Keep", g_root);
	CHECK_EQ_INT(mkdir(pre, 0755), 0);
	CHECK_EQ_INT(stat(pre, &before), 0);
	CHECK_EQ_INT(create("Keep", 1, &mp), NTFSMAC_DMN_OK);
	expect_leaf(&mp, "Keep 2");
	CHECK_EQ_INT(stat(pre, &after), 0);
	CHECK(before.st_ino == after.st_ino && (after.st_mode & 07777) == 0755);
	CHECK(mp.ino != before.st_ino);
	ntfsmac_mountpoint_remove(&mp);
	CHECK_EQ_INT(rmdir(pre), 0);
}

/* U-DMN-MP-05: remove refuses a non-empty directory, then works after it is emptied. */
static void test_remove_nonempty(void)
{
	ntfsmac_mountpoint mp;
	char f[256];

	CHECK_EQ_INT(create("Full", 1, &mp), NTFSMAC_DMN_OK);
	snprintf(f, sizeof f, "%s/x", mp.path);
	CHECK_EQ_INT(close(open(f, O_CREAT | O_WRONLY, 0600)), 0);
	CHECK_EQ_INT(ntfsmac_mountpoint_remove(&mp), NTFSMAC_DMN_E_BUSY);
	CHECK(mp.dirfd >= 0); /* record kept: the call can be repeated */
	CHECK(is_dir(mp.path));
	CHECK_EQ_INT(unlink(f), 0);
	CHECK_EQ_INT(ntfsmac_mountpoint_remove(&mp), NTFSMAC_DMN_OK);
	CHECK(!exists(mp.path));
}

/* U-DMN-MP-06: remove never removes a directory that is not the recorded one. */
static void test_remove_identity(void)
{
	ntfsmac_mountpoint mp;
	char moved[256];
	char sub[256];

	CHECK_EQ_INT(create("Swap", 1, &mp), NTFSMAC_DMN_OK);
	/* Somebody renames ours away and plants another directory under the name. */
	snprintf(moved, sizeof moved, "%s/moved", g_root);
	CHECK_EQ_INT(rename(mp.path, moved), 0);
	CHECK_EQ_INT(mkdir(mp.path, 0700), 0);
	CHECK_EQ_INT(ntfsmac_mountpoint_remove(&mp), NTFSMAC_DMN_E_DENIED);
	CHECK(is_dir(mp.path)); /* the stranger stays */
	CHECK(mp.dirfd == -1);
	CHECK_EQ_INT(rmdir(mp.path), 0);
	CHECK_EQ_INT(rmdir(moved), 0);

	/* A symbolic link under the name is not followed and not removed. */
	CHECK_EQ_INT(create("Link2", 1, &mp), NTFSMAC_DMN_OK);
	snprintf(sub, sizeof sub, "%s/target", g_root);
	CHECK_EQ_INT(mkdir(sub, 0700), 0);
	CHECK_EQ_INT(rename(mp.path, moved), 0);
	CHECK_EQ_INT(symlink(sub, mp.path), 0);
	CHECK_EQ_INT(ntfsmac_mountpoint_remove(&mp), NTFSMAC_DMN_E_DENIED);
	CHECK(exists(mp.path) && !is_dir(mp.path));
	CHECK(is_dir(sub));
	unlink(mp.path);
	rmdir(sub);
	rmdir(moved);

	/* Already gone counts as removed. */
	CHECK_EQ_INT(create("Gone", 1, &mp), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(rmdir(mp.path), 0);
	CHECK_EQ_INT(ntfsmac_mountpoint_remove(&mp), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(count_entries(g_root), 0);
}

/* U-DMN-MP-07: argument checks and parent checks. */
static void test_arguments(void)
{
	ntfsmac_mountpoint mp;
	char longparent[256];
	char link[256];
	char real[256];

	memset(&mp, 0x55, sizeof mp);
	CHECK_EQ_INT(ntfsmac_mountpoint_create_in(g_root, geteuid(), "a", 1, 1, 0, 0, NULL),
		     NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_mountpoint_create_in(NULL, 0, "a", 1, 1, 0, 0, &mp), NTFSMAC_DMN_E_INVAL);
	CHECK(mp.dirfd == -1 && mp.path[0] == '\0'); /* output is reset on every failure */
	CHECK_EQ_INT(ntfsmac_mountpoint_create_in("relative/dir", 0, "a", 1, 1, 0, 0, &mp),
		     NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_mountpoint_create_in(g_root, geteuid(), NULL, 3, 1, 0, 0, &mp),
		     NTFSMAC_DMN_E_INVAL);
	/* NULL label with length 0 is the empty label: fallback name. */
	CHECK_EQ_INT(ntfsmac_mountpoint_create_in(g_root, geteuid(), NULL, 0, 0xF, geteuid(),
						 getegid(), &mp), NTFSMAC_DMN_OK);
	expect_leaf(&mp, "NTFS-0000000F");
	ntfsmac_mountpoint_remove(&mp);

	/* Parent too long for the 128-byte path field. */
	memset(longparent, 'p', sizeof longparent);
	longparent[0] = '/';
	longparent[100] = '\0';
	CHECK_EQ_INT(ntfsmac_mountpoint_create_in(longparent, 0, "a", 1, 1, 0, 0, &mp),
		     NTFSMAC_DMN_E_INVAL);

	/* Missing parent. */
	snprintf(real, sizeof real, "%s/nope", g_root);
	CHECK_EQ_INT(ntfsmac_mountpoint_create_in(real, geteuid(), "a", 1, 1, 0, 0, &mp),
		     NTFSMAC_DMN_E_IO);
	/* Wrong owner of the parent. */
	CHECK_EQ_INT(ntfsmac_mountpoint_create_in(g_root, geteuid() + 1, "a", 1, 1, geteuid(),
						 getegid(), &mp), NTFSMAC_DMN_E_DENIED);
	/* A parent that is a symbolic link is refused (O_NOFOLLOW). */
	snprintf(real, sizeof real, "%s/real", g_root);
	snprintf(link, sizeof link, "%s/link", g_root);
	CHECK_EQ_INT(mkdir(real, 0700), 0);
	CHECK_EQ_INT(symlink(real, link), 0);
	CHECK_EQ_INT(ntfsmac_mountpoint_create_in(link, geteuid(), "a", 1, 1, geteuid(), getegid(),
						 &mp), NTFSMAC_DMN_E_IO);
	CHECK_EQ_INT(count_entries(real), 0);
	/* A parent that is not a directory. */
	snprintf(real, sizeof real, "%s/afile", g_root);
	CHECK_EQ_INT(close(open(real, O_CREAT | O_WRONLY, 0600)), 0);
	CHECK_EQ_INT(ntfsmac_mountpoint_create_in(real, geteuid(), "a", 1, 1, geteuid(), getegid(),
						 &mp), NTFSMAC_DMN_E_IO);
	unlink(real);
	unlink(link);
	snprintf(real, sizeof real, "%s/real", g_root);
	rmdir(real);

	CHECK_EQ_INT(ntfsmac_mountpoint_remove(NULL), NTFSMAC_DMN_E_INVAL);
	memset(&mp, 0, sizeof mp);
	mp.dirfd = -1;
	CHECK_EQ_INT(ntfsmac_mountpoint_remove(&mp), NTFSMAC_DMN_E_INVAL);
	snprintf(mp.path, sizeof mp.path, "%s", "/");
	CHECK_EQ_INT(ntfsmac_mountpoint_remove(&mp), NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(count_entries(g_root), 0);
}

/* U-DMN-MP-08: the production entry point uses /Volumes and root: as a normal user it must refuse. */
static void test_production_entry(void)
{
	ntfsmac_mountpoint mp;
	ntfsmac_dmn_status s;

	s = ntfsmac_mountpoint_create("Zz-ntfsmac-test", 15, 1, geteuid(), getegid(), &mp);
	if (geteuid() != 0) {
		/* /Volumes is owned by root: the owner check or mkdirat refuses; nothing is made. */
		CHECK(s != NTFSMAC_DMN_OK);
		CHECK(mp.dirfd == -1);
		CHECK(!exists("/Volumes/Zz-ntfsmac-test"));
	} else {
		printf("SKIP: mountpoint production entry refusal: runs only when not root\n");
		if (s == NTFSMAC_DMN_OK)
			ntfsmac_mountpoint_remove(&mp);
	}
}

int main(void)
{
	make_root();
	test_basic();
	test_sanitised();
	test_label_bytes();
	test_collisions();
	test_existing_dir_untouched();
	test_remove_nonempty();
	test_remove_identity();
	test_arguments();
	test_production_entry();
	if (rmdir(g_root) != 0)
		fprintf(stderr, "note: %s not empty\n", g_root);
	if (g_failures != 0) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	return 0;
}
