/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-ARGV-01..05 and the builder properties of ARCHITECTURE.md section 3.3:
 * builder output, argv_check() acceptance, exhaustive enumeration, single
 * mutation rejection, rule 7 (descriptor mode).
 */
#include "ntfsmac/options.h"
#include "ntfsmac/policy.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "check.h"

#define WORKER "/usr/local/libexec/ntfs-macos/ntfsmac-worker"
#define LEAF "Backup Disk (2)"
#define MAXTOK 16

/* Independent copies of the lists: the test must not share code with the unit under test. */
static const char *const banned[] = {
	"recover", "remove_hiberfile", "fsname", "rdonly", "no_def_opts", "dev", "suid",
	"allow_root", "efs_raw", "big_writes", "volname", "allow_other", "nonempty",
	"default_permissions", "blkdev", "blksize", "noowners", "noappledouble",
	"noapplexattr", "nobrowse", "daemon_timeout", "iosize", "sync", "async",
	"user_xattr", "umask", "rw",
};
#define NBANNED (sizeof(banned) / sizeof(banned[0]))

static const char *const allowed[] = {
	"backend", "local", "norecover", "noatime", "windows_names", "streams_interface",
	"uid", "gid", "fmask", "dmask", "ro", "hide_hid_files", "permissions", "usermapping",
};
#define NALLOWED (sizeof(allowed) / sizeof(allowed[0]))

static ntfsmac_options_input base_input(ntfsmac_policy_decision d)
{
	ntfsmac_options_input in;

	memset(&in, 0, sizeof(in));
	in.worker_path = WORKER;
	in.device_fd = 5;
	in.leaf = LEAF;
	in.decision = d;
	in.console_uid = 501;
	in.console_gid = 20;
	return in;
}

static ntfsmac_argv_expect expect_for(const ntfsmac_options_input *in)
{
	ntfsmac_argv_expect ex;

	ex.worker_path = in->worker_path;
	ex.leaf = in->leaf;
	ex.decision = in->decision;
	ex.console_uid = in->console_uid;
	ex.console_gid = in->console_gid;
	return ex;
}

