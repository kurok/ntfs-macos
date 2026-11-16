/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-CTL-FUZZ-01: a small deterministic fuzz loop over ntfsmac_ctl_handle_line_env()
 * (the function behind ntfsmac_ctl_handle_line(), with the fake system). It runs
 * in the normal ctest run and under the asan preset. Mutations of valid
 * requests plus random bytes; each input is copied to a heap block of exactly
 * its length, so a read past the end is a sanitizer report.
 *
 * Oracles, for every input:
 *   - no crash, no sanitizer report;
 *   - the reply is NUL terminated inside the buffer, starts with {"ok": and has no newline;
 *   - a peer that is neither console user nor admin never reaches an operation;
 *   - journal_clear and repair reach their operation only when the peer is
 *     admin, not spawned, signed, and the system accepted a form that was not
 *     used before;
 *   - when the parser accepts, every field satisfies its grammar.
 * Environment: NTFSMAC_FUZZ_ITERS (default 200000), NTFSMAC_FUZZ_SEED.
 */
#include <stdlib.h>

#include "check.h"
#include "fakes.h"

static uint64_t rng_state;

static uint64_t rnd(void)
{
	uint64_t x = rng_state;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	rng_state = x;
	return x;
}

static unsigned pick(unsigned n)
{
	return (unsigned)(rnd() % n);
}

static const char *const seeds[] = {
	"{\"cmd\":\"list\"}",
	"{\"cmd\":\"doctor\"}",
	"{\"cmd\":\"status\",\"bsd\":\"disk4s1\"}",
	"{\"cmd\":\"mount\",\"bsd\":\"disk4s1\",\"mode\":\"ro\"}",
	"{\"cmd\":\"unmount\",\"bsd\":\"disk4s1\",\"force\":true}",
	"{\"cmd\":\"journal_clear\",\"bsd\":\"disk3s2\",\"confirm_label\":\"My Disk\","
	"\"confirm_serial\":\"0A1B2C3D\",\"auth\":\"" GOOD_AUTH "\"}",
	"{\"cmd\":\"repair\",\"bsd\":\"disk3s2\",\"action\":\"sync_mirror\",\"confirm_label\":"
	"\"\\u00e9\\ud83d\\ude00\",\"confirm_serial\":\"0A1B2C3D\",\"auth\":\"" GOOD_AUTH "\"}",
};

static const char interesting[] = "{}[]\",:\\/u0123456789abcdefABCDEF\n\r\t \x01\x7f\x80\xc2\xed\xf4\xff";

/* Build one input in `buf`; returns its length. */
static size_t make_input(unsigned char *buf, size_t cap)
{
	size_t n, i;
	unsigned rounds;

	if (pick(8) == 0) { /* pure noise */
		n = pick(6000);
		if (n > cap)
			n = cap;
		for (i = 0; i < n; i++)
			buf[i] = (unsigned char)rnd();
		return n;
	}
	{
		const char *s = seeds[pick(sizeof seeds / sizeof seeds[0])];

		n = strlen(s);
		memcpy(buf, s, n);
	}
	for (rounds = 1 + pick(6); rounds > 0; rounds--) {
		switch (pick(7)) {
		case 0: /* flip a byte */
			if (n > 0)
				buf[pick((unsigned)n)] = (unsigned char)rnd();
			break;
		case 1: /* put an interesting byte */
			if (n > 0)
				buf[pick((unsigned)n)] = (unsigned char)interesting[pick(sizeof interesting - 1)];
			break;
		case 2: /* truncate */
			if (n > 0)
				n = pick((unsigned)n + 1);
			break;
		case 3: /* delete a byte */
			if (n > 0) {
				size_t at = pick((unsigned)n);

				memmove(buf + at, buf + at + 1, n - at - 1);
				n--;
			}
			break;
		case 4: /* insert a byte */
			if (n + 1 < cap) {
				size_t at = pick((unsigned)n + 1);

				memmove(buf + at + 1, buf + at, n - at);
				buf[at] = (unsigned char)interesting[pick(sizeof interesting - 1)];
				n++;
			}
			break;
		case 5: /* repeat a stretch (long values) */
			if (n > 4 && n + 600 < cap) {
				size_t at = pick((unsigned)n - 1), len = 1 + pick(8), k = 1 + pick(70);

				if (at + len > n)
					len = n - at;
				for (i = 0; i < k && n + len < cap; i++) {
					memmove(buf + at + len, buf + at, n - at);
					n += len;
				}
			}
			break;
		default: /* append another seed (trailing data) */
		{
			const char *s = seeds[pick(sizeof seeds / sizeof seeds[0])];
			size_t l = strlen(s);

			if (n + l < cap) {
				memcpy(buf + n, s, l);
				n += l;
			}
		}
		}
	}
	return n;
}

static bool hexdig(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
}

int main(void)
{
	const char *e = getenv("NTFSMAC_FUZZ_ITERS");
	unsigned long iters = e != NULL ? strtoul(e, NULL, 10) : 200000;
	const char *se = getenv("NTFSMAC_FUZZ_SEED");
	unsigned long it, accepted = 0, reached_priv = 0, denied = 0;
	unsigned char *buf = malloc(8192);
	char reply[NTFSMAC_CTL_REPLY_MIN];

	rng_state = se != NULL ? strtoull(se, NULL, 10) | 1 : 0x9E3779B97F4A7C15ull;

	for (it = 0; it < iters; it++) {
		fake_sys_state s;
		ntfsmac_ctl_sys sys;
		ntfsmac_ctl_replay replay;
		ntfsmac_ctl_env env;
		fake_ops_state o;
		ntfsmac_ctl_ops ops = fake_ops();
		ntfsmac_ctl_peer peer = good_peer();
		ntfsmac_ctl_request req;
		unsigned char *in;
		size_t n = make_input(buf, 8000);
		int before_priv;
		ntfsmac_dmn_status st;
		unsigned kind = pick(10);

		fake_sys_init(&s, &sys);
		ntfsmac_ctl_replay_init(&replay);
		env.sys = &sys;
		env.replay = &replay;
		memset(&o, 0, sizeof o);
		o.status = pick(4) == 0 ? NTFSMAC_DMN_E_BUSY : NTFSMAC_DMN_OK;
		/* vary every fact the checks read */
		if (kind == 0) {
			peer.console_user = false;
			peer.admin = false;
		} else if (kind == 1) {
			peer.admin = false;
		} else if (kind == 2) {
			o.spawned[0] = 400;
			o.nspawned = 1;
		} else if (kind == 3) {
			s.signed_answer = (ntfsmac_ctl_sys_answer)pick(3);
		} else if (kind == 4) {
			s.authz_answer = (ntfsmac_ctl_sys_answer)pick(3);
		} else if (kind == 5) {
			peer.has_audit_token = false;
		}

		in = malloc(n > 0 ? n : 1);
		memcpy(in, buf, n);
		memset(reply, 'Z', sizeof reply);
		st = ntfsmac_ctl_handle_line_env(&env, &ops, &o, &peer, (const char *)in, n, reply, sizeof reply);
		before_priv = o.calls[NTFSMAC_CTL_CMD_JOURNAL_CLEAR] + o.calls[NTFSMAC_CTL_CMD_REPAIR];

		CHECK((unsigned)st <= (unsigned)NTFSMAC_DMN_E_DENIED);
		CHECK(memchr(reply, '\0', sizeof reply) != NULL);
		CHECK(strncmp(reply, "{\"ok\":", 6) == 0);
		CHECK(strchr(reply, '\n') == NULL);
		if (st == NTFSMAC_DMN_E_DENIED)
			denied++;

		if (!peer.console_user && !peer.admin)
			CHECK_EQ_INT(fake_total_calls(&o), 0);
		if (before_priv > 0) {
			reached_priv++;
			CHECK(peer.admin);
			CHECK(!f_is_spawned(&o, 500) && !f_is_spawned(&o, 400));
			CHECK_EQ_INT(s.signed_answer, NTFSMAC_CTL_SYS_YES);
			CHECK_EQ_INT(s.authz_answer, NTFSMAC_CTL_SYS_YES);
			CHECK(peer.has_audit_token);
			CHECK_EQ_INT(s.authz_calls, 1);
			CHECK_EQ_INT(before_priv, 1);
		}

		/* The parser alone, on the exact block, and its grammar when it accepts. */
		if (ntfsmac_ctl_parse_request((const char *)in, n, &req) == NTFSMAC_CTL_PARSE_OK) {
			size_t i;

			accepted++;
			CHECK((unsigned)req.cmd < NTFSMAC_CTL_CMD_COUNT_);
			if (req.bsd[0] != '\0')
				CHECK(ntfsmac_ctl_bsd_valid(req.bsd));
			if (req.confirm_serial[0] != '\0') {
				CHECK_EQ_INT(strlen(req.confirm_serial), NTFSMAC_CTL_SERIAL_DIGITS);
				for (i = 0; i < NTFSMAC_CTL_SERIAL_DIGITS; i++)
					CHECK(hexdig(req.confirm_serial[i]));
			}
			CHECK(strlen(req.confirm_label) <= NTFSMAC_CTL_LABEL_MAX);
			CHECK(memchr(req.confirm_label, '\0', sizeof req.confirm_label) != NULL);
		}
		free(in);
	}
	free(buf);
	fprintf(stderr, "ctl_fuzz: %lu inputs, %lu parsed, %lu denied, %lu reached journal_clear/repair\n",
		iters, accepted, denied, reached_priv);
	/* the loop must really exercise both outcomes */
	if (iters >= 10000) {
		CHECK(accepted > 100);
		CHECK(reached_priv > 10);
		CHECK(denied > 100);
	}
	if (g_failures != 0) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	return 0;
}
