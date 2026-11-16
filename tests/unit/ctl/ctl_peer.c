/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-CTL-PEER-01..06: each peer check of ADR-019 alone (ctl_peer.c), with fake
 * system facts. A check is tested without the others: the facts that the
 * other checks read are left "good" and only the fact under test changes.
 *   01 gate and check 1 (console user, admin)
 *   02 check 2, the walk over the parent chain
 *   03 check 3, the signature answer
 *   04 check 4, the authorization answer and the replay table
 *   05 fail closed: missing hooks and unknown answers
 *   06 ntfsmac_ctl_authorise_privileged(): order of the four checks
 */
#include "check.h"
#include "fakes.h"

#define PASS NTFSMAC_CTL_PASS

static void test_gate_and_admin(void)
{
	ntfsmac_ctl_peer p = good_peer();

	/* gate: console user or admin */
	CHECK_EQ_INT(ntfsmac_ctl_check_peer_allowed(&p), PASS);
	p.console_user = false;
	CHECK_EQ_INT(ntfsmac_ctl_check_peer_allowed(&p), PASS); /* admin only */
	p.admin = false;
	CHECK_EQ_INT(ntfsmac_ctl_check_peer_allowed(&p), NTFSMAC_CTL_REFUSE_PEER_NOT_ALLOWED);
	p.console_user = true;
	CHECK_EQ_INT(ntfsmac_ctl_check_peer_allowed(&p), PASS); /* console only */
	CHECK_EQ_INT(ntfsmac_ctl_check_peer_allowed(NULL), NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE);

	/* check 1: admin, and only admin; the console user alone does not pass */
	p = good_peer();
	CHECK_EQ_INT(ntfsmac_ctl_check_admin(&p), PASS);
	p.admin = false;
	CHECK_EQ_INT(ntfsmac_ctl_check_admin(&p), NTFSMAC_CTL_REFUSE_NOT_ADMIN);
	p.console_user = false;
	CHECK_EQ_INT(ntfsmac_ctl_check_admin(&p), NTFSMAC_CTL_REFUSE_NOT_ADMIN);
	p.admin = true;
	CHECK_EQ_INT(ntfsmac_ctl_check_admin(&p), PASS); /* admin without the console is fine */
	CHECK_EQ_INT(ntfsmac_ctl_check_admin(NULL), NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE);
}

static void walk(fake_sys_state *s, const ntfsmac_ctl_sys *sys, fake_ops_state *o,
		 ntfsmac_ctl_refusal want, pid_t from, int line)
{
	ntfsmac_ctl_refusal got = ntfsmac_ctl_check_not_spawned(sys, f_is_spawned, o, from);

	(void)s;
	if (got != want)
		fprintf(stderr, "line %d: got %d, want %d\n", line, (int)got, (int)want);
	CHECK_EQ_INT(got, want);
}
#define WALK(want, from) walk(&s, &sys, &o, want, from, __LINE__)

