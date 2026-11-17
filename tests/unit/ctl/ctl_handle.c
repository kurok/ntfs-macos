/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-CTL-HANDLE-01..06: ntfsmac_ctl_handle_line_env() end to end with fake
 * operations and fake system facts.
 *   01 every command reaches its operation with the right arguments
 *   02 gate: a peer that is neither console user nor admin gets nothing
 *   03 journal_clear and repair: each of checks 1 to 4 failing alone refuses,
 *      and the operation is not called
 *   04 all four pass: the operation is called once; a repeat is refused
 *   05 replies: always one NUL-terminated line of JSON, no newline
 *   06 operations that fail, are missing, or write a newline
 */
#include <stdlib.h>

#include "check.h"
#include "fakes.h"

typedef struct rig {
	fake_sys_state s;
	ntfsmac_ctl_sys sys;
	ntfsmac_ctl_replay replay;
	ntfsmac_ctl_env env;     /* the socket door */
	ntfsmac_ctl_env env_xpc; /* the XPC door: journal_clear and repair */
	fake_ops_state o;
	ntfsmac_ctl_ops ops;
	ntfsmac_ctl_peer peer;
	char reply[NTFSMAC_CTL_REPLY_MIN];
} rig;

static void rig_init(rig *g)
{
	memset(g, 0, sizeof *g);
	fake_sys_init(&g->s, &g->sys);
	ntfsmac_ctl_replay_init(&g->replay);
	g->env.sys = &g->sys;
	g->env.replay = &g->replay;
	g->env_xpc = g->env;
	g->env_xpc.channel = NTFSMAC_CTL_CHAN_XPC;
	g->ops = fake_ops();
	g->peer = good_peer();
	g->o.status = NTFSMAC_DMN_OK;
}

static ntfsmac_dmn_status run(rig *g, const char *line)
{
	memset(g->reply, 'Z', sizeof g->reply); /* poison: the function must write the NUL itself */
	/* A client picks the door by the command: the privileged ones go to XPC. */
	const ntfsmac_ctl_env *env = (strstr(line, "\"journal_clear\"") != NULL ||
				      strstr(line, "\"repair\"") != NULL)
					     ? &g->env_xpc
					     : &g->env;

	return ntfsmac_ctl_handle_line_env(env, &g->ops, &g->o, &g->peer, line, strlen(line),
					   g->reply, sizeof g->reply);
}

static bool reply_has(const rig *g, const char *s)
{
	return strstr(g->reply, s) != NULL;
}

/* A reply is one line of JSON: starts with `{"ok":`, ends with `}`, no newline, terminated. */
static void check_reply_shape(const rig *g)
{
	size_t n = memchr(g->reply, '\0', sizeof g->reply) ? strlen(g->reply) : sizeof g->reply;

	CHECK(n < sizeof g->reply);
	CHECK(n > 2);
	CHECK(strncmp(g->reply, "{\"ok\":", 6) == 0);
	CHECK(g->reply[n - 1] == '}');
	CHECK(strchr(g->reply, '\n') == NULL);
	CHECK(strchr(g->reply, '\r') == NULL);
}

#define LINE_JC(auth)                                                                     \
	"{\"cmd\":\"journal_clear\",\"bsd\":\"disk3s2\",\"confirm_label\":\"My Disk\","   \
	"\"confirm_serial\":\"0a1b2c3d\"" auth "}"
#define LINE_REPAIR(auth)                                                                 \
	"{\"cmd\":\"repair\",\"bsd\":\"disk3s2\",\"action\":\"clear_dirty\","              \
	"\"confirm_label\":\"My Disk\",\"confirm_serial\":\"0a1b2c3d\"" auth "}"
#define AUTH_FIELD ",\"auth\":\"" GOOD_AUTH "\""

