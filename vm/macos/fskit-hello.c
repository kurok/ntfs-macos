/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Minimal read-only libfuse3 file system used to check that the macFUSE FSKit backend
 * works in the golden VM (T-M2-02). One file, /hello.
 *
 * Build in the guest (standard libfuse3 API, not macFUSE's Darwin extensions):
 *   cc -Wall -Wextra -DFUSE_DARWIN_ENABLE_EXTENSIONS=0 -I/usr/local/include/fuse3 \
 *      -o fskit-hello fskit-hello.c -L/usr/local/lib -lfuse3
 * Run: ./fskit-hello -f -o backend=fskit MOUNTPOINT
 */
#define FUSE_USE_VERSION 31
#include <errno.h>
#include <fuse.h>
#include <string.h>

static const char *msg = "hello from fskit\n";

static int h_getattr(const char *p, struct stat *st, struct fuse_file_info *fi)
{
	(void)fi;
	memset(st, 0, sizeof(*st));
	if (strcmp(p, "/") == 0) {
		st->st_mode = S_IFDIR | 0755;
		st->st_nlink = 2;
		return 0;
	}
	if (strcmp(p, "/hello") == 0) {
		st->st_mode = S_IFREG | 0444;
		st->st_nlink = 1;
		st->st_size = (off_t)strlen(msg);
		return 0;
	}
	return -ENOENT;
}

static int h_readdir(const char *p, void *b, fuse_fill_dir_t fill, off_t o,
		     struct fuse_file_info *fi, enum fuse_readdir_flags fl)
{
	(void)o;
	(void)fi;
	(void)fl;
	if (strcmp(p, "/") != 0)
		return -ENOENT;
	fill(b, ".", NULL, 0, 0);
	fill(b, "..", NULL, 0, 0);
	fill(b, "hello", NULL, 0, 0);
	return 0;
}

static int h_open(const char *p, struct fuse_file_info *fi)
{
	(void)fi;
	return strcmp(p, "/hello") == 0 ? 0 : -ENOENT;
}

static int h_read(const char *p, char *buf, size_t n, off_t o, struct fuse_file_info *fi)
{
	size_t len = strlen(msg);

	(void)fi;
	if (strcmp(p, "/hello") != 0)
		return -ENOENT;
	if ((size_t)o >= len)
		return 0;
	if ((size_t)o + n > len)
		n = len - (size_t)o;
	memcpy(buf, msg + o, n);
	return (int)n;
}

static const struct fuse_operations ops = {
	.getattr = h_getattr,
	.readdir = h_readdir,
	.open = h_open,
	.read = h_read,
};

int main(int argc, char *argv[])
{
	return fuse_main(argc, argv, &ops, NULL);
}
