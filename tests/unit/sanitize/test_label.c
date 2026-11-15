/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-LABEL-01..08 (docs/TEST_STRATEGY.md section 4) and the sanitiser-to-argv_check()
 * property of F-SANITIZE-02 on fixed inputs. The expected values are written here from
 * the ADR-016 table; the allow-list oracle (oracle.h) shares no code with src/sanitize/.
 *
 * ADR-016 as amended: a result in which no allowed original code point survived (only `_`
 * that replaced something, or nothing) is the fallback, so "all dots" and "all disallowed"
 * give `NTFS-<serial>`; spaces other than U+0020, Default_Ignorable_Code_Points, Cf, and a
 * combining mark at the start of a part are replaced by `_`.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include "ntfsmac/options.h"
#include "sanitize.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "oracle.h"

#define SERIAL UINT64_C(0x1A2B3C4D5E6F7788)
#define FALLBACK "NTFS-5E6F7788"

/* Sanitise bytes; the output must satisfy every ADR-016 property and equal the reference. */
static void san(const char *in, size_t len, char *out)
{
	size_t n = ntfsmac_label_sanitize(in, len, SERIAL, out, NTFSMAC_LABEL_PART_SIZE);
	char ref[NTFSMAC_LABEL_PART_SIZE + 16];
	const char *bad;

	CHECK(n > 0);
	CHECK_EQ_INT(strlen(out), n);
	bad = oracle_label_violation(out, n, NTFSMAC_LABEL_PART_MAX_BYTES);
	if (bad != NULL) {
		fprintf(stderr, "FAIL: label of %zu bytes -> '%s': %s\n", len, out, bad);
		g_failures++;
	}
	CHECK(ntfsmac_label_part_ok(out, n));
	(void)oracle_label_sanitize(in, len, SERIAL, ref);
	if (strcmp(ref, out) != 0) {
		fprintf(stderr, "FAIL: label of %zu bytes -> '%s', reference '%s'\n", len, out, ref);
		g_failures++;
	}
}

static void expect(const char *in, const char *want)
{
	char out[NTFSMAC_LABEL_PART_SIZE];

	san(in, strlen(in), out);
	if (strcmp(out, want) != 0) {
		fprintf(stderr, "FAIL: '%s' -> '%s', want '%s'\n", in, out, want);
		g_failures++;
	}
}

static void expect_n(const char *in, size_t len, const char *want)
{
	char out[NTFSMAC_LABEL_PART_SIZE];

	san(in, len, out);
	if (strcmp(out, want) != 0) {
		fprintf(stderr, "FAIL: %zu bytes -> '%s', want '%s'\n", len, out, want);
		g_failures++;
	}
}

static void u_label_01(void)
{
	int c;

	for (c = 0; c < 0x80; c++) {
		char in = (char)c, out[NTFSMAC_LABEL_PART_SIZE], want[2] = { (char)c, 0 };
		size_t i, n;

		san(&in, 1, out);
		n = strlen(out);
		/* a space, a dot and every disallowed byte leave nothing that survived: the fallback */
		if (c == ' ' || c == '.' || !oracle_cp_allowed((uint32_t)c)) {
			CHECK(strcmp(out, FALLBACK) == 0);
			continue;
		}
		CHECK(strcmp(out, want) == 0);
		for (i = 0; i < n; i++)
			CHECK(strchr(",=\\/:", out[i]) == NULL && (unsigned char)out[i] >= 0x20 &&
			      out[i] != 0x7F);
	}
	/* The same bytes inside a longer label (NUL included: the length is explicit). */
	for (c = 0; c < 0x80; c++) {
		char in[3] = { 'a', (char)c, 'b' }, out[NTFSMAC_LABEL_PART_SIZE];

		san(in, 3, out);
		CHECK(strlen(out) == 3 && out[0] == 'a' && out[2] == 'b');
		CHECK(oracle_cp_allowed((uint32_t)c) ? out[1] == (char)c : out[1] == '_');
	}
	expect_n("a\0b", 3, "a_b");
}

