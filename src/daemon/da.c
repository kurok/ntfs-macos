/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Disk Arbitration module (T-M4-02; docs/ARCHITECTURE.md sections 4.1 and
 * 8.1, ADR-003, ADR-020). The only daemon module besides main.c that may
 * use DiskArbitration or CoreFoundation.
 *
 * Threads: one serial dispatch queue ("the DA queue") runs every Disk
 * Arbitration callback, and so every ntfsmac_da_callbacks function. The
 * callbacks must be short and must not call ntfsmac_da_unmount,
 * ntfsmac_da_eject or ntfsmac_da_stop: these wait for the DA queue and would
 * deadlock. The other functions may be called from any thread, also from a
 * callback. ntfsmac_da_dissent, called from inside disk_appeared, takes
 * effect before the mount approval of the same disk, because the approval is
 * answered later on the same queue.
 *
 * Dissent: while a BSD name is in the dissent table, the mount-approval
 * callback refuses every mount of it, so Apple's read-only NTFS driver does
 * not mount it. The entry is dropped when the disk disappears.
 * ntfsmac_da_withdraw drops it earlier. If a mount was really refused, the
 * withdrawal also asks Disk Arbitration to mount the disk, so the system
 * handles it as if the daemon had never been there (section 4.1 step 5).
 * A volume the daemon mounts itself keeps its dissent until the disk is gone.
 */
#include "internal.h"

#include "da_filter.h"

#if defined(__APPLE__)

#include <CoreFoundation/CoreFoundation.h>
#include <DiskArbitration/DiskArbitration.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define OP_TIMEOUT_S 30

struct ntfsmac_da {
	DASessionRef session;
	dispatch_queue_t queue;
	ntfsmac_da_callbacks cb;
	void *ctx;
	pthread_mutex_t lock;
	ntfsmac_da_table dissent; /* flag: a mount of this disk was refused */
	ntfsmac_da_table known;   /* candidates seen; flag: reported as mounted */
	ntfsmac_da_fuse_table fuse; /* macFUSE disks of live mounts and their mount paths */
	bool fuse_full_logged;
};

/* ---------------------------------------------------------------- */
/* Description to plain C                                            */
/* ---------------------------------------------------------------- */

static bool cf_to_cstr(CFStringRef s, char *buf, size_t cap)
{
	if (s == NULL || CFGetTypeID(s) != CFStringGetTypeID())
		return false;
	return CFStringGetCString(s, buf, (CFIndex)cap, kCFStringEncodingUTF8);
}

/* Volume kind is a fixed driver name such as "ntfs"; keep a safe subset. */
static void copy_kind(CFStringRef s, char *buf, size_t cap)
{
	size_t i;

	buf[0] = '\0';
	if (!cf_to_cstr(s, buf, cap))
		return;
	for (i = 0; buf[i] != '\0'; i++) {
		char c = buf[i];

		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
		      c == '-')) {
			buf[0] = '\0';
			return;
		}
	}
}

