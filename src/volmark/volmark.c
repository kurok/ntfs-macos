/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ntfsmac-volmark `set`, `clear` and `clear-repair`: steps 1 to 6 of
 * docs/ARCHITECTURE.md section 3.6 (ADR-006, ADR-017, ADR-020).
 *
 *   1  ntfs_device_mount() read/write with NTFS_MNT_FORENSIC only. In
 *      volume.c that skips the hibernation, $LogFile and fix_txf_data()
 *      block, so this mount writes nothing. The volume must fit the device
 *      (as the probe's check 2). Then the same refusals through
 *      the public API: ntfs_volume_check_hiberfile(vol, 1), and
 *      ntfs_check_logfile() + ntfs_is_logfile_clean() with the restart page
 *      2.0 test of the static ntfs_volume_check_logfile(). Exit 10.
 *   2  Binding (ADR-020): sector 0 through the device layer; serial and
 *      sector count must equal the expected values. Exit 15.
 *   3  The raw on-disk flags decide (volmark_next_flags(), args.c). Exit 11
 *      (set) or 13 (clear, clear-repair).
 *   4  `w` on stdout, ntfs_volume_write_flags() with only VOLUME_IS_DIRTY
 *      changed, `d` when it returned 0. This is the only write: record 3 to
 *      $MFT, then to $MFTMirr (ntfs_mft_records_write()). A failed write is
 *      not repeated by the unmount (exit 12).
 *   5  ntfs_umount(); fsync() of the descriptor and the drive flush
 *      (DKIOCSYNCHRONIZE on a device node, F_FULLFSYNC on a regular file;
 *      macOS only, ADR-022). fsync or unmount failure: exit 12; a failed
 *      flush is a warning (R-30).
 *   6  A NTFS_MNT_RDONLY mount reads the flags back (that mount also
 *      compares records 0 to 3 of $MFT and $MFTMirr). Exit 14.
 * Nothing before step 4 can write: every refusal leaves the device as it was.
 * From `w` on, catchable termination signals are blocked so that only
 * SIGKILL (which the daemon does not send until `d` or the 120 s limit) can
 * stop the process inside the write.
 */
#include "posix.h"
#include "volmark.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/disk.h>
#endif

#include <ntfs-3g/attrib.h>
#include <ntfs-3g/device.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/layout.h>
#include <ntfs-3g/logfile.h>
#include <ntfs-3g/logging.h>
#include <ntfs-3g/volume.h>

#include "../common/mftmirr.h"
#include "vdev.h"
#include "volmark_int.h"

static const char *const mode_name[] = { "set", "clear", "clear-repair", "sync-mirror" };

static bool debug;

/* One line on stderr. Fixed texts and errno strings only: nothing read from the disk (ADR-016). */
void volmark_say(const char *what, const char *detail)
{
	fprintf(stderr, "ntfsmac-volmark: %s%s%s\n", what, detail[0] ? ": " : "", detail);
}

/* ntfs_device_mount() through the descriptor layer, or NULL. */
ntfs_volume *volmark_mount_fd(vdev *v, ntfs_mount_flags flags)
{
	struct ntfs_device *dev = ntfs_device_alloc("ntfsmac-volmark-fd", 0, &vdev_ops, v);
	ntfs_volume *vol;

	if (!dev)
		return NULL;
	vol = ntfs_device_mount(dev, flags);
	if (!vol) {
		int e = errno;

		/* A failed ntfs_device_mount() closes the device but does not free it. */
		ntfs_device_free(dev);
		errno = e;
	}
	return vol;
}

/*
 * The $LogFile refusal of ntfs_volume_check_logfile() (static in volume.c),
 * through the public functions: a log that does not check, is not clean, or
 * whose newest restart page is version 2.0 (Windows Fast Startup) is refused.
 * Returns 0 if the log is acceptable.
 */
int volmark_check_log(ntfs_volume *vol)
{
	ntfs_inode *ni;
	ntfs_attr *na;
	RESTART_PAGE_HEADER *rp = NULL;
	int bad = 0;

	ni = ntfs_inode_open(vol, FILE_LogFile);
	if (!ni)
		return -1;
	na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
	if (!na) {
		ntfs_inode_close(ni);
		return -1;
	}
	if (!ntfs_check_logfile(na, &rp) || !ntfs_is_logfile_clean(na, rp))
		bad = 1;
	if (rp && rp->major_ver == const_cpu_to_le16(2) && rp->minor_ver == const_cpu_to_le16(0))
		bad = 1;
	free(rp);
	ntfs_attr_close(na);
	if (ntfs_inode_close(ni))
		bad = 1;
	return bad ? -1 : 0;
}

/*
 * Step 1, size part (as the probe's check 2): the volume must fit the device.
 * The engine mounts a volume cut short as long as its metadata lies in the
 * part that is there (fixture truncated-60); the probe calls it `corrupt`, and
 * volmark refuses it the same way. 0 if it fits.
 */
int volmark_check_fits(ntfs_volume *vol, const vdev *v)
{
	struct ntfs_device *dev = vol->dev;
	uint8_t boot[512];
	ntfsmac_mm_geometry g;
	uint64_t size;

	if (dev->d_ops->pread(dev, boot, (s64)sizeof(boot), 0) != (s64)sizeof(boot) ||
	    ntfsmac_mm_parse_boot(boot, &g) != 0 || vdev_size(v, &size) != 0)
		return -1;
	if (g.total_sectors > UINT64_MAX / g.sector_size || g.total_sectors * g.sector_size > size)
		return -1;
	return 0;
}

/* Step 2: sector 0 through the device layer against the expected serial and size. */
static int check_binding(ntfs_volume *vol, const volmark_args *a)
{
	struct ntfs_device *dev = vol->dev;
	uint8_t boot[512];
	ntfsmac_mm_geometry g;

	if (dev->d_ops->pread(dev, boot, (s64)sizeof(boot), 0) != (s64)sizeof(boot)) {
		volmark_say("binding", "sector 0 is not readable");
		return VOLMARK_EXIT_BINDING;
	}
	if (ntfsmac_mm_parse_boot(boot, &g) != 0) {
		volmark_say("binding", "sector 0 is not a usable NTFS boot sector");
		return VOLMARK_EXIT_BINDING;
	}
	if (g.serial != a->expect_serial) {
		volmark_say("binding", "volume serial number differs from the expected value");
		return VOLMARK_EXIT_BINDING;
	}
	if (g.total_sectors != a->expect_sectors) {
		volmark_say("binding", "number of sectors differs from the expected value");
		return VOLMARK_EXIT_BINDING;
	}
	return VOLMARK_EXIT_OK;
}

/* `w` and `d`: one line each, delivered before the process goes on. */
int volmark_progress(char c)
{
	if (printf("%c\n", c) < 0 || fflush(stdout) != 0)
		return -1;
	return 0;
}

/* Block every catchable signal that would end the process, for the rest of the run. */
void volmark_block_termination(void)
{
	sigset_t s;

	sigemptyset(&s);
	sigaddset(&s, SIGTERM);
	sigaddset(&s, SIGINT);
	sigaddset(&s, SIGHUP);
	sigaddset(&s, SIGQUIT);
	sigaddset(&s, SIGALRM);
	sigaddset(&s, SIGPIPE);
	sigaddset(&s, SIGUSR1);
	sigaddset(&s, SIGUSR2);
	sigaddset(&s, SIGTSTP);
	(void)sigprocmask(SIG_BLOCK, &s, NULL);
}

/* Step 5 after the unmount: fsync, then the drive flush (macOS, ADR-022). */
int volmark_sync_descriptor(const vdev *v)
{
	if (fsync(v->fd) != 0) {
		volmark_say("fsync of the device failed", strerror(errno));
		return -1;
	}
#if defined(__APPLE__)
	if (v->is_block) {
		dk_synchronize_t dk;

		memset(&dk, 0, sizeof(dk));
		if (ioctl(v->fd, DKIOCSYNCHRONIZE, &dk) != 0)
			volmark_say("warning: DKIOCSYNCHRONIZE failed", strerror(errno));
	} else if (fcntl(v->fd, F_FULLFSYNC) == -1) {
		volmark_say("warning: F_FULLFSYNC failed", strerror(errno));
	}
#endif
	return 0;
}

/* Step 6: a read-only mount reads the flags back. */
static int read_back(vdev *v, uint16_t want)
{
	ntfs_volume *vol = volmark_mount_fd(v, NTFS_MNT_RDONLY);
	uint16_t got;

	if (!vol) {
		volmark_say("read-back: read-only mount failed", strerror(errno));
		return VOLMARK_EXIT_READBACK;
	}
	got = le16_to_cpu(vol->flags);
	ntfs_umount(vol, FALSE);
	if (got != want) {
		volmark_say("read-back", "the flags on disk are not the ones written");
		return VOLMARK_EXIT_READBACK;
	}
	return VOLMARK_EXIT_OK;
}

int volmark_run(const volmark_args *a)
{
	vdev v;
	ntfs_volume *vol;
	uint16_t flags, next = 0;
	int rc, wrc;

	if (a->mode == VOLMARK_SYNC_MIRROR)
		return volmark_sync_mirror(a);
	debug = a->debug;
	if (debug)
		ntfs_log_set_handler(ntfs_log_handler_stderr);
	else
		ntfs_log_set_handler(ntfs_log_handler_null);

	if (vdev_init(&v, a->fd) != 0) {
		volmark_say("descriptor not usable", strerror(errno));
		return VOLMARK_EXIT_TOOL;
	}

	/* Step 1: read/write, NTFS_MNT_FORENSIC only. */
	vol = volmark_mount_fd(&v, NTFS_MNT_FORENSIC);
	if (!vol) {
		volmark_say("refused: the volume does not mount", strerror(errno));
		return VOLMARK_EXIT_REFUSED;
	}
	if (NVolReadOnly(vol)) {
		/* Cannot happen with vdev (no EROFS on open); never write through a read-only mount. */
		volmark_say("refused", "the volume mounted read-only");
		ntfs_umount(vol, FALSE);
		return VOLMARK_EXIT_REFUSED;
	}
	if (volmark_check_fits(vol, &v) != 0) {
		volmark_say("refused", "the volume does not fit the device");
		ntfs_umount(vol, FALSE);
		return VOLMARK_EXIT_REFUSED;
	}
	if (ntfs_volume_check_hiberfile(vol, 1) < 0) {
		volmark_say("refused", "hibernation file is active or unreadable");
		ntfs_umount(vol, FALSE);
		return VOLMARK_EXIT_REFUSED;
	}
	if (volmark_check_log(vol) != 0) {
		volmark_say("refused", "$LogFile is not clean, is version 2.0, or is unreadable");
		ntfs_umount(vol, FALSE);
		return VOLMARK_EXIT_REFUSED;
	}

	/* Step 2: binding. */
	rc = check_binding(vol, a);
	if (rc != VOLMARK_EXIT_OK) {
		ntfs_umount(vol, FALSE);
		return rc;
	}

	/* Step 3: the raw on-disk flags. */
	flags = le16_to_cpu(vol->flags);
	rc = volmark_next_flags(a->mode, flags, &next);
	if (rc != VOLMARK_EXIT_OK) {
		volmark_say(mode_name[a->mode],
			    rc == VOLMARK_EXIT_FLAGS ? "refused: flag already set, or a bit outside the mask"
						     : "refused: flags are not as expected");
		ntfs_umount(vol, FALSE);
		return rc;
	}

	/* Step 4. */
	if (a->stop_before_write)
		raise(SIGSTOP); /* test build only: the test kills the process here */
	alarm(0);
	volmark_block_termination();
	if (volmark_progress('w') != 0) {
		/* The daemon would not know that a write is under way: do not start it. */
		volmark_say("refused", "progress mark could not be delivered");
		ntfs_umount(vol, FALSE);
		return VOLMARK_EXIT_TOOL;
	}
	wrc = ntfs_volume_write_flags(vol, cpu_to_le16(next));
	if (wrc != 0) {
		volmark_say("flag write failed", strerror(errno));
		/*
		 * The record is still marked dirty; ntfs_umount() would write it again. Do not:
		 * the writes are exactly the ones the library attempted once (W1, then W2 if W1
		 * succeeded), and the crash table of section 3.6 describes what they leave.
		 */
		NInoClearDirty(vol->vol_ni);
	} else {
		(void)volmark_progress('d'); /* the write is done either way; the exit status tells the rest */
	}

	/* Step 5. */
	if (ntfs_umount(vol, FALSE) != 0) {
		volmark_say("unmount failed", strerror(errno));
		(void)volmark_sync_descriptor(&v);
		return VOLMARK_EXIT_IO;
	}
	if (volmark_sync_descriptor(&v) != 0 || wrc != 0)
		return VOLMARK_EXIT_IO;

	/* Step 6. */
	rc = read_back(&v, next);
	if (rc == VOLMARK_EXIT_OK && debug)
		volmark_say(mode_name[a->mode], "done");
	return rc;
}