static void u_label_02(void)
{
	expect("X,remove_hiberfile", "X_remove_hiberfile");
	expect("X,allow_other", "X_allow_other");
	expect("X=1", "X_1");
	expect("a\\,b", "a__b");
	expect("Backup,2026", "Backup_2026");
	expect("a,b=c\\d/e:f\"g'h", "a_b_c_d_e_f_g_h");
}

static void u_label_03(void)
{
	expect(".", FALLBACK);
	expect("..", FALLBACK);
	expect("../../etc/x", "_._.._etc_x");
	expect(".hidden", "_hidden");
	expect(" . ", FALLBACK);
	expect("  x  ", "x");
	expect("name...", "name");
	expect("name. . .", "name");
	expect("a .", "a");
	expect(" .a", "_a");
	expect("/", FALLBACK);
	expect("/etc/passwd", "_etc_passwd");
	expect("DATA", "DATA");
	expect("My Disk (2)+", "My Disk (2)+");
}

static void u_label_04(void)
{
	/* overlong "/" (C0 AF), lone continuation bytes, surrogates, 5- and 6-byte forms,
	 * truncated sequences, code points above U+10FFFF, bytes that never occur */
	static const char *const bad[] = {
		"\xC0\xAF", "\xC1\xBF", "\x80", "\xBF", "\xED\xA0\x80", "\xED\xBF\xBF",
		"\xF8\x88\x80\x80\x80", "\xFC\x84\x80\x80\x80\x80", "\xE2\x82", "\xF0\x9F\x98",
		"\xF4\x90\x80\x80", "\xF5\x80\x80\x80", "\xFF", "\xFE", "\xE0\x80\xAF",
		"\xF0\x80\x80\xAF", "abc\xFF", "\xC2", "\xE2\x82\xAC\xE2",
	};
	size_t i;

	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
		expect(bad[i], FALLBACK);
	/* Valid text before an invalid byte does not survive. */
	expect("good name\xFF", FALLBACK);
}