static bool describe_disk(DADiskRef disk, ntfsmac_disk_info *out)
{
	CFDictionaryRef d;
	const char *bsd;
	char content[64], model[32], protocol[32];
	CFTypeRef v;
	bool has_content;
	long long size = 0;

	memset(out, 0, sizeof *out);
	bsd = DADiskGetBSDName(disk);
	if (bsd == NULL || !ntfsmac_da_bsd_name_valid(bsd) || strlen(bsd) >= sizeof out->bsd_name)
		return false;
	strcpy(out->bsd_name, bsd);
	d = DADiskCopyDescription(disk);
	if (d == NULL)
		return false;
	v = CFDictionaryGetValue(d, kDADiskDescriptionMediaWholeKey);
	out->whole_disk = v != NULL && CFGetTypeID(v) == CFBooleanGetTypeID() &&
			  CFBooleanGetValue((CFBooleanRef)v);
	v = CFDictionaryGetValue(d, kDADiskDescriptionMediaSizeKey);
	if (v != NULL && CFGetTypeID(v) == CFNumberGetTypeID() &&
	    CFNumberGetValue((CFNumberRef)v, kCFNumberLongLongType, &size) && size > 0)
		out->size_bytes = (uint64_t)size;
	has_content = cf_to_cstr(CFDictionaryGetValue(d, kDADiskDescriptionMediaContentKey),
				 content, sizeof content);
	out->is_ntfs_candidate =
		ntfsmac_da_content_is_candidate(has_content ? content : NULL, out->whole_disk);
	/* macFUSE's own 8 MiB device is never ours: no probe, no claim, no dissent (T-M4-11) */
	if (!cf_to_cstr(CFDictionaryGetValue(d, kDADiskDescriptionDeviceModelKey), model, sizeof model))
		model[0] = '\0';
	if (!cf_to_cstr(CFDictionaryGetValue(d, kDADiskDescriptionDeviceProtocolKey), protocol,
			sizeof protocol))
		protocol[0] = '\0';
	if (ntfsmac_da_is_macfuse_backing(out->size_bytes, out->whole_disk, has_content ? content : NULL,
					  model, protocol))
		out->is_ntfs_candidate = false;
	v = CFDictionaryGetValue(d, kDADiskDescriptionVolumePathKey);
	out->has_volume_path = v != NULL;
	out->volume_path[0] = '\0';
	if (v != NULL && CFGetTypeID(v) == CFURLGetTypeID() &&
	    !CFURLGetFileSystemRepresentation((CFURLRef)v, true, (UInt8 *)out->volume_path,
					      (CFIndex)sizeof out->volume_path))
		out->volume_path[0] = '\0';
	if (!cf_to_cstr(CFDictionaryGetValue(d, kDADiskDescriptionMediaPathKey),
			out->media_path, sizeof out->media_path))
		out->media_path[0] = '\0';
	copy_kind(CFDictionaryGetValue(d, kDADiskDescriptionVolumeKindKey), out->volume_kind,
		  sizeof out->volume_kind);
	CFRelease(d);
	return true;
}

static ntfsmac_dmn_status map_dareturn(DAReturn r)
{
	switch (r) {
	case kDAReturnSuccess:
		return NTFSMAC_DMN_OK;
	case kDAReturnBusy:
	case kDAReturnExclusiveAccess:
		return NTFSMAC_DMN_E_BUSY;
	case kDAReturnNotMounted:
		return NTFSMAC_DMN_E_STATE;
	case kDAReturnNotPermitted:
	case kDAReturnNotPrivileged:
	case kDAReturnNotWritable:
		return NTFSMAC_DMN_E_DENIED;
	case kDAReturnBadArgument:
	case kDAReturnNotFound:
		return NTFSMAC_DMN_E_INVAL;
	default:
		return NTFSMAC_DMN_E_IO;
	}
}

/* ---------------------------------------------------------------- */
/* Callbacks (run on the DA queue)                                   */
/* ---------------------------------------------------------------- */

/*
 * macFUSE's disk of a mount ("macfuse-local") is remembered with its mount path while the path is
 * known: once the disk disappears the description no longer has it.
 */
static void fuse_track(struct ntfsmac_da *da, const ntfsmac_disk_info *info)
{
	bool ok, full;

	if (strcmp(info->volume_kind, "macfuse-local") != 0 || info->volume_path[0] == '\0')
		return;
	pthread_mutex_lock(&da->lock);
	ok = ntfsmac_da_fuse_add(&da->fuse, info->bsd_name, info->volume_path);
	full = !ok && !da->fuse_full_logged;
	if (full)
		da->fuse_full_logged = true;
	pthread_mutex_unlock(&da->lock);
	if (full)
		fprintf(stderr, "ntfsmacd: the table of macFUSE disks is full: unmounts of later mounts are not reported\n");
	/* The entry is kept when a later change shows no path: an orderly umount sends that change
	 * before the disappearance (T-M5-05 VM run). The daemon binds the BSD name to its session. */
	if (ok && da->cb.fuse_volume_mounted != NULL)
		da->cb.fuse_volume_mounted(info->bsd_name, info->volume_path, da->ctx);
}

