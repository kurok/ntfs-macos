/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ntfsmac-probe engine: checks 1 to 6 of docs/ARCHITECTURE.md section 3.1.
 *
 * Order of the checks, each skipped only when an earlier one makes it
 * impossible:
 *   1   boot sector OEM ID: not NTFS or BitLocker -> unsupported, stop
 *   2   boot sector geometry, volume fits the device, read-only ntfs mount
 *   2a  only if the mount failed and the volume fits the device: the strict
 *       stale-mirror rule (src/common/mftmirr.c, rules M1 to M5) and, if it
 *       holds, a read-only mount of an overlay view (rule M6); the checks
 *       below then run on that view
 *   3   $Volume flags
 *   4   $LogFile
 *   5   /hiberfil.sys in the root     (names.c, one walk with 5a)
 *   5a  names that are not NFC        (names.c)
 *   6   anything unexpected: unknown NTFS version, a check that could not run
 *
 * The state is not tracked on the side: it is derived from the set of reason
 * codes (probe_state_of_reasons, state.c), the most severe of them in the order
 *   unsupported > corrupt > mirror_split > hibernated > fast_startup >
 *   unclean_log > dirty_flag > names_not_nfc > unknown > clean
 * so `clean` holds if and only if no code is set (the cross-field rule).
 *
 * The device is only read. It is mounted with NTFS_MNT_RDONLY and
 * NTFS_MNT_FORENSIC through the read-only device layer of devio.c; recover,
 * remove_hiberfile and every write path are not reachable.
 */
#include "posix.h"
#include "probe_core.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <ntfs-3g/attrib.h>
#include <ntfs-3g/device.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/layout.h>
#include <ntfs-3g/logfile.h>
#include <ntfs-3g/logging.h>
#include <ntfs-3g/volume.h>

#include "../common/mftmirr.h"
#include "devio.h"
#include "names.h"
#include "state.h"
#include "text.h"

#define RS(x) NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_##x)
#define VF(x) NTFSMAC_PROBE_VOLFLAG_BIT(NTFSMAC_PROBE_VOLFLAG_##x)

/* $Volume flags (layout.h VOLUME_FLAGS), numeric so the table reads as section 3.1. */
#define VOL_DIRTY 0x0001u
#define VOL_RESIZE_LOG_FILE 0x0002u
#define VOL_UPGRADE_ON_MOUNT 0x0004u
#define VOL_MOUNTED_ON_NT4 0x0008u
#define VOL_DELETE_USN_UNDERWAY 0x0010u
#define VOL_REPAIR_OBJECT_ID 0x0020u
#define VOL_CHKDSK_UNDERWAY 0x4000u
#define VOL_MODIFIED_BY_CHKDSK 0x8000u
#define VOL_FLAGS_MASK 0xC03Fu

#define MIN_DEVICE_SECTOR 512u

/* ---- helpers ------------------------------------------------------------- */

typedef struct probe {
	ntfsmac_probe_verdict *v;
	const ntfsmac_probe_opts *opts;
	devio d;
	ntfsmac_mm_geometry g;
	bool g_ok;
} probe;

static void say(const probe *p, const char *what, const char *detail)
{
	if (p->opts->debug)
		fprintf(stderr, "ntfsmac-probe: %s%s%s\n", what, detail[0] ? ": " : "", detail);
}

static void ntfs_log_silent(void)
{
	ntfs_log_set_handler(ntfs_log_handler_null);
}

/* Mount a read-only view of the device, or NULL. */
static ntfs_volume *mount_view(probe *p)
{
	struct ntfs_device *dev = ntfs_device_alloc("ntfsmac-probe-fd", 0, &devio_ops, &p->d);
	ntfs_volume *vol;

	if (!dev)
		return NULL;
	p->d.pos = 0;
	vol = ntfs_device_mount(dev, NTFS_MNT_RDONLY | NTFS_MNT_FORENSIC);
	if (!vol) {
		int e = errno;

		/* A failed ntfs_device_mount() closes the device but does not free it (ntfs_mount() does). */
		ntfs_device_free(dev);
		say(p, "mount failed", strerror(e));
	}
	return vol;
}

static int mm_read(void *ctx, uint64_t off, void *buf, size_t len)
{
	devio *d = ctx;

	return devio_pread(d, buf, len, off);
}

/* total_sectors * sector_size greater than the device? (no overflow) */
static bool volume_exceeds_device(const probe *p)
{
	uint64_t ss = p->g.sector_size, ts = p->g.total_sectors;

	if (ts > UINT64_MAX / ss)
		return true;
	return ts * ss > p->d.size;
}

