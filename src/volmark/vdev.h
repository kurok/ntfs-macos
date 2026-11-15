/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_VOLMARK_VDEV_H
#define NTFSMAC_VOLMARK_VDEV_H

/*
 * Device layer of ntfsmac-volmark over the inherited O_RDWR descriptor
 * (docs/ARCHITECTURE.md section 3.6: "never by path"). Like the probe's
 * devio.c, libntfs-3g gets its device through a struct ntfs_device_operations
 * built here, so no path is ever opened and the shared file offset of the
 * descriptor is never moved (seek and read keep their own offset). Unlike the
 * probe's, it can write: pwrite, write and sync are the library's own unix_io
 * functions (ntfs_device_unix_io_ops), called on the same descriptor. That
 * keeps the library's write and device-sync path unchanged (patch 0007's
 * fsync then DKIOCSYNCHRONIZE on macOS) and lets the test-only fault layer of
 * patch 0004 (--enable-faultio) see, count and cut every write volmark makes.
 * The descriptor is never closed here: it belongs to the caller.
 */

#include <stdbool.h>
#include <stdint.h>

#include <ntfs-3g/device.h>

typedef struct vdev {
	/*
	 * Must stay the first member: the unix_io functions read the descriptor as
	 * *(int *)dev->d_private (DEV_FD() in unix_io.c).
	 */
	int fd;
	bool is_block; /* S_ISBLK or S_ISCHR */
	uint64_t pos;  /* offset for seek/read/write; the descriptor's own offset is not used */
} vdev;

/*
 * Check the descriptor: open O_RDWR (anything else: EBADF), a regular file or
 * a block or character device (else ENODEV). 0 on success, -1 with errno.
 */
int vdev_init(vdev *v, int fd);

/* Bytes behind the descriptor (file size, or the device size by ioctl). 0 on success, -1 with errno. */
int vdev_size(const vdev *v, uint64_t *size);

/* The operations table for ntfs_device_alloc(); `v` is the private data. */
extern struct ntfs_device_operations vdev_ops;

#endif /* NTFSMAC_VOLMARK_VDEV_H */
