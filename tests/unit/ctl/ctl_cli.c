/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-CTL-CLI-01..05: the `ntfsmac` CLI library.
 *   01 reason, refusal and error text tables cover every code
 *   02 request builder: escaping and the size limit
 *   03 safe printing of strings that are not trustworthy
 *   04 rendering of replies (list table, dump, refusals, exit codes)
 *   05 the CLI against the real control server (fake operations): arguments,
 *      requests on the wire, exit codes, daemon not running
 */
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "cli.h"
#include "fakes.h"
#include "json.h"
#include "ntfsmac/probe.h"

/* ---------------- 01 ---------------- */

static void test_tables(void)
{
	static const char *const reasons[] = {
#define X(id, str) str,
		NTFSMAC_PROBE_REASON_LIST(X)
#undef X
	};
	unsigned i;

	for (i = 0; i < sizeof reasons / sizeof reasons[0]; i++) {
		const char *t = ntfsmac_cli_reason_text(reasons[i]);

		if (t == NULL)
			fprintf(stderr, "no text for reason %s\n", reasons[i]);
		CHECK(t != NULL && t[0] != '\0');
	}
	CHECK_EQ_INT(sizeof reasons / sizeof reasons[0], NTFSMAC_PROBE_REASON_COUNT_);
	CHECK(ntfsmac_cli_reason_text("NO_SUCH_CODE") == NULL);
	CHECK(ntfsmac_cli_reason_text("") == NULL);
	CHECK(ntfsmac_cli_reason_text(NULL) == NULL);
	CHECK(ntfsmac_cli_reason_text("flag_dirty") == NULL); /* codes are exact */

	for (i = 0; i < NTFSMAC_CTL_REFUSE_COUNT_; i++) {
		const char *t = ntfsmac_cli_refusal_text(ntfsmac_ctl_refusal_name((ntfsmac_ctl_refusal)i));

		CHECK(t != NULL && t[0] != '\0');
	}
	CHECK(ntfsmac_cli_refusal_text("x") == NULL);
	for (i = 0; i < NTFSMAC_CTL_ERR_COUNT_; i++) {
		const char *t = ntfsmac_cli_error_text(ntfsmac_ctl_error_name((ntfsmac_ctl_error)i));

		CHECK(t != NULL && t[0] != '\0');
	}
	CHECK(ntfsmac_cli_error_text(NULL) == NULL);
}

/* ---------------- 02 ---------------- */

static void test_req(void)
{
	ntfsmac_cli_req r;
	char big[NTFSMAC_CTL_LINE_MAX + 100];

	ntfsmac_cli_req_begin(&r, NTFSMAC_CTL_CMD_MOUNT);
	ntfsmac_cli_req_str(&r, "bsd", "disk4s1");
	ntfsmac_cli_req_bool(&r, "force", true);
	CHECK_EQ_INT(ntfsmac_cli_req_end(&r), 0);
	CHECK(strcmp(r.buf, "{\"cmd\":\"mount\",\"bsd\":\"disk4s1\",\"force\":true}") == 0);

	ntfsmac_cli_req_begin(&r, NTFSMAC_CTL_CMD_LIST);
	ntfsmac_cli_req_str(&r, "k", "a\"b\\c\n\x01\x1f \xc3\xa9");
	CHECK_EQ_INT(ntfsmac_cli_req_end(&r), 0);
	CHECK(strcmp(r.buf, "{\"cmd\":\"list\",\"k\":\"a\\\"b\\\\c\\u000a\\u0001\\u001f \xc3\xa9\"}") == 0);

	/* too long: refused, never a cut line */
	memset(big, 'a', sizeof big);
	big[sizeof big - 1] = '\0';
	ntfsmac_cli_req_begin(&r, NTFSMAC_CTL_CMD_STATUS);
	ntfsmac_cli_req_str(&r, "bsd", big);
	CHECK_EQ_INT(ntfsmac_cli_req_end(&r), -1);
	CHECK(r.overflow);
	/* the largest value that still fits */
	{
		size_t fixed = strlen("{\"cmd\":\"status\",\"bsd\":\"\"}");
		size_t room = NTFSMAC_CTL_LINE_MAX - 1 - fixed;

		big[room] = '\0';
		ntfsmac_cli_req_begin(&r, NTFSMAC_CTL_CMD_STATUS);
		ntfsmac_cli_req_str(&r, "bsd", big);
		CHECK_EQ_INT(ntfsmac_cli_req_end(&r), 0);
		CHECK(strlen(r.buf) <= NTFSMAC_CTL_LINE_MAX);
	}
}

/* ---------------- 03 ---------------- */

/* Run print_safe into a memory stream. */
static char *safe(const char *s, size_t *cols)
{
	char *out = NULL;
	size_t len = 0;
	FILE *f = open_memstream(&out, &len);

	*cols = ntfsmac_cli_print_safe(f, s);
	fclose(f);
	return out;
}

