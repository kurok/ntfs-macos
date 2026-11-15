/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The one severity table of the probe states (docs/ARCHITECTURE.md section 3.1):
 *   unsupported > corrupt > mirror_split > hibernated > fast_startup >
 *   unclean_log > dirty_flag > names_not_nfc > unknown > clean.
 * The probe derives `state` from it (src/probe/state.c, probe_state_rank()); the public
 * accessor is ntfsmac_probe_state_severity() of include/ntfsmac/probe.h. The verdict parser
 * does not use it: it applies no state-versus-reason consistency rule beyond the cross-field
 * rules of section 3.1.
 * Pure C, no I/O.
 */
#include "ntfsmac/probe.h"

static const signed char severity[NTFSMAC_PROBE_STATE_COUNT_] = {
	[NTFSMAC_PROBE_STATE_UNSUPPORTED] = 9,
	[NTFSMAC_PROBE_STATE_CORRUPT] = 8,
	[NTFSMAC_PROBE_STATE_MIRROR_SPLIT] = 7,
	[NTFSMAC_PROBE_STATE_HIBERNATED] = 6,
	[NTFSMAC_PROBE_STATE_FAST_STARTUP] = 5,
	[NTFSMAC_PROBE_STATE_UNCLEAN_LOG] = 4,
	[NTFSMAC_PROBE_STATE_DIRTY_FLAG] = 3,
	[NTFSMAC_PROBE_STATE_NAMES_NOT_NFC] = 2,
	[NTFSMAC_PROBE_STATE_UNKNOWN] = 1,
	[NTFSMAC_PROBE_STATE_CLEAN] = 0,
};

_Static_assert(NTFSMAC_PROBE_STATE_COUNT_ == 10, "the severity table lists every state once");

int ntfsmac_probe_state_severity(ntfsmac_probe_state s)
{
	if ((unsigned)s >= (unsigned)NTFSMAC_PROBE_STATE_COUNT_)
		return -1;
	return severity[s];
}
