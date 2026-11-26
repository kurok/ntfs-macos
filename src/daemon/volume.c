/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Volume state machine (docs/ARCHITECTURE.md section 5; the data flows of
 * sections 4.1 and 4.5 give the events). Pure C: no I/O, no clock, no
 * platform API. One record belongs to one volume serial queue, so there is no
 * locking here.
 *
 * Transitions (S = state, disk present unless noted):
 *
 *   Appeared   START          -> Probing
 *   Failed     START          -> Probing     (a `mount` request retries)
 *   Probing    PROBE_DONE     -> Deciding
 *   Probing    PROBE_FAILED   -> Failed
 *   Deciding   DECIDED        -> Mounting    (records the planned mode)
 *   Deciding   REFUSED        -> Failed
 *   Mounting   MOUNTED        -> Mounted     (records the real mode)
 *   Mounting   MOUNT_FAILED   -> Failed
 *   Mounted    UNMOUNT        -> Unmounting
 *   Unmounting UNMOUNT_BUSY   -> Mounted     (disk present only)
 *   Unmounting UNMOUNTED      -> Appeared    (disk present) or Gone
 *   Unmounting UNMOUNT_FAILED -> Failed      (disk present) or Gone
 *   Unmounting DISK_GONE      -> Unmounting  (records the loss)
 *   Appeared, Probing, Deciding, Failed  DISK_GONE -> Gone
 *   Mounting, Mounted                    DISK_GONE -> Unmounting
 *
 * Every other pair is refused. Gone accepts nothing.
 */
#include "internal.h"

#include <stddef.h>

#define NO_NEXT NTFSMAC_VOL_STATE_COUNT_

ntfsmac_vol_state ntfsmac_vol_next_state(ntfsmac_vol_state state,
					 ntfsmac_vol_event event,
					 bool disk_present)
{
	if ((unsigned)state >= (unsigned)NTFSMAC_VOL_STATE_COUNT_ ||
	    (unsigned)event >= (unsigned)NTFSMAC_VEV_COUNT_)
		return NO_NEXT;

	switch (state) {
	case NTFSMAC_VOL_APPEARED:
	case NTFSMAC_VOL_FAILED:
		if (event == NTFSMAC_VEV_START)
			return disk_present ? NTFSMAC_VOL_PROBING : NO_NEXT;
		if (event == NTFSMAC_VEV_DISK_GONE)
			return NTFSMAC_VOL_GONE;
		return NO_NEXT;
	case NTFSMAC_VOL_PROBING:
		if (event == NTFSMAC_VEV_PROBE_DONE)
			return NTFSMAC_VOL_DECIDING;
		if (event == NTFSMAC_VEV_PROBE_FAILED)
			return NTFSMAC_VOL_FAILED;
		if (event == NTFSMAC_VEV_DISK_GONE)
			return NTFSMAC_VOL_GONE;
		return NO_NEXT;
	case NTFSMAC_VOL_DECIDING:
		if (event == NTFSMAC_VEV_DECIDED)
			return NTFSMAC_VOL_MOUNTING;
		if (event == NTFSMAC_VEV_REFUSED)
			return NTFSMAC_VOL_FAILED;
		if (event == NTFSMAC_VEV_DISK_GONE)
			return NTFSMAC_VOL_GONE;
		return NO_NEXT;
	case NTFSMAC_VOL_MOUNTING:
		if (event == NTFSMAC_VEV_MOUNTED)
			return NTFSMAC_VOL_MOUNTED;
		if (event == NTFSMAC_VEV_MOUNT_FAILED)
			return NTFSMAC_VOL_FAILED;
		/* The worker, volmark or the mount point may exist: clean up. */
		if (event == NTFSMAC_VEV_DISK_GONE)
			return NTFSMAC_VOL_UNMOUNTING;
		return NO_NEXT;
	case NTFSMAC_VOL_MOUNTED:
		if (event == NTFSMAC_VEV_UNMOUNT || event == NTFSMAC_VEV_DISK_GONE)
			return NTFSMAC_VOL_UNMOUNTING;
		return NO_NEXT;
	case NTFSMAC_VOL_UNMOUNTING:
		if (event == NTFSMAC_VEV_UNMOUNT_BUSY)
			return disk_present ? NTFSMAC_VOL_MOUNTED : NO_NEXT;
		if (event == NTFSMAC_VEV_UNMOUNTED)
			return disk_present ? NTFSMAC_VOL_APPEARED : NTFSMAC_VOL_GONE;
		if (event == NTFSMAC_VEV_UNMOUNT_FAILED)
			return disk_present ? NTFSMAC_VOL_FAILED : NTFSMAC_VOL_GONE;
		if (event == NTFSMAC_VEV_DISK_GONE)
			return NTFSMAC_VOL_UNMOUNTING;
		return NO_NEXT;
	case NTFSMAC_VOL_GONE:
	case NTFSMAC_VOL_STATE_COUNT_:
		break;
	}
	return NO_NEXT;
}

