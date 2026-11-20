/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-DMN-UEV-*: the per-session evidence of rule 3 of the clean-session proof (unmount_ev.c).
 * Found by T-M5-05 and its review: a bounded unmount that timed out, a stale session or another
 * disk must never make a later module death look like a clean unmount.
 */
#include "check.h"
#include "internal.h"

#include <string.h>

static ntfsmac_unmount_ev session(uint64_t id)
{
	ntfsmac_unmount_ev e;

	ntfsmac_uev_begin(&e, id);
	return e;
}

int main(void)
{
	ntfsmac_unmount_ev e;

	/* fresh session: nothing known, the wait goes on until its bound */
	e = session(7);
	CHECK(!ntfsmac_uev_have(&e));
	CHECK(ntfsmac_uev_wait_more(&e, 0, 5000, false));
	CHECK(ntfsmac_uev_wait_more(&e, 4950, 5000, false));
	CHECK(!ntfsmac_uev_wait_more(&e, 5000, 5000, false));
	CHECK(!ntfsmac_uev_wait_more(&e, 0, 5000, true)); /* the daemon is stopping */

	/* the request counts only for OK with no mount left */
	e = session(7);
	ntfsmac_uev_note_unmount_call(&e, NTFSMAC_DMN_OK, false);
	CHECK(e.requested && ntfsmac_uev_have(&e));
	CHECK(!ntfsmac_uev_wait_more(&e, 0, 5000, false)); /* known: no wait */
	e = session(7);
	ntfsmac_uev_note_unmount_call(&e, NTFSMAC_DMN_E_TIMEOUT, false); /* every unmount(2) got EBUSY */
	CHECK(!e.requested);
	ntfsmac_uev_note_unmount_call(&e, NTFSMAC_DMN_E_BUSY, false);
	CHECK(!e.requested);
	ntfsmac_uev_note_unmount_call(&e, NTFSMAC_DMN_E_STATE, false);
	CHECK(!e.requested);
	ntfsmac_uev_note_unmount_call(&e, NTFSMAC_DMN_OK, true); /* OK but the mount is still there */
	CHECK(!e.requested);

	/* cleared again when a later call fails or the BUSY path runs */
	ntfsmac_uev_note_unmount_call(&e, NTFSMAC_DMN_OK, false);
	CHECK(e.requested);
	ntfsmac_uev_note_unmount_call(&e, NTFSMAC_DMN_E_TIMEOUT, false);
	CHECK(!e.requested);
	ntfsmac_uev_note_unmount_call(&e, NTFSMAC_DMN_OK, false);
	ntfsmac_uev_unmount_failed(&e);
	CHECK(!e.requested);

	/* the DA report: only the bound BSD name, only while the session is current */
	e = session(7);
	CHECK(!ntfsmac_uev_da_gone(&e, 7, "disk7s1")); /* nothing bound yet */
	CHECK(!ntfsmac_uev_bind(&e, 8, "disk7s1"));    /* another (newer) session is current */
	CHECK(!ntfsmac_uev_bind(&e, 7, ""));
	CHECK(ntfsmac_uev_bind(&e, 7, "disk7s1"));
	CHECK(!ntfsmac_uev_da_gone(&e, 7, "disk9s1")); /* another disk */
	CHECK(!ntfsmac_uev_da_gone(&e, 7, "disk7"));   /* the whole disk is not the bound one */
	CHECK(!ntfsmac_uev_da_gone(&e, 8, "disk7s1")); /* the volume has a newer session */
	CHECK(!ntfsmac_uev_da_gone(&e, 0, "disk7s1")); /* no session */
	CHECK(!e.da_seen);
	CHECK(ntfsmac_uev_da_gone(&e, 7, "disk7s1"));
	CHECK(e.da_seen && ntfsmac_uev_have(&e));
	CHECK(!ntfsmac_uev_wait_more(&e, 0, 5000, false));

	/* the DA report arriving before the bind of a later session cannot count for it */
	e = session(7);
	CHECK(ntfsmac_uev_bind(&e, 7, "disk7s1"));
	ntfsmac_uev_reset(&e); /* teardown */
	CHECK(!ntfsmac_uev_da_gone(&e, 0, "disk7s1"));
	CHECK(!ntfsmac_uev_bind(&e, 0, "disk7s1"));
	ntfsmac_uev_begin(&e, 9); /* the next attach resets everything */
	CHECK(!e.requested && !e.da_seen && e.fuse_bsd[0] == '\0');
	CHECK(!ntfsmac_uev_da_gone(&e, 9, "disk7s1")); /* the old session's disk: not bound */

	/* attach and teardown reset a session that had both */
	e = session(7);
	ntfsmac_uev_note_unmount_call(&e, NTFSMAC_DMN_OK, false);
	CHECK(ntfsmac_uev_bind(&e, 7, "disk7s1") && ntfsmac_uev_da_gone(&e, 7, "disk7s1"));
	ntfsmac_uev_reset(&e);
	CHECK(!ntfsmac_uev_have(&e) && e.session_id == 0);

	if (g_failures != 0) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	return 0;
}
