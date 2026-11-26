/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-DMN-DOC-01..10: version and plist helpers (pure), and each doctor check
 * against temporary directories.
 */
#include "tmpdir.h"

#include "check.h"
#include "internal.h"

#include "ntfsmac/journal.h"

#define MAXF 16

typedef struct {
	ntfsmac_doctor_finding f[MAXF];
	unsigned n;
	unsigned stop_after; /* 0: never */
} collected;

static int collect(const ntfsmac_doctor_finding *f, void *ctx)
{
	collected *c = ctx;

	if (c->n < MAXF)
		c->f[c->n++] = *f;
	return c->stop_after != 0 && c->n >= c->stop_after;
}

static const ntfsmac_doctor_finding *find(const collected *c, ntfsmac_doctor_code code)
{
	for (unsigned i = 0; i < c->n; i++)
		if (c->f[i].code == (int)code)
			return &c->f[i];
	return NULL;
}

/* Expect a finding with this code, ok flag and value. */
#define EXPECT(c, code, okv, val)                                              \
	do {                                                                   \
		const ntfsmac_doctor_finding *f_ = find(&(c), (code));         \
		CHECK(f_ != NULL);                                             \
		if (f_ != NULL) {                                              \
			CHECK_EQ_INT(f_->ok, (okv));                           \
			CHECK_EQ_INT(f_->value, (val));                        \
		}                                                              \
	} while (0)

static ntfsmac_doctor_ext_state ext_enabled(void *ctx)
{
	(void)ctx;
	return NTFSMAC_EXT_ENABLED;
}

static ntfsmac_doctor_ext_state ext_not_approved(void *ctx)
{
	(void)ctx;
	return NTFSMAC_EXT_NOT_APPROVED;
}

static void test_version(void)
{
	ntfsmac_version a, b;
	char out[16];
	const char *plist =
		"<?xml version=\"1.0\"?>\n<plist><dict>\n"
		"\t<key>CFBundleName</key>\n\t<string>macFUSE</string>\n"
		"\t<key>CFBundleShortVersionString</key>\n\t<string>5.5.0</string>\n"
		"</dict></plist>\n";

	/* U-DMN-DOC-01 */
	CHECK(ntfsmac_version_parse("5.5.0", &a) && a.major == 5 && a.minor == 5 && a.patch == 0);
	CHECK(ntfsmac_version_parse("5.5", &a) && a.major == 5 && a.minor == 5 && a.patch == 0);
	CHECK(ntfsmac_version_parse("12", &a) && a.major == 12 && a.minor == 0);
	CHECK(ntfsmac_version_parse("5.5.1-beta2", &a) && a.patch == 1);
	CHECK(ntfsmac_version_parse("5.5.0.9", &a) && a.patch == 0);
	CHECK(!ntfsmac_version_parse("", &a));
	CHECK(!ntfsmac_version_parse("x5.5", &a));
	CHECK(!ntfsmac_version_parse("5..5", &a));
	CHECK(!ntfsmac_version_parse(".5", &a));
	CHECK(!ntfsmac_version_parse("5.", &a));
	CHECK(!ntfsmac_version_parse("1234567.1", &a));
	CHECK(!ntfsmac_version_parse(NULL, &a));
	CHECK(!ntfsmac_version_parse("5", NULL));

	ntfsmac_version_parse("5.5.0", &a);
	ntfsmac_version_parse("5.4.9", &b);
	CHECK_EQ_INT(ntfsmac_version_cmp(&b, &a), -1);
	CHECK_EQ_INT(ntfsmac_version_cmp(&a, &b), 1);
	CHECK_EQ_INT(ntfsmac_version_cmp(&a, &a), 0);
	ntfsmac_version_parse("5.5.1", &b);
	CHECK_EQ_INT(ntfsmac_version_cmp(&a, &b), -1);
	ntfsmac_version_parse("6.0.0", &b);
	CHECK_EQ_INT(ntfsmac_version_cmp(&a, &b), -1);
	ntfsmac_version_parse("4.99.99", &b);
	CHECK_EQ_INT(ntfsmac_version_cmp(&b, &a), -1);

	/* U-DMN-DOC-02: the plist reader. */
	CHECK(ntfsmac_plist_string(plist, strlen(plist), "CFBundleShortVersionString", out,
				   sizeof out));
	CHECK(strcmp(out, "5.5.0") == 0);
	CHECK(ntfsmac_plist_string(plist, strlen(plist), "CFBundleName", out, sizeof out));
	CHECK(strcmp(out, "macFUSE") == 0);
	CHECK(!ntfsmac_plist_string(plist, strlen(plist), "Missing", out, sizeof out));
	CHECK(!ntfsmac_plist_string(plist, strlen(plist), "CFBundleName", out, 4)); /* too small */
	CHECK(!ntfsmac_plist_string("bplist00\x01\x02", 10, "CFBundleName", out, sizeof out));
	/* A key whose value is not a string. */
	CHECK(!ntfsmac_plist_string("<key>A</key><integer>5</integer>", 32, "A", out, sizeof out));
	/* A truncated value. */
	CHECK(!ntfsmac_plist_string("<key>A</key><string>5.5", 23, "A", out, sizeof out));
	/* The length bounds the search: text after `len` is not read. */
	CHECK(!ntfsmac_plist_string("<key>A</key><string>5</string>", 20, "A", out, sizeof out));
	CHECK(!ntfsmac_plist_string(NULL, 0, "A", out, sizeof out));
}

