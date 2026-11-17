/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The peer checks of ADR-019 (docs/ARCHITECTURE.md section 3.4), one function
 * per check. This file asks the operating system nothing itself: every system
 * fact comes through `ntfsmac_ctl_sys`, so each check can be tested alone.
 *
 * Every check fails closed: when a fact cannot be found out, the request is
 * refused (reason `check_unavailable`), never allowed.
 *
 * Reading guide. A check returns NTFSMAC_CTL_PASS or the refusal to report:
 *
 *   gate  console user or admin          all requests
 *   1     peer uid is in group admin      journal_clear, repair
 *   2     peer is not a spawned process   journal_clear, repair
 *         or one of its descendants
 *   3     peer is the signed CLI          journal_clear, repair
 *   4     fresh administrator password    journal_clear, repair
 *   5     (volume gate, confirmations)    done by the operation, not here
 */
#include <string.h>

#include "ctl_priv.h"

/* ------------------------------------------------------------------ */
/* Gate and check 1                                                    */
/* ------------------------------------------------------------------ */

ntfsmac_ctl_refusal ntfsmac_ctl_check_peer_allowed(const ntfsmac_ctl_peer *peer)
{
	if (peer == NULL)
		return NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE;
	if (peer->console_user || peer->admin)
		return NTFSMAC_CTL_PASS;
	return NTFSMAC_CTL_REFUSE_PEER_NOT_ALLOWED;
}

ntfsmac_ctl_refusal ntfsmac_ctl_check_admin(const ntfsmac_ctl_peer *peer)
{
	if (peer == NULL)
		return NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE;
	return peer->admin ? NTFSMAC_CTL_PASS : NTFSMAC_CTL_REFUSE_NOT_ADMIN;
}

/* ------------------------------------------------------------------ */
/* Check 2: not a spawned process, not a descendant                    */
/* ------------------------------------------------------------------ */

/*
 * The daemon starts every probe, volmark and worker in its own process group,
 * so a spawned child has pgid == pid. A descendant of a child keeps that
 * pgid unless it moves itself to another group. So the walk asks, for the
 * peer and then for each parent in turn: is this pid, or this pid's process
 * group, a live spawned process?
 *
 * Limit, stated in the ADR: a descendant that leaves the group with setsid()
 * and loses its parent (double fork) is not found here. Checks 3 and 4 still
 * hold for it: it must be the signed CLI, and an administrator must type a
 * password.
 */
ntfsmac_ctl_refusal ntfsmac_ctl_check_not_spawned(const ntfsmac_ctl_sys *sys,
						  bool (*is_spawned)(void *, pid_t),
						  void *is_spawned_ctx, pid_t peer_pid)
{
	pid_t pid = peer_pid;
	unsigned depth;

	if (sys == NULL || sys->proc_parent == NULL || is_spawned == NULL || peer_pid <= 1)
		return NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE;

	for (depth = 0; depth < NTFSMAC_CTL_CHAIN_MAX; depth++) {
		pid_t ppid = 0, pgid = 0;
		ntfsmac_ctl_sys_proc r = sys->proc_parent(sys->ctx, pid, &ppid, &pgid);

		if (r == NTFSMAC_CTL_PROC_GONE) {
			/*
			 * The peer itself must exist: it holds the connection. An
			 * ancestor that is gone has died since we read its child; it
			 * can no longer be a live spawned process, so the walk ends.
			 */
			return depth == 0 ? NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE
					  : NTFSMAC_CTL_PASS;
		}
		if (r != NTFSMAC_CTL_PROC_OK)
			return NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE;

		if (is_spawned(is_spawned_ctx, pid) ||
		    (pgid > 1 && is_spawned(is_spawned_ctx, pgid)))
			return NTFSMAC_CTL_REFUSE_SPAWNED_PROCESS;

		if (ppid <= 1)
			return NTFSMAC_CTL_PASS; /* reached launchd or the kernel */
		if (ppid == pid)
			return NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE; /* loop in the answers */
		pid = ppid;
	}
	/* A chain this long is not normal; do not guess. */
	return NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE;
}