static void test_not_spawned(void)
{
	fake_sys_state s;
	ntfsmac_ctl_sys sys;
	fake_ops_state o;
	int i;

	fake_sys_init(&s, &sys);
	memset(&o, 0, sizeof o);

	/* nothing spawned: 500 -> 400 -> 1 passes */
	WALK(PASS, 500);

	/* the peer itself is a spawned process */
	o.spawned[0] = 500;
	o.nspawned = 1;
	WALK(NTFSMAC_CTL_REFUSE_SPAWNED_PROCESS, 500);

	/* the peer is in the process group of a spawned process (pgid = spawned pid) */
	s.procs[0].pgid = 777;
	o.spawned[0] = 777;
	WALK(NTFSMAC_CTL_REFUSE_SPAWNED_PROCESS, 500);

	/* the parent is a spawned process */
	s.procs[0].pgid = 500;
	o.spawned[0] = 400;
	WALK(NTFSMAC_CTL_REFUSE_SPAWNED_PROCESS, 500);

	/* the parent is in the group of a spawned process */
	s.procs[1].pgid = 888;
	o.spawned[0] = 888;
	WALK(NTFSMAC_CTL_REFUSE_SPAWNED_PROCESS, 500);

	/* a grandparent, five levels up */
	memset(&s.procs, 0, sizeof s.procs);
	for (i = 0; i < 6; i++)
		s.procs[i] = (fake_proc){600 + i, 601 + i, 600 + i, NTFSMAC_CTL_PROC_OK};
	s.procs[5].ppid = 1;
	s.nprocs = 6;
	o.nspawned = 1;
	o.spawned[0] = 605;
	WALK(NTFSMAC_CTL_REFUSE_SPAWNED_PROCESS, 600);
	o.spawned[0] = 999; /* some other pid: the same chain passes */
	WALK(PASS, 600);

	/* a pgid of 0 or 1 is never looked up as a spawned process */
	o.spawned[0] = 1;
	s.procs[0].pgid = 1;
	s.procs[5].ppid = 1;
	WALK(PASS, 600);
	o.spawned[0] = 0;
	s.procs[0].pgid = 0;
	WALK(PASS, 600);
	o.nspawned = 0;

	/* the peer's own pid cannot be found: refuse (it holds the connection) */
	fake_sys_init(&s, &sys);
	WALK(NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE, 12345);
	/* an ancestor that has gone since: the walk ends, the peer passes */
	s.procs[1].result = NTFSMAC_CTL_PROC_GONE;
	WALK(PASS, 500);
	/* an error from the lookup, at the peer or higher up: refuse */
	s.procs[1].result = NTFSMAC_CTL_PROC_ERROR;
	WALK(NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE, 500);
	fake_sys_init(&s, &sys);
	s.procs[0].result = NTFSMAC_CTL_PROC_ERROR;
	WALK(NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE, 500);

	/* a loop in the answers (a process that is its own parent) */
	fake_sys_init(&s, &sys);
	s.procs[1].ppid = 400;
	WALK(NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE, 500);
	/* a cycle of two */
	s.procs[1].ppid = 500;
	WALK(NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE, 500);

	/* a chain of 16 parents passes (the limit itself is tested in test_chain_limit) */
	fake_sys_init(&s, &sys);
	for (i = 0; i < 16; i++)
		s.procs[i] = (fake_proc){100 + i, 101 + i, 100 + i, NTFSMAC_CTL_PROC_OK};
	s.procs[15].ppid = 1;
	s.nprocs = 16;
	WALK(PASS, 100);

	/* missing pieces fail closed */
	fake_sys_init(&s, &sys);
	CHECK_EQ_INT(ntfsmac_ctl_check_not_spawned(NULL, f_is_spawned, &o, 500), NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE);
	CHECK_EQ_INT(ntfsmac_ctl_check_not_spawned(&sys, NULL, &o, 500), NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE);
	CHECK_EQ_INT(ntfsmac_ctl_check_not_spawned(&sys, f_is_spawned, &o, 0), NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE);
	CHECK_EQ_INT(ntfsmac_ctl_check_not_spawned(&sys, f_is_spawned, &o, 1), NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE);
	CHECK_EQ_INT(ntfsmac_ctl_check_not_spawned(&sys, f_is_spawned, &o, -5), NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE);
}

/* A long chain: every pid answers with parent pid + 1, forever. */
static ntfsmac_ctl_sys_proc endless(void *ctx, pid_t pid, pid_t *ppid, pid_t *pgid)
{
	(*(int *)ctx)++;
	*ppid = pid + 1;
	*pgid = pid;
	return NTFSMAC_CTL_PROC_OK;
}