static void test_no_macfuse_no_journal(void)
{
	char fs[PATH_MAX], jd[PATH_MAX];
	char base[PATH_MAX];
	collected c = {.n = 0};
	ntfsmac_doctor_env env = {fs, jd, NULL, NULL, 0, 0};
	struct stat st;

	tmp_make(base, sizeof base);
	PATHF(fs, "%s/fs", base);
	PATHF(jd, "%s/journal", base);
	tmp_mkdir(base, "fs");

	/* U-DMN-DOC-03: macFUSE missing; no other driver; no journal directory. */
	CHECK_EQ_INT(ntfsmac_doctor_run_env(&env, collect, &c), NTFSMAC_DMN_OK);
	EXPECT(c, NTFSMAC_DOCTOR_MACFUSE_INSTALLED, false, 0);
	CHECK(find(&c, NTFSMAC_DOCTOR_MACFUSE_VERSION) == NULL);
	CHECK(find(&c, NTFSMAC_DOCTOR_MACFUSE_EXTENSION) == NULL);
	EXPECT(c, NTFSMAC_DOCTOR_CONFLICT_NONE, true, 0);
	EXPECT(c, NTFSMAC_DOCTOR_JOURNAL_ENTRIES, true, 0);
	EXPECT(c, NTFSMAC_DOCTOR_JOURNAL_CORRUPT, true, 0);
	EXPECT(c, NTFSMAC_DOCTOR_JOURNAL_CLEARED, true, 0);
	/* T-M7-06: no banner was ever sent: "unknown", reported as ok, never as a failure. */
	EXPECT(c, NTFSMAC_DOCTOR_NOTIFY_DELIVERY, true, 0);
	/* Doctor only reads: it must not create the journal directory. */
	CHECK(lstat(jd, &st) != 0 && errno == ENOENT);
	tmp_rmrf(base);
}

/* U-DMN-DOC-09 (T-M7-06): the last banner outcome is a finding of its own. */
static void test_notify_delivery(void)
{
	char fs[PATH_MAX], jd[PATH_MAX], base[PATH_MAX];
	ntfsmac_doctor_env env = {fs, jd, NULL, NULL, 0, 0};
	ntfsmac_banner_outcome ok = {NTFSMAC_BANNER_OK, 0};
	ntfsmac_banner_outcome sr = {NTFSMAC_BANNER_SUDO_REFUSED, 1};
	ntfsmac_banner_outcome to = {NTFSMAC_BANNER_TIMEOUT, 0};
	ntfsmac_banner_outcome never = {NTFSMAC_BANNER_NEVER, 0};
	collected c;

	tmp_make(base, sizeof base);
	PATHF(fs, "%s/fs", base);
	PATHF(jd, "%s/journal", base);
	tmp_mkdir(base, "fs");

	memset(&c, 0, sizeof c);
	ntfsmac_banner_record(&sr);
	CHECK_EQ_INT(ntfsmac_doctor_run_env(&env, collect, &c), NTFSMAC_DMN_OK);
	EXPECT(c, NTFSMAC_DOCTOR_NOTIFY_DELIVERY, false, 3001);
	memset(&c, 0, sizeof c);
	ntfsmac_banner_record(&to);
	CHECK_EQ_INT(ntfsmac_doctor_run_env(&env, collect, &c), NTFSMAC_DMN_OK);
	EXPECT(c, NTFSMAC_DOCTOR_NOTIFY_DELIVERY, false, 4000);
	memset(&c, 0, sizeof c);
	ntfsmac_banner_record(&ok);
	CHECK_EQ_INT(ntfsmac_doctor_run_env(&env, collect, &c), NTFSMAC_DMN_OK);
	EXPECT(c, NTFSMAC_DOCTOR_NOTIFY_DELIVERY, true, 1000);
	ntfsmac_banner_record(&never);
	tmp_rmrf(base);
}