static void test_safe(void)
{
	size_t cols;
	char *o;

	o = safe("plain text 123", &cols);
	CHECK(strcmp(o, "plain text 123") == 0);
	CHECK_EQ_INT(cols, 14);
	free(o);
	/* valid UTF-8 passes unchanged, one column per code point */
	o = safe("caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80", &cols);
	CHECK(strcmp(o, "caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80") == 0);
	CHECK_EQ_INT(cols, 8);
	free(o);
	/* control bytes, escape, DEL, NUL-free: shown as \xNN */
	o = safe("a\x1b[31mb\rc\nd\x7f\x07", &cols);
	CHECK(strcmp(o, "a\\x1B[31mb\\x0Dc\\x0Ad\\x7F\\x07") == 0);
	CHECK(strchr(o, '\x1b') == NULL);
	free(o);
	/* C1 controls (U+0080..U+009F) and bidirectional controls */
	o = safe("\xc2\x9b" "31m", &cols);
	CHECK(strcmp(o, "\\xC2\\x9B" "31m") == 0);
	free(o);
	o = safe("x\xe2\x80\xae" "y\xe2\x81\xa6z\xe2\x80\x8e", &cols);
	CHECK(strcmp(o, "x\\xE2\\x80\\xAEy\\xE2\\x81\\xA6z\\xE2\\x80\\x8E") == 0);
	free(o);
	/* invisible and direction-changing code points are escaped, each byte as \xNN */
	{
		static const struct {
			const char *in, *out;
		} cases[] = {
			{"\xd8\x9c", "\\xD8\\x9C"},          /* U+061C */
			{"\xe2\x80\x8b", "\\xE2\\x80\\x8B"}, /* U+200B */
			{"\xe2\x80\x8c", "\\xE2\\x80\\x8C"},
			{"\xe2\x80\x8d", "\\xE2\\x80\\x8D"},
			{"\xe2\x80\x8e", "\\xE2\\x80\\x8E"},
			{"\xe2\x80\x8f", "\\xE2\\x80\\x8F"}, /* U+200F */
			{"\xe2\x80\xa8", "\\xE2\\x80\\xA8"}, /* U+2028 */
			{"\xe2\x80\xa9", "\\xE2\\x80\\xA9"}, /* U+2029 */
			{"\xe2\x80\xaa", "\\xE2\\x80\\xAA"}, /* U+202A */
			{"\xe2\x80\xab", "\\xE2\\x80\\xAB"},
			{"\xe2\x80\xac", "\\xE2\\x80\\xAC"},
			{"\xe2\x80\xad", "\\xE2\\x80\\xAD"},
			{"\xe2\x80\xae", "\\xE2\\x80\\xAE"}, /* U+202E */
			{"\xe2\x81\xa6", "\\xE2\\x81\\xA6"}, /* U+2066 */
			{"\xe2\x81\xa7", "\\xE2\\x81\\xA7"},
			{"\xe2\x81\xa8", "\\xE2\\x81\\xA8"},
			{"\xe2\x81\xa9", "\\xE2\\x81\\xA9"}, /* U+2069 */
			{"\xef\xbb\xbf", "\\xEF\\xBB\\xBF"}, /* U+FEFF */
		};
		unsigned k;

		for (k = 0; k < sizeof cases / sizeof cases[0]; k++) {
			o = safe(cases[k].in, &cols);
			if (strcmp(o, cases[k].out) != 0)
				fprintf(stderr, "case %u: got %s\n", k, o);
			CHECK(strcmp(o, cases[k].out) == 0);
			free(o);
		}
		/* neighbours of the ranges are ordinary text and pass unchanged */
		{
			static const char *const plain[] = {
				"\xd8\x9b",         /* U+061B */
				"\xd8\x9d",         /* U+061D */
				"\xe2\x80\x8a",    /* U+200A hair space */
				"\xe2\x80\x90",    /* U+2010 hyphen */
				"\xe2\x80\xa7",    /* U+2027 */
				"\xe2\x80\xaf",    /* U+202F */
				"\xe2\x81\xa5",    /* U+2065 */
				"\xe2\x81\xaa",    /* U+206A */
				"\xef\xbb\xbe",    /* U+FEFE */
				"\xc2\xa0",         /* U+00A0 */
			};

			for (k = 0; k < sizeof plain / sizeof plain[0]; k++) {
				o = safe(plain[k], &cols);
				CHECK(strcmp(o, plain[k]) == 0);
				CHECK_EQ_INT(cols, 1);
				free(o);
			}
		}
		/* inside text, and the width of the escape is counted */
		o = safe("a\xe2\x80\xae" "b\xef\xbb\xbf" "c", &cols);
		CHECK(strcmp(o, "a\\xE2\\x80\\xAEb\\xEF\\xBB\\xBFc") == 0);
		CHECK_EQ_INT(cols, 1 + 12 + 1 + 12 + 1);
		free(o);
	}
	/* invalid UTF-8 is escaped byte by byte */
	o = safe("\xff\xc0\x80\xed\xa0\x80\xe2\x82", &cols);
	CHECK(strchr(o, '\xff') == NULL);
	CHECK(strstr(o, "\\xFF") != NULL);
	{
		const unsigned char *p;

		for (p = (const unsigned char *)o; *p != '\0'; p++)
			CHECK(*p >= 0x20 && *p < 0x7f);
	}
	free(o);
	/* never a raw byte below 0x20 whatever the input */
	{
		char in[256];
		int b;

		for (b = 1; b < 256; b++)
			in[b - 1] = (char)b;
		in[255] = '\0';
		o = safe(in, &cols);
		{
			const unsigned char *p;

			for (p = (const unsigned char *)o; *p != '\0'; p++)
				CHECK(*p >= 0x20 && *p != 0x7f);
		}
		free(o);
	}
}