/* ------------------------------------------------------------------ */
/* Check 3: the peer is the signed CLI                                 */
/* ------------------------------------------------------------------ */

ntfsmac_ctl_refusal ntfsmac_ctl_check_client_signed(const ntfsmac_ctl_sys *sys,
						    const ntfsmac_ctl_peer *peer)
{
	if (sys == NULL || sys->client_signed == NULL || peer == NULL || !peer->has_audit_token)
		return NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE;
	switch (sys->client_signed(sys->ctx, peer)) {
	case NTFSMAC_CTL_SYS_YES:
		return NTFSMAC_CTL_PASS;
	case NTFSMAC_CTL_SYS_NO:
		return NTFSMAC_CTL_REFUSE_CLIENT_NOT_SIGNED;
	default:
		return NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE;
	}
}

/* ------------------------------------------------------------------ */
/* Check 4: a fresh administrator authorization                        */
/* ------------------------------------------------------------------ */

void ntfsmac_ctl_replay_init(ntfsmac_ctl_replay *r)
{
	memset(r, 0, sizeof *r);
	pthread_mutex_init(&r->mu, NULL);
}

bool ntfsmac_ctl_replay_seen(ntfsmac_ctl_replay *r,
			     const uint8_t form[NTFSMAC_CTL_AUTH_FORM_BYTES])
{
	bool seen = false;
	unsigned i;

	pthread_mutex_lock(&r->mu);
	for (i = 0; i < r->count; i++) {
		if (memcmp(r->form[i], form, NTFSMAC_CTL_AUTH_FORM_BYTES) == 0) {
			seen = true;
			break;
		}
	}
	if (!seen) {
		memcpy(r->form[r->next], form, NTFSMAC_CTL_AUTH_FORM_BYTES);
		r->next = (r->next + 1) % NTFSMAC_CTL_REPLAY_SLOTS;
		if (r->count < NTFSMAC_CTL_REPLAY_SLOTS)
			r->count++;
	}
	pthread_mutex_unlock(&r->mu);
	return seen;
}

ntfsmac_ctl_refusal ntfsmac_ctl_check_right(const ntfsmac_ctl_sys *sys)
{
	if (sys == NULL || sys->right_definition == NULL)
		return NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE;
	switch (sys->right_definition(sys->ctx)) {
	case NTFSMAC_CTL_SYS_YES:
		return NTFSMAC_CTL_PASS;
	case NTFSMAC_CTL_SYS_NO:
		return NTFSMAC_CTL_REFUSE_AUTHORIZATION_RIGHT_INVALID;
	default:
		return NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE;
	}
}

/*
 * The form is a bearer token for one authorization reference of the CLI. It
 * stays valid for as long as that reference lives: the CLI frees the
 * reference after the request (destroying the right) and the rule has
 * timeout 0, but until then any holder of the form can present it again.
 * The replay table is the guard against that: a form is never accepted twice
 * while it is in the table of the last 64. The table is kept in memory only,
 * so a daemon restart empties it; a form copied before the restart is
 * refused only if the CLI has already freed its reference by then.
 */
ntfsmac_ctl_refusal ntfsmac_ctl_check_authorization(const ntfsmac_ctl_sys *sys,
						    ntfsmac_ctl_replay *replay,
						    const uint8_t *form)
{
	ntfsmac_ctl_refusal r;

	if (sys == NULL || sys->authz_verify == NULL || replay == NULL)
		return NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE;
	r = ntfsmac_ctl_check_right(sys);
	if (r != NTFSMAC_CTL_PASS)
		return r;
	if (form == NULL)
		return NTFSMAC_CTL_REFUSE_NO_AUTHORIZATION;
	if (ntfsmac_ctl_replay_seen(replay, form))
		return NTFSMAC_CTL_REFUSE_AUTHORIZATION_REPLAYED;
	switch (sys->authz_verify(sys->ctx, form)) {
	case NTFSMAC_CTL_SYS_YES:
		return NTFSMAC_CTL_PASS;
	case NTFSMAC_CTL_SYS_NO:
		return NTFSMAC_CTL_REFUSE_AUTHORIZATION_DENIED;
	default:
		return NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE;
	}
}

