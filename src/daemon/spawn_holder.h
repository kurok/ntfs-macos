/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Which holder of a device node the sweep of spawn.c may kill (docs/ARCHITECTURE.md section 4.5).
 * A pure function, so the host tests can drive it with a fake holder list.
 */
#ifndef NTFSMAC_DAEMON_SPAWN_HOLDER_H
#define NTFSMAC_DAEMON_SPAWN_HOLDER_H

#include <stdbool.h>
#include <sys/types.h>

#define NTFSMAC_HOLDER_ANC_MAX 16 /* ancestors that are looked at: parent, grandparent, ... */

/* Identity of an executable file: st_dev and st_ino of stat(path), not its name. */
typedef struct ntfsmac_exe_id {
	dev_t dev;
	ino_t ino;
} ntfsmac_exe_id;

/* One process found holding the device. */
typedef struct ntfsmac_holder {
	pid_t pid;
	pid_t ancestors[NTFSMAC_HOLDER_ANC_MAX]; /* parent first; the chain ends at pid 0 or 1 */
	unsigned nancestors;
	const char *exe; /* executable path, for logs; NULL if it could not be read */
	bool exe_known;  /* `exe_id` is valid (the path was read and its file could be stat'ed) */
	ntfsmac_exe_id exe_id;
} ntfsmac_holder;

/*
 * True only for a leftover of the worker we spawned: a process whose executable file is one of the
 * `nknown` files the worker may run (the worker binary, and the macFUSE helper the sandbox lets it
 * exec), compared by device and inode; or, while the worker is still alive (`worker_pid` > 1, not
 * yet reaped, so its pid cannot have been reused), the worker itself or a process with it in the
 * ancestor chain. After the worker is reaped its descendants are reparented and the pid is
 * unreliable: the caller passes -1 and only the executable decides. A holder whose executable is
 * unknown (unreadable, or replaced or deleted after it started) is never a match: the sweep does
 * not kill it, but it counts as still holding the device.
 */
bool ntfsmac_holder_is_worker_leftover(const ntfsmac_holder *h, pid_t worker_pid,
				       const ntfsmac_exe_id *known, unsigned nknown);

#endif /* NTFSMAC_DAEMON_SPAWN_HOLDER_H */
