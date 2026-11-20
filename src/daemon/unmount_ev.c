/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Per-session bookkeeping of rule 3 of the clean-session proof (ARCHITECTURE section 7): what the
 * daemon knows about the unmount of one session. Pure, no locking (the caller holds g_mu).
 */
#include "internal.h"

#include <stdio.h>
#include <string.h>

void ntfsmac_uev_begin(ntfsmac_unmount_ev *e, uint64_t session_id)
{
	memset(e, 0, sizeof *e);
	e->session_id = session_id;
}

void ntfsmac_uev_reset(ntfsmac_unmount_ev *e)
{
	memset(e, 0, sizeof *e);
}

void ntfsmac_uev_note_unmount_call(ntfsmac_unmount_ev *e, ntfsmac_dmn_status st, bool still_mounted)
{
	/* Only a call that returned OK and left no mount counts; a timeout or a busy answer never. */
	e->requested = st == NTFSMAC_DMN_OK && !still_mounted;
}

void ntfsmac_uev_unmount_failed(ntfsmac_unmount_ev *e)
{
	e->requested = false;
}

bool ntfsmac_uev_bind(ntfsmac_unmount_ev *e, uint64_t current_session, const char *bsd)
{
	if (e->session_id == 0 || e->session_id != current_session || bsd == NULL || bsd[0] == '\0' ||
	    strlen(bsd) >= sizeof e->fuse_bsd)
		return false;
	strcpy(e->fuse_bsd, bsd);
	return true;
}

bool ntfsmac_uev_da_gone(ntfsmac_unmount_ev *e, uint64_t current_session, const char *bsd)
{
	if (e->session_id == 0 || e->session_id != current_session || e->fuse_bsd[0] == '\0' ||
	    bsd == NULL || strcmp(e->fuse_bsd, bsd) != 0)
		return false;
	e->da_seen = true;
	return true;
}

bool ntfsmac_uev_have(const ntfsmac_unmount_ev *e)
{
	return e->requested || e->da_seen;
}

bool ntfsmac_uev_wait_more(const ntfsmac_unmount_ev *e, unsigned waited_ms, unsigned max_ms,
			   bool stopping)
{
	return !ntfsmac_uev_have(e) && !stopping && waited_ms < max_ms;
}