static void u_label_05(void)
{
	uint32_t cp;

	/* Every code point of Unicode inside "x" ... "x": output equals the oracle. */
	for (cp = 0; cp <= 0x10FFFF; cp++) {
		unsigned char in[6], out_cp[4];
		char out[NTFSMAC_LABEL_PART_SIZE];
		size_t n, m, k;

		if (cp >= 0xD800 && cp <= 0xDFFF)
			continue;
		in[0] = 'x';
		n = oracle_encode(cp, in + 1);
		in[1 + n] = 'x';
		san((const char *)in, n + 2, out);
		m = strlen(out);
		if (oracle_cp_allowed(cp)) {
			k = oracle_encode(cp, out_cp);
			/* leading dot or space cannot occur: the label starts with 'x' */
			if (m != n + 2 || memcmp(out + 1, out_cp, k) != 0) {
				fprintf(stderr, "FAIL: U+%04X allowed but changed\n", (unsigned)cp);
				g_failures++;
			}
		} else if (m != 3 || out[1] != '_') {
			fprintf(stderr, "FAIL: U+%04X not replaced by '_'\n", (unsigned)cp);
			g_failures++;
		}
	}
	/* The named classes of the ADR, spot checks of the table. */
	expect("a\xC2\x85\x62", "a_b");                    /* C1 NEL */
	expect("a\xE2\x80\xAE\x62", "a_b");                /* RLO */
	expect("a\xE2\x80\x8B\x62", "a_b");                /* ZWSP */
	expect("a\xEF\xBB\xBF\x62", "a_b");                /* BOM */
	expect("a\xE2\x80\xA8\x62", "a_b");                /* LS */
	expect("a\xEE\x80\x80\x62", "a_b");                /* private use */
	expect("a\xF3\xB0\x80\x80\x62", "a_b");            /* plane 15 */
	expect("a\xEF\xB7\x90\x62", "a_b");                /* U+FDD0 */
	expect("a\xEF\xBF\xBF\x62", "a_b");                /* U+FFFF */
	expect("a\xE2\x81\x84\x62", "a_b");                /* U+2044 */
	expect("a\xE2\x88\x95\x62", "a_b");                /* U+2215 */
	expect("a\xE2\xA7\xB8\x62", "a_b");                /* U+29F8 */
	expect("a\xEF\xBC\x8F\x62", "a_b");                /* U+FF0F */
	expect("a\xEF\xBC\x9A\x62", "a_b");                /* U+FF1A */
	expect("a\xC2\xA0\x62", "a_b");                    /* NBSP (Zs) */
	expect("a\xE3\x80\x80\x62", "a_b");                /* U+3000 (Zs) */
	expect("a\xE1\x85\x9F\x62", "a_b");                /* U+115F Hangul filler */
	expect("a\xE3\x85\xA4\x62", "a_b");                /* U+3164 Hangul filler */
	expect("a\xEF\xBE\xA0\x62", "a_b");                /* U+FFA0 Hangul filler */
	expect("a\xCD\x8F\x62", "a_b");                    /* U+034F, default ignorable */
	expect("a\xEF\xB8\x8F\x62", "a_b");                /* U+FE0F variation selector */
	expect("a\xCD\xB8\x62", "a_b");                    /* U+0378, unassigned in 15.1 */
	expect("a\xF0\x9F\xAB\xA9\x62", "a_b");            /* U+1FAE9, assigned after 15.1 */
	expect("a\xCC\x81", "a\xCC\x81");                  /* a combining mark inside stays */
	expect("\xCC\x81" "abc", "_abc");                  /* a combining mark first */
	expect("  \xCC\x81" "abc", "_abc");                /* ... also after leading spaces */
	expect("\xCC\x81", FALLBACK);
	expect("\xE0\xA4\xBE" "x", "_x");                  /* U+093E, spacing mark (Mc) first */
	expect(".\xCC\x81", "_\xCC\x81");                 /* the dot is the one that is replaced */
	expect("caf\xC3\xA9", "caf\xC3\xA9");              /* allowed non-ASCII stays */
	expect("\xD0\x94\xD0\xB0\xD0\xBD\xD0\xBD\xD1\x8B\xD0\xB5", "\xD0\x94\xD0\xB0\xD0\xBD\xD0\xBD\xD1\x8B\xD0\xB5");
	expect("\xF0\x9F\x98\x80", "\xF0\x9F\x98\x80");    /* U+1F600 */
}

static void fill(char *buf, size_t units, uint32_t cp, size_t *len)
{
	unsigned char u[4];
	size_t n = oracle_encode(cp, u), i, k, pos = 0;
	size_t per = cp >= 0x10000 ? 2 : 1; /* UTF-16 units per character */

	for (i = 0; i + per <= units; i += per)
		for (k = 0; k < n; k++)
			buf[pos++] = (char)u[k];
	*len = pos;
}

static void u_label_06(void)
{
	static const uint32_t cps[] = { 'a', 0x00E9, 0x4E2D, 0x1F600 };
	static const size_t want[] = { 64, 64, 63, 64 };
	size_t i;

	for (i = 0; i < 4; i++) {
		char in[600], out[NTFSMAC_LABEL_PART_SIZE];
		size_t len, n;

		fill(in, 128, cps[i], &len);
		n = ntfsmac_label_sanitize(in, len, SERIAL, out, sizeof(out));
		CHECK(n == want[i]);
		CHECK(ntfsmac_utf8_valid((const unsigned char *)out, n));
		CHECK(oracle_label_violation(out, n, 64) == NULL);
	}
	/* Cut on a boundary: 63 ASCII bytes then a 2-byte character leaves 63. */
	{
		char in[80], out[NTFSMAC_LABEL_PART_SIZE];

		memset(in, 'a', 63);
		in[63] = (char)0xC3;
		in[64] = (char)0xA9;
		CHECK(ntfsmac_label_sanitize(in, 65, SERIAL, out, sizeof(out)) == 63);
		/* exactly 64 bytes fit */
		memset(in, 'a', 62);
		in[62] = (char)0xC3;
		in[63] = (char)0xA9;
		in[64] = 'z';
		CHECK(ntfsmac_label_sanitize(in, 65, SERIAL, out, sizeof(out)) == 64);
		CHECK(memcmp(out + 62, "\xC3\xA9", 2) == 0);
		/* trailing spaces left by the cut are removed */
		memset(in, 'a', 60);
		memset(in + 60, ' ', 10);
		CHECK(ntfsmac_label_sanitize(in, 70, SERIAL, out, sizeof(out)) == 60);
	}
	/* Very long input is cut, not rejected. */
	{
		char *big = malloc(1u << 20);
		char out[NTFSMAC_LABEL_PART_SIZE];

		CHECK(big != NULL);
		memset(big, 'q', 1u << 20);
		CHECK(ntfsmac_label_sanitize(big, 1u << 20, SERIAL, out, sizeof(out)) == 64);
		free(big);
	}
}

