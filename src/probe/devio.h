/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_PROBE_DEVIO_H
#define NTFSMAC_PROBE_DEVIO_H

/*
 * Read-only device layer over the inherited descriptor (section 3.1: "Open any
 * path except the inherited fd"). libntfs-3g gets its device through
 * struct ntfs_device_operations built here instead of its own unix_io, so the
 * probe:
 *   - never calls open(2) on a path (the descriptor is only pread()),
 *   - has no write, sync or lseek of the shared descriptor: write and pwrite
 *     fail with EROFS, sync does nothing, the file offset is kept here,
 *   - can serve a few byte ranges from other places on the device (the
 *     read-only overlay view of rule M6, section 3.7). Nothing is written.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <ntfs-3g/device.h>

#define DEVIO_MAX_OVERLAYS 4

typedef struct devio_overlay {
	uint64_t dst; /* device offset that reads as ... */
	uint64_t src; /* ... the bytes found here */
	uint32_t len;
} devio_overlay;

typedef struct devio {
	int fd;
	uint64_t size; /* bytes behind the descriptor */
	bool is_block; /* S_ISBLK or S_ISCHR */
	uint64_t pos;  /* virtual file offset for seek/read */
	unsigned noverlays;
	devio_overlay overlay[DEVIO_MAX_OVERLAYS];
} devio;

/*
 * Check the descriptor (open O_RDONLY, regular file or device) and find its
 * size; a descriptor opened read-write or write-only is refused (EBADF). 0 on success, -1 with errno set.
 */
int devio_init(devio *d, int fd);

/* Add an overlay range; false if the table is full. */
bool devio_add_overlay(devio *d, uint64_t dst, uint64_t src, uint32_t len);

/* Exact read at an offset, overlay applied. 0 on success, -1 on error or EOF. */
int devio_pread(devio *d, void *buf, size_t len, uint64_t off);

/* Read up to len bytes (a short count at end of device), no overlay. */
ssize_t devio_pread_some(const devio *d, void *buf, size_t len, uint64_t off);

/* How many times the library tried to write, sync-write or truncate; each was refused (EROFS). */
unsigned long devio_refused_writes(void);

/* The operations table for ntfs_device_alloc(); `d` is the private data. */
extern struct ntfs_device_operations devio_ops;

#endif /* NTFSMAC_PROBE_DEVIO_H */