static void test_chain_limit(void)
{
	int calls = 0;
	ntfsmac_ctl_sys sys;
	fake_ops_state o;

	memset(&sys, 0, sizeof sys);
	memset(&o, 0, sizeof o);
	sys.ctx = &calls;
	sys.proc_parent = endless;
	CHECK_EQ_INT(ntfsmac_ctl_check_not_spawned(&sys, f_is_spawned, &o, 100),
		     NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE);
	CHECK_EQ_INT(calls, NTFSMAC_CTL_CHAIN_MAX); /* stopped at the limit, no endless loop */
	/* a spawned pid found on the last step of the limit is still found */
	o.spawned[0] = 100 + (pid_t)NTFSMAC_CTL_CHAIN_MAX - 1;
	o.nspawned = 1;
	CHECK_EQ_INT(ntfsmac_ctl_check_not_spawned(&sys, f_is_spawned, &o, 100),
		     NTFSMAC_CTL_REFUSE_SPAWNED_PROCESS);
}

static void test_signature(void)
{
	fake_sys_state s;
	ntfsmac_ctl_sys sys;
	ntfsmac_ctl_peer p = good_peer();

	fake_sys_init(&s, &sys);
	CHECK_EQ_INT(ntfsmac_ctl_check_client_signed(&sys, &p), PASS);
	CHECK_EQ_INT(s.signed_calls, 1);
	s.signed_answer = NTFSMAC_CTL_SYS_NO;
	CHECK_EQ_INT(ntfsmac_ctl_check_client_signed(&sys, &p), NTFSMAC_CTL_REFUSE_CLIENT_NOT_SIGNED);
	s.signed_answer = NTFSMAC_CTL_SYS_UNAVAILABLE;
	CHECK_EQ_INT(ntfsmac_ctl_check_client_signed(&sys, &p), NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE);
	s.signed_answer = (ntfsmac_ctl_sys_answer)77;
	CHECK_EQ_INT(ntfsmac_ctl_check_client_signed(&sys, &p), NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE);

	/* no audit token: the signature of the right process cannot be checked */
	s.signed_answer = NTFSMAC_CTL_SYS_YES;
	s.signed_calls = 0;
	p.has_audit_token = false;
	CHECK_EQ_INT(ntfsmac_ctl_check_client_signed(&sys, &p), NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE);
	CHECK_EQ_INT(s.signed_calls, 0);
	p.has_audit_token = true;
	CHECK_EQ_INT(ntfsmac_ctl_check_client_signed(NULL, &p), NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE);
	CHECK_EQ_INT(ntfsmac_ctl_check_client_signed(&sys, NULL), NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE);
	sys.client_signed = NULL;
	CHECK_EQ_INT(ntfsmac_ctl_check_client_signed(&sys, &p), NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE);
}