/* ---------------- 04 ---------------- */

typedef struct cap {
	char *out, *err;
	size_t outlen, errlen;
	ntfsmac_cli_ctx ctx;
} cap;

static void cap_open(cap *c)
{
	memset(c, 0, sizeof *c);
	c->ctx.out = open_memstream(&c->out, &c->outlen);
	c->ctx.err = open_memstream(&c->err, &c->errlen);
	c->ctx.socket_path = "unused";
}

static void cap_close(cap *c)
{
	fclose(c->ctx.out);
	fclose(c->ctx.err);
}

static void cap_free(cap *c)
{
	free(c->out);
	free(c->err);
}

static void test_render(void)
{
	cap c;
	int rc;

	/* list: a table with a header; hostile text in a label is escaped */
	cap_open(&c);
	rc = ntfsmac_cli_render(&c.ctx, NTFSMAC_CTL_CMD_LIST,
		"{\"ok\":true,\"volumes\":[{\"bsd\":\"disk4s1\",\"label\":\"Photos\",\"state\":\"Mounted\","
		"\"mount\":\"rw\",\"mountpoint\":\"/Volumes/Photos\",\"reason\":\"\"},"
		"{\"bsd\":\"disk5s1\",\"label\":\"E\\u001b[2Jvil\",\"state\":\"Failed\",\"mount\":\"none\","
		"\"mountpoint\":\"\",\"reason\":\"FLAG_DIRTY\"}]}");
	cap_close(&c);
	CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_OK);
	CHECK(strncmp(c.out, "BSD ", 4) == 0);
	CHECK(strstr(c.out, "disk4s1") != NULL && strstr(c.out, "Photos") != NULL);
	CHECK(strstr(c.out, "/Volumes/Photos") != NULL);
	CHECK(strchr(c.out, '\x1b') == NULL);
	CHECK(strstr(c.out, "E\\x1B[2Jvil") != NULL);
	CHECK(strstr(c.out, "FLAG_DIRTY") != NULL);
	CHECK_EQ_INT(c.errlen, 0);
	cap_free(&c);

	cap_open(&c);
	rc = ntfsmac_cli_render(&c.ctx, NTFSMAC_CTL_CMD_LIST, "{\"ok\":true,\"volumes\":[]}");
	cap_close(&c);
	CHECK_EQ_INT(rc, 0);
	CHECK(strcmp(c.out, "No NTFS volumes.\n") == 0);
	cap_free(&c);

	/* status: a dump; reason codes get their text; strings are safe */
	cap_open(&c);
	rc = ntfsmac_cli_render(&c.ctx, NTFSMAC_CTL_CMD_STATUS,
		"{\"ok\":true,\"volume\":{\"bsd\":\"disk4s1\",\"state\":\"Mounted\",\"mount\":\"ro\"},"
		"\"probe\":{\"state\":\"dirty_flag\",\"reason_codes\":[\"FLAG_DIRTY\",\"WHAT_IS_THIS\"],"
		"\"nonnfc_paths\":[\"a\\u001bb\"],\"serial\":\"0x00000000DEADBEEF\",\"nonnfc_count\":1,"
		"\"is_ntfs\":true,\"x\":null}}");
	cap_close(&c);
	CHECK_EQ_INT(rc, 0);
	CHECK(strstr(c.out, "volume:\n  bsd: disk4s1\n") != NULL);
	CHECK(strstr(c.out, "FLAG_DIRTY: The volume is marked dirty") != NULL);
	CHECK(strstr(c.out, "WHAT_IS_THIS\n") != NULL); /* unknown code: shown, without text */
	CHECK(strstr(c.out, "a\\x1Bb") != NULL);
	CHECK(strstr(c.out, "is_ntfs: yes") != NULL);
	CHECK(strchr(c.out, '\x1b') == NULL);
	CHECK(strstr(c.out, "ok:") == NULL);
	cap_free(&c);

	/* refusals: text from our tables, on stderr, exit 1 */
	cap_open(&c);
	rc = ntfsmac_cli_render(&c.ctx, NTFSMAC_CTL_CMD_JOURNAL_CLEAR,
				"{\"ok\":false,\"error\":\"denied\",\"reason\":\"spawned_process\"}");
	cap_close(&c);
	CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_FAILED);
	CHECK(strstr(c.err, "started") != NULL);
	CHECK_EQ_INT(c.outlen, 0);
	cap_free(&c);

	cap_open(&c);
	rc = ntfsmac_cli_render(&c.ctx, NTFSMAC_CTL_CMD_MOUNT,
				"{\"ok\":false,\"error\":\"failed\",\"reason\":\"MFTMIRR_STALE\"}");
	cap_close(&c);
	CHECK_EQ_INT(rc, 1);
	CHECK(strstr(c.err, "MFTMIRR_STALE: The backup copy") != NULL);
	cap_free(&c);

	cap_open(&c);
	rc = ntfsmac_cli_render(&c.ctx, NTFSMAC_CTL_CMD_DOCTOR, "{\"ok\":false,\"error\":\"not_implemented\"}");
	cap_close(&c);
	CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_UNAVAILABLE);
	cap_free(&c);

	/* an error code we do not know is shown safely */
	cap_open(&c);
	rc = ntfsmac_cli_render(&c.ctx, NTFSMAC_CTL_CMD_DOCTOR, "{\"ok\":false,\"error\":\"x\\u001by\"}");
	cap_close(&c);
	CHECK_EQ_INT(rc, 1);
	CHECK(strchr(c.err, '\x1b') == NULL);
	cap_free(&c);

	/* not a reply */
	{
		static const char *const junk[] = {"", "garbage", "[]", "{}", "{\"ok\":1}", "{\"ok\":true",
						   "{\"ok\":true}{}", "null"};
		size_t i;

		for (i = 0; i < sizeof junk / sizeof junk[0]; i++) {
			cap_open(&c);
			rc = ntfsmac_cli_render(&c.ctx, NTFSMAC_CTL_CMD_LIST, junk[i]);
			cap_close(&c);
			CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_UNAVAILABLE);
			cap_free(&c);
		}
	}

	/* the JSON reader's limits: deep nesting and many nodes do not crash */
	{
		char deep[200];
		size_t i;

		for (i = 0; i < 100; i++)
			deep[i] = '[';
		for (i = 0; i < 100; i++)
			deep[100 + i] = ']';
		CHECK(json_parse(deep, 200) == NULL);
	}
}