static void set_placeholder_geometry(probe *p)
{
	ntfsmac_probe_verdict *v = p->v;
	uint64_t sectors = p->d.size / MIN_DEVICE_SECTOR;

	v->serial = 0;
	v->sector_size = 512;
	v->cluster_size = 512;
	v->total_sectors = sectors ? (sectors > (uint64_t)INT64_MAX ? (uint64_t)INT64_MAX : sectors) : 1;
}

/* ---- check 3 -------------------------------------------------------------- */

static void check_flags(probe *p, uint16_t f)
{
	ntfsmac_probe_verdict *v = p->v;

	if (f & VOL_DIRTY) {
		v->volume_flags |= VF(DIRTY);
		v->reason_codes |= RS(FLAG_DIRTY);
	}
	if (f & VOL_RESIZE_LOG_FILE)
		v->volume_flags |= VF(RESIZE_LOG_FILE);
	if (f & VOL_UPGRADE_ON_MOUNT)
		v->volume_flags |= VF(UPGRADE_ON_MOUNT);
	if (f & VOL_DELETE_USN_UNDERWAY)
		v->volume_flags |= VF(DELETE_USN_UNDERWAY);
	if (f & VOL_REPAIR_OBJECT_ID)
		v->volume_flags |= VF(REPAIR_OBJECT_ID);
	if (f & (VOL_RESIZE_LOG_FILE | VOL_UPGRADE_ON_MOUNT | VOL_DELETE_USN_UNDERWAY | VOL_REPAIR_OBJECT_ID))
		v->reason_codes |= RS(FLAG_PENDING_WORK);
	if (f & VOL_CHKDSK_UNDERWAY) {
		v->volume_flags |= VF(CHKDSK_UNDERWAY);
		v->reason_codes |= RS(FLAG_CHKDSK_UNDERWAY);
	}
	/* Informational: listed, no state change (section 3.1 check 3). */
	if (f & VOL_MOUNTED_ON_NT4)
		v->volume_flags |= VF(MOUNTED_ON_NT4);
	if (f & VOL_MODIFIED_BY_CHKDSK)
		v->volume_flags |= VF(MODIFIED_BY_CHKDSK);
	if (f & ~VOL_FLAGS_MASK) {
		v->volume_flags |= VF(UNKNOWN_BITS);
		v->reason_codes |= RS(FLAG_UNKNOWN_BITS);
	}
}

/* ---- check 4 -------------------------------------------------------------- */

static void log_unreadable(ntfsmac_probe_verdict *v)
{
	v->logfile = NTFSMAC_PROBE_LOGFILE_UNREADABLE;
	v->has_logfile_lsn = false;
	v->logfile_lsn = 0;
	v->reason_codes |= RS(LOG_UNREADABLE);
}

/*
 * ntfs_check_logfile() and ntfs_is_logfile_clean() as ntfs_volume_check_logfile()
 * (volume.c) calls them, with its restart page 2.0 rule. The LSN is the
 * current_lsn of the restart area the engine picked (the newer valid page), 0
 * for an empty log.
 */
static void check_log(probe *p, ntfs_volume *vol)
{
	ntfsmac_probe_verdict *v = p->v;
	ntfs_inode *ni;
	ntfs_attr *na;
	RESTART_PAGE_HEADER *rp = NULL;

	ni = ntfs_inode_open(vol, FILE_LogFile);
	if (!ni) {
		log_unreadable(v);
		return;
	}
	na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
	if (!na) {
		log_unreadable(v);
		ntfs_inode_close(ni);
		return;
	}
	if (!ntfs_check_logfile(na, &rp)) {
		log_unreadable(v);
	} else {
		v->has_logfile_lsn = true;
		v->logfile_lsn = 0;
		if (rp) {
			const RESTART_AREA *ra =
				(const RESTART_AREA *)((const u8 *)rp + le16_to_cpu(rp->restart_area_offset));

			v->logfile_lsn = (uint64_t)sle64_to_cpu(ra->current_lsn);
		}
		if (rp && sle16_to_cpu(rp->major_ver) == 2 && sle16_to_cpu(rp->minor_ver) == 0) {
			v->logfile = NTFSMAC_PROBE_LOGFILE_V2_FAST_STARTUP;
			v->reason_codes |= RS(LOG_V2);
		} else if (!ntfs_is_logfile_clean(na, rp)) {
			v->logfile = NTFSMAC_PROBE_LOGFILE_UNCLEAN;
			v->reason_codes |= RS(LOG_UNCLEAN);
		} else {
			v->logfile = NTFSMAC_PROBE_LOGFILE_CLEAN;
		}
	}
	free(rp);
	ntfs_attr_close(na);
	ntfs_inode_close(ni);
}

