/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stub of conflict detection. T-M4-07 replaces this file. */
#include "internal.h"

ntfsmac_dmn_status ntfsmac_conflict_detect(ntfsmac_conflict_report *out)
{
	if (out != NULL) {
		out->passive = false;
		out->drivers_found = 0;
	}
	return NTFSMAC_DMN_E_NOTIMPL;
}