static void test_authorization(void)
{
	fake_sys_state s;
	ntfsmac_ctl_sys sys;
	ntfsmac_ctl_replay replay;
	uint8_t a[NTFSMAC_CTL_AUTH_FORM_BYTES], b[NTFSMAC_CTL_AUTH_FORM_BYTES];
	unsigned i;

	memset(a, 0xA1, sizeof a);
	memset(b, 0xB2, sizeof b);
	fake_sys_init(&s, &sys);
	ntfsmac_ctl_replay_init(&replay);

	/* none sent */
	CHECK_EQ_INT(ntfsmac_ctl_check_authorization(&sys, &replay, NULL), NTFSMAC_CTL_REFUSE_NO_AUTHORIZATION);
	CHECK_EQ_INT(s.authz_calls, 0);

	/* granted once; the same form again is a replay and is not even sent to the system */
	CHECK_EQ_INT(ntfsmac_ctl_check_authorization(&sys, &replay, a), PASS);
	CHECK_EQ_INT(s.authz_calls, 1);
	CHECK(memcmp(s.last_form, a, sizeof a) == 0);
	CHECK_EQ_INT(ntfsmac_ctl_check_authorization(&sys, &replay, a), NTFSMAC_CTL_REFUSE_AUTHORIZATION_REPLAYED);
	CHECK_EQ_INT(s.authz_calls, 1);
	/* a different form is a fresh one */
	CHECK_EQ_INT(ntfsmac_ctl_check_authorization(&sys, &replay, b), PASS);
	CHECK_EQ_INT(s.authz_calls, 2);

	/* denied: the form is used up all the same (a retry needs a new prompt) */
	memset(b, 0xB3, sizeof b);
	s.authz_answer = NTFSMAC_CTL_SYS_NO;
	CHECK_EQ_INT(ntfsmac_ctl_check_authorization(&sys, &replay, b), NTFSMAC_CTL_REFUSE_AUTHORIZATION_DENIED);
	s.authz_answer = NTFSMAC_CTL_SYS_YES;
	CHECK_EQ_INT(ntfsmac_ctl_check_authorization(&sys, &replay, b), NTFSMAC_CTL_REFUSE_AUTHORIZATION_REPLAYED);

	/* system cannot say */
	memset(b, 0xB4, sizeof b);
	s.authz_answer = NTFSMAC_CTL_SYS_UNAVAILABLE;
	CHECK_EQ_INT(ntfsmac_ctl_check_authorization(&sys, &replay, b), NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE);
	memset(b, 0xB5, sizeof b);
	s.authz_answer = (ntfsmac_ctl_sys_answer)9;
	CHECK_EQ_INT(ntfsmac_ctl_check_authorization(&sys, &replay, b), NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE);
	s.authz_answer = NTFSMAC_CTL_SYS_YES;

	/* the table keeps the last 64 and forgets older ones, oldest first */
	ntfsmac_ctl_replay_init(&replay);
	for (i = 0; i < NTFSMAC_CTL_REPLAY_SLOTS + 1; i++) {
		memset(a, 0, sizeof a);
		a[0] = (uint8_t)i;
		a[1] = 0x55;
		CHECK(!ntfsmac_ctl_replay_seen(&replay, a));
	}
	memset(a, 0, sizeof a);
	a[0] = 0;
	a[1] = 0x55;
	CHECK(!ntfsmac_ctl_replay_seen(&replay, a)); /* the first one was overwritten */
	a[0] = (uint8_t)NTFSMAC_CTL_REPLAY_SLOTS;
	CHECK(ntfsmac_ctl_replay_seen(&replay, a));  /* the newest is still there */

	/* missing pieces fail closed */
	CHECK_EQ_INT(ntfsmac_ctl_check_authorization(NULL, &replay, b), NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE);
	CHECK_EQ_INT(ntfsmac_ctl_check_authorization(&sys, NULL, b), NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE);
	sys.authz_verify = NULL;
	CHECK_EQ_INT(ntfsmac_ctl_check_authorization(&sys, &replay, b), NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE);
}