static void u_label_07(void)
{
	expect("", FALLBACK);
	expect(" ", FALLBACK);
	expect("      ", FALLBACK);
	/* nothing but replacements: no allowed original code point survived */
	expect("...", FALLBACK);
	expect(",,,", FALLBACK);
	expect("\x01\x02", FALLBACK);
	expect(".,.", FALLBACK);
	expect(" ,, ", FALLBACK);
	expect("\xE2\x80\xAE\xEF\xBC\x8F", FALLBACK);
	/* one allowed original code point is enough */
	expect("_", "_");
	expect(",_", "__");
	expect(",a", "_a");
	expect("a,", "a_");
	expect("-,", "-_");
	{
		char out[NTFSMAC_LABEL_PART_SIZE];

		/* Fallback digits come from the integer: upper case, low 32 bits. */
		CHECK(ntfsmac_label_sanitize("", 0, 0, out, sizeof(out)) == 13);
		CHECK(strcmp(out, "NTFS-00000000") == 0);
		CHECK(ntfsmac_label_sanitize("", 0, UINT64_C(0xFFFFFFFFABCDEF01), out, sizeof(out)) == 13);
		CHECK(strcmp(out, "NTFS-ABCDEF01") == 0);
	}
	/* Fail closed on a bad buffer: 0 and an empty string, never a partial name. */
	{
		char out[NTFSMAC_LABEL_PART_SIZE - 1];

		memset(out, 'x', sizeof(out));
		CHECK(ntfsmac_label_sanitize("abc", 3, SERIAL, out, sizeof(out)) == 0);
		CHECK(out[0] == '\0');
		CHECK(ntfsmac_label_sanitize("abc", 3, SERIAL, NULL, 100) == 0);
		CHECK(ntfsmac_label_sanitize(NULL, 3, SERIAL, out, sizeof(out)) == 0);
	}
}

static void rm_leaf(int dfd, const char *name)
{
	(void)unlinkat(dfd, name, AT_REMOVEDIR);
	(void)unlinkat(dfd, name, 0);
}

