/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_DAEMON_MOUNTPOINT_IMPL_H
#define NTFSMAC_DAEMON_MOUNTPOINT_IMPL_H

/*
 * Private to src/daemon/mountpoint.c and its tests. The public entry points
 * are in internal.h.
 */

#include "internal.h"

/* The directory under which mount points are made in production. */
#define NTFSMAC_VOLUMES_DIR "/Volumes"

/*
 * Same as ntfsmac_mountpoint_create(), for any parent directory. The parent
 * must be an absolute path to a directory owned by `parent_owner` (root for
 * /Volumes). The created directory is owned by console_uid:console_gid, mode
 * 0700, and its path is `<parent>/<leaf>`.
 */
ntfsmac_dmn_status ntfsmac_mountpoint_create_in(const char *parent, uid_t parent_owner,
						const char *label, size_t label_len,
						uint64_t serial, uid_t console_uid,
						gid_t console_gid, ntfsmac_mountpoint *mp);

#endif
