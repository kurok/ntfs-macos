/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-CTL-XPC-01..04 (T-M4-14): the pure parts of the XPC door (src/daemon/ctl_xpc.c). A Mach
 * service cannot be started by a test, so the listener itself is covered in a guest
 * (tests/integration/daemon/journal_vm.sh); here:
 *   01 request validation: one line, one request, only journal_clear and repair
 *   02 the request goes through the same parser as the socket: strict grammar
 *   03 the listener refuses to start without what it needs
 *   04 the peer is built from the audit token of the message (macOS only)
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "check.h"
#include "fakes.h"

#define JC "{\"cmd\":\"journal_clear\",\"bsd\":\"disk3s2\",\"confirm_label\":\"My Disk\"," \
	"\"confirm_serial\":\"0A1B2C3D\""
#define AUTHF ",\"auth\":\"" GOOD_AUTH "\""

static ntfsmac_ctl_parse_result chk(const char *line, ntfsmac_ctl_request *req, bool *wrong)
{
	return ntfsmac_xpc_check_request(line, line != NULL ? strlen(line) : 0, req, wrong);
}

static void test_validation(void)
{
	ntfsmac_ctl_request req;
	bool wrong = true;
	char big[NTFSMAC_CTL_LINE_MAX + 100];
	char line[256];

	/* the two privileged commands pass, with and without the form */
	CHECK_EQ_INT(chk(JC "}", &req, &wrong), NTFSMAC_CTL_PARSE_OK);
	CHECK(!wrong);
	CHECK_EQ_INT(req.cmd, NTFSMAC_CTL_CMD_JOURNAL_CLEAR);
	CHECK(!req.has_auth);
	CHECK_EQ_INT(chk(JC AUTHF "}", &req, &wrong), NTFSMAC_CTL_PARSE_OK);
	CHECK(req.has_auth && memcmp(req.auth, "\x00\x11\x22\x33", 4) == 0);
	CHECK_EQ_INT(chk("{\"cmd\":\"repair\",\"bsd\":\"disk3s2\",\"action\":\"clear_dirty\","
			 "\"confirm_label\":\"L\",\"confirm_serial\":\"0A1B2C3D\"}",
			 &req, &wrong),
		     NTFSMAC_CTL_PARSE_OK);
	CHECK_EQ_INT(req.action, NTFSMAC_CTL_REPAIR_CLEAR_DIRTY);
	CHECK_EQ_INT(chk("{\"cmd\":\"repair\",\"bsd\":\"disk3s2\",\"action\":\"sync_mirror\","
			 "\"confirm_label\":\"L\",\"confirm_serial\":\"0A1B2C3D\"}",
			 &req, &wrong),
		     NTFSMAC_CTL_PARSE_OK);
	CHECK_EQ_INT(req.action, NTFSMAC_CTL_REPAIR_SYNC_MIRROR);

	/* every other command is for the socket: parsed, then reported as the wrong door */
	CHECK_EQ_INT(chk("{\"cmd\":\"list\"}", &req, &wrong), NTFSMAC_CTL_PARSE_UNKNOWN_COMMAND);
	CHECK(wrong);
	CHECK_EQ_INT(chk("{\"cmd\":\"status\",\"bsd\":\"disk4\"}", &req, &wrong), NTFSMAC_CTL_PARSE_UNKNOWN_COMMAND);
	CHECK(wrong);
	CHECK_EQ_INT(chk("{\"cmd\":\"mount\",\"bsd\":\"disk4\"}", &req, &wrong), NTFSMAC_CTL_PARSE_UNKNOWN_COMMAND);
	CHECK(wrong);
	CHECK_EQ_INT(chk("{\"cmd\":\"unmount\",\"bsd\":\"disk4\"}", &req, &wrong), NTFSMAC_CTL_PARSE_UNKNOWN_COMMAND);
	CHECK(wrong);
	CHECK_EQ_INT(chk("{\"cmd\":\"doctor\"}", &req, &wrong), NTFSMAC_CTL_PARSE_UNKNOWN_COMMAND);
	CHECK(wrong);
	/* a command that does not exist is not "the wrong door" */
	CHECK_EQ_INT(chk("{\"cmd\":\"nothing\"}", &req, &wrong), NTFSMAC_CTL_PARSE_UNKNOWN_COMMAND);
	CHECK(!wrong);

	/* one message, one request: no line ends, no NUL, nothing after the object */
	snprintf(line, sizeof line, "%s}\n", JC);
	CHECK_EQ_INT(chk(line, &req, &wrong), NTFSMAC_CTL_PARSE_BAD_JSON);
	snprintf(line, sizeof line, "%s}\r", JC);
	CHECK_EQ_INT(chk(line, &req, &wrong), NTFSMAC_CTL_PARSE_BAD_JSON);
	snprintf(line, sizeof line, "%s}\n{\"cmd\":\"list\"}", JC);
	CHECK_EQ_INT(chk(line, &req, &wrong), NTFSMAC_CTL_PARSE_BAD_JSON);
	snprintf(line, sizeof line, "%s}", JC);
	CHECK_EQ_INT(ntfsmac_xpc_check_request(line, strlen(line) + 1, &req, &wrong), NTFSMAC_CTL_PARSE_BAD_JSON); /* NUL counted */
	snprintf(line, sizeof line, "%s} x", JC);
	CHECK_EQ_INT(chk(line, &req, &wrong), NTFSMAC_CTL_PARSE_BAD_JSON);

	/* the size limit is the socket's */
	memset(big, ' ', sizeof big);
	memcpy(big, JC "}", strlen(JC "}"));
	CHECK_EQ_INT(ntfsmac_xpc_check_request(big, NTFSMAC_CTL_LINE_MAX + 1, &req, &wrong), NTFSMAC_CTL_PARSE_TOO_LONG);
	CHECK_EQ_INT(ntfsmac_xpc_check_request(big, NTFSMAC_CTL_LINE_MAX, &req, &wrong), NTFSMAC_CTL_PARSE_OK);

	/* a missing string, NULL pointers */
	CHECK_EQ_INT(ntfsmac_xpc_check_request(NULL, 0, &req, &wrong), NTFSMAC_CTL_PARSE_BAD_JSON);
	CHECK_EQ_INT(ntfsmac_xpc_check_request("{}", 2, NULL, &wrong), NTFSMAC_CTL_PARSE_BAD_JSON);
	CHECK_EQ_INT(ntfsmac_xpc_check_request("{}", 2, &req, NULL), NTFSMAC_CTL_PARSE_UNKNOWN_COMMAND);
	CHECK_EQ_INT(chk("", &req, &wrong), NTFSMAC_CTL_PARSE_BAD_JSON);
}