static void test_commands(void)
{
	rig g;

	rig_init(&g);
	g.o.reply = "{\"ok\":true,\"volumes\":[]}";
	CHECK_EQ_INT(run(&g, "{\"cmd\":\"list\"}"), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(g.o.calls[NTFSMAC_CTL_CMD_LIST], 1);
	CHECK(strcmp(g.reply, "{\"ok\":true,\"volumes\":[]}") == 0);

	CHECK_EQ_INT(run(&g, "{\"cmd\":\"status\",\"bsd\":\"disk4s1\"}"), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(g.o.calls[NTFSMAC_CTL_CMD_STATUS], 1);
	CHECK(strcmp(g.o.bsd, "disk4s1") == 0);

	CHECK_EQ_INT(run(&g, "{\"cmd\":\"mount\",\"bsd\":\"disk4s1\",\"mode\":\"ro\"}"), NTFSMAC_DMN_OK);
	CHECK(g.o.read_only);
	CHECK_EQ_INT(run(&g, "{\"cmd\":\"mount\",\"bsd\":\"disk5\"}"), NTFSMAC_DMN_OK);
	CHECK(!g.o.read_only);
	CHECK(strcmp(g.o.bsd, "disk5") == 0);
	CHECK_EQ_INT(g.o.calls[NTFSMAC_CTL_CMD_MOUNT], 2);

	CHECK_EQ_INT(run(&g, "{\"cmd\":\"unmount\",\"bsd\":\"disk4s1\",\"force\":true}"), NTFSMAC_DMN_OK);
	CHECK(g.o.force);
	CHECK_EQ_INT(g.o.calls[NTFSMAC_CTL_CMD_UNMOUNT], 1);

	CHECK_EQ_INT(run(&g, "{\"cmd\":\"doctor\"}"), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(g.o.calls[NTFSMAC_CTL_CMD_DOCTOR], 1);

	/* none of these calls ran an authorization check */
	CHECK_EQ_INT(g.s.proc_calls, 0);
	CHECK_EQ_INT(g.s.signed_calls, 0);
	CHECK_EQ_INT(g.s.authz_calls, 0);

	/* a plain admin who is not the console user, and a console user who is not admin */
	g.peer.console_user = false;
	CHECK_EQ_INT(run(&g, "{\"cmd\":\"doctor\"}"), NTFSMAC_DMN_OK);
	g.peer.console_user = true;
	g.peer.admin = false;
	CHECK_EQ_INT(run(&g, "{\"cmd\":\"doctor\"}"), NTFSMAC_DMN_OK);
}

static void test_gate(void)
{
	static const char *const lines[] = {
		"{\"cmd\":\"list\"}", "{\"cmd\":\"status\",\"bsd\":\"disk4\"}",
		"{\"cmd\":\"mount\",\"bsd\":\"disk4\"}", "{\"cmd\":\"unmount\",\"bsd\":\"disk4\"}",
		"{\"cmd\":\"doctor\"}", LINE_JC(AUTH_FIELD), LINE_REPAIR(AUTH_FIELD),
		"garbage", "", "{\"cmd\":\"nope\"}"};
	size_t i;

	for (i = 0; i < sizeof lines / sizeof lines[0]; i++) {
		rig g;

		rig_init(&g);
		g.peer.console_user = false;
		g.peer.admin = false;
		CHECK_EQ_INT(run(&g, lines[i]), NTFSMAC_DMN_E_DENIED);
		CHECK_EQ_INT(fake_total_calls(&g.o), 0);
		CHECK(reply_has(&g, "\"error\":\"denied\""));
		CHECK(reply_has(&g, "\"reason\":\"peer_not_allowed\""));
		check_reply_shape(&g);
		CHECK_EQ_INT(g.s.authz_calls + g.s.signed_calls + g.s.proc_calls, 0);
	}
}

static void test_privileged_each_check(void)
{
	static const char *const lines[] = {LINE_JC(AUTH_FIELD), LINE_REPAIR(AUTH_FIELD)};
	static const ntfsmac_ctl_cmd cmds[] = {NTFSMAC_CTL_CMD_JOURNAL_CLEAR, NTFSMAC_CTL_CMD_REPAIR};
	size_t i;

	for (i = 0; i < 2; i++) {
		rig g;

		/* check 1: console user but not admin */
		rig_init(&g);
		g.peer.admin = false;
		CHECK_EQ_INT(run(&g, lines[i]), NTFSMAC_DMN_E_DENIED);
		CHECK(reply_has(&g, "\"reason\":\"not_admin\""));
		CHECK_EQ_INT(fake_total_calls(&g.o), 0);
		check_reply_shape(&g);

		/* check 2: the peer is a spawned process */
		rig_init(&g);
		g.o.spawned[0] = g.peer.pid;
		g.o.nspawned = 1;
		CHECK_EQ_INT(run(&g, lines[i]), NTFSMAC_DMN_E_DENIED);
		CHECK(reply_has(&g, "\"reason\":\"spawned_process\""));
		CHECK_EQ_INT(fake_total_calls(&g.o), 0);
		/* ... or a descendant of one (its parent is spawned) */
		rig_init(&g);
		g.o.spawned[0] = 400;
		g.o.nspawned = 1;
		CHECK_EQ_INT(run(&g, lines[i]), NTFSMAC_DMN_E_DENIED);
		CHECK(reply_has(&g, "\"reason\":\"spawned_process\""));
		CHECK_EQ_INT(fake_total_calls(&g.o), 0);
		/* ... or in the process group of one */
		rig_init(&g);
		g.s.procs[0].pgid = 4242;
		g.o.spawned[0] = 4242;
		g.o.nspawned = 1;
		CHECK_EQ_INT(run(&g, lines[i]), NTFSMAC_DMN_E_DENIED);
		CHECK(reply_has(&g, "\"reason\":\"spawned_process\""));

		/* check 3: unsigned or differently signed client */
		rig_init(&g);
		g.s.signed_answer = NTFSMAC_CTL_SYS_NO;
		CHECK_EQ_INT(run(&g, lines[i]), NTFSMAC_DMN_E_DENIED);
		CHECK(reply_has(&g, "\"reason\":\"client_not_signed\""));
		CHECK_EQ_INT(fake_total_calls(&g.o), 0);
		/* ... and a peer with no audit token */
		rig_init(&g);
		g.peer.has_audit_token = false;
		CHECK_EQ_INT(run(&g, lines[i]), NTFSMAC_DMN_E_DENIED);
		CHECK(reply_has(&g, "\"reason\":\"check_unavailable\""));

		/* check 4: no authorization, one the system does not accept, one the system cannot judge */
		rig_init(&g);
		CHECK_EQ_INT(run(&g, i == 0 ? LINE_JC("") : LINE_REPAIR("")), NTFSMAC_DMN_E_DENIED);
		CHECK(reply_has(&g, "\"reason\":\"no_authorization\""));
		CHECK_EQ_INT(fake_total_calls(&g.o), 0);
		rig_init(&g);
		g.s.authz_answer = NTFSMAC_CTL_SYS_NO;
		CHECK_EQ_INT(run(&g, lines[i]), NTFSMAC_DMN_E_DENIED);
		CHECK(reply_has(&g, "\"reason\":\"authorization_denied\""));
		CHECK_EQ_INT(fake_total_calls(&g.o), 0);
		rig_init(&g);
		g.s.authz_answer = NTFSMAC_CTL_SYS_UNAVAILABLE;
		CHECK_EQ_INT(run(&g, lines[i]), NTFSMAC_DMN_E_DENIED);
		CHECK(reply_has(&g, "\"reason\":\"check_unavailable\""));

		/* the right is missing or defined differently: refused, form not verified, nothing runs */
		rig_init(&g);
		g.s.right_answer = NTFSMAC_CTL_SYS_NO;
		CHECK_EQ_INT(run(&g, lines[i]), NTFSMAC_DMN_E_DENIED);
		CHECK(reply_has(&g, "\"reason\":\"authorization_right_invalid\""));
		CHECK_EQ_INT(g.s.authz_calls, 0);
		CHECK_EQ_INT(fake_total_calls(&g.o), 0);
		rig_init(&g);
		g.s.right_answer = NTFSMAC_CTL_SYS_UNAVAILABLE;
		CHECK_EQ_INT(run(&g, lines[i]), NTFSMAC_DMN_E_DENIED);
		CHECK(reply_has(&g, "\"reason\":\"check_unavailable\""));
		CHECK_EQ_INT(fake_total_calls(&g.o), 0);

		/* the daemon core gave no is_spawned: refuse */
		rig_init(&g);
		g.ops.is_spawned = NULL;
		CHECK_EQ_INT(run(&g, lines[i]), NTFSMAC_DMN_E_DENIED);
		CHECK(reply_has(&g, "\"reason\":\"check_unavailable\""));

		/* a malformed request never reaches a check or the operation */
		rig_init(&g);
		CHECK_EQ_INT(run(&g, "{\"cmd\":\"journal_clear\",\"bsd\":\"disk3s2\"}"), NTFSMAC_DMN_E_INVAL);
		CHECK_EQ_INT(g.s.authz_calls, 0);

		/* all pass */
		rig_init(&g);
		g.o.reply = "{\"ok\":true,\"result\":\"cleared\"}";
		CHECK_EQ_INT(run(&g, lines[i]), NTFSMAC_DMN_OK);
		CHECK_EQ_INT(g.o.calls[cmds[i]], 1);
		CHECK_EQ_INT(fake_total_calls(&g.o), 1);
		CHECK(strcmp(g.o.bsd, "disk3s2") == 0);
		CHECK(strcmp(g.o.label, "My Disk") == 0);
		CHECK(strcmp(g.o.serial, "0A1B2C3D") == 0);
		CHECK_EQ_INT(g.o.peer.uid, 501);
		CHECK_EQ_INT(g.o.peer.pid, 500);
		if (i == 1)
			CHECK(strcmp(g.o.action, "clear_dirty") == 0);
		CHECK_EQ_INT(g.s.authz_calls, 1);
		CHECK(strcmp(g.reply, "{\"ok\":true,\"result\":\"cleared\"}") == 0);

		/* the same form again: replayed, and the operation is not called again */
		CHECK_EQ_INT(run(&g, lines[i]), NTFSMAC_DMN_E_DENIED);
		CHECK(reply_has(&g, "\"reason\":\"authorization_replayed\""));
		CHECK_EQ_INT(g.o.calls[cmds[i]], 1);
		CHECK_EQ_INT(g.s.authz_calls, 1);
	}

	/* repair with the other action */
	{
		rig g;

		rig_init(&g);
		CHECK_EQ_INT(run(&g, "{\"cmd\":\"repair\",\"bsd\":\"disk3s2\",\"action\":\"sync_mirror\","
				     "\"confirm_label\":\"L\",\"confirm_serial\":\"00000001\"" AUTH_FIELD "}"),
			     NTFSMAC_DMN_OK);
		CHECK(strcmp(g.o.action, "sync_mirror") == 0);
	}
}

static void test_reply_and_errors(void)
{
	rig g;
	char small[NTFSMAC_CTL_REPLY_MIN - 1];

	/* parse errors */
	rig_init(&g);
	CHECK_EQ_INT(run(&g, "not json"), NTFSMAC_DMN_E_INVAL);
	CHECK(strcmp(g.reply, "{\"ok\":false,\"error\":\"bad_json\"}") == 0);
	CHECK_EQ_INT(run(&g, "{\"cmd\":\"x\"}"), NTFSMAC_DMN_E_INVAL);
	CHECK(strcmp(g.reply, "{\"ok\":false,\"error\":\"unknown_command\"}") == 0);
	CHECK_EQ_INT(run(&g, "{\"cmd\":\"list\",\"zzz\":\"1\"}"), NTFSMAC_DMN_E_INVAL);
	CHECK(strcmp(g.reply, "{\"ok\":false,\"error\":\"bad_field\"}") == 0);
	CHECK_EQ_INT(fake_total_calls(&g.o), 0);
	{
		char big[NTFSMAC_CTL_LINE_MAX + 10];

		memset(big, ' ', sizeof big);
		CHECK_EQ_INT(ntfsmac_ctl_handle_line_env(&g.env, &g.ops, &g.o, &g.peer, big, sizeof big,
							 g.reply, sizeof g.reply), NTFSMAC_DMN_E_INVAL);
		CHECK(strcmp(g.reply, "{\"ok\":false,\"error\":\"line_too_long\"}") == 0);
	}
	/* a reply never echoes request bytes */
	CHECK_EQ_INT(run(&g, "{\"cmd\":\"ZZ-secret-ZZ\"}"), NTFSMAC_DMN_E_INVAL);
	CHECK(!reply_has(&g, "secret"));
	CHECK_EQ_INT(run(&g, "{\"cmd\":\"list\",\"ZZ-secret-ZZ\":\"1\"}"), NTFSMAC_DMN_E_INVAL);
	CHECK(!reply_has(&g, "secret"));
	check_reply_shape(&g);

	/* NULL arguments */
	CHECK_EQ_INT(ntfsmac_ctl_handle_line_env(&g.env, NULL, &g.o, &g.peer, "{}", 2, g.reply, sizeof g.reply),
		     NTFSMAC_DMN_E_INVAL);
	CHECK(strcmp(g.reply, "{\"ok\":false,\"error\":\"internal\"}") == 0);
	CHECK_EQ_INT(ntfsmac_ctl_handle_line_env(&g.env, &g.ops, &g.o, NULL, "{}", 2, g.reply, sizeof g.reply),
		     NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_ctl_handle_line_env(NULL, &g.ops, &g.o, &g.peer, "{}", 2, g.reply, sizeof g.reply),
		     NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_ctl_handle_line_env(&g.env, &g.ops, &g.o, &g.peer, NULL, 0, g.reply, sizeof g.reply),
		     NTFSMAC_DMN_E_INVAL);
	/* a reply buffer that is too small is refused, and not written */
	memset(small, 'Z', sizeof small);
	CHECK_EQ_INT(ntfsmac_ctl_handle_line_env(&g.env, &g.ops, &g.o, &g.peer, "{}", 2, small, sizeof small),
		     NTFSMAC_DMN_E_INVAL);
	CHECK(small[0] == 'Z');
	CHECK_EQ_INT(ntfsmac_ctl_handle_line_env(&g.env, &g.ops, &g.o, &g.peer, "{}", 2, NULL, 0), NTFSMAC_DMN_E_INVAL);

	/* an operation that fails without a reply gets a generic one; its status name is ours */
	rig_init(&g);
	g.o.status = NTFSMAC_DMN_E_STATE;
	CHECK_EQ_INT(run(&g, "{\"cmd\":\"unmount\",\"bsd\":\"disk4\"}"), NTFSMAC_DMN_E_STATE);
	CHECK(strcmp(g.reply, "{\"ok\":false,\"error\":\"failed\",\"status\":\"E_STATE\"}") == 0 ||
	      reply_has(&g, "\"error\":\"failed\""));
	check_reply_shape(&g);
	/* with a reply of its own, that reply is kept */
	g.o.reply = "{\"ok\":false,\"error\":\"failed\",\"reason\":\"FLAG_DIRTY\"}";
	CHECK_EQ_INT(run(&g, "{\"cmd\":\"unmount\",\"bsd\":\"disk4\"}"), NTFSMAC_DMN_E_STATE);
	CHECK(reply_has(&g, "FLAG_DIRTY"));
	/* success without a reply */
	g.o.status = NTFSMAC_DMN_OK;
	g.o.reply = NULL;
	CHECK_EQ_INT(run(&g, "{\"cmd\":\"unmount\",\"bsd\":\"disk4\"}"), NTFSMAC_DMN_OK);
	CHECK(strcmp(g.reply, "{\"ok\":true}") == 0);

	/* a missing operation: not_implemented, for every command */
	rig_init(&g);
	memset(&g.ops, 0, sizeof g.ops);
	g.ops.is_spawned = f_is_spawned;
	CHECK_EQ_INT(run(&g, "{\"cmd\":\"list\"}"), NTFSMAC_DMN_E_NOTIMPL);
	CHECK(reply_has(&g, "\"error\":\"not_implemented\""));
	CHECK_EQ_INT(run(&g, LINE_JC(AUTH_FIELD)), NTFSMAC_DMN_E_NOTIMPL); /* after the four checks */
	CHECK(reply_has(&g, "\"error\":\"not_implemented\""));
	CHECK_EQ_INT(run(&g, LINE_JC(AUTH_FIELD)), NTFSMAC_DMN_E_DENIED); /* the form was used */
	/* an operation that returns NOTIMPL itself is reported the same way */
	rig_init(&g);
	g.o.status = NTFSMAC_DMN_E_NOTIMPL;
	g.o.reply = "{\"ok\":true}";
	CHECK_EQ_INT(run(&g, "{\"cmd\":\"doctor\"}"), NTFSMAC_DMN_E_NOTIMPL);
	CHECK(reply_has(&g, "not_implemented"));

	/* a reply with newlines stays one line; an unterminated one is cut */
	rig_init(&g);
	g.o.reply = "{\"ok\":true,\n\"a\":\r\"b\"}";
	CHECK_EQ_INT(run(&g, "{\"cmd\":\"list\"}"), NTFSMAC_DMN_OK);
	check_reply_shape(&g);
	{
		char hostile[2 * NTFSMAC_CTL_REPLY_MIN];

		memset(hostile, 'x', sizeof hostile - 1);
		hostile[sizeof hostile - 1] = '\0';
		g.o.reply = hostile;
		CHECK_EQ_INT(run(&g, "{\"cmd\":\"list\"}"), NTFSMAC_DMN_OK);
		CHECK(memchr(g.reply, '\0', sizeof g.reply) != NULL);
	}
}

/*
 * U-CTL-HANDLE-05 (T-M4-14): the two doors and the fallback of check 4 through the dispatcher.
 * The socket never serves journal_clear and repair; the XPC door serves nothing else; with the
 * root fallback a root peer needs no form.
 */
static void test_channels_and_fallback(void)
{
	rig g;
	static const char *const plain[] = {"{\"cmd\":\"list\"}", "{\"cmd\":\"status\",\"bsd\":\"disk4\"}",
					    "{\"cmd\":\"mount\",\"bsd\":\"disk4\",\"mode\":\"ro\"}",
					    "{\"cmd\":\"unmount\",\"bsd\":\"disk4\"}", "{\"cmd\":\"doctor\"}"};
	size_t i;

	/* the socket refuses both privileged commands before any check runs and before any operation */
	rig_init(&g);
	CHECK_EQ_INT(ntfsmac_ctl_handle_line_env(&g.env, &g.ops, &g.o, &g.peer, LINE_JC(AUTH_FIELD),
						 strlen(LINE_JC(AUTH_FIELD)), g.reply, sizeof g.reply),
		     NTFSMAC_DMN_E_DENIED);
	CHECK(reply_has(&g, "\"reason\":\"wrong_channel\""));
	check_reply_shape(&g);
	CHECK_EQ_INT(ntfsmac_ctl_handle_line_env(&g.env, &g.ops, &g.o, &g.peer, LINE_REPAIR(AUTH_FIELD),
						 strlen(LINE_REPAIR(AUTH_FIELD)), g.reply, sizeof g.reply),
		     NTFSMAC_DMN_E_DENIED);
	CHECK(reply_has(&g, "\"reason\":\"wrong_channel\""));
	CHECK_EQ_INT(g.s.proc_calls + g.s.signed_calls + g.s.authz_calls + g.s.right_calls, 0);
	CHECK_EQ_INT(fake_total_calls(&g.o), 0);
	CHECK_EQ_INT(g.replay.count, 0);

	/* the XPC door refuses every other command */
	for (i = 0; i < sizeof plain / sizeof plain[0]; i++) {
		CHECK_EQ_INT(ntfsmac_ctl_handle_line_env(&g.env_xpc, &g.ops, &g.o, &g.peer, plain[i],
							 strlen(plain[i]), g.reply, sizeof g.reply),
			     NTFSMAC_DMN_E_DENIED);
		CHECK(reply_has(&g, "\"reason\":\"wrong_channel\""));
	}
	CHECK_EQ_INT(fake_total_calls(&g.o), 0);
	/* a request that does not parse still gets the parser's error on the XPC door */
	CHECK_EQ_INT(ntfsmac_ctl_handle_line_env(&g.env_xpc, &g.ops, &g.o, &g.peer, "{", 1, g.reply,
						 sizeof g.reply),
		     NTFSMAC_DMN_E_INVAL);
	CHECK(reply_has(&g, "bad_json"));
	/* the gate still comes first on the XPC door */
	g.peer.console_user = g.peer.admin = false;
	CHECK_EQ_INT(ntfsmac_ctl_handle_line_env(&g.env_xpc, &g.ops, &g.o, &g.peer, LINE_JC(AUTH_FIELD),
						 strlen(LINE_JC(AUTH_FIELD)), g.reply, sizeof g.reply),
		     NTFSMAC_DMN_E_DENIED);
	CHECK(reply_has(&g, "peer_not_allowed"));

	/* the root fallback: a root peer, no form, the operation is reached */
	rig_init(&g);
	g.env_xpc.authz = NTFSMAC_CTL_AUTHZ_ROOT_PEER;
	g.peer.uid = 0;
	CHECK_EQ_INT(run(&g, LINE_JC("")), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(g.o.calls[NTFSMAC_CTL_CMD_JOURNAL_CLEAR], 1);
	CHECK_EQ_INT(run(&g, LINE_JC("")), NTFSMAC_DMN_OK); /* nothing is used up */
	CHECK_EQ_INT(run(&g, LINE_REPAIR("")), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(g.o.calls[NTFSMAC_CTL_CMD_REPAIR], 1);
	CHECK_EQ_INT(g.s.authz_calls + g.s.right_calls, 0);
	/* not root: refused, whatever form comes with it */
	g.peer.uid = 501;
	CHECK_EQ_INT(run(&g, LINE_JC(AUTH_FIELD)), NTFSMAC_DMN_E_DENIED);
	CHECK(reply_has(&g, "authorization_denied"));
	CHECK_EQ_INT(g.o.calls[NTFSMAC_CTL_CMD_JOURNAL_CLEAR], 2);
	/* the other checks still apply with root: a spawned process that is root is refused */
	g.peer.uid = 0;
	g.o.spawned[0] = 400;
	g.o.nspawned = 1;
	CHECK_EQ_INT(run(&g, LINE_JC("")), NTFSMAC_DMN_E_DENIED);
	CHECK(reply_has(&g, "spawned_process"));
}

int main(void)
{
	test_commands();
	test_gate();
	test_privileged_each_check();
	test_reply_and_errors();
	test_channels_and_fallback();
	if (g_failures != 0) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	return 0;
}