static void test_macfuse(void)
{
	char fs[PATH_MAX], jd[PATH_MAX], base[PATH_MAX], p[PATH_MAX];
	ntfsmac_doctor_env env = {fs, jd, ext_enabled, NULL, 0, 0};
	collected c;

	tmp_make(base, sizeof base);
	PATHF(fs, "%s/fs", base);
	PATHF(jd, "%s/journal", base);
	tmp_mkdir(base, "fs");
	tmp_mkdir(fs, "macfuse.fs");
	tmp_mkdir(fs, "macfuse.fs/Contents");
	PATHF(p, "macfuse.fs/Contents/Info.plist");

	/* U-DMN-DOC-04: version 5.5.0 and an enabled extension: all ok. */
	tmp_write(fs, p,
		  "<plist><dict><key>CFBundleShortVersionString</key>"
		  "<string>5.5.0</string></dict></plist>");
	memset(&c, 0, sizeof c);
	CHECK_EQ_INT(ntfsmac_doctor_run_env(&env, collect, &c), NTFSMAC_DMN_OK);
	EXPECT(c, NTFSMAC_DOCTOR_MACFUSE_INSTALLED, true, 0);
	EXPECT(c, NTFSMAC_DOCTOR_MACFUSE_VERSION, true, 50500);
	EXPECT(c, NTFSMAC_DOCTOR_MACFUSE_EXTENSION, true, NTFSMAC_EXT_ENABLED);

	/* Too old. */
	tmp_write(fs, p,
		  "<plist><dict><key>CFBundleShortVersionString</key>"
		  "<string>5.4.9</string></dict></plist>");
	memset(&c, 0, sizeof c);
	CHECK_EQ_INT(ntfsmac_doctor_run_env(&env, collect, &c), NTFSMAC_DMN_OK);
	EXPECT(c, NTFSMAC_DOCTOR_MACFUSE_VERSION, false, 50409);

	/* A newer one, with only CFBundleVersion present. */
	tmp_write(fs, p, "<plist><dict><key>CFBundleVersion</key><string>6.1.2</string></dict></plist>");
	memset(&c, 0, sizeof c);
	CHECK_EQ_INT(ntfsmac_doctor_run_env(&env, collect, &c), NTFSMAC_DMN_OK);
	EXPECT(c, NTFSMAC_DOCTOR_MACFUSE_VERSION, true, 60102);

	/* Unreadable version: a binary plist, then no plist at all. */
	tmp_write(fs, p, "bplist00garbage");
	memset(&c, 0, sizeof c);
	CHECK_EQ_INT(ntfsmac_doctor_run_env(&env, collect, &c), NTFSMAC_DMN_OK);
	EXPECT(c, NTFSMAC_DOCTOR_MACFUSE_VERSION, false, 0);
	{
		char full[PATH_MAX];

		PATHF(full, "%s/%s", fs, p);
		CHECK(unlink(full) == 0);
	}
	memset(&c, 0, sizeof c);
	CHECK_EQ_INT(ntfsmac_doctor_run_env(&env, collect, &c), NTFSMAC_DMN_OK);
	EXPECT(c, NTFSMAC_DOCTOR_MACFUSE_VERSION, false, 0);

	/* U-DMN-DOC-05: extension state from the hook; unknown without a hook. */
	env.ext_fn = ext_not_approved;
	memset(&c, 0, sizeof c);
	CHECK_EQ_INT(ntfsmac_doctor_run_env(&env, collect, &c), NTFSMAC_DMN_OK);
	EXPECT(c, NTFSMAC_DOCTOR_MACFUSE_EXTENSION, false, NTFSMAC_EXT_NOT_APPROVED);
	env.ext_fn = NULL;
	memset(&c, 0, sizeof c);
	CHECK_EQ_INT(ntfsmac_doctor_run_env(&env, collect, &c), NTFSMAC_DMN_OK);
	EXPECT(c, NTFSMAC_DOCTOR_MACFUSE_EXTENSION, false, NTFSMAC_EXT_UNKNOWN);
	tmp_rmrf(base);
}