/* ---------------- 05 ---------------- */

static char dir[128], path[160];

static void test_end_to_end(void)
{
	fake_sys_state s;
	ntfsmac_ctl_sys sys;
	ntfsmac_ctl_replay replay;
	ntfsmac_ctl_env env;
	fake_ops_state o;
	ntfsmac_ctl_ops ops = fake_ops();
	ntfsmac_ctl *ctl = NULL;
	const char *t = getenv("TMPDIR");
	char *argv[8];
	cap c;
	int rc;

	snprintf(dir, sizeof dir, "%s%sntfsmac-cli-XXXXXX", t != NULL ? t : "/tmp",
		 (t != NULL && t[strlen(t) - 1] == '/') ? "" : "/");
	CHECK(mkdtemp(dir) != NULL);
	snprintf(path, sizeof path, "%s/s", dir);

	fake_sys_init(&s, &sys);
	ntfsmac_ctl_replay_init(&replay);
	memset(&env, 0, sizeof env);
	env.sys = &sys;
	env.replay = &replay;
	memset(&o, 0, sizeof o);
	o.status = NTFSMAC_DMN_OK;
	CHECK_EQ_INT(ntfsmac_ctl_start_env(path, &ops, &o, &env, &ctl), NTFSMAC_DMN_OK);

#define RUN(...)                                                       \
	do {                                                           \
		const char *a_[] = {"ntfsmac", "--socket", path, __VA_ARGS__}; \
		size_t n_ = sizeof a_ / sizeof a_[0], k_;              \
		for (k_ = 0; k_ < n_; k_++)                            \
			argv[k_] = (char *)a_[k_];                     \
		cap_open(&c);                                          \
		rc = ntfsmac_cli_main((int)n_, argv, c.ctx.out, c.ctx.err); \
		cap_close(&c);                                         \
	} while (0)

	o.reply = "{\"ok\":true,\"volumes\":[{\"bsd\":\"disk4s1\",\"label\":\"Photos\",\"state\":\"Mounted\","
		  "\"mount\":\"rw\",\"mountpoint\":\"/Volumes/Photos\",\"reason\":\"\"}]}";
	RUN("list");
	CHECK_EQ_INT(rc, 0);
	CHECK(strstr(c.out, "Photos") != NULL);
	CHECK_EQ_INT(o.calls[NTFSMAC_CTL_CMD_LIST], 1);
	cap_free(&c);

	o.reply = "{\"ok\":true,\"result\":\"mounted\",\"mode\":\"ro\"}";
	RUN("mount", "--ro", "disk4s1");
	CHECK_EQ_INT(rc, 0);
	CHECK(o.read_only && strcmp(o.bsd, "disk4s1") == 0);
	CHECK(strstr(c.out, "mode: ro") != NULL);
	cap_free(&c);
	RUN("mount", "disk4s1");
	CHECK_EQ_INT(rc, 0);
	CHECK(!o.read_only);
	cap_free(&c);

	RUN("unmount", "--force", "disk4s1");
	CHECK_EQ_INT(rc, 0);
	CHECK(o.force);
	cap_free(&c);
	RUN("unmount", "disk4s1");
	CHECK(!o.force);
	cap_free(&c);

	RUN("status", "disk4s1");
	CHECK_EQ_INT(rc, 0);
	CHECK_EQ_INT(o.calls[NTFSMAC_CTL_CMD_STATUS], 1);
	cap_free(&c);
	RUN("doctor");
	CHECK_EQ_INT(rc, 0);
	CHECK_EQ_INT(o.calls[NTFSMAC_CTL_CMD_DOCTOR], 1);
	cap_free(&c);

	/* a peer that fails the gate is closed without a reply: exit 3 and text */
	s.socket_peer.console_user = false;
	s.socket_peer.admin = false;
	RUN("list");
	CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_UNAVAILABLE);
	CHECK(strstr(c.err, "ntfsmac:") != NULL);
	CHECK_EQ_INT(o.calls[NTFSMAC_CTL_CMD_LIST], 1); /* only the call made before */
	cap_free(&c);
	s.socket_peer = good_peer();

	/* a bad BSD name is refused by the daemon (bad_field), not sent on */
	{
		int before = o.calls[NTFSMAC_CTL_CMD_STATUS];

		RUN("status", "../../etc");
		CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_FAILED);
		CHECK_EQ_INT(o.calls[NTFSMAC_CTL_CMD_STATUS], before);
		cap_free(&c);
	}

	/*
	 * journal-clear and repair refuse --socket (L5): a usage error before
	 * anything is asked or sent, so no authorization form can reach another
	 * socket. Without --socket they go to the XPC service (test_privileged).
	 */
	RUN("journal-clear", "disk4s1");
	CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_USAGE);
	CHECK(strstr(c.err, "--socket") != NULL);
	cap_free(&c);
	RUN("repair", "--clear-dirty", "disk4s1");
	CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_USAGE);
	CHECK(strstr(c.err, "--socket") != NULL);
	cap_free(&c);
	RUN("repair", "--sync-mirror", "disk4s1");
	CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_USAGE);
	cap_free(&c);
	CHECK_EQ_INT(o.calls[NTFSMAC_CTL_CMD_JOURNAL_CLEAR] + o.calls[NTFSMAC_CTL_CMD_REPAIR], 0);

	/* usage errors */
	RUN();
	CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_USAGE);
	cap_free(&c);
	RUN("frobnicate");
	CHECK_EQ_INT(rc, 2);
	cap_free(&c);
	RUN("list", "extra");
	CHECK_EQ_INT(rc, 2);
	cap_free(&c);
	RUN("status");
	CHECK_EQ_INT(rc, 2);
	cap_free(&c);
	RUN("status", "/dev/disk4");
	CHECK_EQ_INT(rc, 2);
	cap_free(&c);
	RUN("mount");
	CHECK_EQ_INT(rc, 2);
	cap_free(&c);
	RUN("mount", "--force", "disk4");
	CHECK_EQ_INT(rc, 2);
	cap_free(&c);
	RUN("mount", "--ro", "--ro", "disk4");
	CHECK_EQ_INT(rc, 2);
	cap_free(&c);
	RUN("unmount", "disk4", "disk5");
	CHECK_EQ_INT(rc, 2);
	cap_free(&c);
	RUN("help");
	CHECK_EQ_INT(rc, 0);
	CHECK(strstr(c.out, "usage:") != NULL);
	cap_free(&c);
	{
		char *a2[] = {"ntfsmac", "--socket"};

		cap_open(&c);
		rc = ntfsmac_cli_main(2, a2, c.ctx.out, c.ctx.err);
		cap_close(&c);
		CHECK_EQ_INT(rc, 2);
		cap_free(&c);
	}

	ntfsmac_ctl_stop(ctl);

	/* no daemon */
	RUN("list");
	CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_UNAVAILABLE);
	CHECK(strstr(c.err, "not running") != NULL);
	cap_free(&c);

	rmdir(dir);
}

