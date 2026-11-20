/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * I/O of the usermapping file check (docs/ARCHITECTURE.md section 3.3, ADR-012). The decisions are
 * the pure functions of usermap.c; this file only opens, stats, reads and compares.
 */
#include "usermap_file.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

/* Reads exactly `size` bytes; false on an error, a short read or more data than `size`. */
static bool read_exact(int fd, char *buf, size_t size)
{
	size_t got = 0;
	char extra;

	while (got < size) {
		ssize_t r = read(fd, buf + got, size - got);

		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return false;
		got += (size_t)r;
	}
	for (;;) {
		ssize_t r = read(fd, &extra, 1);

		if (r < 0 && errno == EINTR)
			continue;
		return r == 0;
	}
}

static bool ident_of_fd(int fd, ntfsmac_usermap_ident *id)
{
	struct stat st;

	if (fstat(fd, &st) != 0)
		return false;
	ntfsmac_usermap_ident_from_stat(id, &st);
	return true;
}

/* lstat of the path (no follow); NULL if it fails. */
static const ntfsmac_usermap_ident *ident_of_path(const char *path, ntfsmac_usermap_ident *id)
{
	struct stat st;

	if (lstat(path, &st) != 0)
		return NULL;
	ntfsmac_usermap_ident_from_stat(id, &st);
	return id;
}

void ntfsmac_usermap_file_init(ntfsmac_usermap_file *f)
{
	f->fd = -1;
}

void ntfsmac_usermap_file_close(ntfsmac_usermap_file *f)
{
	if (f->fd >= 0)
		(void)close(f->fd);
	f->fd = -1;
}

ntfsmac_usermap_verdict ntfsmac_usermap_file_open(const char *path, uid_t owner,
						  ntfsmac_usermap_file *f, unsigned *bad_line,
						  ntfsmac_usermap_parse *parse_out)
{
	ntfsmac_usermap_verdict v;
	ntfsmac_usermap_parse p = NTFSMAC_USERMAP_P_OK;
	ntfsmac_usermap_ident now_fd, now_path;
	char *buf = NULL;
	size_t size;
	int fd;

	f->fd = -1;
	if (bad_line != NULL)
		*bad_line = 0;
	if (parse_out != NULL)
		*parse_out = NTFSMAC_USERMAP_P_OK;
	/* O_NONBLOCK: a FIFO at the path must not make the open (or the read) wait. */
	fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0)
		return NTFSMAC_USERMAP_V_UNREADABLE;
	if (!ident_of_fd(fd, &f->opened)) {
		(void)close(fd);
		return NTFSMAC_USERMAP_V_UNREADABLE;
	}
	v = ntfsmac_usermap_stat_verdict(&f->opened, owner);
	if (v != NTFSMAC_USERMAP_V_ENABLE) {
		(void)close(fd);
		return v;
	}
	size = (size_t)f->opened.size;
	buf = malloc(size != 0 ? size : 1u);
	if (buf == NULL || !read_exact(fd, buf, size)) {
		free(buf);
		(void)close(fd);
		return NTFSMAC_USERMAP_V_UNREADABLE;
	}
	p = ntfsmac_usermap_parse_text(buf, size, bad_line);
	free(buf);
	if (parse_out != NULL)
		*parse_out = p;
	if (!ident_of_fd(fd, &now_fd)) {
		(void)close(fd);
		return NTFSMAC_USERMAP_V_UNREADABLE;
	}
	v = ntfsmac_usermap_decide(&f->opened, owner, p, &now_fd, ident_of_path(path, &now_path));
	if (v != NTFSMAC_USERMAP_V_ENABLE) {
		(void)close(fd);
		return v;
	}
	f->fd = fd;
	f->parse = p;
	return NTFSMAC_USERMAP_V_ENABLE;
}

ntfsmac_usermap_verdict ntfsmac_usermap_file_reverify(const char *path, uid_t owner,
						      const ntfsmac_usermap_file *f)
{
	ntfsmac_usermap_ident now_fd, now_path;

	if (f == NULL || f->fd < 0)
		return NTFSMAC_USERMAP_V_UNREADABLE;
	if (!ident_of_fd(f->fd, &now_fd))
		return NTFSMAC_USERMAP_V_UNREADABLE;
	return ntfsmac_usermap_decide(&f->opened, owner, f->parse, &now_fd,
				      ident_of_path(path, &now_path));
}
