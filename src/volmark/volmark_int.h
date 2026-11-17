/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_VOLMARK_INT_H
#define NTFSMAC_VOLMARK_INT_H

/*
 * What volmark.c (modes set, clear, clear-repair) shares with syncmirror.c (mode sync-mirror).
 * Needs libntfs-3g; include after posix.h.
 */

#include <ntfs-3g/device.h>
#include <ntfs-3g/volume.h>

#include "vdev.h"

/* One line on stderr: fixed texts and errno strings only, nothing read from the disk (ADR-016). */
void volmark_say(const char *what, const char *detail);

/* ntfs_device_mount() through the descriptor layer, or NULL with errno. */
ntfs_volume *volmark_mount_fd(vdev *v, ntfs_mount_flags flags);

/* The $LogFile refusal of volume.c, through the public functions. 0 if the log is acceptable. */
int volmark_check_log(ntfs_volume *vol);

/* The volume must fit the device (the probe's check 2, size part). 0 if it fits. */
int volmark_check_fits(ntfs_volume *vol, const vdev *v);

/* `w` and `d`: one line each on stdout, delivered before the process goes on. 0, or -1. */
int volmark_progress(char c);

/* Block every catchable signal that would end the process, for the rest of the run. */
void volmark_block_termination(void);

/* Step 5: fsync, then the drive flush (macOS, ADR-022). 0, or -1 if fsync failed. */
int volmark_sync_descriptor(const vdev *v);

#endif /* NTFSMAC_VOLMARK_INT_H */
