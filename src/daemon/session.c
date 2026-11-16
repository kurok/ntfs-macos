/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stub of the session lifecycle module. T-M4-14 replaces this file. */
#include "internal.h"

ntfsmac_dmn_status ntfsmac_session_begin(ntfsmac_journal *j, const ntfsmac_probe_verdict *v,
					 ntfsmac_policy_decision decision,
					 ntfsmac_session **out, ntfsmac_vol_mode *mode)
{
	(void)j;
	(void)v;
	(void)decision;
	if (out != NULL)
		*out = NULL;
	if (mode != NULL)
		*mode = NTFSMAC_VMODE_NONE;
	return NTFSMAC_DMN_E_NOTIMPL;
}

ntfsmac_dmn_status ntfsmac_session_end(ntfsmac_session *s, const ntfsmac_errwatch *w,
				       bool unmount_seen_first, int worker_exit_status)
{
	(void)s;
	(void)w;
	(void)unmount_seen_first;
	(void)worker_exit_status;
	return NTFSMAC_DMN_E_NOTIMPL;
}

void ntfsmac_session_free(ntfsmac_session *s)
{
	(void)s;
}