/* Bounded copy (no snprintf: gcc cannot see the 96-byte token size through the array). */
static void scopy(char *dst, size_t size, const char *src)
{
	size_t n = strlen(src);

	if (n >= size)
		n = size - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

static size_t split_tokens(const char *s, char tok[][96], size_t max)
{
	size_t n = 0;

	while (n < max) {
		const char *c = strchr(s, ',');
		size_t len = c ? (size_t)(c - s) : strlen(s);

		memcpy(tok[n], s, len);
		tok[n][len] = '\0';
		n++;
		if (!c)
			break;
		s = c + 1;
	}
	return n;
}

static int has_key(char tok[][96], size_t n, const char *key)
{
	size_t i, kl = strlen(key);

	for (i = 0; i < n; i++)
		if (strncmp(tok[i], key, kl) == 0 && (tok[i][kl] == '\0' || tok[i][kl] == '='))
			return 1;
	return 0;
}

static ntfsmac_argv_status check_vec(const ntfsmac_worker_argv *w, const char *optstr,
				     const ntfsmac_argv_expect *ex)
{
	const char *v[NTFSMAC_WORKER_ARGC];
	size_t i;

	for (i = 0; i < NTFSMAC_WORKER_ARGC; i++)
		v[i] = w->argv[i];
	v[4] = optstr;
	return ntfsmac_argv_check_text(v, NTFSMAC_WORKER_ARGC, ex);
}

static void test_golden(void)
{
	ntfsmac_worker_argv w;
	ntfsmac_options_input in = base_input(NTFSMAC_POLICY_RW);

	CHECK_EQ_INT(ntfsmac_options_build(&in, &w), NTFSMAC_OPTIONS_OK);
	CHECK(strcmp(w.optstr,
		     "backend=fskit,local,norecover,noatime,windows_names,"
		     "streams_interface=openxattr,uid=501,gid=20,fmask=0133,dmask=0022") == 0);
	CHECK(strcmp(w.argv[0], WORKER) == 0);
	CHECK(strcmp(w.argv[1], "/dev/fd/5") == 0);
	CHECK(strcmp(w.argv[2], "/Volumes/" LEAF) == 0);
	CHECK(strcmp(w.argv[3], "-o") == 0);
	CHECK(w.argv[4] == w.optstr && w.argv[5] == NULL);

	in.decision = NTFSMAC_POLICY_RO;
	in.hide_hid_files = true;
	in.usermapping = true;
	CHECK_EQ_INT(ntfsmac_options_build(&in, &w), NTFSMAC_OPTIONS_OK);
	CHECK(strcmp(w.optstr,
		     "backend=fskit,local,norecover,noatime,windows_names,"
		     "streams_interface=openxattr,uid=501,gid=20,fmask=0133,dmask=0022,"
		     "ro,hide_hid_files,usermapping=/usr/local/etc/ntfs-macos/UserMapping") == 0);

	/* permissions omits uid, gid, fmask, dmask (ADR-017 / T-M2-04). */
	in = base_input(NTFSMAC_POLICY_RW);
	in.permissions = true;
	CHECK_EQ_INT(ntfsmac_options_build(&in, &w), NTFSMAC_OPTIONS_OK);
	CHECK(strcmp(w.optstr,
		     "backend=fskit,local,norecover,noatime,windows_names,"
		     "streams_interface=openxattr,permissions") == 0);
	CHECK(strstr(w.optstr, "uid") == NULL && strstr(w.optstr, "gid") == NULL &&
	      strstr(w.optstr, "fmask") == NULL && strstr(w.optstr, "dmask") == NULL);

	/* Maximum values and zero. */
	in = base_input(NTFSMAC_POLICY_RW);
	in.console_uid = 4294967295u;
	in.console_gid = 0;
	CHECK_EQ_INT(ntfsmac_options_build(&in, &w), NTFSMAC_OPTIONS_OK);
	CHECK(strstr(w.optstr, "uid=4294967295,gid=0,") != NULL);
	{
		ntfsmac_argv_expect ex = expect_for(&in);

		CHECK_EQ_INT(check_vec(&w, w.optstr, &ex), NTFSMAC_ARGV_OK);
	}
}

static void test_errors(void)
{
	ntfsmac_worker_argv w;
	ntfsmac_options_input in;
	char longleaf[80];
	int fd;

	memset(&w, 0xAA, sizeof(w));
	CHECK_EQ_INT(ntfsmac_options_build(NULL, &w), NTFSMAC_OPTIONS_ERR_INVALID);
	in = base_input(NTFSMAC_POLICY_RW);
	CHECK_EQ_INT(ntfsmac_options_build(&in, NULL), NTFSMAC_OPTIONS_ERR_INVALID);
	in.worker_path = NULL;
	CHECK_EQ_INT(ntfsmac_options_build(&in, &w), NTFSMAC_OPTIONS_ERR_INVALID);
	in = base_input(NTFSMAC_POLICY_RW);
	in.leaf = NULL;
	CHECK_EQ_INT(ntfsmac_options_build(&in, &w), NTFSMAC_OPTIONS_ERR_INVALID);

	in = base_input(NTFSMAC_POLICY_REFUSE);
	memset(&w, 0xAA, sizeof(w));
	CHECK_EQ_INT(ntfsmac_options_build(&in, &w), NTFSMAC_OPTIONS_ERR_DECISION);
	{
		const unsigned char *p = (const unsigned char *)&w;
		size_t i;
		int zero = 1;

		for (i = 0; i < sizeof(w); i++)
			if (p[i] != 0)
				zero = 0;
		CHECK(zero); /* *out is zeroed on error */
	}
	in.decision = (ntfsmac_policy_decision)9;
	CHECK_EQ_INT(ntfsmac_options_build(&in, &w), NTFSMAC_OPTIONS_ERR_DECISION);
	in.decision = (ntfsmac_policy_decision)-1;
	CHECK_EQ_INT(ntfsmac_options_build(&in, &w), NTFSMAC_OPTIONS_ERR_DECISION);

	in = base_input(NTFSMAC_POLICY_RW);
	in.worker_path = "";
	CHECK_EQ_INT(ntfsmac_options_build(&in, &w), NTFSMAC_OPTIONS_ERR_WORKER);
	in.worker_path = "relative/worker";
	CHECK_EQ_INT(ntfsmac_options_build(&in, &w), NTFSMAC_OPTIONS_ERR_WORKER);

	for (fd = -2; fd <= 300; fd++) {
		in = base_input(NTFSMAC_POLICY_RW);
		in.device_fd = fd;
		CHECK_EQ_INT(ntfsmac_options_build(&in, &w),
			     (fd >= 3 && fd <= 255) ? NTFSMAC_OPTIONS_OK : NTFSMAC_OPTIONS_ERR_FD);
	}
	in = base_input(NTFSMAC_POLICY_RW);
	in.device_fd = 0x7fffffff;
	CHECK_EQ_INT(ntfsmac_options_build(&in, &w), NTFSMAC_OPTIONS_ERR_FD);

	in = base_input(NTFSMAC_POLICY_RW);
	in.leaf = "";
	CHECK_EQ_INT(ntfsmac_options_build(&in, &w), NTFSMAC_OPTIONS_ERR_LEAF);
	in.leaf = "a/b";
	CHECK_EQ_INT(ntfsmac_options_build(&in, &w), NTFSMAC_OPTIONS_ERR_LEAF);
	in.leaf = "/";
	CHECK_EQ_INT(ntfsmac_options_build(&in, &w), NTFSMAC_OPTIONS_ERR_LEAF);
	memset(longleaf, 'a', 67);
	longleaf[67] = '\0';
	in.leaf = longleaf;
	CHECK_EQ_INT(ntfsmac_options_build(&in, &w), NTFSMAC_OPTIONS_OK);
	longleaf[67] = 'a';
	longleaf[68] = '\0';
	CHECK_EQ_INT(ntfsmac_options_build(&in, &w), NTFSMAC_OPTIONS_ERR_LEAF);
	memset(longleaf, 'a', 79);
	longleaf[79] = '\0';
	CHECK_EQ_INT(ntfsmac_options_build(&in, &w), NTFSMAC_OPTIONS_ERR_LEAF);

	CHECK(strcmp(ntfsmac_argv_status_name(NTFSMAC_ARGV_OK), "ok") == 0);
	CHECK(ntfsmac_argv_status_name((ntfsmac_argv_status)99) == NULL);
	{
		int s;

		for (s = 0; s <= NTFSMAC_ARGV_ERR_FD_MODE; s++)
			CHECK(ntfsmac_argv_status_name((ntfsmac_argv_status)s) != NULL);
	}
}

/*
 * U-POL-NOBAN / U-ARGV-01: enumerate decision x opt-ins x ids x fds x leaves.
 * Every OK build emits only allow-list keys, never a banned key, `ro` iff RO,
 * no ownership key with `permissions`, and passes argv_check_text().
 */
static void test_builder_property(void)
{
	static const int decisions[] = { 0, 1, 2, 3, -1 };
	static const uint32_t ids[] = { 0, 1, 501, 65534, 4294967295u };
	static const int fds[] = { 2, 3, 4, 100, 255, 256 };
	static const char *const leaves[] = { "a", LEAF, "NTFS-1A2B3C4D", "Caf\xC3\xA9 Disk", "x/y", "" };
	size_t d, u, f, l;
	int hide, perm, um;
	long ok_builds = 0;

	for (d = 0; d < sizeof(decisions) / sizeof(decisions[0]); d++)
	for (hide = 0; hide < 2; hide++)
	for (perm = 0; perm < 2; perm++)
	for (um = 0; um < 2; um++)
	for (u = 0; u < sizeof(ids) / sizeof(ids[0]); u++)
	for (f = 0; f < sizeof(fds) / sizeof(fds[0]); f++)
	for (l = 0; l < sizeof(leaves) / sizeof(leaves[0]); l++) {
		ntfsmac_options_input in = base_input((ntfsmac_policy_decision)decisions[d]);
		ntfsmac_worker_argv w;
		ntfsmac_options_status st;
		char tok[MAXTOK][96];
		size_t n, i, b;
		int valid_dec = decisions[d] == 0 || decisions[d] == 1;
		int valid_fd = fds[f] >= 3 && fds[f] <= 255;
		int valid_leaf = l < 4;

		in.hide_hid_files = hide;
		in.permissions = perm;
		in.usermapping = um;
		in.console_uid = ids[u];
		in.console_gid = ids[(u + 1) % (sizeof(ids) / sizeof(ids[0]))];
		in.device_fd = fds[f];
		in.leaf = leaves[l];
		st = ntfsmac_options_build(&in, &w);
		if (!(valid_dec && valid_fd && valid_leaf)) {
			CHECK(st != NTFSMAC_OPTIONS_OK);
			continue;
		}
		CHECK_EQ_INT(st, NTFSMAC_OPTIONS_OK);
		if (st != NTFSMAC_OPTIONS_OK)
			continue;
		ok_builds++;
		CHECK(strlen(w.optstr) <= NTFSMAC_OPTSTR_MAX_BYTES);
		n = split_tokens(w.optstr, tok, MAXTOK);
		CHECK(n < MAXTOK);
		for (i = 0; i < n; i++) {
			char key[96];
			char *eq;
			int known = 0;
			size_t a;

			scopy(key, sizeof(key), tok[i]);
			eq = strchr(key, '=');
			if (eq)
				*eq = '\0';
			for (b = 0; b < NBANNED; b++)
				CHECK(strcmp(key, banned[b]) != 0);
			for (a = 0; a < NALLOWED; a++)
				if (strcmp(key, allowed[a]) == 0)
					known = 1;
			CHECK(known);
		}
		CHECK_EQ_INT(has_key(tok, n, "ro"), decisions[d] == NTFSMAC_POLICY_RO);
		CHECK_EQ_INT(has_key(tok, n, "hide_hid_files"), hide);
		CHECK_EQ_INT(has_key(tok, n, "permissions"), perm);
		CHECK_EQ_INT(has_key(tok, n, "usermapping"), um);
		CHECK_EQ_INT(has_key(tok, n, "uid"), !perm);
		CHECK_EQ_INT(has_key(tok, n, "gid"), !perm);
		CHECK_EQ_INT(has_key(tok, n, "fmask"), !perm);
		CHECK_EQ_INT(has_key(tok, n, "dmask"), !perm);
		CHECK(has_key(tok, n, "norecover") && has_key(tok, n, "backend") &&
		      has_key(tok, n, "local") && has_key(tok, n, "noatime") &&
		      has_key(tok, n, "windows_names") && has_key(tok, n, "streams_interface"));
		CHECK(strstr(w.optstr, "recover") == w.optstr + (strstr(w.optstr, "norecover") - w.optstr) + 2);
		CHECK(strstr(w.optstr, "remove_hiberfile") == NULL);
		CHECK(strstr(w.optstr, "fsname") == NULL);
		CHECK(strstr(w.optstr, leaves[l]) == NULL || strlen(leaves[l]) < 2);
		{
			ntfsmac_argv_expect ex = expect_for(&in);

			CHECK_EQ_INT(ntfsmac_argv_check_text(w.argv, NTFSMAC_WORKER_ARGC, &ex),
				     NTFSMAC_ARGV_OK);
			/* the same vector against a different decision is rejected */
			ex.decision = in.decision == NTFSMAC_POLICY_RW ? NTFSMAC_POLICY_RO
								      : NTFSMAC_POLICY_RW;
			CHECK(ntfsmac_argv_check_text(w.argv, NTFSMAC_WORKER_ARGC, &ex) !=
			      NTFSMAC_ARGV_OK);
		}
	}
	CHECK(ok_builds > 1000);
}

/* Policy to builder, over every policy input: banned keys unreachable (ADR-005). */
static void test_policy_to_builder(void)
{
	int st, mode, ev, jc, perm, hide;
	long built = 0, refused = 0;

	for (st = 0; st < (int)NTFSMAC_PROBE_STATE_COUNT_; st++)
	for (mode = 0; mode < 2; mode++)
	for (ev = 0; ev < 2; ev++)
	for (jc = 0; jc < 2; jc++)
	for (perm = 0; perm < 2; perm++)
	for (hide = 0; hide < 2; hide++) {
		ntfsmac_probe_verdict v;
		ntfsmac_journal_entry e;
		ntfsmac_policy_input pin;
		ntfsmac_policy_result r;
		ntfsmac_options_input in;
		ntfsmac_worker_argv w;
		ntfsmac_options_status s;

		memset(&v, 0, sizeof(v));
		v.state = (ntfsmac_probe_state)st;
		v.reason_codes = st == (int)NTFSMAC_PROBE_STATE_CLEAN ? 0 : 1;
		v.has_logfile_lsn = true;
		v.logfile_lsn = 9;
		v.serial = 5;
		v.total_sectors = 100;
		memset(&e, 0, sizeof(e));
		e.serial = 5;
		e.total_sectors = 100;
		e.logfile_lsn_at_start = 3;
		e.marker = NTFSMAC_JOURNAL_MARKER_SET;
		pin.verdict = &v;
		pin.journal = jc ? &e : NULL;
		pin.mode = (ntfsmac_policy_mode)mode;
		pin.evidence_rule_enabled = ev != 0;
		r = ntfsmac_policy_decide(&pin);

		in = base_input(r.decision);
		in.permissions = perm;
		in.hide_hid_files = hide;
		s = ntfsmac_options_build(&in, &w);
		if (r.decision == NTFSMAC_POLICY_REFUSE) {
			CHECK_EQ_INT(s, NTFSMAC_OPTIONS_ERR_DECISION);
			refused++;
			continue;
		}
		CHECK_EQ_INT(s, NTFSMAC_OPTIONS_OK);
		built++;
		CHECK(strstr(w.optstr, "remove_hiberfile") == NULL);
		CHECK(strstr(w.optstr, ",recover") == NULL && strstr(w.optstr, "recover") != w.optstr);
		CHECK(strstr(w.optstr, "fsname") == NULL);
		{
			char tok[MAXTOK][96];
			size_t n = split_tokens(w.optstr, tok, MAXTOK);

			CHECK_EQ_INT(has_key(tok, n, "ro"), r.decision == NTFSMAC_POLICY_RO);
			CHECK(has_key(tok, n, "norecover"));
		}
	}
	CHECK(built > 0 && refused > 0);
}

static const char *const ro_extra = ",ro";

/* Valid vectors for the mutation tests: every decision and opt-in combination. */
typedef struct variant {
	ntfsmac_options_input in;
	ntfsmac_worker_argv w;
	ntfsmac_argv_expect ex;
} variant;

static void mk_variant(variant *x, int dec, int hide, int perm, int um)
{
	memset(x, 0, sizeof(*x));
	x->in = base_input(dec ? NTFSMAC_POLICY_RO : NTFSMAC_POLICY_RW);
	x->in.hide_hid_files = hide;
	x->in.permissions = perm;
	x->in.usermapping = um;
	CHECK_EQ_INT(ntfsmac_options_build(&x->in, &x->w), NTFSMAC_OPTIONS_OK);
	x->ex = expect_for(&x->in);
}

static void join_skip(char *out, size_t size, char tok[][96], size_t n, size_t skip, size_t dup)
{
	size_t i;

	out[0] = '\0';
	for (i = 0; i < n; i++) {
		if (i == skip)
			continue;
		if (out[0])
			strncat(out, ",", size - strlen(out) - 1);
		strncat(out, tok[i], size - strlen(out) - 1);
		if (i == dup) {
			strncat(out, ",", size - strlen(out) - 1);
			strncat(out, tok[i], size - strlen(out) - 1);
		}
	}
}

static int is_mandatory(variant *x, const char *tok)
{
	static const char *const always[] = { "backend=fskit", "local", "norecover", "noatime",
					      "windows_names", "streams_interface=openxattr" };
	size_t i;

	for (i = 0; i < sizeof(always) / sizeof(always[0]); i++)
		if (strcmp(tok, always[i]) == 0)
			return 1;
	if (strcmp(tok, "ro") == 0 && x->in.decision == NTFSMAC_POLICY_RO)
		return 1;
	if (strncmp(tok, "uid=", 4) == 0 || strncmp(tok, "gid=", 4) == 0 ||
	    strncmp(tok, "fmask=", 6) == 0 || strncmp(tok, "dmask=", 6) == 0)
		return 1;
	if (strcmp(tok, "permissions") == 0)
		return 1; /* its removal leaves uid..dmask missing */
	return 0;
}

#define REJECT(ex_, opt_)                                                         \
	do {                                                                      \
		ntfsmac_argv_status s_ = check_vec(&x.w, (opt_), (ex_));          \
		if (s_ == NTFSMAC_ARGV_OK)                                        \
			fprintf(stderr, "accepted mutation: [%s]\n", (opt_));     \
		CHECK(s_ != NTFSMAC_ARGV_OK);                                     \
	} while (0)

/* U-ARGV-02 and U-ARGV-04 (text part) over every variant. */
static void test_mutations(void)
{
	static const char *const vals[] = {
		"uid=0", "uid=500", "uid=0501", "uid=+501", "uid=501a", "uid=", "uid",
		"uid=501=1", "gid=21", "gid=-20", "fmask=0644", "fmask=133", "fmask=",
		"dmask=0777", "dmask=0022=", "streams_interface=xattr", "streams_interface=windows",
		"streams_interface=", "streams_interface", "streams_interface=openxattr=1",
		"backend=fuse", "backend=FSKIT", "backend", "backend=fskit=1", "local=1", "local=",
		"norecover=1", "noatime=1", "ro=1", "hide_hid_files=1", "permissions=1",
		"usermapping", "usermapping=", "usermapping=/etc/passwd",
		"usermapping=/usr/local/etc/ntfs-macos/UserMapping2",
		"usermapping=/usr/local/etc/ntfs-macos/User Mapping",
		"usermapping=/usr/local/etc/ntfs-macos/UserMapping=1",
		"bogus", "bogus=1", "Ro", "NORECOVER", "=1", "=", "ro,", "volname=X",
		"streams_interface=none,x",
	};
	static const char inj[] = { ',', '=', '\\', ' ', '\t', '\n', '\r', 0x01, 0x1F, 0x7F, '\0' };
	int dec, hide, perm, um;
	size_t i, j, b;

	for (dec = 0; dec < 2; dec++)
	for (hide = 0; hide < 2; hide++)
	for (perm = 0; perm < 2; perm++)
	for (um = 0; um < 2; um++) {
		variant x;
		char tok[MAXTOK][96];
		char buf[2048];
		size_t n;

		mk_variant(&x, dec, hide, perm, um);
		n = split_tokens(x.w.optstr, tok, MAXTOK);
		CHECK_EQ_INT(check_vec(&x.w, x.w.optstr, &x.ex), NTFSMAC_ARGV_OK);

		/* drop each mandatory token */
		for (i = 0; i < n; i++) {
			if (!is_mandatory(&x, tok[i]))
				continue;
			join_skip(buf, sizeof(buf), tok, n, i, (size_t)-1);
			REJECT(&x.ex, buf);
		}
		/* duplicate each token */
		for (i = 0; i < n; i++) {
			join_skip(buf, sizeof(buf), tok, n, (size_t)-1, i);
			REJECT(&x.ex, buf);
		}
		/* add each banned key, bare, with a value, and in front */
		for (b = 0; b < NBANNED; b++) {
			snprintf(buf, sizeof(buf), "%s,%s", x.w.optstr, banned[b]);
			REJECT(&x.ex, buf);
			snprintf(buf, sizeof(buf), "%s,%s=1", x.w.optstr, banned[b]);
			REJECT(&x.ex, buf);
			snprintf(buf, sizeof(buf), "%s,%s", banned[b], x.w.optstr);
			REJECT(&x.ex, buf);
			snprintf(buf, sizeof(buf), "%s=%s", banned[b], x.w.optstr);
			REJECT(&x.ex, buf);
		}
		/* wrong or malformed values, replacing a token and appended */
		for (j = 0; j < sizeof(vals) / sizeof(vals[0]); j++) {
			snprintf(buf, sizeof(buf), "%s,%s", x.w.optstr, vals[j]);
			REJECT(&x.ex, buf);
			for (i = 0; i < n; i++) {
				char key[96], *eq;

				scopy(key, sizeof(key), tok[i]);
				eq = strchr(key, '=');
				if (eq)
					*eq = '\0';
				if (strncmp(vals[j], key, strlen(key)) != 0 ||
				    (vals[j][strlen(key)] != '=' && vals[j][strlen(key)] != '\0'))
					continue;
				/* same key, bad value: replace the token */
				{
					char saved[96];

					scopy(saved, sizeof(saved), tok[i]);
					scopy(tok[i], sizeof(tok[i]), vals[j]);
					join_skip(buf, sizeof(buf), tok, n, (size_t)-1, (size_t)-1);
					scopy(tok[i], sizeof(tok[i]), saved);
					REJECT(&x.ex, buf);
				}
			}
		}
		/* inject each hostile byte at the start, middle and end */
		for (j = 0; j < sizeof(inj); j++) {
			size_t len = strlen(x.w.optstr), at;

			for (at = 0; at <= len; at += (len / 4) ? (len / 4) : 1) {
				memcpy(buf, x.w.optstr, at);
				buf[at] = inj[j];
				memcpy(buf + at + 1, x.w.optstr + at, len - at + 1);
				if (inj[j] == '\0') {
					/* a NUL ends the string: the checker sees a prefix.
					 * Cut inside the mandatory part, which must fail. */
					if (at < 66)
						REJECT(&x.ex, buf);
					continue;
				}
				REJECT(&x.ex, buf);
			}
		}
		/* empty tokens */
		snprintf(buf, sizeof(buf), ",%s", x.w.optstr);
		REJECT(&x.ex, buf);
		snprintf(buf, sizeof(buf), "%s,", x.w.optstr);
		REJECT(&x.ex, buf);
		snprintf(buf, sizeof(buf), "%s,,ro", x.w.optstr);
		REJECT(&x.ex, buf);
		REJECT(&x.ex, "");
		REJECT(&x.ex, ",");
		REJECT(&x.ex, ",,");
		/* high bytes and over-length */
		snprintf(buf, sizeof(buf), "%s,x\xC3\xA9", x.w.optstr);
		REJECT(&x.ex, buf);
		memset(buf, 'a', 1025);
		buf[1025] = '\0';
		CHECK_EQ_INT(check_vec(&x.w, buf, &x.ex), NTFSMAC_ARGV_ERR_OPT_LENGTH);
		memset(buf, 'a', 1024);
		buf[1024] = '\0';
		CHECK_EQ_INT(check_vec(&x.w, buf, &x.ex), NTFSMAC_ARGV_ERR_UNKNOWN_KEY);
		/* ro flipped against the decision; permissions next to uid */
		if (dec) {
			join_skip(buf, sizeof(buf), tok, n, 0, (size_t)-1); /* sanity: some drop */
			x.ex.decision = NTFSMAC_POLICY_RW;
			REJECT(&x.ex, x.w.optstr);
			x.ex.decision = NTFSMAC_POLICY_RO;
		} else {
			snprintf(buf, sizeof(buf), "%s%s", x.w.optstr, ro_extra);
			REJECT(&x.ex, buf);
			x.ex.decision = NTFSMAC_POLICY_RO;
			REJECT(&x.ex, x.w.optstr);
			x.ex.decision = NTFSMAC_POLICY_RW;
		}
		if (!perm) {
			snprintf(buf, sizeof(buf), "%s,permissions", x.w.optstr);
			CHECK_EQ_INT(check_vec(&x.w, buf, &x.ex), NTFSMAC_ARGV_ERR_OWNERSHIP);
		} else {
			snprintf(buf, sizeof(buf), "%s,uid=501", x.w.optstr);
			CHECK_EQ_INT(check_vec(&x.w, buf, &x.ex), NTFSMAC_ARGV_ERR_OWNERSHIP);
			snprintf(buf, sizeof(buf), "%s,fmask=0133", x.w.optstr);
			CHECK_EQ_INT(check_vec(&x.w, buf, &x.ex), NTFSMAC_ARGV_ERR_OWNERSHIP);
		}
		/* specific rule codes */
		snprintf(buf, sizeof(buf), "%s,recover", x.w.optstr);
		CHECK_EQ_INT(check_vec(&x.w, buf, &x.ex), NTFSMAC_ARGV_ERR_BANNED);
		snprintf(buf, sizeof(buf), "%s,remove_hiberfile", x.w.optstr);
		CHECK_EQ_INT(check_vec(&x.w, buf, &x.ex), NTFSMAC_ARGV_ERR_BANNED);
		snprintf(buf, sizeof(buf), "%s,fsname=/dev/fd/5", x.w.optstr);
		CHECK_EQ_INT(check_vec(&x.w, buf, &x.ex), NTFSMAC_ARGV_ERR_BANNED);
		snprintf(buf, sizeof(buf), "%s,bogus", x.w.optstr);
		CHECK_EQ_INT(check_vec(&x.w, buf, &x.ex), NTFSMAC_ARGV_ERR_UNKNOWN_KEY);
		snprintf(buf, sizeof(buf), "%s,uid=501", x.w.optstr);
		CHECK(check_vec(&x.w, buf, &x.ex) != NTFSMAC_ARGV_OK);
		/* wrong console identity in the expectation */
		x.ex.console_uid++;
		if (!perm)
			CHECK_EQ_INT(check_vec(&x.w, x.w.optstr, &x.ex), NTFSMAC_ARGV_ERR_VALUE);
		x.ex.console_uid--;
		x.ex.console_gid++;
		if (!perm)
			CHECK_EQ_INT(check_vec(&x.w, x.w.optstr, &x.ex), NTFSMAC_ARGV_ERR_VALUE);
	}
}

/* U-ARGV-01 (argv layout), U-ARGV-05 and rule 1 mutations. */
static void test_argv_layout(void)
{
	variant x;
	const char *v[8];
	ntfsmac_argv_status s;
	char mp[128];
	size_t i;
	static const char *const bad_dev[] = {
		"/dev/fd/2", "/dev/fd/256", "/dev/fd/03", "/dev/fd/+5", "/dev/fd/-5", "/dev/fd/",
		"/dev/fd/5x", "/dev/fd/0x10", "/dev/fd/ 5", "/dev/fd/5 ", "/dev/fd/1000", "/dev/fd/0",
		"/dev/fd/5/6", "/dev/fd/../fd/5", "/dev/disk4", "dev/fd/5", "/dev/fd5", "/dev/fd/٥", "",
		"/dev/fd/4294967301",
	};
	static const char *const bad_mp[] = {
		"/Volumes/", "/Volumes", "Volumes/" LEAF, "/Volume/" LEAF, "/volumes/" LEAF,
		"/tmp/" LEAF, "/Volumes/" LEAF "/", "/Volumes/" LEAF "/x", "/Volumes//" LEAF,
		"/Volumes/../" LEAF, "/Volumes/.", "/Volumes/..", "/Volumes/Other", "/Volumes/" LEAF " ",
		"/Volumes/ " LEAF, "/Volumes/.hidden", "/Volumes/a,b", "/Volumes/a=b",
		"/Volumes/a\\b", "/Volumes/a:b", "", "relative",
	};
	static const char *const bad_leafs_in_expect[] = { "", "a/b", ".x", " x", "x ", "x.", "a,b" };

	mk_variant(&x, 0, 0, 0, 0);
	for (i = 0; i < NTFSMAC_WORKER_ARGC; i++)
		v[i] = x.w.argv[i];
	CHECK_EQ_INT(ntfsmac_argv_check_text(v, 5, &x.ex), NTFSMAC_ARGV_OK);

	CHECK_EQ_INT(ntfsmac_argv_check_text(NULL, 5, &x.ex), NTFSMAC_ARGV_ERR_INVALID);
	CHECK_EQ_INT(ntfsmac_argv_check_text(v, 5, NULL), NTFSMAC_ARGV_ERR_INVALID);
	{
		ntfsmac_argv_expect ex = x.ex;

		ex.worker_path = NULL;
		CHECK_EQ_INT(ntfsmac_argv_check_text(v, 5, &ex), NTFSMAC_ARGV_ERR_INVALID);
		ex = x.ex;
		ex.leaf = NULL;
		CHECK_EQ_INT(ntfsmac_argv_check_text(v, 5, &ex), NTFSMAC_ARGV_ERR_INVALID);
		ex = x.ex;
		ex.decision = NTFSMAC_POLICY_REFUSE;
		CHECK_EQ_INT(ntfsmac_argv_check_text(v, 5, &ex), NTFSMAC_ARGV_ERR_INVALID);
		ex.decision = (ntfsmac_policy_decision)17;
		CHECK_EQ_INT(ntfsmac_argv_check_text(v, 5, &ex), NTFSMAC_ARGV_ERR_INVALID);
		for (i = 0; i < sizeof(bad_leafs_in_expect) / sizeof(bad_leafs_in_expect[0]); i++) {
			ex = x.ex;
			ex.leaf = bad_leafs_in_expect[i];
			snprintf(mp, sizeof(mp), "/Volumes/%s", ex.leaf);
			v[2] = mp;
			CHECK(ntfsmac_argv_check_text(v, 5, &ex) != NTFSMAC_ARGV_OK);
		}
		v[2] = x.w.argv[2];
	}

	/* argc */
	CHECK_EQ_INT(ntfsmac_argv_check_text(v, 0, &x.ex), NTFSMAC_ARGV_ERR_ARGC);
	CHECK_EQ_INT(ntfsmac_argv_check_text(v, 4, &x.ex), NTFSMAC_ARGV_ERR_ARGC);
	v[5] = x.w.optstr;
	CHECK_EQ_INT(ntfsmac_argv_check_text(v, 6, &x.ex), NTFSMAC_ARGV_ERR_ARGC);
	v[5] = "-o";
	CHECK_EQ_INT(ntfsmac_argv_check_text(v, 6, &x.ex), NTFSMAC_ARGV_ERR_ARGC);
	CHECK_EQ_INT(ntfsmac_argv_check_text(v, (size_t)-1, &x.ex), NTFSMAC_ARGV_ERR_ARGC);
	for (i = 0; i < 5; i++) {
		const char *save = v[i];

		v[i] = NULL;
		CHECK(ntfsmac_argv_check_text(v, 5, &x.ex) != NTFSMAC_ARGV_OK);
		v[i] = save;
	}

	/* reorder every pair of elements */
	{
		size_t a, b;

		for (a = 0; a < 5; a++)
			for (b = a + 1; b < 5; b++) {
				const char *t = v[a];

				v[a] = v[b];
				v[b] = t;
				CHECK(ntfsmac_argv_check_text(v, 5, &x.ex) != NTFSMAC_ARGV_OK);
				t = v[a];
				v[a] = v[b];
				v[b] = t;
			}
	}
	CHECK_EQ_INT(ntfsmac_argv_check_text(v, 5, &x.ex), NTFSMAC_ARGV_OK);

	v[0] = "/usr/bin/ntfs-3g";
	CHECK_EQ_INT(ntfsmac_argv_check_text(v, 5, &x.ex), NTFSMAC_ARGV_ERR_WORKER_PATH);
	v[0] = WORKER " ";
	CHECK_EQ_INT(ntfsmac_argv_check_text(v, 5, &x.ex), NTFSMAC_ARGV_ERR_WORKER_PATH);
	v[0] = x.w.argv[0];

	for (i = 0; i < sizeof(bad_dev) / sizeof(bad_dev[0]); i++) {
		v[1] = bad_dev[i];
		CHECK_EQ_INT(ntfsmac_argv_check_text(v, 5, &x.ex), NTFSMAC_ARGV_ERR_DEVICE);
	}
	{
		static const char *const good_dev[] = { "/dev/fd/3", "/dev/fd/9", "/dev/fd/10",
							"/dev/fd/99", "/dev/fd/100", "/dev/fd/255" };

		for (i = 0; i < sizeof(good_dev) / sizeof(good_dev[0]); i++) {
			v[1] = good_dev[i];
			CHECK_EQ_INT(ntfsmac_argv_check_text(v, 5, &x.ex), NTFSMAC_ARGV_OK);
		}
	}
	v[1] = x.w.argv[1];

	for (i = 0; i < sizeof(bad_mp) / sizeof(bad_mp[0]); i++) {
		v[2] = bad_mp[i];
		s = ntfsmac_argv_check_text(v, 5, &x.ex);
		CHECK(s == NTFSMAC_ARGV_ERR_MOUNTPOINT || s == NTFSMAC_ARGV_ERR_LEAF);
	}
	v[2] = "/Volumes/" LEAF "/";
	CHECK_EQ_INT(ntfsmac_argv_check_text(v, 5, &x.ex), NTFSMAC_ARGV_ERR_LEAF);
	v[2] = "/tmp/" LEAF;
	CHECK_EQ_INT(ntfsmac_argv_check_text(v, 5, &x.ex), NTFSMAC_ARGV_ERR_MOUNTPOINT);
	v[2] = "/Volumes/Other";
	CHECK_EQ_INT(ntfsmac_argv_check_text(v, 5, &x.ex), NTFSMAC_ARGV_ERR_LEAF);
	v[2] = x.w.argv[2];

	{
		static const char *const bad_o[] = { "-O", "-o ", "--o", "-oro", "o", "", "-p" };

		for (i = 0; i < sizeof(bad_o) / sizeof(bad_o[0]); i++) {
			v[3] = bad_o[i];
			CHECK_EQ_INT(ntfsmac_argv_check_text(v, 5, &x.ex), NTFSMAC_ARGV_ERR_DASH_O);
		}
		v[3] = "-o";
	}

	/* The leaf of a valid label is accepted, UTF-8 included; hostile bytes are not. */
	{
		static const char *const good_leaf[] = { "a", "Caf\xC3\xA9", "My Disk (1)+x_y-z.v",
							 "\xE2\x82\xAC uro", "NTFS-DEADBEEF" };
		static const char *const bad_leaf[] = { "a,b", "a=b", "a\\b", "a\001b", "a\x7f",
							"a:b", "a\"b", "a'b", "\xC3", "\xC0\x80", "\xED\xA0\x80",
							"a\xF4\x90\x80\x80", "a;b", "a|b", "a*b", "a?b", "a~b", "a$b" };
		ntfsmac_argv_expect ex = x.ex;

		for (i = 0; i < sizeof(good_leaf) / sizeof(good_leaf[0]); i++) {
			ntfsmac_options_input in = x.in;
			ntfsmac_worker_argv w;

			in.leaf = good_leaf[i];
			ex.leaf = good_leaf[i];
			CHECK_EQ_INT(ntfsmac_options_build(&in, &w), NTFSMAC_OPTIONS_OK);
			CHECK_EQ_INT(ntfsmac_argv_check_text(w.argv, 5, &ex), NTFSMAC_ARGV_OK);
		}
		for (i = 0; i < sizeof(bad_leaf) / sizeof(bad_leaf[0]); i++) {
			ntfsmac_options_input in = x.in;
			ntfsmac_worker_argv w;

			in.leaf = bad_leaf[i];
			ex.leaf = bad_leaf[i];
			if (ntfsmac_options_build(&in, &w) == NTFSMAC_OPTIONS_OK)
				CHECK_EQ_INT(ntfsmac_argv_check_text(w.argv, 5, &ex), NTFSMAC_ARGV_ERR_LEAF);
		}
	}
}

/* U-ARGV-03: rule 7. */
static void test_fd_mode(void)
{
	int rw = open("/dev/null", O_RDWR);
	int ro = open("/dev/null", O_RDONLY);
	int wo = open("/dev/null", O_WRONLY);
	int closed = open("/dev/null", O_RDONLY);
	int dec;

	CHECK(rw >= 3 && rw <= 255 && ro >= 3 && ro <= 255 && wo >= 3 && wo <= 255 &&
	      closed >= 3 && closed <= 255);
	if (rw < 3 || ro < 3 || wo < 3 || closed < 3 || rw > 255 || ro > 255 || wo > 255 || closed > 255)
		return;
	close(closed);

	for (dec = 0; dec < 2; dec++) {
		int fds[4] = { rw, ro, wo, closed };
		int i;

		for (i = 0; i < 4; i++) {
			ntfsmac_options_input in = base_input(dec ? NTFSMAC_POLICY_RO : NTFSMAC_POLICY_RW);
			ntfsmac_worker_argv w;
			ntfsmac_argv_expect ex;
			ntfsmac_argv_status s;
			int want_ok = (dec == 0 && i == 0) || (dec == 1 && i == 1);

			in.device_fd = fds[i];
			CHECK_EQ_INT(ntfsmac_options_build(&in, &w), NTFSMAC_OPTIONS_OK);
			ex = expect_for(&in);
			CHECK_EQ_INT(ntfsmac_argv_check_text(w.argv, 5, &ex), NTFSMAC_ARGV_OK);
			s = ntfsmac_argv_check(w.argv, 5, &ex);
			CHECK_EQ_INT(s, want_ok ? NTFSMAC_ARGV_OK : NTFSMAC_ARGV_ERR_FD_MODE);
		}
	}
	/* Text failures come first and are not masked by rule 7. */
	{
		ntfsmac_options_input in = base_input(NTFSMAC_POLICY_RW);
		ntfsmac_worker_argv w;
		ntfsmac_argv_expect ex;
		const char *v[5];
		size_t i;

		in.device_fd = rw;
		CHECK_EQ_INT(ntfsmac_options_build(&in, &w), NTFSMAC_OPTIONS_OK);
		ex = expect_for(&in);
		for (i = 0; i < 5; i++)
			v[i] = w.argv[i];
		v[4] = "backend=fskit";
		CHECK_EQ_INT(ntfsmac_argv_check(v, 5, &ex), NTFSMAC_ARGV_ERR_MISSING);
		CHECK_EQ_INT(ntfsmac_argv_check(v, 4, &ex), NTFSMAC_ARGV_ERR_ARGC);
	}
	close(rw);
	close(ro);
	close(wo);
}

int main(void)
{
	test_golden();
	test_errors();
	test_builder_property();
	test_policy_to_builder();
	test_mutations();
	test_argv_layout();
	test_fd_mode();
	if (g_failures != 0) {
		fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	printf("options: all checks passed\n");
	return 0;
}