/* U-DMN-DOC-08: the pluginkit parser, with the outputs recorded in a macOS 27.2 guest. */
#define PK_LINE(mark)                                                                          \
	mark "    io.macfuse.app.fsmodule.macfuse-local((null))\tAAF1FA77-228C-4AB8-BD8C-96CAF4E8AC24\t" \
	"2026-11-14 10:39:39 +0000\t/Library/Filesystems/macfuse.fs/Contents/Resources/macfuse.app/" \
	"Contents/Extensions/io.macfuse.app.fsmodule.macfuse-local.appex\n"
#define PK_TRAILER " (1 plug-in)\n"

static ntfsmac_doctor_ext_state ext_parse(const char *s)
{
	return ntfsmac_doctor_ext_parse(s, strlen(s));
}

static void test_ext_parse(void)
{
	char big[NTFSMAC_EXT_OUT_MAX + 64];

	CHECK_EQ_INT(ext_parse(PK_LINE("+") PK_TRAILER), NTFSMAC_EXT_ENABLED);
	CHECK_EQ_INT(ext_parse(PK_LINE("-") PK_TRAILER), NTFSMAC_EXT_NOT_APPROVED);
	CHECK_EQ_INT(ext_parse(PK_LINE(" ") PK_TRAILER), NTFSMAC_EXT_NOT_APPROVED);
	/* unexpected mark, no match, empty, NULL, no trailing newline */
	CHECK_EQ_INT(ext_parse(PK_LINE("!") PK_TRAILER), NTFSMAC_EXT_UNKNOWN);
	CHECK_EQ_INT(ext_parse("  (no matches)\n"), NTFSMAC_EXT_UNKNOWN);
	CHECK_EQ_INT(ext_parse(""), NTFSMAC_EXT_UNKNOWN);
	CHECK_EQ_INT(ntfsmac_doctor_ext_parse(NULL, 5), NTFSMAC_EXT_UNKNOWN);
	CHECK_EQ_INT(ntfsmac_doctor_ext_parse("+", 0), NTFSMAC_EXT_UNKNOWN);
	CHECK_EQ_INT(ext_parse(PK_LINE("+") " (1 plug-in)"), NTFSMAC_EXT_UNKNOWN);
	/* no trailer, two trailers, a second module line, a foreign line, plural trailer */
	CHECK_EQ_INT(ext_parse(PK_LINE("+")), NTFSMAC_EXT_UNKNOWN);
	CHECK_EQ_INT(ext_parse(PK_LINE("+") PK_TRAILER PK_TRAILER), NTFSMAC_EXT_UNKNOWN);
	CHECK_EQ_INT(ext_parse(PK_LINE("+") PK_LINE("+") PK_TRAILER), NTFSMAC_EXT_UNKNOWN);
	CHECK_EQ_INT(ext_parse(PK_LINE("+") "warning: something\n" PK_TRAILER), NTFSMAC_EXT_UNKNOWN);
	CHECK_EQ_INT(ext_parse(PK_LINE("+") " (2 plug-ins)\n"), NTFSMAC_EXT_UNKNOWN);
	/* another module id, an id that only starts like ours, a bundle outside macfuse.fs */
	CHECK_EQ_INT(ext_parse("+    com.apple.fskit.exfat((null))\t260C94B1\t2026\t/System/x.appex\n"
			       PK_TRAILER), NTFSMAC_EXT_UNKNOWN);
	CHECK_EQ_INT(ext_parse("+    io.macfuse.app.fsmodule.macfuse-localx((null))\tx\ty\t"
			       "/Library/Filesystems/macfuse.fs/z\n" PK_TRAILER), NTFSMAC_EXT_UNKNOWN);
	CHECK_EQ_INT(ext_parse("+    io.macfuse.app.fsmodule.macfuse-local((null))\tx\ty\t/tmp/evil.appex\n"
			       PK_TRAILER), NTFSMAC_EXT_UNKNOWN);
	/* a control byte, a NUL inside the length, a byte above 0x7e */
	CHECK_EQ_INT(ext_parse(PK_LINE("+") "\x1b" PK_TRAILER), NTFSMAC_EXT_UNKNOWN);
	CHECK_EQ_INT(ntfsmac_doctor_ext_parse(PK_LINE("+") PK_TRAILER, strlen(PK_LINE("+") PK_TRAILER) + 1),
		     NTFSMAC_EXT_UNKNOWN);
	CHECK_EQ_INT(ext_parse(PK_LINE("+") "\xc3\xa9\n" PK_TRAILER), NTFSMAC_EXT_UNKNOWN);
	/* oversized: a valid text padded past the cap is refused, not truncated */
	memset(big, ' ', sizeof big);
	memcpy(big, PK_LINE("+") PK_TRAILER, strlen(PK_LINE("+") PK_TRAILER));
	big[sizeof big - 1] = '\n';
	CHECK_EQ_INT(ntfsmac_doctor_ext_parse(big, sizeof big), NTFSMAC_EXT_UNKNOWN);
	CHECK_EQ_INT(ntfsmac_doctor_ext_parse(big, NTFSMAC_EXT_OUT_MAX + 1), NTFSMAC_EXT_UNKNOWN);
}