/* ---- $Volume: label ------------------------------------------------------- */

#define LABEL_MAX_UNITS 128

static void read_label(const probe *p, ntfs_volume *vol)
{
	ntfsmac_probe_verdict *v = p->v;
	ntfs_inode *ni = ntfs_inode_open(vol, FILE_Volume);
	ntfs_attr *na;
	u8 raw[LABEL_MAX_UNITS * 2];
	uint16_t u[LABEL_MAX_UNITS];
	s64 want, got;
	int i;

	v->label[0] = '\0';
	v->label_len = 0;
	if (!ni)
		return;
	na = ntfs_attr_open(ni, AT_VOLUME_NAME, AT_UNNAMED, 0);
	if (na) {
		want = na->data_size < (s64)sizeof(raw) ? na->data_size : (s64)sizeof(raw);
		want &= ~(s64)1;
		got = want > 0 ? ntfs_attr_pread(na, 0, want, raw) : 0;
		if (got > 0) {
			for (i = 0; i < got / 2; i++)
				u[i] = (uint16_t)(raw[2 * i] | (raw[2 * i + 1] << 8));
			v->label_len = probe_utf16_to_utf8(u, (size_t)(got / 2), v->label, sizeof(v->label));
		}
		ntfs_attr_close(na);
	}
	ntfs_inode_close(ni);
	(void)p;
}

/* ---- the run -------------------------------------------------------------- */

static void finish(ntfsmac_probe_verdict *v)
{
	v->state = probe_state_of_reasons(v->reason_codes);
	v->nonnfc_path_count = v->nonnfc_count < NTFSMAC_PROBE_MAX_NONNFC_PATHS ? v->nonnfc_count
										 : NTFSMAC_PROBE_MAX_NONNFC_PATHS;
}

int64_t ntfsmac_probe_now_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The scan budget: an explicit test value as given, else what remains of the default since start. */
static int64_t scan_budget(const ntfsmac_probe_opts *o)
{
	int64_t left;

	if (o->scan_budget_ms >= 0)
		return o->scan_budget_ms;
	if (o->start_ms < 0)
		return NTFSMAC_PROBE_DEFAULT_SCAN_BUDGET_MS;
	left = NTFSMAC_PROBE_DEFAULT_SCAN_BUDGET_MS - (ntfsmac_probe_now_ms() - o->start_ms);
	return left > 0 ? left : 0;
}

/* Checks 3 to 5a (and the version of check 6) on a mounted view. */
static void checks_on_volume(probe *p, ntfs_volume *vol)
{
	ntfsmac_probe_verdict *v = p->v;
	names_result *nr;

	v->has_ntfs_version = false;
	if (vol->major_ver >= 1 && vol->major_ver <= 3 && vol->minor_ver <= 9) {
		v->has_ntfs_version = true;
		v->ntfs_version_major = vol->major_ver;
		v->ntfs_version_minor = vol->minor_ver;
	} else {
		v->reason_codes |= RS(VERSION_UNKNOWN);
	}
	read_label(p, vol);
	check_flags(p, le16_to_cpu(vol->flags));
	check_log(p, vol);

	nr = malloc(sizeof(*nr));
	if (!nr) {
		v->hiberfile = NTFSMAC_PROBE_HIBERFILE_UNREADABLE;
		v->reason_codes |= RS(HIBER_UNREADABLE) | RS(CHECK_FAILED);
		return;
	}
	names_scan(vol, scan_budget(p->opts), nr);
	if (p->opts->debug)
		fprintf(stderr, "ntfsmac-probe: name scan: %llu directories, %llu names, %u hiberfil.sys records, %s\n",
			(unsigned long long)nr->dirs_listed, (unsigned long long)nr->names_seen,
			nr->hiber_records, nr->complete ? "complete" : "not complete");
	v->hiberfile = nr->hiberfile;
	v->reason_codes |= nr->hiber_reasons;
	if (!nr->complete) {
		/*
		 * A walk that did not finish is check 6, `unknown`: what it found before it
		 * stopped is dropped, so that the names fields always describe a complete scan
		 * (and the state reads the same whichever directory was damaged). The volume is
		 * read-only either way.
		 */
		v->reason_codes |= RS(CHECK_FAILED);
		say(p, "name scan did not finish", "");
	} else {
		uint32_t i;

		v->nonnfc_count = nr->nonnfc_count;
		if (nr->nonnfc_count)
			v->reason_codes |= RS(NAMES_NOT_NFC);
		for (i = 0; i < nr->path_count && i < NTFSMAC_PROBE_MAX_NONNFC_PATHS; i++)
			memcpy(v->nonnfc_paths[i], nr->paths[i], sizeof(v->nonnfc_paths[i]));
	}
	free(nr);
}

