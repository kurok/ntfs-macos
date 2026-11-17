/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ntfsmac-volmark `sync-mirror`: the repair of a stale `$MFTMirr`, steps 1 to 5 of
 * docs/ARCHITECTURE.md section 3.7 (ADR-021, ADR-022).
 *
 *   1  Binding (ADR-020): sector 0, read by pread() on the inherited descriptor; serial and
 *      sector count must equal the expected values. Exit 15, nothing written.
 *   2  The rule M1 to M5 on its own reads (ntfsmac_mm_check(), src/common/mftmirr.c: the same
 *      code as the probe's check 2a), then M6: a read-only overlay view, in which each differing
 *      `$MFTMirr` record reads as the `$MFT` copy, must mount (NTFS_MNT_RDONLY with
 *      NTFS_MNT_FORENSIC), and on that view the hibernation file and `$LogFile` refusals of
 *      section 3.6 step 1 and the flags check (volmark_next_flags()) hold. The view refuses every
 *      write. Exit 10 (rule, mount, hibernation, log, or nothing differs) or 13 (flags). Nothing
 *      is written until all of this has passed.
 *   3  The raw bytes of every differing `$MFT` record k (0 to 3) are those the rule checked (both
 *      copies are read once into memory). Then `w`, and for
 *      each such k in ascending order one pwrite() of exactly one record to its place in
 *      `$MFTMirr`, through the library's own device write (so that the test-only fault layer of
 *      patch 0004 sees it); then `d`. Nothing else is written: not `$MFT`, not `$LogFile`, no
 *      flag, no byte outside those records. A failed write ends the run (exit 12) without a
 *      second try.
 *   4  fsync() of the descriptor and the drive flush (DKIOCSYNCHRONIZE on a device node,
 *      F_FULLFSYNC on a regular file; macOS only, ADR-022). fsync failure: exit 12; a failed
 *      flush is a warning (R-30).
 *   5  Read back: every record written equals its `$MFT` copy byte for byte, and an ordinary
 *      read-only mount (which does the engine's compare of the two copies) succeeds with the
 *      same flags as the view had. Otherwise exit 14.
 *
 * ntfsfix is never run, `$MFT` is never written, and a record that did not differ is never
 * touched. A rerun after any cut that was not inside a record write finishes the job; a second run
 * on a repaired volume finds nothing to do and exits 10.
 */
#include "posix.h"
#include "volmark.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ntfs-3g/device.h>
#include <ntfs-3g/layout.h>
#include <ntfs-3g/logging.h>
#include <ntfs-3g/volume.h>

#include "../common/mftmirr.h"
#include "vdev.h"
#include "volmark_int.h"

/* Read callback of the rule: exact pread on the descriptor. 0, or -1. */
static int fd_read(void *ctx, uint64_t off, void *buf, size_t len)
{
	const int fd = *(const int *)ctx;
	uint8_t *p = buf;

	while (len > 0) {
		ssize_t r = pread(fd, p, len, (off_t)off);

		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return -1;
		p += r;
		off += (uint64_t)r;
		len -= (size_t)r;
	}
	return 0;
}

/* The rule reads from memory: the first records of both copies, read once (see volmark_sync_mirror). */
typedef struct mem_regions {
	const uint8_t *mft, *mir;
	uint64_t mft_off, mir_off;
	size_t mft_len, mir_len;
} mem_regions;

static int mem_read(void *ctx, uint64_t off, void *buf, size_t len)
{
	const mem_regions *m = ctx;

	if (off >= m->mft_off && off - m->mft_off <= m->mft_len && len <= m->mft_len - (size_t)(off - m->mft_off)) {
		memcpy(buf, m->mft + (off - m->mft_off), len);
		return 0;
	}
	if (off >= m->mir_off && off - m->mir_off <= m->mir_len && len <= m->mir_len - (size_t)(off - m->mir_off)) {
		memcpy(buf, m->mir + (off - m->mir_off), len);
		return 0;
	}
	return -1;
}

/* Step 2, M6: the view mounts, and its hibernation file, log and flags are acceptable. */
static int check_view(vdev *view, uint16_t *flags_out)
{
	ntfs_volume *vol;
	uint16_t flags, next;
	int rc = VOLMARK_EXIT_OK;

	vol = volmark_mount_fd(view, (ntfs_mount_flags)(NTFS_MNT_RDONLY | NTFS_MNT_FORENSIC));
	if (!vol) {
		volmark_say("refused: the overlay view of the volume does not mount", strerror(errno));
		return VOLMARK_EXIT_REFUSED;
	}
	if (ntfs_volume_check_hiberfile(vol, 1) < 0) {
		volmark_say("refused", "hibernation file is active or unreadable");
		rc = VOLMARK_EXIT_REFUSED;
	} else if (volmark_check_log(vol) != 0) {
		volmark_say("refused", "$LogFile is not clean, is version 2.0, or is unreadable");
		rc = VOLMARK_EXIT_REFUSED;
	} else {
		flags = le16_to_cpu(vol->flags);
		rc = volmark_next_flags(VOLMARK_SYNC_MIRROR, flags, &next);
		if (rc != VOLMARK_EXIT_OK)
			volmark_say("sync-mirror", "refused: a volume flag besides the informational ones is set");
		else
			*flags_out = flags;
	}
	ntfs_umount(vol, FALSE);
	return rc;
}

/* Step 5, second part: an ordinary read-only mount works and shows the flags of the view. */
static int read_back_mount(int fd, uint16_t want_flags)
{
	vdev v;
	ntfs_volume *vol;
	uint16_t got;

	if (vdev_init(&v, fd) != 0) {
		volmark_say("read-back: descriptor not usable", strerror(errno));
		return VOLMARK_EXIT_READBACK;
	}
	vol = volmark_mount_fd(&v, NTFS_MNT_RDONLY);
	if (!vol) {
		volmark_say("read-back: read-only mount failed", strerror(errno));
		return VOLMARK_EXIT_READBACK;
	}
	got = le16_to_cpu(vol->flags);
	ntfs_umount(vol, FALSE);
	if (got != want_flags) {
		volmark_say("read-back", "the volume flags are not those of the view");
		return VOLMARK_EXIT_READBACK;
	}
	return VOLMARK_EXIT_OK;
}

int volmark_sync_mirror(const volmark_args *a)
{
	vdev v, view;
	uint8_t boot[512];
	ntfsmac_mm_geometry g;
	ntfsmac_mm_result res;
	ntfsmac_mm_status st;
	uint64_t size;
	uint16_t flags = 0;
	uint8_t *mftbuf = NULL, *mirbuf = NULL;
	mem_regions mr;
	size_t region;
	struct ntfs_device *dev;
	unsigned k;
	int rc, wrc = 0;
	int fd = a->fd;

	if (a->debug)
		ntfs_log_set_handler(ntfs_log_handler_stderr);
	else
		ntfs_log_set_handler(ntfs_log_handler_null);

	if (vdev_init(&v, fd) != 0) {
		volmark_say("descriptor not usable", strerror(errno));
		return VOLMARK_EXIT_TOOL;
	}

	/* Step 1: binding. */
	if (fd_read(&fd, 0, boot, sizeof(boot)) != 0) {
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

	/* Step 2: M1 (the volume fits the device), M1 to M5 on raw bytes, then M6 on the view. */
	if (vdev_size(&v, &size) != 0 || g.total_sectors > UINT64_MAX / g.sector_size ||
	    g.total_sectors * g.sector_size > size) {
		volmark_say("refused", "the volume does not fit the device");
		return VOLMARK_EXIT_REFUSED;
	}
	/*
	 * The first NTFSMAC_MM_MAX_COMPARED records of both copies are read once. The rule runs on
	 * these bytes and the records written in step 3 are copied from the same bytes, so what was
	 * checked is what is written.
	 */
	region = (size_t)NTFSMAC_MM_MAX_COMPARED * g.record_size;
	mftbuf = malloc(region);
	mirbuf = malloc(region);
	if (!mftbuf || !mirbuf) {
		volmark_say("refused", "out of memory");
		rc = VOLMARK_EXIT_TOOL;
		goto out;
	}
	memset(&mr, 0, sizeof(mr));
	mr.mft = mftbuf;
	mr.mir = mirbuf;
	mr.mft_off = ntfsmac_mm_mft_offset(&g, 0);
	mr.mir_off = ntfsmac_mm_mirr_offset(&g, 0);
	mr.mft_len = mr.mir_len = region;
	if (mr.mft_off > size || size - mr.mft_off < region || mr.mir_off > size || size - mr.mir_off < region ||
	    fd_read(&fd, mr.mft_off, mftbuf, region) != 0 || fd_read(&fd, mr.mir_off, mirbuf, region) != 0) {
		volmark_say("refused", "the first records of $MFT and $MFTMirr could not be read");
		rc = VOLMARK_EXIT_REFUSED;
		goto out;
	}
	st = ntfsmac_mm_check(mem_read, &mr, &g, &res);
	if (st != NTFSMAC_MM_SPLIT) {
		volmark_say(st == NTFSMAC_MM_IO_ERROR ? "refused: read error while checking the mirror"
						      : "refused: the mirror rule does not hold",
			    res.why != NULL ? res.why : "");
		rc = VOLMARK_EXIT_REFUSED;
		goto out;
	}
	view = v;
	for (k = 0; k < NTFSMAC_MM_DIFF_RECORDS; k++)
		if ((res.diff_mask & (1u << k)) &&
		    !vdev_add_overlay(&view, ntfsmac_mm_mirr_offset(&g, k), ntfsmac_mm_mft_offset(&g, k),
				      g.record_size)) {
			volmark_say("refused", "overlay table is full");
			rc = VOLMARK_EXIT_REFUSED;
			goto out;
		}
	rc = check_view(&view, &flags);
	if (rc != VOLMARK_EXIT_OK)
		goto out;

	dev = ntfs_device_alloc("ntfsmac-volmark-fd", 0, &vdev_ops, &v);
	if (!dev || dev->d_ops->open(dev, O_RDWR) != 0) {
		volmark_say("refused", "the device layer could not be opened for writing");
		if (dev)
			ntfs_device_free(dev);
		rc = VOLMARK_EXIT_TOOL;
		goto out;
	}

	/* Step 3: the only writes. */
	if (a->stop_before_write)
		raise(SIGSTOP); /* test build only: the test kills the process here */
	alarm(0);
	volmark_block_termination();
	if (volmark_progress('w') != 0) {
		/* The daemon would not know that a write is under way: do not start it. */
		volmark_say("refused", "progress mark could not be delivered");
		dev->d_ops->close(dev);
		ntfs_device_free(dev);
		rc = VOLMARK_EXIT_TOOL;
		goto out;
	}
	for (k = 0; k < NTFSMAC_MM_DIFF_RECORDS && wrc == 0; k++) {
		if (!(res.diff_mask & (1u << k)))
			continue;
		if (dev->d_ops->pwrite(dev, mftbuf + (size_t)k * g.record_size, (s64)g.record_size, (s64)ntfsmac_mm_mirr_offset(&g, k)) !=
		    (s64)g.record_size) {
			volmark_say("write of a $MFTMirr record failed", strerror(errno));
			wrc = -1;
		}
	}
	if (wrc == 0)
		(void)volmark_progress('d');
	/* Our own flush below covers the device; the close need not repeat it. */
	NDevClearDirty(dev);
	dev->d_ops->close(dev);
	ntfs_device_free(dev);

	/* Step 4. */
	if (volmark_sync_descriptor(&v) != 0 || wrc != 0) {
		rc = VOLMARK_EXIT_IO;
		goto out;
	}

	/* Step 5. */
	for (k = 0; k < NTFSMAC_MM_DIFF_RECORDS; k++) {
		uint8_t *chk;

		if (!(res.diff_mask & (1u << k)))
			continue;
		chk = malloc(g.record_size);
		if (!chk || fd_read(&fd, ntfsmac_mm_mirr_offset(&g, k), chk, g.record_size) != 0 ||
		    memcmp(chk, mftbuf + (size_t)k * g.record_size, g.record_size) != 0) {
			volmark_say("read-back", "a $MFTMirr record is not the $MFT record that was written");
			free(chk);
			rc = VOLMARK_EXIT_READBACK;
			goto out;
		}
		free(chk);
	}
	rc = read_back_mount(fd, flags);
	if (rc == VOLMARK_EXIT_OK && a->debug)
		volmark_say("sync-mirror", "done");
out:
	free(mftbuf);
	free(mirbuf);
	return rc;
}
