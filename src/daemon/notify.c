/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stub of notifications. T-M4-08 replaces this file. */
#include "internal.h"

ntfsmac_dmn_status ntfsmac_notify_post(ntfsmac_notify_kind kind, const char *label,
				       const char *reason)
{
	(void)kind;
	(void)label;
	(void)reason;
	return NTFSMAC_DMN_E_NOTIMPL;
}