static void test_strict_grammar(void)
{
	ntfsmac_ctl_request req;
	bool wrong;
	char line[512];

	/* the same field rules as on the socket: unknown, duplicate, misplaced, malformed fields */
	snprintf(line, sizeof line, "%s,\"x\":\"y\"}", JC);
	CHECK_EQ_INT(chk(line, &req, &wrong), NTFSMAC_CTL_PARSE_BAD_FIELD);
	snprintf(line, sizeof line, "%s,\"force\":true}", JC);
	CHECK_EQ_INT(chk(line, &req, &wrong), NTFSMAC_CTL_PARSE_BAD_FIELD);
	snprintf(line, sizeof line, "%s,\"bsd\":\"disk4\"}", JC);
	CHECK_EQ_INT(chk(line, &req, &wrong), NTFSMAC_CTL_PARSE_BAD_FIELD);
	CHECK_EQ_INT(chk("{\"cmd\":\"journal_clear\",\"bsd\":\"/dev/disk3\",\"confirm_label\":\"L\","
			 "\"confirm_serial\":\"0A1B2C3D\"}", &req, &wrong), NTFSMAC_CTL_PARSE_BAD_FIELD);
	CHECK_EQ_INT(chk("{\"cmd\":\"journal_clear\",\"bsd\":\"disk3\",\"confirm_label\":\"L\","
			 "\"confirm_serial\":\"0a1b2c3d\"}", &req, &wrong), NTFSMAC_CTL_PARSE_OK);
	CHECK(strcmp(req.confirm_serial, "0A1B2C3D") == 0); /* the parser hands on upper case */
	CHECK_EQ_INT(chk("{\"cmd\":\"journal_clear\",\"bsd\":\"disk3\",\"confirm_label\":\"L\","
			 "\"confirm_serial\":\"0a1b2c3\"}", &req, &wrong), NTFSMAC_CTL_PARSE_BAD_FIELD); /* 8 digits */
	CHECK_EQ_INT(chk("{\"cmd\":\"journal_clear\",\"bsd\":\"disk3\",\"confirm_label\":\"L\\u001b\","
			 "\"confirm_serial\":\"0A1B2C3D\"}", &req, &wrong), NTFSMAC_CTL_PARSE_BAD_FIELD); /* control code point */
	snprintf(line, sizeof line, "%s,\"auth\":\"abcd\"}", JC);
	CHECK_EQ_INT(chk(line, &req, &wrong), NTFSMAC_CTL_PARSE_BAD_FIELD); /* a form is 64 hex digits */
	CHECK_EQ_INT(chk("{\"cmd\":\"repair\",\"bsd\":\"disk3\",\"action\":\"fix_it\","
			 "\"confirm_label\":\"L\",\"confirm_serial\":\"0A1B2C3D\"}", &req, &wrong), NTFSMAC_CTL_PARSE_BAD_FIELD);
	CHECK_EQ_INT(chk("{\"cmd\":\"repair\",\"bsd\":\"disk3\","
			 "\"confirm_label\":\"L\",\"confirm_serial\":\"0A1B2C3D\"}", &req, &wrong), NTFSMAC_CTL_PARSE_BAD_FIELD);
}