static void on_appeared(DADiskRef disk, void *context)
{
	struct ntfsmac_da *da = context;
	ntfsmac_disk_info info;
	bool known;

	if (!describe_disk(disk, &info))
		return;
	fuse_track(da, &info);
	if (!info.is_ntfs_candidate)
		return;
	pthread_mutex_lock(&da->lock);
	known = ntfsmac_da_table_add(&da->known, info.bsd_name);
	if (known)
		ntfsmac_da_table_set_flag(&da->known, info.bsd_name, info.has_volume_path);
	pthread_mutex_unlock(&da->lock);
	if (!known)
		return; /* table full: fail closed, the disk is not handled */
	if (da->cb.disk_appeared != NULL)
		da->cb.disk_appeared(&info, da->ctx);
	/* Already mounted when first seen: report it as a mount too. */
	if (info.has_volume_path && da->cb.disk_mounted != NULL)
		da->cb.disk_mounted(&info, da->ctx);
}

static void on_disappeared(DADiskRef disk, void *context)
{
	struct ntfsmac_da *da = context;
	const char *bsd = DADiskGetBSDName(disk);
	char name[NTFSMAC_DA_NAME_MAX], fuse_path[NTFSMAC_DA_FUSE_PATH_MAX];
	bool was_known, flag, was_fuse;

	if (bsd == NULL || !ntfsmac_da_bsd_name_valid(bsd) || strlen(bsd) >= sizeof name)
		return;
	strcpy(name, bsd);
	pthread_mutex_lock(&da->lock);
	was_fuse = ntfsmac_da_fuse_take(&da->fuse, name, fuse_path, sizeof fuse_path);
	pthread_mutex_unlock(&da->lock);
	if (was_fuse && da->cb.fuse_volume_gone != NULL)
		da->cb.fuse_volume_gone(name, da->ctx);
	pthread_mutex_lock(&da->lock);
	was_known = ntfsmac_da_table_remove(&da->known, name, &flag);
	ntfsmac_da_table_remove(&da->dissent, name, &flag);
	pthread_mutex_unlock(&da->lock);
	if (was_known && da->cb.disk_disappeared != NULL)
		da->cb.disk_disappeared(name, da->ctx);
}

static void on_changed(DADiskRef disk, CFArrayRef keys, void *context)
{
	struct ntfsmac_da *da = context;
	ntfsmac_disk_info info;
	bool known, was_mounted = false;

	(void)keys;
	if (!describe_disk(disk, &info))
		return;
	fuse_track(da, &info);
	pthread_mutex_lock(&da->lock);
	known = ntfsmac_da_table_get_flag(&da->known, info.bsd_name, &was_mounted);
	if (known && was_mounted != info.has_volume_path)
		ntfsmac_da_table_set_flag(&da->known, info.bsd_name, info.has_volume_path);
	pthread_mutex_unlock(&da->lock);
	if (!known || was_mounted == info.has_volume_path)
		return;
	if (info.has_volume_path) {
		if (da->cb.disk_mounted != NULL)
			da->cb.disk_mounted(&info, da->ctx);
	} else if (da->cb.disk_unmounted != NULL) {
		da->cb.disk_unmounted(info.bsd_name, da->ctx);
	}
}

/* DiskArbitration releases the dissenter that an approval callback returns (a +1 object). */
static DADissenterRef on_approval(DADiskRef disk, void *context) CF_RETURNS_RETAINED;
static DADissenterRef on_approval(DADiskRef disk, void *context)
{
	struct ntfsmac_da *da = context;
	const char *bsd = DADiskGetBSDName(disk);
	bool dissent;

	if (bsd == NULL)
		return NULL;
	pthread_mutex_lock(&da->lock);
	dissent = ntfsmac_da_table_contains(&da->dissent, bsd);
	if (dissent)
		ntfsmac_da_table_set_flag(&da->dissent, bsd, true);
	pthread_mutex_unlock(&da->lock);
	if (!dissent)
		return NULL;
	return DADissenterCreate(kCFAllocatorDefault, kDAReturnNotPermitted,
				 CFSTR("claimed by ntfsmacd"));
}

/* ---------------------------------------------------------------- */
/* Start and stop                                                    */
/* ---------------------------------------------------------------- */

