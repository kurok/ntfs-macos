/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_DAEMON_UNMOUNT_IMPL_H
#define NTFSMAC_DAEMON_UNMOUNT_IMPL_H

/*
 * Private to src/daemon/unmount.c and its tests. The public entry point is
 * ntfsmac_unmount_path() in internal.h. The hooks let a test drive the retry
 * logic without a mount and without waiting.
 */

#include "internal.h"

/* Wait between two plain unmount attempts after EBUSY (T-M2-04 saw transient EBUSY). */
#define NTFSMAC_UNMOUNT_RETRY_DELAY_MS 300u
/* Upper bound for `retries`, so a bad value cannot stall a volume's queue for long. */
#define NTFSMAC_UNMOUNT_RETRIES_MAX 20u

typedef struct ntfsmac_unmount_hooks {
	/* unmount(2) of `path`; `force` adds MNT_FORCE. Returns 0 or -1 with errno. */
	int (*unmount)(void *ctx, const char *path, bool force);
	/* A process that holds the volume (cwd, open file or mapping), or 0. */
	pid_t (*find_busy)(void *ctx, const char *path);
	void (*sleep_ms)(void *ctx, unsigned ms);
	void *ctx;
} ntfsmac_unmount_hooks;

/* The real system calls. */
extern const ntfsmac_unmount_hooks ntfsmac_unmount_system_hooks;

ntfsmac_dmn_status ntfsmac_unmount_run(const char *mount_point, const ntfsmac_unmount_opts *opts,
				       pid_t *busy_pid, const ntfsmac_unmount_hooks *hooks);

#endif
