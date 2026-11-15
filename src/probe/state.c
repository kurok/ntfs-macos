/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "state.h"

int probe_state_rank(ntfsmac_probe_state s)
{
	return ntfsmac_probe_state_severity(s);
}

ntfsmac_probe_state probe_state_of_reason(ntfsmac_probe_reason r)
{
	switch (r) {
	case NTFSMAC_PROBE_REASON_NOT_NTFS:
	case NTFSMAC_PROBE_REASON_BITLOCKER:
		return NTFSMAC_PROBE_STATE_UNSUPPORTED;
	case NTFSMAC_PROBE_REASON_MOUNT_FAILED:
		return NTFSMAC_PROBE_STATE_CORRUPT;
	case NTFSMAC_PROBE_REASON_MFTMIRR_STALE:
		return NTFSMAC_PROBE_STATE_MIRROR_SPLIT;
	case NTFSMAC_PROBE_REASON_HIBER_SIGNATURE:
	case NTFSMAC_PROBE_REASON_HIBER_WAKE:
	case NTFSMAC_PROBE_REASON_HIBER_SHORT:
		return NTFSMAC_PROBE_STATE_HIBERNATED;
	case NTFSMAC_PROBE_REASON_LOG_V2:
		return NTFSMAC_PROBE_STATE_FAST_STARTUP;
	case NTFSMAC_PROBE_REASON_LOG_UNCLEAN:
		return NTFSMAC_PROBE_STATE_UNCLEAN_LOG;
	case NTFSMAC_PROBE_REASON_FLAG_DIRTY:
	case NTFSMAC_PROBE_REASON_FLAG_CHKDSK_UNDERWAY:
	case NTFSMAC_PROBE_REASON_FLAG_PENDING_WORK:
	case NTFSMAC_PROBE_REASON_FLAG_UNKNOWN_BITS:
		return NTFSMAC_PROBE_STATE_DIRTY_FLAG;
	case NTFSMAC_PROBE_REASON_NAMES_NOT_NFC:
		return NTFSMAC_PROBE_STATE_NAMES_NOT_NFC;
	case NTFSMAC_PROBE_REASON_LOG_UNREADABLE:
	case NTFSMAC_PROBE_REASON_HIBER_UNREADABLE:
	case NTFSMAC_PROBE_REASON_VERSION_UNKNOWN:
	case NTFSMAC_PROBE_REASON_CHECK_FAILED:
	case NTFSMAC_PROBE_REASON_COUNT_:
	default:
		return NTFSMAC_PROBE_STATE_UNKNOWN;
	}
}

ntfsmac_probe_state probe_state_of_reasons(uint32_t reasons)
{
	ntfsmac_probe_state best = NTFSMAC_PROBE_STATE_CLEAN;
	unsigned r;

	for (r = 0; r < (unsigned)NTFSMAC_PROBE_REASON_COUNT_; r++) {
		ntfsmac_probe_state s;

		if (!(reasons & NTFSMAC_PROBE_REASON_BIT(r)))
			continue;
		s = probe_state_of_reason((ntfsmac_probe_reason)r);
		if (probe_state_rank(s) > probe_state_rank(best))
			best = s;
	}
	return best;
}