static void test_start(void)
{
	fake_sys_state s;
	ntfsmac_ctl_sys sys;
	ntfsmac_ctl_replay replay;
	ntfsmac_ctl_env env;
	ntfsmac_ctl_ops ops = fake_ops();
	fake_ops_state o;
	ntfsmac_xpc *x = (ntfsmac_xpc *)1;
	ntfsmac_dmn_status st;

	memset(&o, 0, sizeof o);
	fake_sys_init(&s, &sys);
	ntfsmac_ctl_replay_init(&replay);
	memset(&env, 0, sizeof env);
	env.sys = &sys;
	env.replay = &replay;
	CHECK_EQ_INT(ntfsmac_xpc_start(NULL, &ops, &o, &env, &x), NTFSMAC_DMN_E_INVAL);
	CHECK(x == NULL);
	CHECK_EQ_INT(ntfsmac_xpc_start("org.example.test", NULL, &o, &env, &x), NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_xpc_start("org.example.test", &ops, &o, NULL, &x), NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_xpc_start("org.example.test", &ops, &o, &env, NULL), NTFSMAC_DMN_E_INVAL);
	/*
	 * Without a readable code requirement (the fake has none) the service does not start: the
	 * identity check of the connection is part of the decision. Off macOS there is no service.
	 */
	st = ntfsmac_xpc_start("org.example.test", &ops, &o, &env, &x);
#if defined(__APPLE__)
	CHECK_EQ_INT(st, NTFSMAC_DMN_E_DENIED);
#else
	CHECK_EQ_INT(st, NTFSMAC_DMN_E_NOTIMPL);
#endif
	CHECK(x == NULL);
	ntfsmac_xpc_stop(NULL);
}

static void test_peer_from_token(void)
{
#if defined(__APPLE__)
	const ntfsmac_ctl_sys *sys = ntfsmac_ctl_sys_default();
	uint32_t tok[8];
	ntfsmac_ctl_peer p;

	CHECK(sys->peer_from_token != NULL && sys->requirement != NULL);
	/* audit_token_t: word 1 is the effective uid, word 5 the pid */
	memset(tok, 0, sizeof tok);
	tok[1] = (uint32_t)geteuid();
	tok[5] = (uint32_t)getpid();
	CHECK_EQ_INT(sys->peer_from_token(sys->ctx, tok, &p), 0);
	CHECK(p.uid == geteuid() && p.pid == getpid() && p.has_audit_token);
	CHECK(memcmp(p.audit_token, tok, sizeof tok) == 0);
	/* the identity comes from the token and from nothing else: another uid is another peer */
	tok[1] = 12345u;
	CHECK_EQ_INT(sys->peer_from_token(sys->ctx, tok, &p), 0);
	CHECK_EQ_INT(p.uid, 12345);
	CHECK(!p.console_user || p.uid == 12345);
	/* no pid: no peer */
	tok[5] = 0;
	CHECK(sys->peer_from_token(sys->ctx, tok, &p) != 0);
#else
	const ntfsmac_ctl_sys *sys = ntfsmac_ctl_sys_default();
	uint32_t tok[8] = {0};
	ntfsmac_ctl_peer p;

	CHECK(sys->peer_from_token(sys->ctx, tok, &p) != 0); /* fails closed off macOS */
	CHECK(!sys->requirement(sys->ctx, (char[8]){0}, 8));
#endif
}

int main(void)
{
	test_validation();
	test_strict_grammar();
	test_start();
	test_peer_from_token();
	if (g_failures != 0) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	return 0;
}