/* The stored definition of the right (M2): missing, wrong, unreadable, and the order with check 4. */
static void test_right(void)
{
	fake_sys_state s;
	ntfsmac_ctl_sys sys;
	ntfsmac_ctl_replay replay;
	uint8_t a[NTFSMAC_CTL_AUTH_FORM_BYTES];

	memset(a, 0xC3, sizeof a);
	fake_sys_init(&s, &sys);
	ntfsmac_ctl_replay_init(&replay);

	CHECK_EQ_INT(ntfsmac_ctl_check_right(&sys), PASS);
	CHECK_EQ_INT(s.right_calls, 1);
	/* missing right, or a definition that differs: its own reason */
	s.right_answer = NTFSMAC_CTL_SYS_NO;
	CHECK_EQ_INT(ntfsmac_ctl_check_right(&sys), NTFSMAC_CTL_REFUSE_AUTHORIZATION_RIGHT_INVALID);
	/* cannot be read, or an answer out of range: cannot tell */
	s.right_answer = NTFSMAC_CTL_SYS_UNAVAILABLE;
	CHECK_EQ_INT(ntfsmac_ctl_check_right(&sys), NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE);
	s.right_answer = (ntfsmac_ctl_sys_answer)9;
	CHECK_EQ_INT(ntfsmac_ctl_check_right(&sys), NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE);
	s.right_answer = NTFSMAC_CTL_SYS_YES;
	/* no way to look: refuse, never allow */
	CHECK_EQ_INT(ntfsmac_ctl_check_right(NULL), NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE);
	sys.right_definition = NULL;
	CHECK_EQ_INT(ntfsmac_ctl_check_right(&sys), NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE);
	CHECK_EQ_INT(ntfsmac_ctl_check_authorization(&sys, &replay, a), NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE);
	sys.right_definition = fs_right;

	/* check 4 reads the definition first: a wrong right refuses before the form is looked at */
	s.right_calls = 0;
	s.right_answer = NTFSMAC_CTL_SYS_NO;
	CHECK_EQ_INT(ntfsmac_ctl_check_authorization(&sys, &replay, a), NTFSMAC_CTL_REFUSE_AUTHORIZATION_RIGHT_INVALID);
	CHECK_EQ_INT(ntfsmac_ctl_check_authorization(&sys, &replay, NULL), NTFSMAC_CTL_REFUSE_AUTHORIZATION_RIGHT_INVALID);
	CHECK_EQ_INT(s.right_calls, 2);
	CHECK_EQ_INT(s.authz_calls, 0);
	/* the refused form was not recorded as used: once the right is fine it is accepted */
	s.right_answer = NTFSMAC_CTL_SYS_YES;
	CHECK_EQ_INT(ntfsmac_ctl_check_authorization(&sys, &replay, a), PASS);
	CHECK_EQ_INT(s.authz_calls, 1);
	/* each call reads it again */
	s.right_calls = 0;
	CHECK_EQ_INT(ntfsmac_ctl_check_authorization(&sys, &replay, a), NTFSMAC_CTL_REFUSE_AUTHORIZATION_REPLAYED);
	CHECK_EQ_INT(s.right_calls, 1);
	s.right_answer = NTFSMAC_CTL_SYS_NO;
	CHECK_EQ_INT(ntfsmac_ctl_check_authorization(&sys, &replay, a), NTFSMAC_CTL_REFUSE_AUTHORIZATION_RIGHT_INVALID);
}

