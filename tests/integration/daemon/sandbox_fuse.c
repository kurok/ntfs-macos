/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * T-M4-06: stand-in for a compromised worker that asks macFUSE for a second mount (I-SBX-09,
 * R-07). It serves one small file system with libfuse, in the foreground, on the directory
 * `victim` next to this program, and says on stderr what happened:
 *
 *   FUSE2-START <dir>        about to mount
 *   FUSE2-RETURN <what>      the mount failed, or the file system was unmounted
 *   FUSE2-SERVING            the first request reached this process, so the mount is live
 *
 * sandbox_vm starts it through the spawn module under the worker profile, as the console user,
 * and looks at the mount table and at the files the mount serves. The arguments the spawn
 * module gives a worker (device path, mount point, -o options) are not used.
 */
#define FUSE_USE_VERSION 26
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <limits.h>
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char g_leaf[] = "pwned.txt";
static const char g_name[] = "/pwned.txt";
static const char g_text[] = "served by the second mount\n";

static int say_serving(void)
{
	static int said;

	if (!said) {
		said = 1;
		fprintf(stderr, "FUSE2-SERVING\n");
	}
	return 0;
}

static int op_getattr(const char *path, struct stat *st)
{
	say_serving();
	memset(st, 0, sizeof(*st));
	if (strcmp(path, "/") == 0) {
		st->st_mode = S_IFDIR | 0755;
		st->st_nlink = 2;
		return 0;
	}
	if (strcmp(path, g_name) == 0) {
		st->st_mode = S_IFREG | 0644;
		st->st_nlink = 1;
		st->st_size = (off_t)strlen(g_text);
		return 0;
	}
	return -ENOENT;
}

static int op_readdir(const char *path, void *buf, fuse_fill_dir_t fill, off_t off, struct fuse_file_info *fi)
{
	(void)off;
	(void)fi;
	if (strcmp(path, "/") != 0)
		return -ENOENT;
	fill(buf, ".", NULL, 0);
	fill(buf, "..", NULL, 0);
	fill(buf, g_leaf, NULL, 0);
	return 0;
}

static int op_open(const char *path, struct fuse_file_info *fi)
{
	(void)fi;
	return strcmp(path, g_name) == 0 ? 0 : -ENOENT;
}

static int op_read(const char *path, char *buf, size_t size, off_t off, struct fuse_file_info *fi)
{
	size_t len = strlen(g_text);

	(void)fi;
	if (strcmp(path, g_name) != 0)
		return -ENOENT;
	if ((size_t)off >= len)
		return 0;
	if (size > len - (size_t)off)
		size = len - (size_t)off;
	memcpy(buf, g_text + off, size);
	return (int)size;
}

static const struct fuse_operations g_ops = {
	.getattr = op_getattr,
	.readdir = op_readdir,
	.open = op_open,
	.read = op_read,
};

int main(int argc, char **argv)
{
	/* `victim` in the directory of this program; macFUSE's FSKit backend, as the real worker uses it. */
	char dir[PATH_MAX];
	struct fuse_args args = FUSE_ARGS_INIT(0, NULL);
	struct fuse_chan *ch;
	struct fuse *fuse;
	char *slash;
	int rc;

	if (argc < 1 || strlen(argv[0]) + sizeof("/victim") >= sizeof(dir))
		return 2;
	snprintf(dir, sizeof(dir), "%s", argv[0]);
	slash = strrchr(dir, '/');
	if (slash == NULL)
		return 2;
	snprintf(slash, sizeof(dir) - (size_t)(slash - dir), "/victim");
	fprintf(stderr, "FUSE2-START %s\n", dir);
	/*
	 * The calls NTFS-3G makes (fuse_mount, fuse_new, fuse_loop), not fuse_main(): fuse_main also
	 * opens a Disk Arbitration session, which the worker profile does not allow and the real
	 * worker does not need, and it fails without it after the mount is made.
	 */
	if (fuse_opt_add_arg(&args, argv[0]) != 0 || fuse_opt_add_arg(&args, "-o") != 0
	    || fuse_opt_add_arg(&args, "backend=fskit,local") != 0)
		return 2;
	ch = fuse_mount(dir, &args);
	if (ch == NULL) {
		fprintf(stderr, "FUSE2-RETURN fuse_mount failed\n");
		return 1;
	}
	fuse = fuse_new(ch, &args, &g_ops, sizeof(g_ops), NULL);
	if (fuse == NULL) {
		fuse_unmount(dir, ch);
		fprintf(stderr, "FUSE2-RETURN fuse_new failed\n");
		return 1;
	}
	rc = fuse_loop(fuse);
	fuse_unmount(dir, ch);
	fuse_destroy(fuse);
	fuse_opt_free_args(&args);
	fprintf(stderr, "FUSE2-RETURN %d\n", rc);
	return rc == 0 ? 0 : 1;
}