int ntfsmac_probe_run(int fd, const ntfsmac_probe_opts *opts, ntfsmac_probe_verdict *v)
{
	probe p;
	uint8_t boot[512];
	ssize_t got;
	ntfs_volume *vol = NULL;
	bool size_bad = false, split = false, oem_ntfs;
	ntfsmac_mm_result mr;

	memset(v, 0, sizeof(*v));
	memset(&p, 0, sizeof(p));
	p.v = v;
	p.opts = opts;
	if (!opts->debug)
		ntfs_log_silent();

	if (devio_init(&p.d, fd) != 0) {
		say(&p, "descriptor not usable", strerror(errno));
		return NTFSMAC_PROBE_EXIT_IO;
	}
	/* Everything not examined reads as `unreadable` (no reason code of its own). */
	v->logfile = NTFSMAC_PROBE_LOGFILE_UNREADABLE;
	v->hiberfile = NTFSMAC_PROBE_HIBERFILE_UNREADABLE;
	set_placeholder_geometry(&p);

	/* Check 1: the boot sector. */
	memset(boot, 0, sizeof(boot));
	got = devio_pread_some(&p.d, boot, sizeof(boot), 0);
	if (got < 0) {
		say(&p, "boot sector unreadable", strerror(errno));
		return NTFSMAC_PROBE_EXIT_IO;
	}
	oem_ntfs = got == (ssize_t)sizeof(boot) && memcmp(boot + 3, "NTFS    ", 8) == 0;
	if (got == (ssize_t)sizeof(boot) && memcmp(boot + 3, "-FVE-FS-", 8) == 0) {
		v->bitlocker = true;
		v->reason_codes |= RS(BITLOCKER);
		finish(v);
		return NTFSMAC_PROBE_EXIT_OK;
	}
	if (!oem_ntfs) {
		v->reason_codes |= RS(NOT_NTFS);
		finish(v);
		return NTFSMAC_PROBE_EXIT_OK;
	}
	v->is_ntfs = true;

	/* Check 2: geometry, size against the device, then the mount. */
	p.g_ok = ntfsmac_mm_parse_boot(boot, &p.g) == 0;
	if (!p.g_ok) {
		say(&p, "boot sector geometry is not usable", "");
		v->reason_codes |= RS(MOUNT_FAILED);
		finish(v);
		return NTFSMAC_PROBE_EXIT_OK;
	}
	v->serial = p.g.serial;
	v->total_sectors = p.g.total_sectors;
	v->cluster_size = p.g.cluster_size;
	v->sector_size = p.g.sector_size;
	size_bad = volume_exceeds_device(&p);
	if (size_bad)
		say(&p, "volume is larger than the device", "");

	vol = mount_view(&p);
	if (!vol && !size_bad) {
		/* Check 2a: the strict stale-mirror rule, then the overlay view (M6). */
		ntfsmac_mm_status st = ntfsmac_mm_check(mm_read, &p.d, &p.g, &mr);

		if (st == NTFSMAC_MM_SPLIT) {
			unsigned k;

			for (k = 0; k < NTFSMAC_MM_DIFF_RECORDS; k++)
				if (mr.diff_mask & (1u << k))
					devio_add_overlay(&p.d, ntfsmac_mm_mirr_offset(&p.g, k),
							  ntfsmac_mm_mft_offset(&p.g, k), p.g.record_size);
			vol = mount_view(&p);
			if (vol)
				split = true;
			else
				say(&p, "overlay view does not mount", "");
		} else {
			char buf[96];

			snprintf(buf, sizeof(buf), "rule M%u: %s", mr.failed_rule, mr.why);
			say(&p, st == NTFSMAC_MM_IO_ERROR ? "mirror rule: read error" : "mirror rule does not hold", buf);
		}
	}
	if (!vol || size_bad)
		v->reason_codes |= RS(MOUNT_FAILED);
	if (split)
		v->reason_codes |= RS(MFTMIRR_STALE);
	if (!vol) {
		finish(v);
		return NTFSMAC_PROBE_EXIT_OK;
	}

	/* Checks 3 to 5a and 6 on the mounted view. */
	checks_on_volume(&p, vol);
	ntfs_umount(vol, FALSE);
	finish(v);
	return NTFSMAC_PROBE_EXIT_OK;
}