/* Compose check 1 to 4 with one fact bad at a time. */
static void test_order(void)
{
	fake_sys_state s;
	ntfsmac_ctl_sys sys;
	ntfsmac_ctl_replay replay;
	ntfsmac_ctl_env env;
	fake_ops_state o;
	ntfsmac_ctl_ops ops = fake_ops();
	ntfsmac_ctl_peer p;
	ntfsmac_ctl_request req;

	memset(&o, 0, sizeof o);
	memset(&req, 0, sizeof req);
	req.cmd = NTFSMAC_CTL_CMD_JOURNAL_CLEAR;
	req.has_auth = true;
	memset(req.auth, 0x42, sizeof req.auth);

#define FRESH()                                                         \
	do {                                                            \
		fake_sys_init(&s, &sys);                                \
		ntfsmac_ctl_replay_init(&replay);                       \
		env.sys = &sys;                                         \
		env.replay = &replay;                                   \
		p = good_peer();                                        \
		memset(&o, 0, sizeof o);                                \
	} while (0)
#define AUTH() ntfsmac_ctl_authorise_privileged(&env, &ops, &o, &p, &req)

	FRESH();
	CHECK_EQ_INT(AUTH(), PASS);
	CHECK_EQ_INT(s.authz_calls, 1);

	/* each fact bad on its own gives its own refusal */
	FRESH();
	p.admin = false;
	CHECK_EQ_INT(AUTH(), NTFSMAC_CTL_REFUSE_NOT_ADMIN);
	FRESH();
	o.spawned[0] = 400;
	o.nspawned = 1;
	CHECK_EQ_INT(AUTH(), NTFSMAC_CTL_REFUSE_SPAWNED_PROCESS);
	FRESH();
	s.signed_answer = NTFSMAC_CTL_SYS_NO;
	CHECK_EQ_INT(AUTH(), NTFSMAC_CTL_REFUSE_CLIENT_NOT_SIGNED);
	FRESH();
	req.has_auth = false;
	CHECK_EQ_INT(AUTH(), NTFSMAC_CTL_REFUSE_NO_AUTHORIZATION);
	req.has_auth = true;
	FRESH();
	s.authz_answer = NTFSMAC_CTL_SYS_NO;
	CHECK_EQ_INT(AUTH(), NTFSMAC_CTL_REFUSE_AUTHORIZATION_DENIED);
	FRESH();
	s.right_answer = NTFSMAC_CTL_SYS_NO;
	CHECK_EQ_INT(AUTH(), NTFSMAC_CTL_REFUSE_AUTHORIZATION_RIGHT_INVALID);
	CHECK_EQ_INT(s.authz_calls, 0);
	FRESH();
	s.right_answer = NTFSMAC_CTL_SYS_UNAVAILABLE;
	CHECK_EQ_INT(AUTH(), NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE);
	/* checks 1 to 3 come before the right */
	FRESH();
	s.right_answer = NTFSMAC_CTL_SYS_NO;
	s.signed_answer = NTFSMAC_CTL_SYS_NO;
	CHECK_EQ_INT(AUTH(), NTFSMAC_CTL_REFUSE_CLIENT_NOT_SIGNED);
	CHECK_EQ_INT(s.right_calls, 0);

	/* a refusal stops the later checks: no signature lookup for a non-admin, and no
	 * prompt-side call for an unsigned client */
	FRESH();
	p.admin = false;
	s.authz_answer = NTFSMAC_CTL_SYS_NO;
	(void)AUTH();
	CHECK_EQ_INT(s.proc_calls, 0);
	CHECK_EQ_INT(s.signed_calls, 0);
	CHECK_EQ_INT(s.authz_calls, 0);
	FRESH();
	o.spawned[0] = 500;
	o.nspawned = 1;
	(void)AUTH();
	CHECK_EQ_INT(s.signed_calls, 0);
	CHECK_EQ_INT(s.authz_calls, 0);
	FRESH();
	s.signed_answer = NTFSMAC_CTL_SYS_NO;
	(void)AUTH();
	CHECK_EQ_INT(s.authz_calls, 0);

	/* two bad at once: the earlier check of the ADR is reported */
	FRESH();
	p.admin = false;
	o.spawned[0] = 500;
	o.nspawned = 1;
	s.signed_answer = NTFSMAC_CTL_SYS_NO;
	s.authz_answer = NTFSMAC_CTL_SYS_NO;
	CHECK_EQ_INT(AUTH(), NTFSMAC_CTL_REFUSE_NOT_ADMIN);
	FRESH();
	o.spawned[0] = 500;
	o.nspawned = 1;
	s.signed_answer = NTFSMAC_CTL_SYS_NO;
	CHECK_EQ_INT(AUTH(), NTFSMAC_CTL_REFUSE_SPAWNED_PROCESS);
	FRESH();
	s.signed_answer = NTFSMAC_CTL_SYS_NO;
	s.authz_answer = NTFSMAC_CTL_SYS_NO;
	CHECK_EQ_INT(AUTH(), NTFSMAC_CTL_REFUSE_CLIENT_NOT_SIGNED);

	/* is_spawned not provided by the daemon core: refuse, never allow */
	FRESH();
	ops.is_spawned = NULL;
	CHECK_EQ_INT(AUTH(), NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE);
	ops = fake_ops();

	/* NULL arguments */
	FRESH();
	CHECK_EQ_INT(ntfsmac_ctl_authorise_privileged(NULL, &ops, &o, &p, &req), NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE);
	CHECK_EQ_INT(ntfsmac_ctl_authorise_privileged(&env, NULL, &o, &p, &req), NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE);
	CHECK_EQ_INT(ntfsmac_ctl_authorise_privileged(&env, &ops, &o, NULL, &req), NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE);
	CHECK_EQ_INT(ntfsmac_ctl_authorise_privileged(&env, &ops, &o, &p, NULL), NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE);
}

int main(void)
{
	test_gate_and_admin();
	test_not_spawned();
	test_chain_limit();
	test_signature();
	test_authorization();
	test_right();
	test_order();
	if (g_failures != 0) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	return 0;
}
