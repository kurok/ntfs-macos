/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Read/write device layer of ntfsmac-volmark; see vdev.h. */
#include "posix.h"
#include "vdev.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/disk.h>
#elif defined(__linux__)
#include <linux/fs.h>
#endif

#include <ntfs-3g/types.h>

/*
 * libntfs-3g's own unix device operations (unix_io.c). Exported by the static
 * library; declared here because device_io.h needs the library's config.h.
 */
extern struct ntfs_device_operations ntfs_device_unix_io_ops;

/* DEV_FD() of unix_io.c is *(int *)d_private. */
_Static_assert(offsetof(vdev, fd) == 0, "vdev.fd must be the first member");

int vdev_init(vdev *v, int fd)
{
	struct stat st;
	int fl;

	memset(v, 0, sizeof(*v));
	v->fd = fd;
	fl = fcntl(fd, F_GETFL);
	if (fl == -1)
		return -1;
	/* The daemon passes an O_RDWR descriptor (section 3.6); anything else is a caller mistake. */
	if ((fl & O_ACCMODE) != O_RDWR) {
		errno = EBADF;
		return -1;
	}
	if (fstat(fd, &st) != 0)
		return -1;
	if (!S_ISREG(st.st_mode) && !S_ISBLK(st.st_mode) && !S_ISCHR(st.st_mode)) {
		errno = ENODEV;
		return -1;
	}
	v->is_block = !S_ISREG(st.st_mode);
	return 0;
}

int vdev_size(const vdev *v, uint64_t *size)
{
	struct stat st;

	if (fstat(v->fd, &st) != 0)
		return -1;
	if (S_ISREG(st.st_mode)) {
		if (st.st_size < 0) {
			errno = EINVAL;
			return -1;
		}
		*size = (uint64_t)st.st_size;
		return 0;
	}
#if defined(__APPLE__)
	{
		uint64_t blocks = 0;
		uint32_t bsize = 0;

		if (ioctl(v->fd, DKIOCGETBLOCKCOUNT, &blocks) == 0 && ioctl(v->fd, DKIOCGETBLOCKSIZE, &bsize) == 0 &&
		    bsize != 0 && blocks <= UINT64_MAX / bsize) {
			*size = blocks * bsize;
			return 0;
		}
	}
#elif defined(__linux__)
	{
		uint64_t bytes = 0;

		if (ioctl(v->fd, BLKGETSIZE64, &bytes) == 0) {
			*size = bytes;
			return 0;
		}
	}
#endif
	errno = ENOTSUP;
	return -1;
}

bool vdev_add_overlay(vdev *v, uint64_t dst, uint64_t src, uint32_t len)
{
	if (len == 0 || v->noverlays == VDEV_MAX_OVERLAYS)
		return false;
	v->overlay[v->noverlays].dst = dst;
	v->overlay[v->noverlays].src = src;
	v->overlay[v->noverlays].len = len;
	v->noverlays++;
	return true;
}

static vdev *priv(struct ntfs_device *dev)
{
	return (vdev *)dev->d_private;
}

/* Exact pread of the source bytes of an overlay; 0, or -1 with errno. */
static int pread_all(int fd, void *buf, size_t len, uint64_t off)
{
	uint8_t *p = buf;

	while (len > 0) {
		ssize_t r = pread(fd, p, len, (off_t)off);

		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0) {
			if (r == 0)
				errno = EIO;
			return -1;
		}
		p += r;
		off += (uint64_t)r;
		len -= (size_t)r;
	}
	return 0;
}

/* Put the overlay bytes over buf[0..n), which was read at device offset `off`. 0, or -1 with errno. */
static int apply_overlays(const vdev *v, void *buf, size_t n, uint64_t off)
{
	unsigned i;

	for (i = 0; i < v->noverlays; i++) {
		uint64_t dst = v->overlay[i].dst, len = v->overlay[i].len;
		uint64_t from = off > dst ? off : dst;
		uint64_t to = off + n < dst + len ? off + n : dst + len;

		if (from >= to)
			continue;
		if (pread_all(v->fd, (uint8_t *)buf + (from - off), (size_t)(to - from),
			      v->overlay[i].src + (from - dst)) != 0)
			return -1;
	}
	return 0;
}

