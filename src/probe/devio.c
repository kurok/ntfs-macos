/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Read-only device layer; see devio.h. */
#include "posix.h"
#include "devio.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/disk.h>
#elif defined(__linux__)
#include <linux/fs.h>
#endif

#include <ntfs-3g/types.h>

/* pread() until len bytes or EOF or error. Returns the bytes read, or -1 if none and an error. */
static ssize_t pread_loop(int fd, void *buf, size_t len, uint64_t off)
{
	size_t done = 0;

	while (done < len) {
		ssize_t r = pread(fd, (char *)buf + done, len - done, (off_t)(off + done));

		if (r < 0) {
			if (errno == EINTR)
				continue;
			return done ? (ssize_t)done : -1;
		}
		if (r == 0)
			break;
		done += (size_t)r;
	}
	return (ssize_t)done;
}

static int device_size(int fd, const struct stat *st, uint64_t *size)
{
	if (S_ISREG(st->st_mode)) {
		if (st->st_size < 0)
			return -1;
		*size = (uint64_t)st->st_size;
		return 0;
	}
#if defined(__APPLE__)
	{
		uint64_t blocks = 0;
		uint32_t bsize = 0;

		if (ioctl(fd, DKIOCGETBLOCKCOUNT, &blocks) == 0 && ioctl(fd, DKIOCGETBLOCKSIZE, &bsize) == 0 &&
		    bsize != 0 && blocks <= UINT64_MAX / bsize) {
			*size = blocks * bsize;
			return 0;
		}
	}
#elif defined(__linux__)
	{
		uint64_t bytes = 0;

		if (ioctl(fd, BLKGETSIZE64, &bytes) == 0) {
			*size = bytes;
			return 0;
		}
	}
#endif
	errno = ENOTSUP;
	return -1;
}

int devio_init(devio *d, int fd)
{
	struct stat st;
	int fl;

	memset(d, 0, sizeof(*d));
	d->fd = fd;
	fl = fcntl(fd, F_GETFL);
	if (fl == -1)
		return -1;
	/* The daemon passes the probe an O_RDONLY descriptor; anything wider is refused, so a
	 * mistake in the caller cannot turn into a descriptor this process could write through. */
	if ((fl & O_ACCMODE) != O_RDONLY) {
		errno = EBADF;
		return -1;
	}
	if (fstat(fd, &st) != 0)
		return -1;
	if (!S_ISREG(st.st_mode) && !S_ISBLK(st.st_mode) && !S_ISCHR(st.st_mode)) {
		errno = ENODEV;
		return -1;
	}
	d->is_block = !S_ISREG(st.st_mode);
	return device_size(fd, &st, &d->size);
}

bool devio_add_overlay(devio *d, uint64_t dst, uint64_t src, uint32_t len)
{
	if (d->noverlays == DEVIO_MAX_OVERLAYS)
		return false;
	d->overlay[d->noverlays].dst = dst;
	d->overlay[d->noverlays].src = src;
	d->overlay[d->noverlays].len = len;
	d->noverlays++;
	return true;
}

ssize_t devio_pread_some(const devio *d, void *buf, size_t len, uint64_t off)
{
	return pread_loop(d->fd, buf, len, off);
}

/* Replace the parts of buf[0..n) at device offset `off` that an overlay covers. */
static int apply_overlays(const devio *d, void *buf, size_t n, uint64_t off)
{
	unsigned i;

	for (i = 0; i < d->noverlays; i++) {
		const devio_overlay *o = &d->overlay[i];
		uint64_t from = off > o->dst ? off : o->dst;
		uint64_t to = off + n < o->dst + o->len ? off + n : o->dst + o->len;

		if (from >= to)
			continue;
		if (pread_loop(d->fd, (char *)buf + (from - off), (size_t)(to - from), o->src + (from - o->dst)) !=
		    (ssize_t)(to - from)) {
			errno = EIO;
			return -1;
		}
	}
	return 0;
}

