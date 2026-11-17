/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * `repair` with action `sync_mirror` (T-M4-16, docs/ARCHITECTURE.md sections 3.2, 3.4 and 3.7,
 * ADR-021).
 *
 * By the time this runs, checks 1 to 4 of ADR-019 have passed on the XPC door (ctl.c refuses the
 * privileged requests on the socket, and ctl_peer.c does the checks before any operation is
 * called). What is left is check 5, as for `repair --clear-dirty`: the volume exists, a probe run
 * for this request shows what the gate needs, and the typed label and serial match what that
 * fresh probe says. Then the one write, `ntfsmac-volmark sync-mirror` on the descriptor bound to
 * the probed volume (volmark re-checks the binding and the whole rule itself), and a second fresh
 * probe whose state goes back in the reply.
 *
 * This request never reads, writes or removes a journal entry (ADR-021 item 5): the next attach
 * goes through the policy table, so a volume that comes back with the dirty flag set is `ro`.
 * It never mounts the volume. Every refusal is a fixed word, as in ctl_journal.c.
 */
#include <stdio.h>
#include <string.h>

#include "internal.h"

/*
 * Did volmark stop before its first write? Its exits 10 (refused), 13 (flags), 2 and 3 (usage,
 * descriptor) and the pre-write timeout (-1) and "not run" (-3) all leave the volume as it was.
 * Everything else (the write timeout -2, exits 12 and 14, E_IO after exit 0 where sector 0
 * changed, E_BUSY with -4, an unknown exit code) may have written: the mirror may be partly written.
 */
static bool nothing_written(int vexit)
{
	return vexit == 10 || vexit == 13 || vexit == 2 || vexit == 3 || vexit == -1 || vexit == -3;
}

/* State of a fresh probe, as a fixed word; "unknown" if it cannot be run. */
static const char *fresh_state(const ntfsmac_priv *p, const char *bsd)
{
	static ntfsmac_probe_verdict after; /* serialised by the caller's mutex */
	ntfsmac_priv_bind b;
	const char *state = "unknown";

	memset(&after, 0, sizeof after);
	b.fd = -1;
	if (p->sys->probe_fresh(p->sys->ctx, bsd, &after, &b) == NTFSMAC_DMN_OK) {
		const char *n = ntfsmac_probe_state_name(after.state);

		if (n != NULL)
			state = n;
	}
	ntfsmac_priv_release_bind(p, &b);
	return state;
}

ntfsmac_dmn_status ntfsmac_priv_repair_mirror(ntfsmac_priv *p, const char *bsd,
					      const char *confirm_label, const char *confirm_serial,
					      char *reply, size_t cap)
{
	ntfsmac_priv_volume vol;
	ntfsmac_priv_bind bind;
	ntfsmac_policy_repair_input in;
	ntfsmac_policy_gate gate;
	ntfsmac_probe_verdict *v;
	ntfsmac_dmn_status ws;
	const char *state, *reason;
	int vexit = -3;
	/* About 10 KiB each; the caller holds the mutex of the privileged requests. */
	static ntfsmac_probe_verdict before;

	if (reply == NULL || cap == 0)
		return NTFSMAC_DMN_E_INVAL;
	reply[0] = '\0';
	if (p == NULL || p->sys == NULL || bsd == NULL || confirm_label == NULL || confirm_serial == NULL)
		return NTFSMAC_DMN_E_INVAL;
	if (p->sys->sync_mirror == NULL)
		return NTFSMAC_DMN_E_NOTIMPL;
	v = &before;
	memset(&vol, 0, sizeof vol);
	if (!p->sys->volume(p->sys->ctx, bsd, &vol))
		return ntfsmac_priv_refuse(reply, cap, "no_volume");
	memset(v, 0, sizeof *v);
	bind.fd = -1;
	if (p->sys->probe_fresh(p->sys->ctx, bsd, v, &bind) != NTFSMAC_DMN_OK) {
		ntfsmac_priv_release_bind(p, &bind);
		return ntfsmac_priv_refuse(reply, cap, "probe_failed");
	}

	memset(&in, 0, sizeof in);
	in.probe = v;
	in.mounted = vol.mounted;
	in.peer_authorised = true; /* checks 1 to 4 ran before this function was called */
	in.confirmation_ok = ntfsmac_priv_confirmation_ok(v, confirm_label, confirm_serial);
	gate = ntfsmac_policy_gate_sync_mirror(&in);
	if (gate != NTFSMAC_GATE_ALLOWED) {
		ntfsmac_priv_release_bind(p, &bind);
		return ntfsmac_priv_refuse(reply, cap, ntfsmac_priv_gate_name(gate));
	}

	/* The write takes over and closes the descriptor, whatever it returns. */
	ws = p->sys->sync_mirror(p->sys->ctx, bsd, v, &bind, &vexit);
	bind.fd = -1;
	state = fresh_state(p, bsd);
	if (ws == NTFSMAC_DMN_OK) {
		snprintf(reply, cap,
			 "{\"ok\":true,\"result\":\"repaired\",\"state\":\"%s\",\"journal_entry\":\"untouched\"}",
			 state);
		return NTFSMAC_DMN_OK;
	}
	/*
	 * Not repaired. Say whether volmark stopped before it printed `w` (nothing was written) or
	 * after (a mirror record may be torn), and what a fresh probe shows now.
	 */
	if (ws == NTFSMAC_DMN_E_DENIED || vexit == 15)
		reason = "binding";
	else if (nothing_written(vexit))
		reason = "volmark_refused";
	else
		reason = NULL;
	if (reason != NULL) {
		snprintf(reply, cap, "{\"ok\":false,\"error\":\"denied\",\"reason\":\"%s\",\"state\":\"%s\"}",
			 reason, state);
		return NTFSMAC_DMN_E_DENIED;
	}
	snprintf(reply, cap,
		 "{\"ok\":false,\"error\":\"failed\",\"status\":\"%s\",\"reason\":\"volmark_failed\",\"state\":\"%s\"}",
		 ntfsmac_dmn_status_name(ws), state);
	return ws;
}
