/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_TEST_STATEMACHINE_COMMON_H
#define NTFSMAC_TEST_STATEMACHINE_COMMON_H

#include "internal.h"

/* Apply an event that must succeed, for building a start position. */
static inline void must(ntfsmac_volume *v, ntfsmac_vol_event e, ntfsmac_vol_mode mode)
{
	ntfsmac_vol_event_arg a = { mode, NTFSMAC_VREASON_NONE };

	if (ntfsmac_volume_apply(v, e, &a) != NTFSMAC_VRES_OK) {
		fprintf(stderr, "setup: %s refused in %s\n", ntfsmac_vol_event_name(e),
			ntfsmac_vol_state_name(v->state));
		g_failures++;
	}
}

/*
 * Put a fresh volume into `state` by legal events only. `mode` is the mode
 * decided and mounted for states past Deciding. For Unmounting, `gone` says
 * whether the disk disappeared on the way (Unmounting is reached by an eject
 * with the disk present, or by DISK_GONE).
 */
static inline ntfsmac_volume volume_in(ntfsmac_vol_state state, ntfsmac_vol_mode mode, bool gone)
{
	ntfsmac_volume v;

	ntfsmac_volume_init(&v);
	switch (state) {
	case NTFSMAC_VOL_APPEARED:
		break;
	case NTFSMAC_VOL_PROBING:
		must(&v, NTFSMAC_VEV_START, NTFSMAC_VMODE_NONE);
		break;
	case NTFSMAC_VOL_DECIDING:
		must(&v, NTFSMAC_VEV_START, NTFSMAC_VMODE_NONE);
		must(&v, NTFSMAC_VEV_PROBE_DONE, NTFSMAC_VMODE_NONE);
		break;
	case NTFSMAC_VOL_MOUNTING:
		must(&v, NTFSMAC_VEV_START, NTFSMAC_VMODE_NONE);
		must(&v, NTFSMAC_VEV_PROBE_DONE, NTFSMAC_VMODE_NONE);
		must(&v, NTFSMAC_VEV_DECIDED, mode);
		break;
	case NTFSMAC_VOL_MOUNTED:
		must(&v, NTFSMAC_VEV_START, NTFSMAC_VMODE_NONE);
		must(&v, NTFSMAC_VEV_PROBE_DONE, NTFSMAC_VMODE_NONE);
		must(&v, NTFSMAC_VEV_DECIDED, mode);
		must(&v, NTFSMAC_VEV_MOUNTED, mode);
		break;
	case NTFSMAC_VOL_UNMOUNTING:
		must(&v, NTFSMAC_VEV_START, NTFSMAC_VMODE_NONE);
		must(&v, NTFSMAC_VEV_PROBE_DONE, NTFSMAC_VMODE_NONE);
		must(&v, NTFSMAC_VEV_DECIDED, mode);
		must(&v, NTFSMAC_VEV_MOUNTED, mode);
		if (gone)
			must(&v, NTFSMAC_VEV_DISK_GONE, NTFSMAC_VMODE_NONE);
		else
			must(&v, NTFSMAC_VEV_UNMOUNT, NTFSMAC_VMODE_NONE);
		break;
	case NTFSMAC_VOL_GONE:
		must(&v, NTFSMAC_VEV_DISK_GONE, NTFSMAC_VMODE_NONE);
		break;
	case NTFSMAC_VOL_FAILED:
		must(&v, NTFSMAC_VEV_START, NTFSMAC_VMODE_NONE);
		must(&v, NTFSMAC_VEV_PROBE_FAILED, NTFSMAC_VMODE_NONE);
		break;
	case NTFSMAC_VOL_STATE_COUNT_:
		break;
	}
	if (v.state != state) {
		fprintf(stderr, "setup: wanted %s, got %s\n", ntfsmac_vol_state_name(state),
			ntfsmac_vol_state_name(v.state));
		g_failures++;
	}
	return v;
}

#endif