/* ------------------------------------------------------------------ */
/* Check 4, the seam between the two strategies                        */
/* ------------------------------------------------------------------ */

ntfsmac_ctl_authz_strategy ntfsmac_ctl_default_authz(void)
{
#if defined(NTFSMAC_CTL_AUTHZ_FALLBACK_ROOT) && NTFSMAC_CTL_AUTHZ_FALLBACK_ROOT
	return NTFSMAC_CTL_AUTHZ_ROOT_PEER;
#else
	return NTFSMAC_CTL_AUTHZ_EXTERNAL_FORM;
#endif
}

const char *ntfsmac_ctl_authz_name(ntfsmac_ctl_authz_strategy a)
{
	return a == NTFSMAC_CTL_AUTHZ_EXTERNAL_FORM ? "external_form"
	       : a == NTFSMAC_CTL_AUTHZ_ROOT_PEER   ? "root_peer"
						    : "unknown";
}

/*
 * Fallback of ADR-019: no right, no form. The peer must be root, and the uid comes from the
 * audit token of the message (the XPC channel) or from LOCAL_PEERCRED (the socket never
 * reaches this check for the privileged commands). A `sudo ntfsmac ...` run qualifies. An
 * `auth` field, if the client sent one, is ignored: nothing is verified or recorded for it.
 */
ntfsmac_ctl_refusal ntfsmac_ctl_check_root_peer(const ntfsmac_ctl_peer *peer)
{
	if (peer == NULL)
		return NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE;
	return peer->uid == 0 ? NTFSMAC_CTL_PASS : NTFSMAC_CTL_REFUSE_AUTHORIZATION_DENIED;
}

ntfsmac_ctl_refusal ntfsmac_ctl_check_admin_authorization(const ntfsmac_ctl_env *env,
							  const ntfsmac_ctl_peer *peer,
							  const ntfsmac_ctl_request *req)
{
	if (env == NULL || peer == NULL || req == NULL)
		return NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE;
	switch (env->authz) {
	case NTFSMAC_CTL_AUTHZ_EXTERNAL_FORM:
		return ntfsmac_ctl_check_authorization(env->sys, env->replay,
						       req->has_auth ? req->auth : NULL);
	case NTFSMAC_CTL_AUTHZ_ROOT_PEER:
		return ntfsmac_ctl_check_root_peer(peer);
	default:
		return NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE; /* an unknown strategy never passes */
	}
}

/* ------------------------------------------------------------------ */
/* All four, in the order of the ADR                                   */
/* ------------------------------------------------------------------ */

ntfsmac_ctl_refusal ntfsmac_ctl_authorise_privileged(const ntfsmac_ctl_env *env,
						     const ntfsmac_ctl_ops *ops, void *ctx,
						     const ntfsmac_ctl_peer *peer,
						     const ntfsmac_ctl_request *req)
{
	ntfsmac_ctl_refusal r;

	if (env == NULL || ops == NULL || peer == NULL || req == NULL)
		return NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE;

	r = ntfsmac_ctl_check_admin(peer);
	if (r != NTFSMAC_CTL_PASS)
		return r;
	r = ntfsmac_ctl_check_not_spawned(env->sys, ops->is_spawned, ctx, peer->pid);
	if (r != NTFSMAC_CTL_PASS)
		return r;
	r = ntfsmac_ctl_check_client_signed(env->sys, peer);
	if (r != NTFSMAC_CTL_PASS)
		return r;
	return ntfsmac_ctl_check_admin_authorization(env, peer, req);
}