static void u_label_08(void)
{
	const char *tmp = getenv("TMPDIR");
	char dir[512], leaf[NTFSMAC_LEAF_SIZE], want[NTFSMAC_LEAF_SIZE];
	int dfd, n, i;
	dev_t dev = 0;
	ino_t ino = 0;
	struct stat st;
	char names[100][NTFSMAC_LEAF_SIZE];
	size_t nn = 0;

	if (tmp == NULL || tmp[0] == '\0')
		tmp = "/tmp";
	n = snprintf(dir, sizeof(dir), "%s/ntfsmac-leaf-XXXXXX", tmp);
	CHECK(n > 0 && (size_t)n < sizeof(dir));
	CHECK(mkdtemp(dir) != NULL);
	dfd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	CHECK(dfd >= 0);

	/* Free name: used as is, device and inode of the created directory returned. */
	CHECK(ntfsmac_leaf_create(dfd, "DATA", 0755, leaf, sizeof(leaf), &dev, &ino) == 0);
	CHECK(strcmp(leaf, "DATA") == 0);
	snprintf(names[nn++], NTFSMAC_LEAF_SIZE, "DATA");
	CHECK(fstatat(dfd, "DATA", &st, AT_SYMLINK_NOFOLLOW) == 0);
	CHECK(S_ISDIR(st.st_mode) && st.st_dev == dev && st.st_ino == ino);

	/* DATA exists: DATA 2, then DATA 3 ... DATA 99. */
	for (i = 2; i <= 99; i++) {
		snprintf(want, sizeof(want), "DATA %d", i);
		CHECK(ntfsmac_leaf_create(dfd, "DATA", 0755, leaf, sizeof(leaf), NULL, NULL) == 0);
		if (strcmp(leaf, want) != 0) {
			fprintf(stderr, "FAIL: collision %d gave '%s', want '%s'\n", i, leaf, want);
			g_failures++;
		}
		CHECK(strlen(leaf) <= NTFSMAC_LEAF_MAX_BYTES);
		snprintf(names[nn++], NTFSMAC_LEAF_SIZE, "%s", leaf);
	}
	/* After 99: refusal, nothing created. */
	errno = 0;
	CHECK(ntfsmac_leaf_create(dfd, "DATA", 0755, leaf, sizeof(leaf), NULL, NULL) == -1);
	CHECK(errno == EEXIST);
	CHECK(leaf[0] == '\0');
	CHECK(fstatat(dfd, "DATA 100", &st, AT_SYMLINK_NOFOLLOW) != 0);

	/* A pre-existing symlink (dangling and pointing at a directory) is never followed. */
	{
		char target[600];

		CHECK(symlinkat("nowhere", dfd, "LINK") == 0);
		CHECK(ntfsmac_leaf_create(dfd, "LINK", 0755, leaf, sizeof(leaf), NULL, NULL) == 0);
		CHECK(strcmp(leaf, "LINK 2") == 0);
		CHECK(fstatat(dfd, "nowhere", &st, AT_SYMLINK_NOFOLLOW) != 0); /* target not created */
		CHECK(fstatat(dfd, "LINK", &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(st.st_mode));
		rm_leaf(dfd, "LINK 2");

		snprintf(target, sizeof(target), "%s", dir);
		CHECK(symlinkat(target, dfd, "LINK") != 0 && errno == EEXIST);
		CHECK(unlinkat(dfd, "LINK", 0) == 0);
		CHECK(symlinkat(target, dfd, "LINK") == 0); /* now a link to this directory */
		CHECK(ntfsmac_leaf_create(dfd, "LINK", 0755, leaf, sizeof(leaf), NULL, NULL) == 0);
		CHECK(strcmp(leaf, "LINK 2") == 0);
		rm_leaf(dfd, "LINK 2");
		rm_leaf(dfd, "LINK");
	}
	/* A pre-existing regular file is not reused either. */
	{
		int fd = openat(dfd, "FILE", O_WRONLY | O_CREAT | O_EXCL, 0600);

		CHECK(fd >= 0);
		close(fd);
		CHECK(ntfsmac_leaf_create(dfd, "FILE", 0755, leaf, sizeof(leaf), NULL, NULL) == 0);
		CHECK(strcmp(leaf, "FILE 2") == 0);
		rm_leaf(dfd, "FILE 2");
		rm_leaf(dfd, "FILE");
	}

	/* Arguments outside the contract. */
	errno = 0;
	CHECK(ntfsmac_leaf_create(dfd, "", 0755, leaf, sizeof(leaf), NULL, NULL) == -1 && errno == EINVAL);
	errno = 0;
	CHECK(ntfsmac_leaf_create(dfd, "a/b", 0755, leaf, sizeof(leaf), NULL, NULL) == -1 && errno == EINVAL);
	errno = 0;
	CHECK(ntfsmac_leaf_create(dfd, "..", 0755, leaf, sizeof(leaf), NULL, NULL) == -1 && errno == EINVAL);
	errno = 0;
	CHECK(ntfsmac_leaf_create(dfd, "x", 0755, leaf, NTFSMAC_LEAF_SIZE - 1, NULL, NULL) == -1 && errno == EINVAL);
	{
		char b65[66];

		memset(b65, 'a', 65);
		b65[65] = '\0';
		errno = 0;
		CHECK(ntfsmac_leaf_create(dfd, b65, 0755, leaf, sizeof(leaf), NULL, NULL) == -1 && errno == EINVAL);
	}
	/* A base that is not a label part is refused: metacharacters, look-alikes, edge dots and spaces. */
	{
		static const char *const bad_base[] = {
			"a,b", ".x", "x.", " x", "x ", "a\xE2\x80\xAE\x62", "a\xEF\xBC\x8F\x62", "\xCC\x81x",
			"a\\b", "a:b", "a=b", "a\xC0\xAF",
		};
		size_t k;

		for (k = 0; k < sizeof(bad_base) / sizeof(bad_base[0]); k++) {
			errno = 0;
			if (ntfsmac_leaf_create(dfd, bad_base[k], 0755, leaf, sizeof(leaf), NULL, NULL) != -1 ||
			    errno != EINVAL) {
				fprintf(stderr, "FAIL: leaf_create accepted the base '%s'\n", bad_base[k]);
				g_failures++;
			}
		}
	}
	/* A 64-byte base with the longest suffix is 67 bytes. */
	{
		char b64[65];

		memset(b64, 'a', 64);
		b64[64] = '\0';
		CHECK(ntfsmac_leaf_create(dfd, b64, 0755, leaf, sizeof(leaf), NULL, NULL) == 0);
		CHECK(ntfsmac_leaf_create(dfd, b64, 0755, leaf, sizeof(leaf), NULL, NULL) == 0);
		CHECK(strlen(leaf) == 66 && memcmp(leaf + 64, " 2", 3) == 0);
		rm_leaf(dfd, b64);
		rm_leaf(dfd, leaf);
	}

	/*
	 * An NFC name with an existing NFD directory of the same name (U+0065 U+0301 against
	 * U+00E9). APFS compares names by normalisation, so the second mkdirat gets EEXIST
	 * and the leaf gets a suffix, never the existing directory. A file system that does
	 * not normalise (Linux ext4, tmpfs) sees two different names: skip the assertion
	 * there, but still check that the existing directory is untouched.
	 */
	{
		CHECK(mkdirat(dfd, "caf\x65\xCC\x81", 0755) == 0);
		CHECK(ntfsmac_leaf_create(dfd, "caf\xC3\xA9", 0755, leaf, sizeof(leaf), NULL, NULL) == 0);
#if defined(__APPLE__)
		CHECK(strcmp(leaf, "caf\xC3\xA9 2") == 0);
#else
		CHECK(strcmp(leaf, "caf\xC3\xA9") == 0 || strcmp(leaf, "caf\xC3\xA9 2") == 0);
#endif
		rm_leaf(dfd, leaf);
		rm_leaf(dfd, "caf\x65\xCC\x81");
	}

	for (i = 0; (size_t)i < nn; i++)
		rm_leaf(dfd, names[i]);
	close(dfd);
	CHECK(rmdir(dir) == 0);
}

/*
 * F-SANITIZE-02 on fixed inputs: the sanitised label becomes the leaf of the builder's
 * vector and ntfsmac_argv_check_text() accepts it (with and without a collision suffix).
 */
static void leaf_into_argv_check(const char *in, size_t len, const char *suffix)
{
	char part[NTFSMAC_LABEL_PART_SIZE], leaf[NTFSMAC_LEAF_SIZE];
	ntfsmac_options_input oi;
	ntfsmac_worker_argv w;
	ntfsmac_argv_expect ex;
	size_t n = ntfsmac_label_sanitize(in, len, SERIAL, part, sizeof(part));

	CHECK(n > 0);
	snprintf(leaf, sizeof(leaf), "%s%s", part, suffix);
	memset(&oi, 0, sizeof(oi));
	oi.worker_path = "/usr/local/libexec/ntfs-macos/ntfsmac-worker";
	oi.device_fd = 5;
	oi.leaf = leaf;
	oi.decision = NTFSMAC_POLICY_RO;
	oi.console_uid = 501;
	oi.console_gid = 20;
	CHECK(ntfsmac_options_build(&oi, &w) == NTFSMAC_OPTIONS_OK);
	ex.worker_path = oi.worker_path;
	ex.leaf = leaf;
	ex.decision = oi.decision;
	ex.console_uid = oi.console_uid;
	ex.console_gid = oi.console_gid;
	if (ntfsmac_argv_check_text(w.argv, NTFSMAC_WORKER_ARGC, &ex) != NTFSMAC_ARGV_OK) {
		fprintf(stderr, "FAIL: argv_check rejects the leaf of label '%.*s'\n", (int)len, in);
		g_failures++;
	}
}

static void u_label_argv(void)
{
	static const char *const labels[] = {
		"DATA", "X,remove_hiberfile", ".hidden", "../../etc/x", "a\\,b", "caf\xC3\xA9",
		"\xC0\xAF", "", "   ", "\xF0\x9F\x98\x80 x", "x\xEF\xBC\x8Fy", "a\001b", "Backup,2026",
	};
	size_t i;

	for (i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
		leaf_into_argv_check(labels[i], strlen(labels[i]), "");
		leaf_into_argv_check(labels[i], strlen(labels[i]), " 2");
		leaf_into_argv_check(labels[i], strlen(labels[i]), " 99");
	}
	{
		char big[300];

		memset(big, 'z', sizeof(big));
		leaf_into_argv_check(big, sizeof(big), " 99");
	}
}

/* The shared allow-list against the independent oracle, for every code point. */
static void cp_exhaustive(void)
{
	uint32_t cp;
	unsigned long allowed = 0, marks = 0;

	for (cp = 0; cp <= 0x10FFFF; cp++) {
		bool a = ntfsmac_label_cp_allowed(cp), o = oracle_cp_allowed(cp);

		if (a != o) {
			fprintf(stderr, "FAIL: U+%04X allowed %d, oracle %d\n", (unsigned)cp, a, o);
			g_failures++;
		}
		if (cp >= 0x80 && ntfsmac_label_cp_is_mark(cp) != oracle_is_mark(cp)) {
			fprintf(stderr, "FAIL: U+%04X mark %d, oracle %d\n", (unsigned)cp,
				ntfsmac_label_cp_is_mark(cp), oracle_is_mark(cp));
			g_failures++;
		}
		allowed += a;
		marks += cp >= 0x80 && ntfsmac_label_cp_is_mark(cp);
	}
	CHECK(!ntfsmac_label_cp_allowed(0x110000) && !ntfsmac_label_cp_allowed(0xFFFFFFFFu));
	CHECK(!ntfsmac_label_cp_is_mark('a') && !ntfsmac_label_cp_is_mark(0x110000));
	CHECK(allowed > 100000 && marks > 1000); /* the tables are not empty */
	printf("test_label: %lu allowed code points, %lu of them combining marks\n", allowed, marks);
}

/* Rule 2 of argv_check(): the status for a vector whose leaf is `leaf`. */
static ntfsmac_argv_status leaf_status(const char *leaf)
{
	ntfsmac_argv_expect ex;
	char mp[256];
	const char *argv[NTFSMAC_WORKER_ARGC];

	snprintf(mp, sizeof(mp), "/Volumes/%s", leaf);
	ex.worker_path = "/usr/local/libexec/ntfs-macos/ntfsmac-worker";
	ex.leaf = leaf;
	ex.decision = NTFSMAC_POLICY_RO;
	ex.console_uid = 501;
	ex.console_gid = 20;
	argv[0] = ex.worker_path;
	argv[1] = "/dev/fd/5";
	argv[2] = mp;
	argv[3] = "-o";
	argv[4] = "backend=fskit,local,norecover,noatime,windows_names,nocompression,no_detach,"
		  "streams_interface=openxattr,uid=501,gid=20,fmask=0133,dmask=0022,ro";
	return ntfsmac_argv_check_text(argv, NTFSMAC_WORKER_ARGC, &ex);
}

static void u_argv_rule2(void)
{
	static const char *const refused[] = {
		"a\xE2\x80\xAE\x62",       /* U+202E */
		"a\xEF\xBC\x8F\x62",       /* U+FF0F */
		"a\xC2\x85\x62",           /* U+0085 */
		"a\xEE\x80\x80\x62",       /* U+E000 */
		"a\xEF\xBF\xBF\x62",       /* U+FFFF */
		"a\xC2\xA0\x62",           /* NBSP */
		"a\xEF\xB8\x8F\x62",       /* U+FE0F */
		"a\xE3\x85\xA4\x62",       /* U+3164 */
		"a\xCD\xB8\x62",           /* unassigned */
		"\xCC\x81\x62",            /* combining mark first */
		"a,b", "a=b", "a\\b", "a:b", "a\x01\x62", "a\x7F\x62", ".a", " a", "a.", "a ",
		"a\xC0\xAF\x62", "a\xED\xA0\x80\x62",
	};
	char buf[128];
	size_t i, n;

	for (i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
		if (leaf_status(refused[i]) == NTFSMAC_ARGV_OK) {
			fprintf(stderr, "FAIL: argv_check accepts the leaf '%s'\n", refused[i]);
			g_failures++;
		}
	}
	/* 65, 66 and 67 bytes without a suffix; 64 bytes plus a bad suffix */
	for (n = 65; n <= 67; n++) {
		memset(buf, 'a', n);
		buf[n] = '\0';
		CHECK(leaf_status(buf) != NTFSMAC_ARGV_OK);
	}
	memset(buf, 'a', 64);
	buf[64] = '\0';
	CHECK(leaf_status(buf) == NTFSMAC_ARGV_OK);
	{
		static const char *const bad_suffix[] = { " 01", " 1", " 0", " 100", " 00", " 2x", "  2", " 2 ", " -2", " +2", "-2" };
		static const char *const good_suffix[] = { " 2", " 9", " 10", " 19", " 99", " 55" };

		for (i = 0; i < sizeof(bad_suffix) / sizeof(bad_suffix[0]); i++) {
			memset(buf, 'a', 64);
			strcpy(buf + 64, bad_suffix[i]);
			if (leaf_status(buf) == NTFSMAC_ARGV_OK) {
				fprintf(stderr, "FAIL: argv_check accepts a 64 byte base and '%s'\n", bad_suffix[i]);
				g_failures++;
			}
		}
		for (i = 0; i < sizeof(good_suffix) / sizeof(good_suffix[0]); i++) {
			memset(buf, 'a', 64);
			strcpy(buf + 64, good_suffix[i]);
			CHECK(leaf_status(buf) == NTFSMAC_ARGV_OK);
		}
	}
	/* a bad base with a good suffix */
	CHECK(leaf_status("a,b 2") != NTFSMAC_ARGV_OK);
	CHECK(leaf_status("a\xE2\x80\xAE" "b 2") != NTFSMAC_ARGV_OK);
	CHECK(leaf_status(". 2") != NTFSMAC_ARGV_OK);
	/* what stays accepted */
	CHECK(leaf_status("DATA") == NTFSMAC_ARGV_OK);
	CHECK(leaf_status("DATA 2") == NTFSMAC_ARGV_OK);
	CHECK(leaf_status("caf\xC3\xA9 (2)+") == NTFSMAC_ARGV_OK);
	CHECK(leaf_status("a\xCC\x81\x62") == NTFSMAC_ARGV_OK);
	CHECK(leaf_status("\xF0\x9F\x98\x80 x") == NTFSMAC_ARGV_OK);
	CHECK(leaf_status("a 01") == NTFSMAC_ARGV_OK); /* a short label may hold " 01" */
	CHECK(leaf_status("NTFS-5E6F7788") == NTFSMAC_ARGV_OK);
}

int main(void)
{
	cp_exhaustive();
	u_argv_rule2();
	u_label_01();
	u_label_02();
	u_label_03();
	u_label_04();
	u_label_05();
	u_label_06();
	u_label_07();
	u_label_08();
	u_label_argv();
	if (g_failures != 0) {
		fprintf(stderr, "test_label: %d failure(s)\n", g_failures);
		return 1;
	}
	printf("test_label: ok\n");
	return 0;
}