/* ---------------- 06 ---------------- */

typedef struct priv_fake {
	int acquire_calls, release_calls, xchg_calls;
	int acquire_rc;
	char acquire_err[64];
	char prompt[256];
	char service[128];
	char line[NTFSMAC_CTL_LINE_MAX + 1];
	const char *reply;
	int xchg_rc;
	int root;
	int xchg_at_release; /* order check: the form is freed only after the exchange */
} priv_fake;

static priv_fake g_pf;

static int pf_acquire(const char *prompt, char hex[2 * NTFSMAC_CTL_AUTH_FORM_BYTES + 1],
		      ntfsmac_cli_authz **out, char *err_text, size_t err_cap)
{
	g_pf.acquire_calls++;
	snprintf(g_pf.prompt, sizeof g_pf.prompt, "%s", prompt);
	*out = NULL;
	if (g_pf.acquire_rc != 0) {
		snprintf(err_text, err_cap, "%s", g_pf.acquire_err);
		return -1;
	}
	memset(hex, 'a', 2 * NTFSMAC_CTL_AUTH_FORM_BYTES);
	hex[2 * NTFSMAC_CTL_AUTH_FORM_BYTES] = '\0';
	*out = (ntfsmac_cli_authz *)&g_pf;
	return 0;
}

static void pf_release(ntfsmac_cli_authz *h)
{
	CHECK(h == (ntfsmac_cli_authz *)&g_pf);
	g_pf.release_calls++;
	g_pf.xchg_at_release = g_pf.xchg_calls;
}