static void test_conflicts(void)
{
	char fs[PATH_MAX], jd[PATH_MAX], base[PATH_MAX];
	ntfsmac_doctor_env env = {fs, jd, NULL, NULL, 0, 0};
	collected c;

	tmp_make(base, sizeof base);
	PATHF(fs, "%s/fs", base);
	PATHF(jd, "%s/journal", base);
	tmp_mkdir(base, "fs");

	/* U-DMN-DOC-06: a dummy Paragon bundle is named; fuse-t is not a conflict. */
	tmp_mkdir(fs, "fuse-t.fs");
	tmp_mkdir(fs, "ufsd_NTFS.fs");
	memset(&c, 0, sizeof c);
	CHECK_EQ_INT(ntfsmac_doctor_run_env(&env, collect, &c), NTFSMAC_DMN_OK);
	EXPECT(c, NTFSMAC_DOCTOR_CONFLICT_PARAGON, false, 0);
	CHECK(find(&c, NTFSMAC_DOCTOR_CONFLICT_NONE) == NULL);
	CHECK(find(&c, NTFSMAC_DOCTOR_CONFLICT_TUXERA) == NULL);

	tmp_mkdir(fs, "tuxera_ntfs.fs");
	tmp_mkdir(fs, "acme-ntfs.fs");
	memset(&c, 0, sizeof c);
	CHECK_EQ_INT(ntfsmac_doctor_run_env(&env, collect, &c), NTFSMAC_DMN_OK);
	EXPECT(c, NTFSMAC_DOCTOR_CONFLICT_PARAGON, false, 0);
	EXPECT(c, NTFSMAC_DOCTOR_CONFLICT_TUXERA, false, 0);
	EXPECT(c, NTFSMAC_DOCTOR_CONFLICT_OTHER, false, 0);

	/* An unreadable bundle directory (a file instead): reported, not ignored. */
	tmp_write(base, "notdir", "x");
	PATHF(fs, "%s/notdir", base);
	memset(&c, 0, sizeof c);
	CHECK_EQ_INT(ntfsmac_doctor_run_env(&env, collect, &c), NTFSMAC_DMN_OK);
	EXPECT(c, NTFSMAC_DOCTOR_CONFLICT_UNREADABLE, false, 0);
	tmp_rmrf(base);
}

static void make_journal_entry(ntfsmac_journal *j, uint64_t serial)
{
	ntfsmac_journal_entry e;

	memset(&e, 0, sizeof e);
	e.serial = serial;
	strcpy(e.label, "TESTVOL");
	e.total_sectors = 2048;
	e.session_id = 1;
	e.created = 1700000000;
	e.updated = 1700000000;
	e.marker = NTFSMAC_JOURNAL_MARKER_PENDING;
	e.state = NTFSMAC_JOURNAL_STATE_OPEN;
	CHECK_EQ_INT(ntfsmac_journal_create(j, &e), NTFSMAC_JOURNAL_OK);
}

