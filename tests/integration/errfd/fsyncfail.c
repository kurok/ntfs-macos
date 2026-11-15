// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * fsyncfail - T-M3-18 (I-ERR-02): test-only LD_PRELOAD library for Environment A (Linux).
 *
 * fsync() fails with EIO while the file named by NTFSMAC_FSYNCFAIL_FLAG exists; otherwise it is the
 * real call. Loaded into a worker that the case built itself; nothing of it is part of a shipped build.
 * It lets the case fail the device sync (ntfs_device_unix_io_sync) and the sync in the device close
 * without failing any write, which the fault layer of patch 0004 cannot do.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

int fsync(int fd)
{
	static int (*real)(int);
	const char *flag = getenv("NTFSMAC_FSYNCFAIL_FLAG");

	if (flag != NULL && access(flag, F_OK) == 0) {
		errno = EIO;
		return -1;
	}
	if (real == NULL)
		real = (int (*)(int))dlsym(RTLD_NEXT, "fsync");
	if (real == NULL) {
		errno = ENOSYS;
		return -1;
	}
	return real(fd);
}