void ntfsmac_volume_init(ntfsmac_volume *v)
{
	if (v == NULL)
		return;
	v->state = NTFSMAC_VOL_APPEARED;
	v->planned_mode = NTFSMAC_VMODE_NONE;
	v->mode = NTFSMAC_VMODE_NONE;
	v->reason = NTFSMAC_VREASON_NONE;
	v->disk_present = true;
}

static bool mode_ok(ntfsmac_vol_mode m)
{
	return m == NTFSMAC_VMODE_RO || m == NTFSMAC_VMODE_RW;
}

/* The reason an event stands for when the caller gives none. */
static ntfsmac_vol_reason default_reason(ntfsmac_vol_event event)
{
	switch (event) {
	case NTFSMAC_VEV_PROBE_FAILED:
		return NTFSMAC_VREASON_PROBE_FAILED;
	case NTFSMAC_VEV_REFUSED:
		return NTFSMAC_VREASON_POLICY_REFUSED;
	case NTFSMAC_VEV_MOUNT_FAILED:
		return NTFSMAC_VREASON_SPAWN;
	case NTFSMAC_VEV_UNMOUNT_FAILED:
		return NTFSMAC_VREASON_UNMOUNT;
	default:
		return NTFSMAC_VREASON_NONE;
	}
}

ntfsmac_vol_result ntfsmac_volume_apply(ntfsmac_volume *v,
					ntfsmac_vol_event event,
					const ntfsmac_vol_event_arg *arg)
{
	ntfsmac_volume n;
	ntfsmac_vol_state next;

	if (v == NULL || (unsigned)v->state >= (unsigned)NTFSMAC_VOL_STATE_COUNT_ ||
	    (unsigned)event >= (unsigned)NTFSMAC_VEV_COUNT_)
		return NTFSMAC_VRES_BAD_ARG;

	next = ntfsmac_vol_next_state(v->state, event, v->disk_present);
	if (next == NO_NEXT)
		return NTFSMAC_VRES_INVALID_TRANSITION;

	n = *v;
	n.state = next;

	switch (event) {
	case NTFSMAC_VEV_START:
		n.planned_mode = NTFSMAC_VMODE_NONE;
		n.mode = NTFSMAC_VMODE_NONE;
		n.reason = NTFSMAC_VREASON_NONE;
		break;
	case NTFSMAC_VEV_DECIDED:
		if (arg == NULL || !mode_ok(arg->mode))
			return NTFSMAC_VRES_BAD_ARG;
		n.planned_mode = arg->mode;
		break;
	case NTFSMAC_VEV_MOUNTED:
		if (arg == NULL || !mode_ok(arg->mode))
			return NTFSMAC_VRES_BAD_ARG;
		/* A ro decision with a writable mount is a failure (step 8). */
		if (v->planned_mode == NTFSMAC_VMODE_RO && arg->mode == NTFSMAC_VMODE_RW)
			return NTFSMAC_VRES_MODE_MISMATCH;
		n.mode = arg->mode; /* rw decision, ro mount: recorded as ro */
		break;
	case NTFSMAC_VEV_PROBE_FAILED:
	case NTFSMAC_VEV_REFUSED:
	case NTFSMAC_VEV_MOUNT_FAILED:
	case NTFSMAC_VEV_UNMOUNT_FAILED: {
		ntfsmac_vol_reason r = default_reason(event);

		if (arg != NULL && arg->reason != NTFSMAC_VREASON_NONE) {
			if ((unsigned)arg->reason >= (unsigned)NTFSMAC_VREASON_COUNT_)
				return NTFSMAC_VRES_BAD_ARG;
			r = arg->reason;
		}
		if (next == NTFSMAC_VOL_FAILED) {
			n.reason = r;
			n.planned_mode = NTFSMAC_VMODE_NONE;
			n.mode = NTFSMAC_VMODE_NONE;
		}
		break;
	}
	case NTFSMAC_VEV_DISK_GONE:
		n.disk_present = false;
		break;
	default:
		break;
	}

	if (next == NTFSMAC_VOL_APPEARED || next == NTFSMAC_VOL_GONE) {
		n.planned_mode = NTFSMAC_VMODE_NONE;
		n.mode = NTFSMAC_VMODE_NONE;
		n.reason = NTFSMAC_VREASON_NONE;
	}

	*v = n;
	return NTFSMAC_VRES_OK;
}