int devio_pread(devio *d, void *buf, size_t len, uint64_t off)
{
	if (len == 0)
		return 0;
	if (pread_loop(d->fd, buf, len, off) != (ssize_t)len) {
		errno = EIO;
		return -1;
	}
	return apply_overlays(d, buf, len, off);
}

/* ---- struct ntfs_device_operations ------------------------------------- */

static devio *priv(struct ntfs_device *dev)
{
	return (devio *)dev->d_private;
}

static int op_open(struct ntfs_device *dev, int flags)
{
	(void)flags; /* a read-write request is still served read-only: every write fails */
	if (NDevOpen(dev)) {
		errno = EBUSY;
		return -1;
	}
	if (priv(dev)->is_block)
		NDevSetBlock(dev);
	NDevSetReadOnly(dev);
	NDevSetOpen(dev);
	return 0;
}

static int op_close(struct ntfs_device *dev)
{
	/* The descriptor belongs to the caller and stays open. */
	NDevClearOpen(dev);
	return 0;
}

static s64 op_seek(struct ntfs_device *dev, s64 offset, int whence)
{
	devio *d = priv(dev);
	s64 base;

	switch (whence) {
	case SEEK_SET:
		base = 0;
		break;
	case SEEK_CUR:
		base = (s64)d->pos;
		break;
	case SEEK_END:
		base = (s64)d->size;
		break;
	default:
		errno = EINVAL;
		return -1;
	}
	if (offset > 0 && base > INT64_MAX - offset) {
		errno = EINVAL;
		return -1;
	}
	if (base + offset < 0) {
		errno = EINVAL;
		return -1;
	}
	d->pos = (uint64_t)(base + offset);
	return (s64)d->pos;
}

static s64 op_pread(struct ntfs_device *dev, void *buf, s64 count, s64 offset)
{
	devio *d = priv(dev);
	ssize_t r;

	if (count < 0 || offset < 0) {
		errno = EINVAL;
		return -1;
	}
	if (count == 0)
		return 0;
	r = pread(d->fd, buf, (size_t)count, (off_t)offset);
	if (r > 0 && apply_overlays(d, buf, (size_t)r, (uint64_t)offset) != 0)
		return -1;
	return r;
}

static s64 op_read(struct ntfs_device *dev, void *buf, s64 count)
{
	devio *d = priv(dev);
	s64 r = op_pread(dev, buf, count, (s64)d->pos);

	if (r > 0)
		d->pos += (uint64_t)r;
	return r;
}

static unsigned long refused_writes;

unsigned long devio_refused_writes(void)
{
	return refused_writes;
}

static s64 op_write(struct ntfs_device *dev, const void *buf, s64 count)
{
	(void)dev;
	(void)buf;
	(void)count;
	refused_writes++;
	errno = EROFS;
	return -1;
}

static s64 op_pwrite(struct ntfs_device *dev, const void *buf, s64 count, s64 offset)
{
	(void)dev;
	(void)buf;
	(void)count;
	(void)offset;
	refused_writes++;
	errno = EROFS;
	return -1;
}

static int op_sync(struct ntfs_device *dev)
{
	(void)dev; /* nothing is ever dirty */
	return 0;
}

static int op_stat(struct ntfs_device *dev, struct stat *buf)
{
	return fstat(priv(dev)->fd, buf);
}

static int op_ioctl(struct ntfs_device *dev, unsigned long request, void *argp)
{
	/* No size, geometry or block-size queries: the library falls back to its own. */
	(void)dev;
	(void)request;
	(void)argp;
	errno = ENOTTY;
	return -1;
}

struct ntfs_device_operations devio_ops = {
	.open = op_open,
	.close = op_close,
	.seek = op_seek,
	.read = op_read,
	.write = op_write,
	.pread = op_pread,
	.pwrite = op_pwrite,
	.sync = op_sync,
	.stat = op_stat,
	.ioctl = op_ioctl,
};