static void test_journal(void)
{
	char fs[PATH_MAX], jd[PATH_MAX], base[PATH_MAX], name[NTFSMAC_JOURNAL_NAME_SIZE];
	ntfsmac_doctor_env env = {fs, jd, NULL, NULL, 0, 0};
	ntfsmac_journal *j = NULL;
	collected c;

	tmp_make(base, sizeof base);
	PATHF(fs, "%s/fs", base);
	PATHF(jd, "%s/journal", base);
	tmp_mkdir(base, "fs");

	/* U-DMN-DOC-07: two entries, one unreadable, one tombstone. */
	CHECK_EQ_INT(ntfsmac_journal_open(jd, &j), NTFSMAC_JOURNAL_OK);
	make_journal_entry(j, 0x11);
	make_journal_entry(j, 0x22);
	make_journal_entry(j, 0x33);
	CHECK_EQ_INT(ntfsmac_journal_remove(j, 0x33, 2048, NTFSMAC_JOURNAL_REMOVE_JOURNAL_CLEAR,
					    1700000100),
		     NTFSMAC_JOURNAL_OK);
	CHECK_EQ_INT(ntfsmac_journal_format_name(name, sizeof name, 0x44), 0);
	tmp_write(jd, name, "this is not a journal entry\n");
	ntfsmac_journal_close(j);

	memset(&c, 0, sizeof c);
	CHECK_EQ_INT(ntfsmac_doctor_run_env(&env, collect, &c), NTFSMAC_DMN_OK);
	EXPECT(c, NTFSMAC_DOCTOR_JOURNAL_ENTRIES, false, 2);
	EXPECT(c, NTFSMAC_DOCTOR_JOURNAL_CORRUPT, false, 1);
	EXPECT(c, NTFSMAC_DOCTOR_JOURNAL_CLEARED, true, 1);

	/* No warning about the size of a small journal. */
	CHECK(find(&c, NTFSMAC_DOCTOR_JOURNAL_LARGE) == NULL);

	/*
	 * U-DMN-DOC-10 (T-M7-15): more entries than one listing holds (a bound of 1 here) is not
	 * "unreadable": the entries listed are counted and a fixed warning names journal-clear.
	 */
	env.journal_list_max = 1;
	memset(&c, 0, sizeof c);
	CHECK_EQ_INT(ntfsmac_doctor_run_env(&env, collect, &c), NTFSMAC_DMN_OK);
	CHECK(find(&c, NTFSMAC_DOCTOR_JOURNAL_UNREADABLE) == NULL);
	CHECK(find(&c, NTFSMAC_DOCTOR_JOURNAL_ENTRIES) != NULL);
	EXPECT(c, NTFSMAC_DOCTOR_JOURNAL_LARGE, false, 3); /* 2 entries and the unreadable one */
	env.journal_list_max = 0;
	/* The soft limit (2 here) warns even when everything was listed. */
	env.journal_soft_max = 2;
	memset(&c, 0, sizeof c);
	CHECK_EQ_INT(ntfsmac_doctor_run_env(&env, collect, &c), NTFSMAC_DMN_OK);
	EXPECT(c, NTFSMAC_DOCTOR_JOURNAL_ENTRIES, false, 2);
	EXPECT(c, NTFSMAC_DOCTOR_JOURNAL_LARGE, false, 3);
	env.journal_soft_max = 0;

	/* A journal path that is a file: unreadable, reported. */
	tmp_write(base, "jfile", "x");
	PATHF(jd, "%s/jfile", base);
	memset(&c, 0, sizeof c);
	CHECK_EQ_INT(ntfsmac_doctor_run_env(&env, collect, &c), NTFSMAC_DMN_OK);
	EXPECT(c, NTFSMAC_DOCTOR_JOURNAL_UNREADABLE, false, 0);
	CHECK(find(&c, NTFSMAC_DOCTOR_JOURNAL_ENTRIES) == NULL);
	tmp_rmrf(base);
}

static void test_run_control(void)
{
	char fs[PATH_MAX], jd[PATH_MAX], base[PATH_MAX];
	ntfsmac_doctor_env env = {fs, jd, NULL, NULL, 0, 0};
	collected c;

	tmp_make(base, sizeof base);
	PATHF(fs, "%s/fs", base);
	PATHF(jd, "%s/journal", base);
	tmp_mkdir(base, "fs");

	/* The callback can stop the run; a NULL callback is refused. */
	memset(&c, 0, sizeof c);
	c.stop_after = 1;
	CHECK_EQ_INT(ntfsmac_doctor_run_env(&env, collect, &c), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(c.n, 1);
	CHECK_EQ_INT(ntfsmac_doctor_run_env(&env, NULL, NULL), NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_doctor_run(NULL, NULL), NTFSMAC_DMN_E_INVAL);
	tmp_rmrf(base);
}

int main(void)
{
	test_version();
	test_no_macfuse_no_journal();
	test_notify_delivery();
	test_macfuse();
	test_ext_parse();
	test_conflicts();
	test_journal();
	test_run_control();
	if (g_failures != 0) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	return 0;
}