ntfsmac_dmn_status ntfsmac_da_start(const ntfsmac_da_callbacks *cb, void *ctx,
				    ntfsmac_da **out)
{
	struct ntfsmac_da *da;
	CFStringRef watch_key = kDADiskDescriptionVolumePathKey;
	CFArrayRef watch;

	if (out != NULL)
		*out = NULL;
	if (cb == NULL || out == NULL || cb->disk_appeared == NULL)
		return NTFSMAC_DMN_E_INVAL;
	da = calloc(1, sizeof *da);
	if (da == NULL)
		return NTFSMAC_DMN_E_IO;
	da->cb = *cb;
	da->ctx = ctx;
	pthread_mutex_init(&da->lock, NULL);
	ntfsmac_da_table_init(&da->dissent);
	ntfsmac_da_table_init(&da->known);
	ntfsmac_da_fuse_init(&da->fuse);
	da->session = DASessionCreate(kCFAllocatorDefault);
	da->queue = dispatch_queue_create("ntfsmac.da", DISPATCH_QUEUE_SERIAL);
	watch = CFArrayCreate(kCFAllocatorDefault, (const void **)&watch_key, 1,
			      &kCFTypeArrayCallBacks);
	if (da->session == NULL || da->queue == NULL || watch == NULL) {
		if (watch != NULL)
			CFRelease(watch);
		if (da->session != NULL)
			CFRelease(da->session);
		if (da->queue != NULL)
			dispatch_release(da->queue);
		pthread_mutex_destroy(&da->lock);
		free(da);
		return NTFSMAC_DMN_E_IO;
	}
	/* The approval callback first, so there is no window without it. */
	DARegisterDiskMountApprovalCallback(da->session, NULL, on_approval, da);
	DARegisterDiskAppearedCallback(da->session, NULL, on_appeared, da);
	DARegisterDiskDisappearedCallback(da->session, NULL, on_disappeared, da);
	DARegisterDiskDescriptionChangedCallback(da->session, NULL, watch, on_changed, da);
	CFRelease(watch);
	DASessionSetDispatchQueue(da->session, da->queue);
	*out = da;
	return NTFSMAC_DMN_OK;
}

static void noop(void *p)
{
	(void)p;
}

void ntfsmac_da_stop(ntfsmac_da *da)
{
	if (da == NULL)
		return;
	DASessionSetDispatchQueue(da->session, NULL);
	DAUnregisterCallback(da->session, (void *)on_approval, da);
	DAUnregisterCallback(da->session, (void *)on_appeared, da);
	DAUnregisterCallback(da->session, (void *)on_disappeared, da);
	DAUnregisterCallback(da->session, (void *)on_changed, da);
	dispatch_sync_f(da->queue, NULL, noop); /* drain callbacks in flight */
	CFRelease(da->session);
	dispatch_release(da->queue);
	pthread_mutex_destroy(&da->lock);
	free(da);
}

/* ---------------------------------------------------------------- */
/* Dissent, describe                                                 */
/* ---------------------------------------------------------------- */

ntfsmac_dmn_status ntfsmac_da_dissent(ntfsmac_da *da, const char *bsd_name)
{
	bool ok;

	if (da == NULL || !ntfsmac_da_bsd_name_valid(bsd_name))
		return NTFSMAC_DMN_E_INVAL;
	pthread_mutex_lock(&da->lock);
	ok = ntfsmac_da_table_add(&da->dissent, bsd_name);
	pthread_mutex_unlock(&da->lock);
	return ok ? NTFSMAC_DMN_OK : NTFSMAC_DMN_E_BUSY;
}

static void mount_done(DADiskRef disk, DADissenterRef dissenter, void *context)
{
	(void)disk;
	(void)dissenter;
	(void)context;
}