static int op_open(struct ntfs_device *dev, int flags)
{
	if (NDevOpen(dev)) {
		errno = EBUSY;
		return -1;
	}
	/*
	 * vdev_init() accepted only an O_RDWR descriptor, so a read/write request
	 * never fails here; in particular never with EROFS, on which
	 * ntfs_device_mount() would silently retry read-only.
	 */
	if (priv(dev)->is_block)
		NDevSetBlock(dev);
	if ((flags & O_ACCMODE) != O_RDWR)
		NDevSetReadOnly(dev);
	else
		NDevClearReadOnly(dev);
	NDevClearDirty(dev);
	priv(dev)->pos = 0;
	NDevSetOpen(dev);
	return 0;
}

static int op_close(struct ntfs_device *dev)
{
	int rc = 0;

	if (!NDevOpen(dev)) {
		errno = EBADF;
		return -1;
	}
	/*
	 * As unix_io's close: a dirty device is synced first, through the library's sync. A
	 * failed sync is reported, but the device is closed anyway (the descriptor belongs to
	 * the caller and stays open), so that ntfs_device_free() can release it.
	 */
	if (NDevDirty(dev) && ntfs_device_unix_io_ops.sync(dev))
		rc = -1;
	NDevClearOpen(dev);
	return rc;
}

static s64 op_seek(struct ntfs_device *dev, s64 offset, int whence)
{
	vdev *v = priv(dev);
	struct stat st;
	s64 base;

	switch (whence) {
	case SEEK_SET:
		base = 0;
		break;
	case SEEK_CUR:
		base = (s64)v->pos;
		break;
	case SEEK_END:
		/* Only regular files: the library sizes devices by reading (no ioctl is served). */
		if (fstat(v->fd, &st) != 0)
			return -1;
		if (!S_ISREG(st.st_mode)) {
			errno = EINVAL;
			return -1;
		}
		base = (s64)st.st_size;
		break;
	default:
		errno = EINVAL;
		return -1;
	}
	if ((offset > 0 && base > INT64_MAX - offset) || base + offset < 0) {
		errno = EINVAL;
		return -1;
	}
	v->pos = (uint64_t)(base + offset);
	return (s64)v->pos;
}

static s64 op_pread(struct ntfs_device *dev, void *buf, s64 count, s64 offset)
{
	s64 r = ntfs_device_unix_io_ops.pread(dev, buf, count, offset);

	if (r > 0 && priv(dev)->noverlays != 0 && apply_overlays(priv(dev), buf, (size_t)r, (uint64_t)offset) != 0)
		return -1;
	return r;
}

static s64 op_read(struct ntfs_device *dev, void *buf, s64 count)
{
	vdev *v = priv(dev);
	s64 r = op_pread(dev, buf, count, (s64)v->pos);

	if (r > 0)
		v->pos += (uint64_t)r;
	return r;
}

static s64 op_pwrite(struct ntfs_device *dev, const void *buf, s64 count, s64 offset)
{
	if (priv(dev)->noverlays != 0) {
		errno = EPERM; /* a view is never written through */
		return -1;
	}
	/* Refuses with EROFS on a read-only open and marks the device dirty. */
	return ntfs_device_unix_io_ops.pwrite(dev, buf, count, offset);
}

static s64 op_write(struct ntfs_device *dev, const void *buf, s64 count)
{
	vdev *v = priv(dev);
	s64 r;

	if (v->noverlays != 0) {
		errno = EPERM;
		return -1;
	}
	r = ntfs_device_unix_io_ops.pwrite(dev, buf, count, (s64)v->pos);

	if (r > 0)
		v->pos += (uint64_t)r;
	return r;
}

static int op_sync(struct ntfs_device *dev)
{
	return ntfs_device_unix_io_ops.sync(dev);
}

static int op_stat(struct ntfs_device *dev, struct stat *buf)
{
	return fstat(priv(dev)->fd, buf);
}

static int op_ioctl(struct ntfs_device *dev, unsigned long request, void *argp)
{
	/* No size, geometry or block-size queries: the library falls back to its own (as the probe). */
	(void)dev;
	(void)request;
	(void)argp;
	errno = ENOTTY;
	return -1;
}

struct ntfs_device_operations vdev_ops = {
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