bool ntfsmac_volume_accepts_mount(const ntfsmac_volume *v)
{
	return ntfsmac_volume_mount_refusal(v) == NULL;
}

const char *ntfsmac_volume_mount_refusal(const ntfsmac_volume *v)
{
	if (v == NULL || !v->disk_present ||
	    (v->state != NTFSMAC_VOL_APPEARED && v->state != NTFSMAC_VOL_FAILED))
		return "wrong_state";
	/* Another file system: a `mount` would only unmount it and refuse it again (T-M7-11). */
	if (v->state == NTFSMAC_VOL_FAILED && v->reason == NTFSMAC_VREASON_NOT_NTFS)
		return "not_ntfs";
	return NULL;
}

bool ntfsmac_volume_listed(const ntfsmac_volume *v)
{
	return v != NULL && v->state != NTFSMAC_VOL_GONE &&
	       !(v->state == NTFSMAC_VOL_FAILED && v->reason == NTFSMAC_VREASON_NOT_NTFS);
}

const char *ntfsmac_vol_state_name(ntfsmac_vol_state s)
{
	switch (s) {
	case NTFSMAC_VOL_APPEARED: return "Appeared";
	case NTFSMAC_VOL_PROBING: return "Probing";
	case NTFSMAC_VOL_DECIDING: return "Deciding";
	case NTFSMAC_VOL_MOUNTING: return "Mounting";
	case NTFSMAC_VOL_MOUNTED: return "Mounted";
	case NTFSMAC_VOL_UNMOUNTING: return "Unmounting";
	case NTFSMAC_VOL_GONE: return "Gone";
	case NTFSMAC_VOL_FAILED: return "Failed";
	case NTFSMAC_VOL_STATE_COUNT_: break;
	}
	return "?";
}

const char *ntfsmac_vol_event_name(ntfsmac_vol_event e)
{
	switch (e) {
	case NTFSMAC_VEV_START: return "START";
	case NTFSMAC_VEV_PROBE_DONE: return "PROBE_DONE";
	case NTFSMAC_VEV_PROBE_FAILED: return "PROBE_FAILED";
	case NTFSMAC_VEV_DECIDED: return "DECIDED";
	case NTFSMAC_VEV_REFUSED: return "REFUSED";
	case NTFSMAC_VEV_MOUNTED: return "MOUNTED";
	case NTFSMAC_VEV_MOUNT_FAILED: return "MOUNT_FAILED";
	case NTFSMAC_VEV_UNMOUNT: return "UNMOUNT";
	case NTFSMAC_VEV_UNMOUNT_BUSY: return "UNMOUNT_BUSY";
	case NTFSMAC_VEV_UNMOUNTED: return "UNMOUNTED";
	case NTFSMAC_VEV_UNMOUNT_FAILED: return "UNMOUNT_FAILED";
	case NTFSMAC_VEV_DISK_GONE: return "DISK_GONE";
	case NTFSMAC_VEV_COUNT_: break;
	}
	return "?";
}

const char *ntfsmac_vol_result_name(ntfsmac_vol_result r)
{
	switch (r) {
	case NTFSMAC_VRES_OK: return "ok";
	case NTFSMAC_VRES_INVALID_TRANSITION: return "invalid transition";
	case NTFSMAC_VRES_BAD_ARG: return "bad argument";
	case NTFSMAC_VRES_MODE_MISMATCH: return "mode mismatch";
	}
	return "?";
}

long ntfsmac_vol_reuse_pick(const ntfsmac_vol_slot *slots, size_t n)
{
	long best = -1;
	size_t i;

	if (slots == NULL)
		return -1;
	for (i = 0; i < n; i++) {
		const ntfsmac_vol_slot *s = &slots[i];
		const ntfsmac_vol_slot *b = best >= 0 ? &slots[best] : NULL;

		if (s->state != NTFSMAC_VOL_GONE || s->worker_live || s->pending)
			continue;
		if (b == NULL || (s->not_ntfs && !b->not_ntfs) ||
		    (s->not_ntfs == b->not_ntfs && s->gone_seq < b->gone_seq))
			best = (long)i;
	}
	return best;
}