ntfsmac_dmn_status ntfsmac_da_withdraw(ntfsmac_da *da, const char *bsd_name)
{
	bool refused = false;
	bool had;
	DADiskRef disk;

	if (da == NULL || !ntfsmac_da_bsd_name_valid(bsd_name))
		return NTFSMAC_DMN_E_INVAL;
	pthread_mutex_lock(&da->lock);
	had = ntfsmac_da_table_remove(&da->dissent, bsd_name, &refused);
	pthread_mutex_unlock(&da->lock);
	if (!had || !refused)
		return NTFSMAC_DMN_OK;
	disk = DADiskCreateFromBSDName(kCFAllocatorDefault, da->session, bsd_name);
	if (disk == NULL)
		return NTFSMAC_DMN_E_INVAL;
	DADiskMount(disk, NULL, kDADiskMountOptionDefault, mount_done, NULL);
	CFRelease(disk);
	return NTFSMAC_DMN_OK;
}

ntfsmac_dmn_status ntfsmac_da_describe(ntfsmac_da *da, const char *bsd_name,
				       ntfsmac_disk_info *out)
{
	DADiskRef disk;
	bool ok;

	if (out != NULL)
		memset(out, 0, sizeof *out);
	if (da == NULL || out == NULL || !ntfsmac_da_bsd_name_valid(bsd_name))
		return NTFSMAC_DMN_E_INVAL;
	disk = DADiskCreateFromBSDName(kCFAllocatorDefault, da->session, bsd_name);
	if (disk == NULL)
		return NTFSMAC_DMN_E_INVAL;
	ok = describe_disk(disk, out);
	CFRelease(disk);
	return ok ? NTFSMAC_DMN_OK : NTFSMAC_DMN_E_INVAL;
}

/* ---------------------------------------------------------------- */
/* Unmount and eject: start, then wait for the completion callback    */
/* ---------------------------------------------------------------- */

typedef struct waiter {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	bool done;
	bool abandoned;
	DAReturn result;
} waiter;

static void waiter_done(DADiskRef disk, DADissenterRef dissenter, void *context)
{
	waiter *w = context;
	bool free_it;

	(void)disk;
	pthread_mutex_lock(&w->lock);
	w->result = dissenter != NULL ? DADissenterGetStatus(dissenter) : kDAReturnSuccess;
	if (w->result == kDAReturnSuccess && dissenter != NULL)
		w->result = kDAReturnError;
	w->done = true;
	free_it = w->abandoned;
	pthread_cond_broadcast(&w->cond);
	pthread_mutex_unlock(&w->lock);
	if (free_it) {
		pthread_mutex_destroy(&w->lock);
		pthread_cond_destroy(&w->cond);
		free(w);
	}
}

static ntfsmac_dmn_status wait_for(waiter *w)
{
	struct timespec ts;
	ntfsmac_dmn_status st;
	int rc = 0;

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += OP_TIMEOUT_S;
	pthread_mutex_lock(&w->lock);
	while (!w->done && rc == 0)
		rc = pthread_cond_timedwait(&w->cond, &w->lock, &ts);
	if (!w->done) {
		/* The callback frees the waiter when it finally runs. */
		w->abandoned = true;
		pthread_mutex_unlock(&w->lock);
		return NTFSMAC_DMN_E_TIMEOUT;
	}
	st = map_dareturn(w->result);
	pthread_mutex_unlock(&w->lock);
	pthread_mutex_destroy(&w->lock);
	pthread_cond_destroy(&w->cond);
	free(w);
	return st;
}

static waiter *waiter_new(void)
{
	waiter *w = calloc(1, sizeof *w);

	if (w == NULL)
		return NULL;
	pthread_mutex_init(&w->lock, NULL);
	pthread_cond_init(&w->cond, NULL);
	return w;
}

ntfsmac_dmn_status ntfsmac_da_unmount(ntfsmac_da *da, const char *bsd_name)
{
	DADiskRef disk;
	waiter *w;

	if (da == NULL || !ntfsmac_da_bsd_name_valid(bsd_name))
		return NTFSMAC_DMN_E_INVAL;
	disk = DADiskCreateFromBSDName(kCFAllocatorDefault, da->session, bsd_name);
	if (disk == NULL)
		return NTFSMAC_DMN_E_INVAL;
	w = waiter_new();
	if (w == NULL) {
		CFRelease(disk);
		return NTFSMAC_DMN_E_IO;
	}
	DADiskUnmount(disk, kDADiskUnmountOptionDefault, waiter_done, w);
	CFRelease(disk);
	return wait_for(w);
}