static int pf_xchg(const char *service, const char *line, int timeout_ms, char *reply, size_t cap,
		   char *err_text, size_t err_cap)
{
	(void)timeout_ms;
	g_pf.xchg_calls++;
	snprintf(g_pf.service, sizeof g_pf.service, "%s", service);
	snprintf(g_pf.line, sizeof g_pf.line, "%s", line);
	if (g_pf.xchg_rc != 0) {
		snprintf(err_text, err_cap, "the ntfsmac daemon is not running");
		return -1;
	}
	snprintf(reply, cap, "%s", g_pf.reply);
	return 0;
}

static const char *g_status_reply;
static int g_status_calls;

static int pf_sock(const char *path, const char *line, int timeout_ms, char *reply, size_t cap,
		   char *err_text, size_t err_cap)
{
	(void)path;
	(void)timeout_ms;
	(void)err_cap;
	g_status_calls++;
	CHECK(strstr(line, "\"cmd\":\"status\"") != NULL);
	if (g_status_reply == NULL) {
		snprintf(err_text, err_cap, "no daemon");
		return -1;
	}
	snprintf(reply, cap, "%s", g_status_reply);
	return 0;
}

static int pf_is_root(void)
{
	return g_pf.root;
}

static void slow_fn(void *arg)
{
	(void)arg;
	usleep(1500 * 1000);
}

static void quick_fn(void *arg)
{
	*(int *)arg = 7;
}

static const char *const priv_ok_reply = "{\"ok\":true,\"result\":\"cleared\",\"warning\":\"none\"}";

/* Run `ntfsmac journal-clear --label L --serial 12345678 disk4s1` against the fake state in g_pf. */
static int run_jc(cap *c, const ntfsmac_cli_priv_hooks *hk)
{
	char *a[] = {"ntfsmac", "journal-clear", "--label", "L", "--serial", "12345678", "disk4s1"};
	int rc;

	cap_open(c);
	rc = ntfsmac_cli_main_hooks(7, a, c->ctx.out, c->ctx.err, hk);
	cap_close(c);
	return rc;
}

static void pf_reset(const char *reply)
{
	g_status_calls = 0;
	memset(&g_pf, 0, sizeof g_pf);
	g_pf.reply = reply;
}

static int run_priv(cap *c, ntfsmac_cli_priv_hooks *hk, FILE *in, int argc, char **argv)
{
	int rc;

	pf_reset(priv_ok_reply);
	hk->in = in;
	cap_open(c);
	rc = ntfsmac_cli_main_hooks(argc, argv, c->ctx.out, c->ctx.err, hk);
	cap_close(c);
	return rc;
}

