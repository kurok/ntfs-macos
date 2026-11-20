/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * F-CTL-01 (docs/TEST_STRATEGY.md section 8, "Control socket parser"; T-M5-09): libFuzzer on
 * ntfsmac_ctl_handle_line_env() and ntfsmac_ctl_parse_request() with the fake system and fake
 * operations of tests/unit/ctl/fakes.h (nothing here touches the operating system).
 *
 * Input: byte 0 selects the fact that is varied (as U-CTL-FUZZ-01 does: kind = byte % 10, bit 4 the
 * channel, bit 5 the authorization strategy (set: ROOT_PEER, clear: EXTERNAL_FORM), bit 6 the peer uid
 * (set: 0, clear: 501), bit 7 a busy operation), byte 1 the answers of the fake system for kinds 3 and 4
 * (signature: byte % 3, authorization: byte / 3 % 3), the rest is the request line. The line is
 * copied to a heap block of exactly its length, so a read past the end is a sanitizer report.
 *
 * Oracles (the same as U-CTL-FUZZ-01; a failure aborts):
 *   - no crash, no sanitizer report;
 *   - the reply is NUL terminated inside the buffer, starts with {"ok": and has no newline;
 *   - a peer that is neither console user nor admin never reaches an operation;
 *   - journal_clear and repair reach their operation only for an admin peer that is not spawned,
 *     from a signed client, exactly once, and with check 4 of the strategy in force: under ROOT_PEER the
 *     peer uid is 0; under EXTERNAL_FORM the system accepted the form (once);
 *   - when the parser accepts, every field satisfies its grammar.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fakes.h"

#define FAIL(msg)                                                                          \
	do {                                                                               \
		fprintf(stderr, "F-CTL-01 oracle failure: %s (line %d)\n", msg, __LINE__); \
		abort();                                                                   \
	} while (0)
#define ORACLE(c)                 \
	do {                      \
		if (!(c))         \
			FAIL(#c); \
	} while (0)

static int hexdig(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	fake_sys_state s;
	ntfsmac_ctl_sys sys;
	ntfsmac_ctl_replay replay;
	ntfsmac_ctl_env env;
	fake_ops_state o;
	ntfsmac_ctl_ops ops = fake_ops();
	ntfsmac_ctl_peer peer = good_peer();
	ntfsmac_ctl_request req;
	char reply[NTFSMAC_CTL_REPLY_MIN];
	unsigned char *in;
	size_t n;
	int before_priv;
	ntfsmac_dmn_status st;
	unsigned k0, k1;

	if (size < 2)
		return 0;
	k0 = data[0];
	k1 = data[1];
	data += 2;
	n = size - 2;

	fake_sys_init(&s, &sys);
	ntfsmac_ctl_replay_init(&replay);
	memset(&env, 0, sizeof env);
	env.sys = &sys;
	env.replay = &replay;
	env.channel = ((k0 >> 4) & 1) ? NTFSMAC_CTL_CHAN_SOCKET : NTFSMAC_CTL_CHAN_XPC;
	env.authz = ((k0 >> 5) & 1) ? NTFSMAC_CTL_AUTHZ_ROOT_PEER : NTFSMAC_CTL_AUTHZ_EXTERNAL_FORM;
	peer.uid = ((k0 >> 6) & 1) ? 0 : 501;
	memset(&o, 0, sizeof o);
	o.status = (k0 >> 7) ? NTFSMAC_DMN_E_BUSY : NTFSMAC_DMN_OK;
	switch (k0 % 10) {
	case 0:
		peer.console_user = false;
		peer.admin = false;
		break;
	case 1:
		peer.admin = false;
		break;
	case 2:
		o.spawned[0] = 400;
		o.nspawned = 1;
		break;
	case 3:
		s.signed_answer = (ntfsmac_ctl_sys_answer)(k1 % 3);
		break;
	case 4:
		s.authz_answer = (ntfsmac_ctl_sys_answer)((k1 / 3) % 3);
		break;
	case 5:
		peer.has_audit_token = false;
		break;
	default:
		break;
	}

	in = malloc(n > 0 ? n : 1);
	if (in == NULL)
		abort();
	memcpy(in, data, n);
	memset(reply, 'Z', sizeof reply);
	st = ntfsmac_ctl_handle_line_env(&env, &ops, &o, &peer, (const char *)in, n, reply, sizeof reply);
	before_priv = o.calls[NTFSMAC_CTL_CMD_JOURNAL_CLEAR] + o.calls[NTFSMAC_CTL_CMD_REPAIR];

	ORACLE((unsigned)st <= (unsigned)NTFSMAC_DMN_E_DENIED);
	ORACLE(memchr(reply, '\0', sizeof reply) != NULL);
	ORACLE(strncmp(reply, "{\"ok\":", 6) == 0);
	ORACLE(strchr(reply, '\n') == NULL);
	if (!peer.console_user && !peer.admin)
		ORACLE(fake_total_calls(&o) == 0);
	if (before_priv > 0) {
		ORACLE(peer.admin);
		ORACLE(!f_is_spawned(&o, 500) && !f_is_spawned(&o, 400));
		ORACLE(s.signed_answer == NTFSMAC_CTL_SYS_YES);
		ORACLE(peer.has_audit_token);
		ORACLE(before_priv == 1);
		if (env.authz == NTFSMAC_CTL_AUTHZ_ROOT_PEER) {
			ORACLE(peer.uid == 0);
		} else {
			ORACLE(s.authz_answer == NTFSMAC_CTL_SYS_YES);
			ORACLE(s.authz_calls == 1);
		}
	}
	if (ntfsmac_ctl_parse_request((const char *)in, n, &req) == NTFSMAC_CTL_PARSE_OK) {
		size_t i;

		ORACLE((unsigned)req.cmd < NTFSMAC_CTL_CMD_COUNT_);
		if (req.bsd[0] != '\0')
			ORACLE(ntfsmac_ctl_bsd_valid(req.bsd));
		if (req.confirm_serial[0] != '\0') {
			ORACLE(strlen(req.confirm_serial) == NTFSMAC_CTL_SERIAL_DIGITS);
			for (i = 0; i < NTFSMAC_CTL_SERIAL_DIGITS; i++)
				ORACLE(hexdig(req.confirm_serial[i]));
		}
		ORACLE(strlen(req.confirm_label) <= NTFSMAC_CTL_LABEL_MAX);
		ORACLE(memchr(req.confirm_label, '\0', sizeof req.confirm_label) != NULL);
	}
	free(in);
	return 0;
}