ntfsmac_dmn_status ntfsmac_da_eject(ntfsmac_da *da, const char *bsd_name)
{
	DADiskRef disk, whole;
	waiter *w;

	if (da == NULL || !ntfsmac_da_bsd_name_valid(bsd_name))
		return NTFSMAC_DMN_E_INVAL;
	disk = DADiskCreateFromBSDName(kCFAllocatorDefault, da->session, bsd_name);
	if (disk == NULL)
		return NTFSMAC_DMN_E_INVAL;
	whole = DADiskCopyWholeDisk(disk);
	CFRelease(disk);
	if (whole == NULL)
		return NTFSMAC_DMN_E_INVAL;
	w = waiter_new();
	if (w == NULL) {
		CFRelease(whole);
		return NTFSMAC_DMN_E_IO;
	}
	DADiskEject(whole, kDADiskEjectOptionDefault, waiter_done, w);
	CFRelease(whole);
	return wait_for(w);
}

bool ntfsmac_da_same_path(const char *a, const char *b)
{
	CFStringRef x, y;
	CFMutableStringRef mx, my;
	bool same;

	if (a == NULL || b == NULL)
		return false;
	if (strcmp(a, b) == 0)
		return true;
	x = CFStringCreateWithCString(kCFAllocatorDefault, a, kCFStringEncodingUTF8);
	y = CFStringCreateWithCString(kCFAllocatorDefault, b, kCFStringEncodingUTF8);
	if (x == NULL || y == NULL) {
		if (x != NULL)
			CFRelease(x);
		if (y != NULL)
			CFRelease(y);
		return false;
	}
	mx = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, x);
	my = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, y);
	CFRelease(x);
	CFRelease(y);
	if (mx == NULL || my == NULL) {
		if (mx != NULL)
			CFRelease(mx);
		if (my != NULL)
			CFRelease(my);
		return false;
	}
	CFStringNormalize(mx, kCFStringNormalizationFormC);
	CFStringNormalize(my, kCFStringNormalizationFormC);
	same = CFStringCompare(mx, my, 0) == kCFCompareEqualTo;
	CFRelease(mx);
	CFRelease(my);
	return same;
}

#else /* !__APPLE__ */

bool ntfsmac_da_same_path(const char *a, const char *b)
{
	return a != NULL && b != NULL && strcmp(a, b) == 0;
}

/* No Disk Arbitration on this host: every call reports "not implemented". */

ntfsmac_dmn_status ntfsmac_da_start(const ntfsmac_da_callbacks *cb, void *ctx,
				    ntfsmac_da **out)
{
	(void)cb;
	(void)ctx;
	if (out != NULL)
		*out = NULL;
	return NTFSMAC_DMN_E_NOTIMPL;
}

void ntfsmac_da_stop(ntfsmac_da *da)
{
	(void)da;
}

ntfsmac_dmn_status ntfsmac_da_dissent(ntfsmac_da *da, const char *bsd_name)
{
	(void)da;
	(void)bsd_name;
	return NTFSMAC_DMN_E_NOTIMPL;
}

ntfsmac_dmn_status ntfsmac_da_withdraw(ntfsmac_da *da, const char *bsd_name)
{
	(void)da;
	(void)bsd_name;
	return NTFSMAC_DMN_E_NOTIMPL;
}

ntfsmac_dmn_status ntfsmac_da_describe(ntfsmac_da *da, const char *bsd_name,
				       ntfsmac_disk_info *out)
{
	(void)da;
	(void)bsd_name;
	(void)out;
	return NTFSMAC_DMN_E_NOTIMPL;
}

ntfsmac_dmn_status ntfsmac_da_unmount(ntfsmac_da *da, const char *bsd_name)
{
	(void)da;
	(void)bsd_name;
	return NTFSMAC_DMN_E_NOTIMPL;
}

ntfsmac_dmn_status ntfsmac_da_eject(ntfsmac_da *da, const char *bsd_name)
{
	(void)da;
	(void)bsd_name;
	return NTFSMAC_DMN_E_NOTIMPL;
}

#endif /* __APPLE__ */