static void test_privileged(void)
{
	ntfsmac_cli_priv_hooks hk = {pf_acquire, pf_release, pf_xchg, pf_is_root, pf_sock, NULL};
	cap c;
	int rc;
	FILE *in;

	/* options given: the dialog line, one prompt, the request on the XPC service with the form */
	{
		char *a[] = {"ntfsmac", "journal-clear", "--label", "My Disk", "--serial", "1a2b3c4d", "disk4s1"};

		rc = run_priv(&c, &hk, NULL, 7, a);
	}
	CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_OK);
	CHECK(strstr(c.err, "password") != NULL && strstr(c.err, "dialog") != NULL);
	CHECK(strstr(c.err, "seconds") != NULL); /* the prompt has its own limit */
	CHECK_EQ_INT(g_pf.acquire_calls, 1);
	CHECK_EQ_INT(g_pf.xchg_calls, 1);
	CHECK(strcmp(g_pf.service, NTFSMAC_CTL_XPC_SERVICE) == 0);
	CHECK(strstr(g_pf.line, "\"cmd\":\"journal_clear\"") != NULL);
	CHECK(strstr(g_pf.line, "\"bsd\":\"disk4s1\"") != NULL);
	CHECK(strstr(g_pf.line, "\"confirm_label\":\"My Disk\"") != NULL);
	CHECK(strstr(g_pf.line, "\"confirm_serial\":\"1A2B3C4D\"") != NULL); /* upper-cased */
	CHECK(strstr(g_pf.line, "\"auth\":\"aaaaaaaa") != NULL);
	CHECK_EQ_INT(g_pf.release_calls, 1);
	CHECK_EQ_INT(g_pf.xchg_at_release, 1); /* freed after the exchange, not before */
	CHECK(strstr(c.out, "cleared") != NULL);
	CHECK(strstr(c.err, "does not check or repair") != NULL); /* what it does and does not do */
	cap_free(&c);

	/* the typed confirmation, from the terminal */
	in = tmpfile();
	CHECK(in != NULL);
	fputs("Photos\n0000beef\n", in);
	rewind(in);
	{
		char *a[] = {"ntfsmac", "journal-clear", "disk4s1"};

		rc = run_priv(&c, &hk, in, 3, a);
	}
	CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_OK);
	CHECK(strstr(g_pf.line, "\"confirm_label\":\"Photos\"") != NULL);
	CHECK(strstr(g_pf.line, "\"confirm_serial\":\"0000BEEF\"") != NULL);
	cap_free(&c);
	fclose(in);

	/* nothing typed: nothing sent, no dialog */
	in = tmpfile();
	{
		char *a[] = {"ntfsmac", "journal-clear", "disk4s1"};

		rc = run_priv(&c, &hk, in, 3, a);
	}
	CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_USAGE);
	CHECK_EQ_INT(g_pf.acquire_calls + g_pf.xchg_calls, 0);
	cap_free(&c);
	fclose(in);

	/* a serial of the wrong shape, a label that is too long, bad names and options */
	{
		char big[400];
		char *a1[] = {"ntfsmac", "journal-clear", "--label", "L", "--serial", "12345", "disk4s1"};
		char *a2[] = {"ntfsmac", "journal-clear", "--label", "L", "--serial", "1234567G", "disk4s1"};
		char *a3[] = {"ntfsmac", "journal-clear", "--label", big, "--serial", "12345678", "disk4s1"};
		char *a4[] = {"ntfsmac", "journal-clear", "--label", "L", "--serial", "12345678", "/dev/disk4s1"};
		char *a5[] = {"ntfsmac", "journal-clear", "--label", "L", "--serial", "12345678", "disk4"};
		char *a6[] = {"ntfsmac", "journal-clear", "--label", "L", "--serial", "12345678", "disk4s1", "disk5s1"};
		char *a7[] = {"ntfsmac", "journal-clear", "--force", "disk4s1"};
		char *a8[] = {"ntfsmac", "journal-clear", "--label", "L", "--label", "M", "--serial", "12345678", "disk4s1"};
		char *a9[] = {"ntfsmac", "journal-clear", "--label", "L", "--serial", "12345678", "diskX"};

		memset(big, 'x', sizeof big);
		big[sizeof big - 1] = '\0';
		rc = run_priv(&c, &hk, NULL, 7, a1);
		CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_USAGE);
		CHECK_EQ_INT(g_pf.acquire_calls + g_pf.xchg_calls, 0);
		cap_free(&c);
		rc = run_priv(&c, &hk, NULL, 7, a2);
		CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_USAGE);
		cap_free(&c);
		rc = run_priv(&c, &hk, NULL, 7, a3);
		CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_USAGE);
		CHECK_EQ_INT(g_pf.acquire_calls + g_pf.xchg_calls, 0);
		cap_free(&c);
		rc = run_priv(&c, &hk, NULL, 7, a4);
		CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_USAGE);
		cap_free(&c);
		rc = run_priv(&c, &hk, NULL, 7, a5);
		CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_OK); /* a whole disk name has the shape; the daemon decides */
		cap_free(&c);
		rc = run_priv(&c, &hk, NULL, 8, a6);
		CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_USAGE);
		cap_free(&c);
		rc = run_priv(&c, &hk, NULL, 4, a7);
		CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_USAGE);
		cap_free(&c);
		rc = run_priv(&c, &hk, NULL, 9, a8);
		CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_USAGE);
		cap_free(&c);
		rc = run_priv(&c, &hk, NULL, 7, a9);
		CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_USAGE);
		cap_free(&c);
	}

	/* the dialog fails or times out: nothing sent */
	pf_reset(priv_ok_reply);
	g_pf.acquire_rc = -1;
	snprintf(g_pf.acquire_err, sizeof g_pf.acquire_err, "no answer to the administrator prompt");
	rc = run_jc(&c, &hk);
	CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_FAILED);
	CHECK(strstr(c.err, "no answer") != NULL);
	CHECK_EQ_INT(g_pf.xchg_calls, 0);
	CHECK_EQ_INT(g_pf.release_calls, 0);
	cap_free(&c);

	/* root: no dialog, no form in the request, said so */
	pf_reset(priv_ok_reply);
	g_pf.root = 1;
	rc = run_jc(&c, &hk);
	CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_OK);
	CHECK_EQ_INT(g_pf.acquire_calls, 0);
	CHECK(strstr(g_pf.line, "\"auth\"") == NULL);
	CHECK(strstr(c.err, "root") != NULL);
	CHECK_EQ_INT(g_pf.release_calls, 0);
	cap_free(&c);

	/* the daemon refuses: text from the table, exit code 1, the form still freed */
	pf_reset("{\"ok\":false,\"error\":\"denied\",\"reason\":\"probe_not_clean\"}");
	rc = run_jc(&c, &hk);
	CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_FAILED);
	CHECK(strstr(c.err, "not clean") != NULL);
	CHECK_EQ_INT(g_pf.release_calls, 1);
	cap_free(&c);
	pf_reset("{\"ok\":false,\"error\":\"denied\",\"reason\":\"wrong_channel\"}");
	rc = run_jc(&c, &hk);
	CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_FAILED);
	CHECK(strstr(c.err, "not served on this connection") != NULL);
	cap_free(&c);

	/* no service: unavailable, form freed */
	pf_reset(NULL);
	g_pf.xchg_rc = -1;
	rc = run_jc(&c, &hk);
	CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_UNAVAILABLE);
	CHECK(strstr(c.err, "not running") != NULL);
	CHECK_EQ_INT(g_pf.release_calls, 1);
	cap_free(&c);

	/* the stronger words of section 3.5 reach the user after the reply */
	pf_reset("{\"ok\":true,\"result\":\"cleared\",\"warning\":\"write_error\"}");
	rc = run_jc(&c, &hk);
	CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_OK);
	CHECK(strstr(c.err, "write error") != NULL);
	cap_free(&c);
	pf_reset("{\"ok\":true,\"result\":\"cleared\",\"warning\":\"lost_flush\"}");
	rc = run_jc(&c, &hk);
	CHECK(strstr(c.err, "nothing shows that this volume was checked") != NULL);
	cap_free(&c);

	/* repair --clear-dirty: the repair request with the action; --sync-mirror is not here yet */
	{
		char *a1[] = {"ntfsmac", "repair", "--clear-dirty", "--label", "L", "--serial", "12345678", "disk4s1"};
		char *a2[] = {"ntfsmac", "repair", "--sync-mirror", "disk4s1"};
		char *a3[] = {"ntfsmac", "repair", "disk4s1"};
		char *a4[] = {"ntfsmac", "repair"};

		rc = run_priv(&c, &hk, NULL, 8, a1);
		CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_OK);
		CHECK(strstr(g_pf.line, "\"cmd\":\"repair\"") != NULL);
		CHECK(strstr(g_pf.line, "\"action\":\"clear_dirty\"") != NULL);
		CHECK(strstr(g_pf.line, "\"auth\":") != NULL);
		CHECK(strstr(c.err, "does not check or repair") != NULL);
		cap_free(&c);
		rc = run_priv(&c, &hk, NULL, 4, a2);
		CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_UNAVAILABLE);
		CHECK_EQ_INT(g_pf.acquire_calls + g_pf.xchg_calls, 0);
		cap_free(&c);
		rc = run_priv(&c, &hk, NULL, 3, a3);
		CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_USAGE);
		cap_free(&c);
		rc = run_priv(&c, &hk, NULL, 2, a4);
		CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_USAGE);
		cap_free(&c);
	}

	/*
	 * ADR-017: the specific warning is printed BEFORE the confirmation is read and before the
	 * dialog: the status reply's journal member says what is special about this entry.
	 */
	{
		char *a[] = {"ntfsmac", "journal-clear", "disk4s1"};
		char *warn;
		char *conf;

		g_status_reply = "{\"ok\":true,\"journal\":{\"marker\":\"set\",\"state\":\"write_error\","
				 "\"warning\":\"write_error\",\"left_open\":\"2026-11-16T09:30:00Z\"}}";
		in = tmpfile();
		fputs("L\n12345678\n", in);
		rewind(in);
		rc = run_priv(&c, &hk, in, 3, a);
		CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_OK);
		CHECK_EQ_INT(g_status_calls, 1);
		warn = strstr(c.err, "ended with a write error");
		conf = strstr(c.err, "Type the volume label");
		CHECK(strstr(c.err, "left the volume open on 2026-11-16T09:30:00Z") != NULL);
		CHECK(warn != NULL && conf != NULL && warn < conf); /* the warning comes first */
		CHECK(conf != NULL && strstr(c.err, "dialog") != NULL && conf < strstr(c.err, "dialog"));
		cap_free(&c);
		fclose(in);

		g_status_reply = "{\"ok\":true,\"journal\":{\"marker\":\"set\",\"state\":\"open\","
				 "\"warning\":\"lost_flush\",\"left_open\":\"2026-11-16T09:30:00Z\"}}";
		in = tmpfile();
		fputs("L\n12345678\n", in);
		rewind(in);
		rc = run_priv(&c, &hk, in, 3, a);
		CHECK(strstr(c.err, "nothing shows that this volume was checked") != NULL);
		CHECK(strstr(c.err, "nothing shows") < strstr(c.err, "Type the volume label"));
		cap_free(&c);
		fclose(in);

		/* no entry, or no answer: nothing special is said, the request goes on */
		g_status_reply = "{\"ok\":true,\"volume\":{}}";
		in = tmpfile();
		fputs("L\n12345678\n", in);
		rewind(in);
		rc = run_priv(&c, &hk, in, 3, a);
		CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_OK);
		CHECK(strstr(c.err, "left the volume open on") == NULL);
		cap_free(&c);
		fclose(in);
		g_status_reply = NULL;
		in = tmpfile();
		fputs("L\n12345678\n", in);
		rewind(in);
		rc = run_priv(&c, &hk, in, 3, a);
		CHECK_EQ_INT(rc, NTFSMAC_CLI_EXIT_OK);
		cap_free(&c);
		fclose(in);
	}

	/* the time limit helper (U17 finding 2: a prompt that never returns) */
	{
		int v = 0;

		CHECK(ntfsmac_cli_run_with_timeout(quick_fn, &v, 2000));
		CHECK_EQ_INT(v, 7);
		CHECK(!ntfsmac_cli_run_with_timeout(slow_fn, NULL, 100));
		CHECK(!ntfsmac_cli_run_with_timeout(NULL, NULL, 100));
	}
}

int main(void)
{
	test_tables();
	test_req();
	test_safe();
	test_render();
	test_end_to_end();
	test_privileged();
	if (g_failures != 0) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	return 0;
}
