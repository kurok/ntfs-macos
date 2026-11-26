/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Session journal unit tests (T-M3-13): naming (U-JRN-NAME-01), transitions,
 * create/get/update, the four removal paths, tombstones, strict parsing and
 * listing. The crash tests are in test_journal_crash.c.
 */

#include "jtest.h"

#include "ntfsmac/log.h"

#include <stdint.h>
#include <sys/stat.h>

static int g_tests;

#define RUN(fn)                                                              \
	do {                                                                 \
		fn();                                                        \
		g_tests++;                                                   \
	} while (0)

static ntfsmac_journal *open_fresh(char *base, size_t bsize, char *jdir,
				   size_t jsize)
{
	ntfsmac_journal *j = NULL;

	jt_jdir(base, bsize, jdir, jsize);
	CHECK_ST(ntfsmac_journal_open(jdir, &j), NTFSMAC_JOURNAL_OK);
	CHECK(j != NULL);
	return j;
}

/* The format-2 entry of (serial, JT_TS), the one create() writes for jt_entry(serial). */
static void entry_path(char *out, size_t size, const char *jdir, uint64_t serial)
{
	char name[NTFSMAC_JOURNAL_IDNAME_SIZE];

	CHECK(ntfsmac_journal_format_idname(name, sizeof(name), serial, JT_TS) == 0);
	jt_path(out, size, jdir, name, NULL);
}

/* The format-1 entry of `serial` (written before the identity rule, T-M7-13). */
static void legacy_path(char *out, size_t size, const char *jdir, uint64_t serial)
{
	char name[NTFSMAC_JOURNAL_NAME_SIZE];

	CHECK(ntfsmac_journal_format_name(name, sizeof(name), serial) == 0);
	jt_path(out, size, jdir, name, NULL);
}

/* ------------------------------------------------------------------ */

static void test_names(void)
{
	char n[NTFSMAC_JOURNAL_NAME_SIZE];
	char t[NTFSMAC_JOURNAL_TMPNAME_SIZE];
	char b[NTFSMAC_JOURNAL_TOMBNAME_SIZE];

	CHECK(ntfsmac_journal_format_name(n, sizeof(n), 0) == 0);
	CHECK(strcmp(n, "0000000000000000") == 0);
	CHECK(ntfsmac_journal_format_name(n, sizeof(n), 1) == 0);
	CHECK(strcmp(n, "0000000000000001") == 0);
	CHECK(ntfsmac_journal_format_name(n, sizeof(n), UINT64_MAX) == 0);
	CHECK(strcmp(n, "FFFFFFFFFFFFFFFF") == 0);
	CHECK(ntfsmac_journal_format_name(n, sizeof(n), 0x0123456789ABCDEFull) == 0);
	CHECK(strcmp(n, "0123456789ABCDEF") == 0);
	CHECK(ntfsmac_journal_format_name(n, 16, 1) == -1);
	CHECK(ntfsmac_journal_format_name(NULL, 17, 1) == -1);

	CHECK(ntfsmac_journal_format_tmpname(t, sizeof(t), 2) == 0);
	CHECK(strcmp(t, ".0000000000000002.tmp") == 0);
	CHECK(ntfsmac_journal_format_tmpname(t, sizeof(t), UINT64_MAX) == 0);
	CHECK(strcmp(t, ".FFFFFFFFFFFFFFFF.tmp") == 0);
	CHECK(ntfsmac_journal_format_tmpname(t, 21, 2) == -1);

	CHECK(ntfsmac_journal_format_tombstone(b, sizeof(b), 0xAB, 0) == 0);
	CHECK(strcmp(b, "00000000000000AB-19700101T000000Z") == 0);
	CHECK(ntfsmac_journal_format_tombstone(b, sizeof(b), 1, 1700000000) == 0);
	CHECK(strcmp(b, "0000000000000001-20231114T221320Z") == 0);
	CHECK(ntfsmac_journal_format_tombstone(b, sizeof(b), 1, 951782400) == 0);
	CHECK(strcmp(b, "0000000000000001-20000229T000000Z") == 0);
	CHECK(ntfsmac_journal_format_tombstone(b, sizeof(b), 1, -1) == 0);
	CHECK(strcmp(b, "0000000000000001-19691231T235959Z") == 0);
	CHECK(ntfsmac_journal_format_tombstone(b, sizeof(b), UINT64_MAX, 253402300799ll) == 0);
	CHECK(strcmp(b, "FFFFFFFFFFFFFFFF-99991231T235959Z") == 0);
	CHECK(ntfsmac_journal_format_tombstone(b, sizeof(b), 1, 253402300800ll) == -1);
	CHECK(ntfsmac_journal_format_tombstone(b, sizeof(b), 1, -62135596800ll) == 0);
	CHECK(strcmp(b, "0000000000000001-00010101T000000Z") == 0);
	CHECK(ntfsmac_journal_format_tombstone(b, sizeof(b), 1, -62135596801ll) == -1);
	CHECK(ntfsmac_journal_format_tombstone(b, sizeof(b), 1, INT64_MIN) == -1);
	CHECK(ntfsmac_journal_format_tombstone(b, sizeof(b), 1, INT64_MAX) == -1);
	CHECK(ntfsmac_journal_format_tombstone(b, 33, 1, 0) == -1);
}

static void test_transitions(void)
{
	const ntfsmac_journal_marker M[4] = {
		NTFSMAC_JOURNAL_MARKER_PENDING, NTFSMAC_JOURNAL_MARKER_SET,
		NTFSMAC_JOURNAL_MARKER_FAILED, NTFSMAC_JOURNAL_MARKER_NONE
	};
	/* [from][to], index order as in M. Only pending moves, to set or failed. */
	const int want[4][4] = {
		{ 1, 1, 1, 0 },
		{ 0, 1, 0, 0 },
		{ 0, 0, 1, 0 },
		{ 0, 0, 0, 1 },
	};

	for (int f = 0; f < 4; f++)
		for (int t = 0; t < 4; t++)
			CHECK(ntfsmac_journal_marker_transition_ok(M[f], M[t]) ==
			      (want[f][t] != 0));
	CHECK(ntfsmac_journal_state_transition_ok(NTFSMAC_JOURNAL_STATE_OPEN,
						  NTFSMAC_JOURNAL_STATE_OPEN));
	CHECK(ntfsmac_journal_state_transition_ok(NTFSMAC_JOURNAL_STATE_OPEN,
						  NTFSMAC_JOURNAL_STATE_WRITE_ERROR));
	CHECK(ntfsmac_journal_state_transition_ok(NTFSMAC_JOURNAL_STATE_WRITE_ERROR,
						  NTFSMAC_JOURNAL_STATE_WRITE_ERROR));
	CHECK(!ntfsmac_journal_state_transition_ok(NTFSMAC_JOURNAL_STATE_WRITE_ERROR,
						   NTFSMAC_JOURNAL_STATE_OPEN));
	/* Out-of-range values never pass, not even as identity. */
	CHECK(!ntfsmac_journal_marker_transition_ok((ntfsmac_journal_marker)9,
						    (ntfsmac_journal_marker)9));
	CHECK(!ntfsmac_journal_marker_transition_ok(NTFSMAC_JOURNAL_MARKER_PENDING,
						    (ntfsmac_journal_marker)9));
	CHECK(!ntfsmac_journal_state_transition_ok((ntfsmac_journal_state)5,
						   (ntfsmac_journal_state)5));
}

static void test_open(void)
{
	char base[PATH_MAX_T], jdir[PATH_MAX_T], p[PATH_MAX_T], q[PATH_MAX_T];
	ntfsmac_journal *j = NULL;
	struct stat st;

	CHECK_ST(ntfsmac_journal_open(NULL, &j), NTFSMAC_JOURNAL_ERR_INVALID);
	CHECK_ST(ntfsmac_journal_open("", &j), NTFSMAC_JOURNAL_ERR_INVALID);
	CHECK_ST(ntfsmac_journal_open("/nonexistent-ntfsmac/x/y", &j), NTFSMAC_JOURNAL_ERR_IO);
	CHECK(j == NULL);

	/* Missing directory is created 0700 with cleared/ below it. */
	j = open_fresh(base, sizeof(base), jdir, sizeof(jdir));
	CHECK(stat(jdir, &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & 0777) == 0700);
	jt_path(p, sizeof(p), jdir, "cleared", NULL);
	CHECK(stat(p, &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & 0777) == 0700);
	ntfsmac_journal_close(j);
	/* Reopen is idempotent. */
	CHECK_ST(ntfsmac_journal_open(jdir, &j), NTFSMAC_JOURNAL_OK);
	ntfsmac_journal_close(j);
	ntfsmac_journal_close(NULL);

	/* Symlinked journal directory (to a real directory): refused. */
	jt_path(q, sizeof(q), base, "link", NULL);
	CHECK(symlink(jdir, q) == 0);
	j = NULL;
	CHECK_ST(ntfsmac_journal_open(q, &j), NTFSMAC_JOURNAL_ERR_IO);
	CHECK(j == NULL);
	/* Dangling symlink: refused, and nothing is created behind it. */
	jt_path(q, sizeof(q), base, "dangling", NULL);
	jt_path(p, sizeof(p), base, "elsewhere", NULL);
	CHECK(symlink(p, q) == 0);
	CHECK_ST(ntfsmac_journal_open(q, &j), NTFSMAC_JOURNAL_ERR_IO);
	CHECK(lstat(p, &st) != 0);
	/* A regular file at the path. */
	jt_path(q, sizeof(q), base, "file", NULL);
	jt_write_file(q, "x", 1, 0600);
	CHECK_ST(ntfsmac_journal_open(q, &j), NTFSMAC_JOURNAL_ERR_IO);
	/* A directory others can write to. */
	jt_path(q, sizeof(q), base, "open-dir", NULL);
	CHECK(mkdir(q, 0700) == 0 && chmod(q, 0777) == 0);
	CHECK_ST(ntfsmac_journal_open(q, &j), NTFSMAC_JOURNAL_ERR_IO);
	/* cleared/ as a symlink: refused. */
	jt_path(q, sizeof(q), base, "j2", NULL);
	CHECK(mkdir(q, 0700) == 0);
	jt_path(p, sizeof(p), q, "cleared", NULL);
	CHECK(symlink(base, p) == 0);
	CHECK_ST(ntfsmac_journal_open(q, &j), NTFSMAC_JOURNAL_ERR_IO);
	jt_rmrf(base);
}

static void test_open_cleans_stray_temporaries(void)
{
	char base[PATH_MAX_T], jdir[PATH_MAX_T], p[PATH_MAX_T], victim[PATH_MAX_T];
	char buf[16];
	ntfsmac_journal *j;

	j = open_fresh(base, sizeof(base), jdir, sizeof(jdir));
	ntfsmac_journal_close(j);
	jt_path(victim, sizeof(victim), base, "victim", NULL);
	jt_write_file(victim, "VICTIM", 6, 0600);

	jt_path(p, sizeof(p), jdir, ".0000000000000001.tmp", NULL);
	jt_write_file(p, "partial", 7, 0600);
	jt_path(p, sizeof(p), jdir, ".00000000000000FF.tmp", NULL);
	CHECK(symlink(victim, p) == 0);
	/* Not matching the temporary grammar: left alone. */
	jt_path(p, sizeof(p), jdir, ".0000000000000FF.tmp", NULL); /* 15 digits */
	jt_write_file(p, "keep", 4, 0600);
	jt_path(p, sizeof(p), jdir, ".1.tmp", NULL);
	jt_write_file(p, "keep", 4, 0600);
	CHECK(jt_count_names(jdir, jt_is_tmp) == 4);

	CHECK_ST(ntfsmac_journal_open(jdir, &j), NTFSMAC_JOURNAL_OK);
	CHECK(jt_count_names(jdir, jt_is_tmp) == 2);
	CHECK(jt_read_file(victim, buf, sizeof(buf)) == 6 && memcmp(buf, "VICTIM", 6) == 0);
	ntfsmac_journal_close(j);
	jt_rmrf(base);
}

static void test_create_get(void)
{
	char base[PATH_MAX_T], jdir[PATH_MAX_T], p[PATH_MAX_T];
	ntfsmac_journal *j = open_fresh(base, sizeof(base), jdir, sizeof(jdir));
	ntfsmac_journal_entry e = jt_entry(0xAB), g, bad;
	struct stat st;
	const uint64_t serials[] = { 0, 1, 2, UINT64_MAX, 0x8000000000000000ull };

	CHECK_ST(ntfsmac_journal_get(j, 0xAB, JT_TS, &g), NTFSMAC_JOURNAL_ERR_NOT_FOUND);
	CHECK_ST(ntfsmac_journal_create(j, &e), NTFSMAC_JOURNAL_OK);
	CHECK_ST(ntfsmac_journal_get(j, 0xAB, JT_TS, &g), NTFSMAC_JOURNAL_OK);
	CHECK(jt_entry_eq(&e, &g));
	entry_path(p, sizeof(p), jdir, 0xAB);
	CHECK(stat(p, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0777) == 0600);
	CHECK(jt_count_names(jdir, jt_is_tmp) == 0);

	/* Create never replaces. */
	e.label[0] = 'X';
	CHECK_ST(ntfsmac_journal_create(j, &e), NTFSMAC_JOURNAL_ERR_EXISTS);
	CHECK_ST(ntfsmac_journal_get(j, 0xAB, JT_TS, &g), NTFSMAC_JOURNAL_OK);
	CHECK(strcmp(g.label, "TESTVOL") == 0);
	CHECK(jt_count_names(jdir, jt_is_tmp) == 0);

	/* Extremes round trip. */
	for (size_t i = 0; i < sizeof(serials) / sizeof(serials[0]); i++) {
		ntfsmac_journal_entry x = jt_entry(serials[i]);

		x.total_sectors = UINT64_MAX;
		x.session_id = UINT64_MAX;
		x.created = INT64_MIN;
		x.updated = INT64_MAX;
		x.logfile_lsn_at_start = UINT64_MAX;
		x.marker = (i & 1) ? NTFSMAC_JOURNAL_MARKER_NONE : NTFSMAC_JOURNAL_MARKER_PENDING;
		CHECK_ST(ntfsmac_journal_create(j, &x), NTFSMAC_JOURNAL_OK);
		CHECK_ST(ntfsmac_journal_get(j, serials[i], x.total_sectors, &g), NTFSMAC_JOURNAL_OK);
		CHECK(jt_entry_eq(&x, &g));
	}
	{ /* Empty label, 64-byte label with UTF-8. */
		ntfsmac_journal_entry x = jt_entry(0x77);

		x.label[0] = '\0';
		CHECK_ST(ntfsmac_journal_create(j, &x), NTFSMAC_JOURNAL_OK);
		CHECK_ST(ntfsmac_journal_get(j, 0x77, JT_TS, &g), NTFSMAC_JOURNAL_OK);
		CHECK(jt_entry_eq(&x, &g));
		x = jt_entry(0x78);
		memset(x.label, 'a', 62);
		x.label[62] = (char)0xC3;
		x.label[63] = (char)0xA9; /* 64 bytes */
		x.label[64] = '\0';
		CHECK_ST(ntfsmac_journal_create(j, &x), NTFSMAC_JOURNAL_OK);
		CHECK_ST(ntfsmac_journal_get(j, 0x78, JT_TS, &g), NTFSMAC_JOURNAL_OK);
		CHECK(jt_entry_eq(&x, &g));
	}

	/* Invalid input is rejected and creates nothing. */
	bad = jt_entry(0x90);
	memset(bad.label, 'a', sizeof(bad.label)); /* not NUL terminated */
	CHECK_ST(ntfsmac_journal_create(j, &bad), NTFSMAC_JOURNAL_ERR_INVALID);
	bad = jt_entry(0x90);
	bad.label[2] = '\n';
	CHECK_ST(ntfsmac_journal_create(j, &bad), NTFSMAC_JOURNAL_ERR_INVALID);
	bad.label[2] = '\0';
	bad.label[1] = 0x7f;
	CHECK_ST(ntfsmac_journal_create(j, &bad), NTFSMAC_JOURNAL_ERR_INVALID);
	bad = jt_entry(0x90);
	bad.session_id = 0;
	CHECK_ST(ntfsmac_journal_create(j, &bad), NTFSMAC_JOURNAL_ERR_INVALID);
	bad = jt_entry(0x90);
	bad.total_sectors = 0;
	CHECK_ST(ntfsmac_journal_create(j, &bad), NTFSMAC_JOURNAL_ERR_INVALID);
	bad = jt_entry(0x90);
	bad.marker = (ntfsmac_journal_marker)7;
	CHECK_ST(ntfsmac_journal_create(j, &bad), NTFSMAC_JOURNAL_ERR_INVALID);
	bad = jt_entry(0x90);
	bad.state = (ntfsmac_journal_state)7;
	CHECK_ST(ntfsmac_journal_create(j, &bad), NTFSMAC_JOURNAL_ERR_INVALID);
	/* Section 3.5: a new entry is open with marker pending or none. */
	bad = jt_entry(0x90);
	bad.state = NTFSMAC_JOURNAL_STATE_WRITE_ERROR;
	CHECK_ST(ntfsmac_journal_create(j, &bad), NTFSMAC_JOURNAL_ERR_INVALID);
	bad = jt_entry(0x90);
	bad.marker = NTFSMAC_JOURNAL_MARKER_SET;
	CHECK_ST(ntfsmac_journal_create(j, &bad), NTFSMAC_JOURNAL_ERR_INVALID);
	bad.marker = NTFSMAC_JOURNAL_MARKER_FAILED;
	CHECK_ST(ntfsmac_journal_create(j, &bad), NTFSMAC_JOURNAL_ERR_INVALID);
	CHECK_ST(ntfsmac_journal_get(j, 0x90, JT_TS, &g), NTFSMAC_JOURNAL_ERR_NOT_FOUND);
	CHECK_ST(ntfsmac_journal_create(NULL, &e), NTFSMAC_JOURNAL_ERR_INVALID);
	CHECK_ST(ntfsmac_journal_create(j, NULL), NTFSMAC_JOURNAL_ERR_INVALID);
	CHECK_ST(ntfsmac_journal_get(j, 1, JT_TS, NULL), NTFSMAC_JOURNAL_ERR_INVALID);
	CHECK(jt_count_names(jdir, jt_is_tmp) == 0);

	ntfsmac_journal_close(j);
	jt_rmrf(base);
}

static void test_update(void)
{
	char base[PATH_MAX_T], jdir[PATH_MAX_T];
	ntfsmac_journal *j = open_fresh(base, sizeof(base), jdir, sizeof(jdir));
	ntfsmac_journal_entry e = jt_entry(0xC1), u, g;

	CHECK_ST(ntfsmac_journal_update(j, &e), NTFSMAC_JOURNAL_ERR_NOT_FOUND);
	CHECK_ST(ntfsmac_journal_create(j, &e), NTFSMAC_JOURNAL_OK);

	/* Identity, with a new `updated`. */
	u = e;
	u.updated = e.updated + 5;
	CHECK_ST(ntfsmac_journal_update(j, &u), NTFSMAC_JOURNAL_OK);
	CHECK_ST(ntfsmac_journal_get(j, 0xC1, JT_TS, &g), NTFSMAC_JOURNAL_OK);
	CHECK(jt_entry_eq(&u, &g));

	/* Each immutable field. */
	{
		ntfsmac_journal_entry m[5];

		for (int i = 0; i < 5; i++)
			m[i] = u;
		m[0].label[0] = 'Z';
		m[1].total_sectors++;
		m[2].session_id++;
		m[3].created++;
		m[4].logfile_lsn_at_start++;
		/* total_sectors is part of the key (T-M7-13): another value names another entry */
		for (int i = 0; i < 5; i++)
			CHECK_ST(ntfsmac_journal_update(j, &m[i]), i == 1 ? NTFSMAC_JOURNAL_ERR_NOT_FOUND
								    : NTFSMAC_JOURNAL_ERR_IMMUTABLE);
		m[1] = u;
		m[1].legacy = true; /* the format-1 entry of the serial: there is none */
		CHECK_ST(ntfsmac_journal_update(j, &m[1]), NTFSMAC_JOURNAL_ERR_NOT_FOUND);
		CHECK_ST(ntfsmac_journal_get(j, 0xC1, JT_TS, &g), NTFSMAC_JOURNAL_OK);
		CHECK(jt_entry_eq(&u, &g));
	}

	/* pending -> set; then no way back or sideways. */
	u.marker = NTFSMAC_JOURNAL_MARKER_SET;
	u.updated++;
	CHECK_ST(ntfsmac_journal_update(j, &u), NTFSMAC_JOURNAL_OK);
	{
		ntfsmac_journal_marker bad[3] = {
			NTFSMAC_JOURNAL_MARKER_PENDING, NTFSMAC_JOURNAL_MARKER_FAILED,
			NTFSMAC_JOURNAL_MARKER_NONE
		};

		for (int i = 0; i < 3; i++) {
			ntfsmac_journal_entry b = u;

			b.marker = bad[i];
			CHECK_ST(ntfsmac_journal_update(j, &b), NTFSMAC_JOURNAL_ERR_TRANSITION);
		}
	}
	/* open -> write_error; never back. */
	u.state = NTFSMAC_JOURNAL_STATE_WRITE_ERROR;
	u.updated++;
	CHECK_ST(ntfsmac_journal_update(j, &u), NTFSMAC_JOURNAL_OK);
	{
		ntfsmac_journal_entry b = u;

		b.state = NTFSMAC_JOURNAL_STATE_OPEN;
		CHECK_ST(ntfsmac_journal_update(j, &b), NTFSMAC_JOURNAL_ERR_TRANSITION);
	}
	CHECK_ST(ntfsmac_journal_get(j, 0xC1, JT_TS, &g), NTFSMAC_JOURNAL_OK);
	CHECK(jt_entry_eq(&u, &g));

	/* pending -> failed is final; none is final. */
	e = jt_entry(0xC2);
	CHECK_ST(ntfsmac_journal_create(j, &e), NTFSMAC_JOURNAL_OK);
	e.marker = NTFSMAC_JOURNAL_MARKER_FAILED;
	CHECK_ST(ntfsmac_journal_update(j, &e), NTFSMAC_JOURNAL_OK);
	e.marker = NTFSMAC_JOURNAL_MARKER_SET;
	CHECK_ST(ntfsmac_journal_update(j, &e), NTFSMAC_JOURNAL_ERR_TRANSITION);
	e = jt_entry(0xC3);
	e.marker = NTFSMAC_JOURNAL_MARKER_NONE;
	CHECK_ST(ntfsmac_journal_create(j, &e), NTFSMAC_JOURNAL_OK);
	e.marker = NTFSMAC_JOURNAL_MARKER_SET;
	CHECK_ST(ntfsmac_journal_update(j, &e), NTFSMAC_JOURNAL_ERR_TRANSITION);

	/* Invalid input. */
	e = jt_entry(0xC1);
	e.session_id = 0;
	CHECK_ST(ntfsmac_journal_update(j, &e), NTFSMAC_JOURNAL_ERR_INVALID);
	CHECK_ST(ntfsmac_journal_update(j, NULL), NTFSMAC_JOURNAL_ERR_INVALID);
	CHECK(jt_count_names(jdir, jt_is_tmp) == 0);

	ntfsmac_journal_close(j);
	jt_rmrf(base);
}

/* ------------------------------------------------------------------ */

struct tomb_ctx {
	unsigned n;
	uint64_t serial[64];
	int64_t when[64];
};

static int tomb_cb(uint64_t serial, int64_t when, void *ctx)
{
	struct tomb_ctx *c = ctx;

	if (c->n < 64) {
		c->serial[c->n] = serial;
		c->when[c->n] = when;
	}
	c->n++;
	return 0;
}

static void tombs(ntfsmac_journal *j, struct tomb_ctx *c)
{
	memset(c, 0, sizeof(*c));
	CHECK_ST(ntfsmac_journal_list_tombstones(j, tomb_cb, c), NTFSMAC_JOURNAL_OK);
}

static int has_tomb(const struct tomb_ctx *c, uint64_t serial, int64_t when)
{
	for (unsigned i = 0; i < c->n && i < 64; i++)
		if (c->serial[i] == serial && c->when[i] == when)
			return 1;
	return 0;
}

static void make_entry(ntfsmac_journal *j, uint64_t serial,
		       ntfsmac_journal_marker marker, ntfsmac_journal_state state)
{
	ntfsmac_journal_entry e = jt_entry(serial);

	e.marker = marker == NTFSMAC_JOURNAL_MARKER_NONE ? NTFSMAC_JOURNAL_MARKER_NONE
							 : NTFSMAC_JOURNAL_MARKER_PENDING;
	CHECK_ST(ntfsmac_journal_create(j, &e), NTFSMAC_JOURNAL_OK);
	if (marker == NTFSMAC_JOURNAL_MARKER_SET || marker == NTFSMAC_JOURNAL_MARKER_FAILED) {
		e.marker = marker;
		e.updated++;
		CHECK_ST(ntfsmac_journal_update(j, &e), NTFSMAC_JOURNAL_OK);
	}
	if (state == NTFSMAC_JOURNAL_STATE_WRITE_ERROR) {
		e.state = state;
		e.updated++;
		CHECK_ST(ntfsmac_journal_update(j, &e), NTFSMAC_JOURNAL_OK);
	}
}

static char g_log[512];

static void log_capture(ntfsmac_log_level level, const char *msg, void *ctx)
{
	(void)level;
	(void)ctx;
	snprintf(g_log, sizeof(g_log), "%s", msg);
}

static void test_removal_paths(void)
{
	static const ntfsmac_journal_marker M[4] = {
		NTFSMAC_JOURNAL_MARKER_PENDING, NTFSMAC_JOURNAL_MARKER_SET,
		NTFSMAC_JOURNAL_MARKER_FAILED, NTFSMAC_JOURNAL_MARKER_NONE
	};
	static const ntfsmac_journal_state S[2] = {
		NTFSMAC_JOURNAL_STATE_OPEN, NTFSMAC_JOURNAL_STATE_WRITE_ERROR
	};
	static const ntfsmac_journal_removal P[4] = {
		NTFSMAC_JOURNAL_REMOVE_CLEAN_SESSION, NTFSMAC_JOURNAL_REMOVE_EXTERNAL_CHECK,
		NTFSMAC_JOURNAL_REMOVE_JOURNAL_CLEAR, NTFSMAC_JOURNAL_REMOVE_REPAIR
	};
	char base[PATH_MAX_T], jdir[PATH_MAX_T];
	ntfsmac_journal *j = open_fresh(base, sizeof(base), jdir, sizeof(jdir));
	uint64_t serial = 0x1000;
	int64_t now = 1700000000;
	unsigned allowed = 0, refused = 0;

	for (int m = 0; m < 4; m++)
		for (int s = 0; s < 2; s++)
			for (int p = 0; p < 4; p++) {
				ntfsmac_journal_entry before, after;
				struct tomb_ctx t;
				int ok;

				switch (P[p]) {
				case NTFSMAC_JOURNAL_REMOVE_CLEAN_SESSION:
					ok = S[s] == NTFSMAC_JOURNAL_STATE_OPEN &&
					     (M[m] == NTFSMAC_JOURNAL_MARKER_SET ||
					      M[m] == NTFSMAC_JOURNAL_MARKER_NONE);
					break;
				case NTFSMAC_JOURNAL_REMOVE_EXTERNAL_CHECK:
					ok = M[m] == NTFSMAC_JOURNAL_MARKER_SET;
					break;
				default:
					ok = 1;
					break;
				}
				serial++;
				now += 10;
				make_entry(j, serial, M[m], S[s]);
				CHECK_ST(ntfsmac_journal_get(j, serial, JT_TS, &before), NTFSMAC_JOURNAL_OK);
				tombs(j, &t);
				{
					unsigned t0 = t.n;
					ntfsmac_journal_status st =
						ntfsmac_journal_remove(j, serial, JT_TS, P[p], now);

					if (ok) {
						CHECK(st == NTFSMAC_JOURNAL_OK);
						CHECK_ST(ntfsmac_journal_get(j, serial, JT_TS, &after),
							 NTFSMAC_JOURNAL_ERR_NOT_FOUND);
						tombs(j, &t);
						CHECK(t.n == t0 + 1 && has_tomb(&t, serial, now));
						allowed++;
					} else {
						CHECK(st == NTFSMAC_JOURNAL_ERR_PATH_CONDITION);
						CHECK_ST(ntfsmac_journal_get(j, serial, JT_TS, &after),
							 NTFSMAC_JOURNAL_OK);
						CHECK(jt_entry_eq(&before, &after));
						tombs(j, &t);
						CHECK(t.n == t0);
						refused++;
					}
				}
			}
	CHECK(allowed + refused == 32);
	/* clean-session 2, external-check 2, journal-clear 8, repair 8. */
	CHECK(allowed == 20 && refused == 12);

	/* Not found, invalid arguments. */
	CHECK_ST(ntfsmac_journal_remove(j, 0xDEAD, JT_TS, NTFSMAC_JOURNAL_REMOVE_JOURNAL_CLEAR, now),
		 NTFSMAC_JOURNAL_ERR_NOT_FOUND);
	make_entry(j, 0xBEEF, NTFSMAC_JOURNAL_MARKER_NONE, NTFSMAC_JOURNAL_STATE_OPEN);
	CHECK_ST(ntfsmac_journal_remove(j, 0xBEEF, JT_TS, (ntfsmac_journal_removal)0, now),
		 NTFSMAC_JOURNAL_ERR_INVALID);
	CHECK_ST(ntfsmac_journal_remove(j, 0xBEEF, JT_TS, (ntfsmac_journal_removal)5, now),
		 NTFSMAC_JOURNAL_ERR_INVALID);
	CHECK_ST(ntfsmac_journal_remove(j, 0xBEEF, JT_TS, NTFSMAC_JOURNAL_REMOVE_REPAIR, INT64_MAX),
		 NTFSMAC_JOURNAL_ERR_INVALID);
	CHECK_ST(ntfsmac_journal_remove(NULL, 0xBEEF, JT_TS, NTFSMAC_JOURNAL_REMOVE_REPAIR, now),
		 NTFSMAC_JOURNAL_ERR_INVALID);
	{
		ntfsmac_journal_entry g;

		CHECK_ST(ntfsmac_journal_get(j, 0xBEEF, JT_TS, &g), NTFSMAC_JOURNAL_OK);
	}

	/* The removal is logged with the path. */
	ntfsmac_log_set_sink(log_capture, NULL);
	ntfsmac_log_set_level(NTFSMAC_LOG_INFO);
	g_log[0] = '\0';
	CHECK_ST(ntfsmac_journal_remove(j, 0xBEEF, JT_TS, NTFSMAC_JOURNAL_REMOVE_REPAIR, now + 1000),
		 NTFSMAC_JOURNAL_OK);
	ntfsmac_log_set_level(NTFSMAC_LOG_NONE);
	ntfsmac_log_set_sink(NULL, NULL);
	CHECK(strstr(g_log, "000000000000BEEF") != NULL);
	CHECK(strstr(g_log, "repair") != NULL);

	ntfsmac_journal_close(j);
	jt_rmrf(base);
}

static void test_tombstones(void)
{
	char base[PATH_MAX_T], jdir[PATH_MAX_T], p[PATH_MAX_T];
	ntfsmac_journal *j = open_fresh(base, sizeof(base), jdir, sizeof(jdir));
	struct tomb_ctx t;
	const int64_t t0 = 1700000000;
	ntfsmac_journal_entry g;

	/* Same-second collision: ERR_EXISTS, entry stays; one second later OK. */
	make_entry(j, 0x42, NTFSMAC_JOURNAL_MARKER_NONE, NTFSMAC_JOURNAL_STATE_OPEN);
	CHECK_ST(ntfsmac_journal_remove(j, 0x42, JT_TS, NTFSMAC_JOURNAL_REMOVE_CLEAN_SESSION, t0),
		 NTFSMAC_JOURNAL_OK);
	make_entry(j, 0x42, NTFSMAC_JOURNAL_MARKER_NONE, NTFSMAC_JOURNAL_STATE_OPEN);
	CHECK_ST(ntfsmac_journal_remove(j, 0x42, JT_TS, NTFSMAC_JOURNAL_REMOVE_CLEAN_SESSION, t0),
		 NTFSMAC_JOURNAL_ERR_EXISTS);
	CHECK_ST(ntfsmac_journal_get(j, 0x42, JT_TS, &g), NTFSMAC_JOURNAL_OK);
	tombs(j, &t);
	CHECK(t.n == 1);
	CHECK_ST(ntfsmac_journal_remove(j, 0x42, JT_TS, NTFSMAC_JOURNAL_REMOVE_CLEAN_SESSION, t0 + 1),
		 NTFSMAC_JOURNAL_OK);
	tombs(j, &t);
	CHECK(t.n == 2 && t.when[0] == t0 + 1 && t.when[1] == t0); /* newest first */
	ntfsmac_journal_close(j);
	jt_rmrf(base);

	/* Pruning to 32; names outside the grammar are neither listed nor pruned. */
	j = open_fresh(base, sizeof(base), jdir, sizeof(jdir));
	jt_path(p, sizeof(p), jdir, "cleared", "junk");
	jt_write_file(p, "j", 1, 0600);
	jt_path(p, sizeof(p), jdir, "cleared", "0000000000000001-20230230T000000Z");
	jt_write_file(p, "j", 1, 0600);
	jt_path(p, sizeof(p), jdir, "cleared", "000000000000000a-20230101T000000Z");
	jt_write_file(p, "j", 1, 0600);
	for (int i = 0; i < 40; i++) {
		make_entry(j, 100 + (uint64_t)i, NTFSMAC_JOURNAL_MARKER_NONE,
			   NTFSMAC_JOURNAL_STATE_OPEN);
		CHECK_ST(ntfsmac_journal_remove(j, 100 + (uint64_t)i, JT_TS,
						NTFSMAC_JOURNAL_REMOVE_CLEAN_SESSION,
						t0 + i * 10),
			 NTFSMAC_JOURNAL_OK);
		tombs(j, &t);
		CHECK(t.n == (unsigned)(i + 1 < 32 ? i + 1 : 32));
	}
	tombs(j, &t);
	CHECK(t.n == 32);
	for (unsigned i = 0; i < 32; i++) { /* newest first: serial 139 down to 108 */
		CHECK(t.serial[i] == 139 - i);
		CHECK(t.when[i] == t0 + (int64_t)(39 - i) * 10);
	}
	jt_path(p, sizeof(p), jdir, "cleared", "junk");
	CHECK(access(p, F_OK) == 0);
	jt_path(p, sizeof(p), jdir, "cleared", "0000000000000001-20230230T000000Z");
	CHECK(access(p, F_OK) == 0);
	jt_path(p, sizeof(p), jdir, "cleared", "000000000000000a-20230101T000000Z");
	CHECK(access(p, F_OK) == 0);

	/* A removal older than every tombstone still leaves its own tombstone;
	 * the oldest of the others goes. */
	make_entry(j, 500, NTFSMAC_JOURNAL_MARKER_NONE, NTFSMAC_JOURNAL_STATE_OPEN);
	CHECK_ST(ntfsmac_journal_remove(j, 500, JT_TS, NTFSMAC_JOURNAL_REMOVE_CLEAN_SESSION, 1000),
		 NTFSMAC_JOURNAL_OK);
	tombs(j, &t);
	CHECK(t.n == 32);
	CHECK(has_tomb(&t, 500, 1000));
	CHECK(!has_tomb(&t, 108, t0 + 80));
	CHECK(has_tomb(&t, 109, t0 + 90));
	ntfsmac_journal_close(j);
	jt_rmrf(base);
}

static void test_removal_of_damaged_entries(void)
{
	char base[PATH_MAX_T], jdir[PATH_MAX_T], p[PATH_MAX_T], victim[PATH_MAX_T];
	ntfsmac_journal *j = open_fresh(base, sizeof(base), jdir, sizeof(jdir));
	ntfsmac_journal_entry g;
	struct stat st;
	char buf[16];
	char tomb[NTFSMAC_JOURNAL_TOMBNAME_SIZE], tp[PATH_MAX_T];

	/* Garbage file: paths 1 and 2 cannot see its marker and refuse; paths 3
	 * and 4 have no precondition and clear it. */
	entry_path(p, sizeof(p), jdir, 0x55);
	jt_write_file(p, "garbage", 7, 0600);
	CHECK_ST(ntfsmac_journal_get(j, 0x55, JT_TS, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);
	CHECK_ST(ntfsmac_journal_remove(j, 0x55, JT_TS, NTFSMAC_JOURNAL_REMOVE_CLEAN_SESSION, 5000),
		 NTFSMAC_JOURNAL_ERR_CORRUPT);
	CHECK_ST(ntfsmac_journal_remove(j, 0x55, JT_TS, NTFSMAC_JOURNAL_REMOVE_EXTERNAL_CHECK, 5000),
		 NTFSMAC_JOURNAL_ERR_CORRUPT);
	CHECK(access(p, F_OK) == 0);
	CHECK_ST(ntfsmac_journal_remove(j, 0x55, JT_TS, NTFSMAC_JOURNAL_REMOVE_JOURNAL_CLEAR, 5000),
		 NTFSMAC_JOURNAL_OK);
	CHECK(access(p, F_OK) != 0);
	entry_path(p, sizeof(p), jdir, 0x56);
	jt_write_file(p, "garbage", 7, 0600);
	CHECK_ST(ntfsmac_journal_remove(j, 0x56, JT_TS, NTFSMAC_JOURNAL_REMOVE_REPAIR, 5000),
		 NTFSMAC_JOURNAL_OK);

	/* A symlink at the entry name is moved, never followed. */
	jt_path(victim, sizeof(victim), base, "victim", NULL);
	jt_write_file(victim, "VICTIM", 6, 0600);
	entry_path(p, sizeof(p), jdir, 0x57);
	CHECK(symlink(victim, p) == 0);
	CHECK_ST(ntfsmac_journal_remove(j, 0x57, JT_TS, NTFSMAC_JOURNAL_REMOVE_JOURNAL_CLEAR, 5000),
		 NTFSMAC_JOURNAL_OK);
	CHECK(lstat(p, &st) != 0);
	CHECK(ntfsmac_journal_format_tombstone(tomb, sizeof(tomb), 0x57, 5000) == 0);
	jt_path(tp, sizeof(tp), jdir, "cleared", tomb);
	CHECK(lstat(tp, &st) == 0 && S_ISLNK(st.st_mode));
	CHECK(jt_read_file(victim, buf, sizeof(buf)) == 6 && memcmp(buf, "VICTIM", 6) == 0);

	/* A directory at the name stays. */
	entry_path(p, sizeof(p), jdir, 0x58);
	CHECK(mkdir(p, 0700) == 0);
	CHECK_ST(ntfsmac_journal_remove(j, 0x58, JT_TS, NTFSMAC_JOURNAL_REMOVE_JOURNAL_CLEAR, 5000),
		 NTFSMAC_JOURNAL_ERR_CORRUPT);
	CHECK(access(p, F_OK) == 0);

	ntfsmac_journal_close(j);
	jt_rmrf(base);
}

/* ------------------------------------------------------------------ */
/* Strict parsing                                                     */
/* ------------------------------------------------------------------ */

#define BASE_SERIAL 0xABull

/* Replace the whole line that starts with `key ` (or equals key) by `line`. */
static size_t set_line(char *text, size_t len, size_t cap, const char *key,
		       const char *line)
{
	char out[2048];
	size_t o = 0, i = 0, klen = strlen(key);
	int done = 0;

	while (i < len) {
		const char *nl = memchr(text + i, '\n', len - i);
		size_t ll = nl ? (size_t)(nl - (text + i)) : len - i;

		if (!done && ll >= klen && memcmp(text + i, key, klen) == 0) {
			size_t n = strlen(line);

			memcpy(out + o, line, n);
			o += n;
			done = 1;
		} else {
			memcpy(out + o, text + i, ll);
			o += ll;
		}
		i += ll;
		if (nl) {
			out[o++] = '\n';
			i++;
		}
	}
	CHECK(done && o < cap);
	memcpy(text, out, o);
	return o;
}

static void test_strict_parsing(void)
{
	char base[PATH_MAX_T], jdir[PATH_MAX_T], p[PATH_MAX_T];
	ntfsmac_journal *j = open_fresh(base, sizeof(base), jdir, sizeof(jdir));
	ntfsmac_journal_entry e = jt_entry(BASE_SERIAL), g;
	char good[2048], text[2048];
	size_t glen;
	struct { const char *key, *line; } cases[] = {
		{ "ntfsmac-journal", "ntfsmac-journal 1" }, /* format 1 under a format-2 name */
		{ "ntfsmac-journal", "ntfsmac-journal 3" },
		{ "ntfsmac-journal", "ntfsmac-journal  2" },
		{ "ntfsmac-journal", "Ntfsmac-journal 2" },
		{ "serial", "serial 00000000000000ab" },
		{ "serial", "serial 0000000000000AB" },
		{ "serial", "serial 000000000000000AB" },
		{ "serial", "serial 00000000000000AC" },
		{ "serial", "serial +0000000000000AB" },
		{ "serial", "serial 00000000000000AG" },
		{ "serial", "serial" },
		{ "label", "label 54455354564F4C" },
		{ "label", "label 54455354564f4" },
		{ "label", "label 0a" },
		{ "label", "label 00" },
		{ "label", "label 7f" },
		{ "label", "label 1f" },
		{ "label", "label 5445 5354" },
		{ "label", "label 6161616161616161616161616161616161616161616161616161616161616161"
			   "6161616161616161616161616161616161616161616161616161616161616161"
			   "61" },
		{ "label", "label" },
		{ "total_sectors", "total_sectors 0976773167" },
		{ "total_sectors", "total_sectors +976773167" },
		{ "total_sectors", "total_sectors 0" },
		{ "total_sectors", "total_sectors -1" },
		{ "total_sectors", "total_sectors 18446744073709551616" },
		{ "total_sectors", "total_sectors 99999999999999999999" },
		{ "total_sectors", "total_sectors  976773167" },
		{ "total_sectors", "total_sectors 976773167 " },
		{ "total_sectors", "total_sectors 0x10" },
		{ "total_sectors", "total_sectors " },
		{ "session_id", "session_id 0" },
		{ "session_id", "session_id 00" },
		{ "session_id", "session_id abc" },
		{ "created", "created 01" },
		{ "created", "created -0" },
		{ "created", "created -01" },
		{ "created", "created 9223372036854775808" },
		{ "created", "created -9223372036854775809" },
		{ "created", "created 1.5" },
		{ "created", "created " },
		{ "updated", "updated --1" },
		{ "updated", "updated 1e3" },
		{ "logfile_lsn_at_start", "logfile_lsn_at_start -1" },
		{ "logfile_lsn_at_start", "logfile_lsn_at_start 18446744073709551616" },
		{ "marker", "marker SET" },
		{ "marker", "marker bogus" },
		{ "marker", "marker " },
		{ "marker", "marker set " },
		{ "marker", "marker sets" },
		{ "marker", "marker" },
		{ "state", "state Open" },
		{ "state", "state write-error" },
		{ "state", "state open2" },
		{ "state", "state" },
	};

	CHECK_ST(ntfsmac_journal_create(j, &e), NTFSMAC_JOURNAL_OK);
	entry_path(p, sizeof(p), jdir, BASE_SERIAL);
	glen = jt_read_file(p, good, sizeof(good));
	CHECK(glen > 0 && glen < sizeof(good));

	/* Every proper prefix fails closed. */
	for (size_t n = 0; n < glen; n++) {
		jt_write_file(p, good, n, 0600);
		CHECK_ST(ntfsmac_journal_get(j, BASE_SERIAL, JT_TS, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);
	}
	jt_write_file(p, good, glen, 0600);
	CHECK_ST(ntfsmac_journal_get(j, BASE_SERIAL, JT_TS, &g), NTFSMAC_JOURNAL_OK);

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		size_t n;

		memcpy(text, good, glen);
		n = set_line(text, glen, sizeof(text), cases[i].key, cases[i].line);
		jt_write_file(p, text, n, 0600);
		if (ntfsmac_journal_get(j, BASE_SERIAL, JT_TS, &g) != NTFSMAC_JOURNAL_ERR_CORRUPT) {
			fprintf(stderr, "FAIL: accepted: %s\n", cases[i].line);
			exit(1);
		}
	}

	/* Structure: trailing data, CRLF, missing LF, NUL, doubled body. */
	memcpy(text, good, glen);
	text[glen] = 'x';
	jt_write_file(p, text, glen + 1, 0600);
	CHECK_ST(ntfsmac_journal_get(j, BASE_SERIAL, JT_TS, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);
	text[glen] = '\n';
	jt_write_file(p, text, glen + 1, 0600);
	CHECK_ST(ntfsmac_journal_get(j, BASE_SERIAL, JT_TS, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);
	memcpy(text, good, glen);
	memcpy(text + glen, good, glen);
	jt_write_file(p, text, 2 * glen, 0600);
	CHECK_ST(ntfsmac_journal_get(j, BASE_SERIAL, JT_TS, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);
	{ /* CRLF */
		size_t o = 0;

		for (size_t i = 0; i < glen; i++) {
			if (good[i] == '\n')
				text[o++] = '\r';
			text[o++] = good[i];
		}
		jt_write_file(p, text, o, 0600);
		CHECK_ST(ntfsmac_journal_get(j, BASE_SERIAL, JT_TS, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);
	}
	memcpy(text, good, glen);
	text[10] = '\0';
	jt_write_file(p, text, glen, 0600);
	CHECK_ST(ntfsmac_journal_get(j, BASE_SERIAL, JT_TS, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);
	{ /* swapped lines */
		const char *a = strstr(good, "created ");
		const char *b = strstr(good, "updated ");
		size_t la = (size_t)(b - a), lb = (size_t)(strstr(good, "logfile_lsn_at_start") - b);
		size_t o = (size_t)(a - good);

		memcpy(text, good, o);
		memcpy(text + o, b, lb);
		memcpy(text + o + lb, a, la);
		memcpy(text + o + lb + la, good + o + la + lb, glen - o - la - lb);
		jt_write_file(p, text, glen, 0600);
		CHECK_ST(ntfsmac_journal_get(j, BASE_SERIAL, JT_TS, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);
	}
	/* Entry of another serial under this name. */
	{
		ntfsmac_journal_entry o = jt_entry(0xAC);
		char op[PATH_MAX_T], ot[2048];
		size_t n;

		CHECK_ST(ntfsmac_journal_create(j, &o), NTFSMAC_JOURNAL_OK);
		entry_path(op, sizeof(op), jdir, 0xAC);
		n = jt_read_file(op, ot, sizeof(ot));
		jt_write_file(p, ot, n, 0600);
		CHECK_ST(ntfsmac_journal_get(j, BASE_SERIAL, JT_TS, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);
	}
	/* A corrupt entry is never repaired by update, and create sees it as existing. */
	e.marker = NTFSMAC_JOURNAL_MARKER_SET;
	CHECK_ST(ntfsmac_journal_update(j, &e), NTFSMAC_JOURNAL_ERR_CORRUPT);
	e.marker = NTFSMAC_JOURNAL_MARKER_PENDING;
	CHECK_ST(ntfsmac_journal_create(j, &e), NTFSMAC_JOURNAL_ERR_EXISTS);

	/* Restore: valid again. */
	jt_write_file(p, good, glen, 0600);
	CHECK_ST(ntfsmac_journal_get(j, BASE_SERIAL, JT_TS, &g), NTFSMAC_JOURNAL_OK);

	/* File properties: mode, size, type. */
	CHECK(chmod(p, 0666) == 0);
	CHECK_ST(ntfsmac_journal_get(j, BASE_SERIAL, JT_TS, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);
	CHECK(chmod(p, 0620) == 0);
	CHECK_ST(ntfsmac_journal_get(j, BASE_SERIAL, JT_TS, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);
	CHECK(chmod(p, 0600) == 0);
	CHECK_ST(ntfsmac_journal_get(j, BASE_SERIAL, JT_TS, &g), NTFSMAC_JOURNAL_OK);
	{
		char big[4096];

		memset(big, 'a', sizeof(big));
		memcpy(big, good, glen);
		jt_write_file(p, big, sizeof(big), 0600);
		CHECK_ST(ntfsmac_journal_get(j, BASE_SERIAL, JT_TS, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);
	}
	CHECK(unlink(p) == 0);
	CHECK(mkfifo(p, 0600) == 0); /* must not hang the open */
	CHECK_ST(ntfsmac_journal_get(j, BASE_SERIAL, JT_TS, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);
	CHECK(unlink(p) == 0);
	CHECK(mkdir(p, 0700) == 0);
	CHECK_ST(ntfsmac_journal_get(j, BASE_SERIAL, JT_TS, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);
	CHECK(rmdir(p) == 0);
	{ /* symlink at the name to a perfectly valid entry file */
		char target[PATH_MAX_T];

		jt_path(target, sizeof(target), base, "valid-copy", NULL);
		jt_write_file(target, good, glen, 0600);
		CHECK(symlink(target, p) == 0);
		CHECK_ST(ntfsmac_journal_get(j, BASE_SERIAL, JT_TS, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);
	}

	ntfsmac_journal_close(j);
	jt_rmrf(base);
}

struct list_ctx {
	unsigned n;
	uint64_t serial[16];
	uint64_t ts[16];
	unsigned stop_after;
};

static int list_cb(const ntfsmac_journal_entry *e, void *ctx)
{
	struct list_ctx *c = ctx;

	if (c->n < 16) {
		c->serial[c->n] = e->serial;
		c->ts[c->n] = e->total_sectors;
	}
	c->n++;
	return c->stop_after != 0 && c->n >= c->stop_after;
}

static void test_list(void)
{
	char base[PATH_MAX_T], jdir[PATH_MAX_T], p[PATH_MAX_T];
	ntfsmac_journal *j = open_fresh(base, sizeof(base), jdir, sizeof(jdir));
	struct list_ctx c;
	unsigned skipped = 99;

	CHECK_ST(ntfsmac_journal_list(j, list_cb, &c, &skipped), NTFSMAC_JOURNAL_OK);
	CHECK(skipped == 0);
	memset(&c, 0, sizeof(c));
	for (uint64_t s = 3; s >= 1; s--)
		make_entry(j, s, NTFSMAC_JOURNAL_MARKER_NONE, NTFSMAC_JOURNAL_STATE_OPEN);
	entry_path(p, sizeof(p), jdir, 9);
	jt_write_file(p, "garbage", 7, 0600);
	jt_path(p, sizeof(p), jdir, ".0000000000000001.tmp", NULL);
	jt_write_file(p, "stray", 5, 0600);
	jt_path(p, sizeof(p), jdir, "not-an-entry", NULL);
	jt_write_file(p, "x", 1, 0600);
	CHECK_ST(ntfsmac_journal_list(j, list_cb, &c, &skipped), NTFSMAC_JOURNAL_OK);
	CHECK(c.n == 3 && c.serial[0] == 1 && c.serial[1] == 2 && c.serial[2] == 3);
	CHECK(skipped == 1);
	memset(&c, 0, sizeof(c));
	c.stop_after = 2;
	CHECK_ST(ntfsmac_journal_list(j, list_cb, &c, NULL), NTFSMAC_JOURNAL_OK);
	CHECK(c.n == 2);
	CHECK_ST(ntfsmac_journal_list(j, NULL, &c, NULL), NTFSMAC_JOURNAL_ERR_INVALID);
	CHECK_ST(ntfsmac_journal_list_tombstones(j, NULL, NULL), NTFSMAC_JOURNAL_ERR_INVALID);
	ntfsmac_journal_close(j);
	jt_rmrf(base);
}

/*
 * U-JRN-LIST-02 (T-M7-15): more entries than the bound of one listing (a small bound through the
 * parameter of ntfsmac_journal_list_ex): the listing is cut, not failed; it says how many it left
 * out; no entry is removed; every entry is still found by serial.
 */
static void test_list_bound(void)
{
	char base[PATH_MAX_T], jdir[PATH_MAX_T];
	ntfsmac_journal *j = open_fresh(base, sizeof(base), jdir, sizeof(jdir));
	ntfsmac_journal_list_info info;
	ntfsmac_journal_entry g;
	struct list_ctx c;

	for (uint64_t s = 1; s <= 7; s++)
		make_entry(j, s, NTFSMAC_JOURNAL_MARKER_NONE, NTFSMAC_JOURNAL_STATE_OPEN);
	memset(&c, 0, sizeof(c));
	CHECK_ST(ntfsmac_journal_list_ex(j, 3, list_cb, &c, &info), NTFSMAC_JOURNAL_OK);
	CHECK(c.n == 3);
	CHECK(info.found == 7 && info.unlisted == 4 && info.skipped == 0);
	CHECK(c.serial[0] == 1 && c.serial[1] == 2 && c.serial[2] == 3); /* the lowest, sorted */
	/* the default bound lists them all */
	memset(&c, 0, sizeof(c));
	CHECK_ST(ntfsmac_journal_list_ex(j, 0, list_cb, &c, &info), NTFSMAC_JOURNAL_OK);
	CHECK(c.n == 7 && info.found == 7 && info.unlisted == 0);
	CHECK_ST(ntfsmac_journal_list_ex(j, 3, list_cb, &c, NULL), NTFSMAC_JOURNAL_OK);
	CHECK_ST(ntfsmac_journal_list_ex(j, 3, NULL, &c, &info), NTFSMAC_JOURNAL_ERR_INVALID);
	/* nothing was removed by a cut listing */
	for (uint64_t s = 1; s <= 7; s++)
		CHECK_ST(ntfsmac_journal_get(j, s, JT_TS, &g), NTFSMAC_JOURNAL_OK);
	/*
	 * T-M7-13: format-2 identities share a serial; the cut keeps the lowest identities, ordered
	 * by serial and then total_sectors, whatever the order of creation (so of the directory).
	 */
	{
		static const uint64_t ts[] = {JT_TS + 5, JT_TS + 2, JT_TS + 9, JT_TS + 1};
		ntfsmac_journal_entry e;
		struct list_ctx d;
		unsigned k, got = 0;

		for (k = 0; k < 4; k++) {
			e = jt_entry(1);
			e.total_sectors = ts[k];
			e.session_id = 100 + k;
			CHECK_ST(ntfsmac_journal_create(j, &e), NTFSMAC_JOURNAL_OK);
		}
		/* 11 entries: serial 1 with five sizes, then serials 2..7; keep 3 */
		memset(&d, 0, sizeof(d));
		CHECK_ST(ntfsmac_journal_list_ex(j, 3, list_cb, &d, &info), NTFSMAC_JOURNAL_OK);
		CHECK(d.n == 3 && info.found == 11 && info.unlisted == 8);
		for (k = 0; k < 3; k++)
			got += d.serial[k] == 1;
		CHECK(got == 3); /* three identities of serial 1, none of a higher serial */
		CHECK(d.ts[0] == JT_TS && d.ts[1] == JT_TS + 1 && d.ts[2] == JT_TS + 2);
		memset(&d, 0, sizeof(d));
		CHECK_ST(ntfsmac_journal_list_ex(j, 6, list_cb, &d, &info), NTFSMAC_JOURNAL_OK);
		CHECK(d.n == 6 && d.serial[4] == 1 && d.ts[4] == JT_TS + 9 && d.serial[5] == 2);
	}
	ntfsmac_journal_close(j);
	jt_rmrf(base);
}

/* ------------------------------------------------------------------ */
/* U-JRN-NAME-01                                                      */
/* ------------------------------------------------------------------ */

static int is_hex16_upper(const char *s)
{
	if (strlen(s) != 16)
		return 0;
	for (int i = 0; i < 16; i++)
		if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'A' && s[i] <= 'F')))
			return 0;
	return 1;
}

/* <16 hex> (format 1) or <16 hex>-<16 hex> (format 2, T-M7-13) */
static int is_entry_name(const char *name)
{
	char a[17], b[17];

	if (strlen(name) == 16)
		return is_hex16_upper(name);
	if (strlen(name) != 33 || name[16] != '-')
		return 0;
	memcpy(a, name, 16);
	a[16] = '\0';
	memcpy(b, name + 17, 16);
	b[16] = '\0';
	return is_hex16_upper(a) && is_hex16_upper(b);
}

static int is_entry_or_cleared(const char *name)
{
	if (!jt_is_any(name))
		return 1;
	return strcmp(name, "cleared") == 0 || is_entry_name(name);
}

static int not_entry_or_cleared(const char *name)
{
	return !is_entry_or_cleared(name);
}

static int is_tomb_name(const char *name)
{
	char head[17];

	if (strlen(name) != 33 || name[16] != '-' || name[25] != 'T' || name[32] != 'Z')
		return 0;
	memcpy(head, name, 16);
	head[16] = '\0';
	for (int i = 17; i < 32; i++)
		if (i != 25 && (name[i] < '0' || name[i] > '9'))
			return 0;
	return is_hex16_upper(head);
}

static int not_tomb_name(const char *name)
{
	return jt_is_any(name) && !is_tomb_name(name);
}

static void test_naming_u_jrn_name_01(void)
{
	char base[PATH_MAX_T], jdir[PATH_MAX_T], p[PATH_MAX_T], victim[PATH_MAX_T],
		cdir[PATH_MAX_T], buf[16], tomb[NTFSMAC_JOURNAL_TOMBNAME_SIZE];
	ntfsmac_journal *j = open_fresh(base, sizeof(base), jdir, sizeof(jdir));
	/* 0x2E2E2F2E2E2F2E2E reads "../../.." as ASCII bytes. */
	const uint64_t weird[] = { 0, 1, UINT64_MAX, 0x2E2E2F2E2E2F2E2Eull, 0x00000000000000FFull };
	ntfsmac_journal_entry g, e;
	struct stat st;

	/* Weird serials: names are exactly 16 upper-case hex digits. */
	for (size_t i = 0; i < sizeof(weird) / sizeof(weird[0]); i++) {
		e = jt_entry(weird[i]);
		CHECK_ST(ntfsmac_journal_create(j, &e), NTFSMAC_JOURNAL_OK);
		CHECK_ST(ntfsmac_journal_get(j, weird[i], JT_TS, &g), NTFSMAC_JOURNAL_OK);
	}
	CHECK(jt_count_names(jdir, jt_is_any) == (int)(sizeof(weird) / sizeof(weird[0])) + 1);
	CHECK(jt_count_names(jdir, not_entry_or_cleared) == 0);
	for (size_t i = 0; i < sizeof(weird) / sizeof(weird[0]); i++) {
		CHECK_ST(ntfsmac_journal_remove(j, weird[i], JT_TS, NTFSMAC_JOURNAL_REMOVE_JOURNAL_CLEAR,
						1700000000 + (int64_t)i),
			 NTFSMAC_JOURNAL_OK);
	}
	jt_path(cdir, sizeof(cdir), jdir, "cleared", NULL);
	CHECK(jt_count_names(cdir, jt_is_any) == (int)(sizeof(weird) / sizeof(weird[0])));
	CHECK(jt_count_names(cdir, not_tomb_name) == 0);
	CHECK(jt_count_names(jdir, jt_is_tmp) == 0);
	CHECK(ntfsmac_journal_format_tombstone(tomb, sizeof(tomb), 0x2E2E2F2E2E2F2E2Eull,
					       1700000003) == 0);
	jt_path(p, sizeof(p), jdir, "cleared", tomb);
	CHECK(lstat(p, &st) == 0 && S_ISREG(st.st_mode));

	/* Pre-planted symlink at the entry name: never followed. */
	jt_path(victim, sizeof(victim), base, "victim", NULL);
	jt_write_file(victim, "VICTIM", 6, 0600);
	entry_path(p, sizeof(p), jdir, 0xA1);
	CHECK(symlink(victim, p) == 0);
	e = jt_entry(0xA1);
	CHECK_ST(ntfsmac_journal_create(j, &e), NTFSMAC_JOURNAL_ERR_EXISTS);
	CHECK(lstat(p, &st) == 0 && S_ISLNK(st.st_mode));
	CHECK(jt_read_file(victim, buf, sizeof(buf)) == 6 && memcmp(buf, "VICTIM", 6) == 0);
	CHECK_ST(ntfsmac_journal_get(j, 0xA1, JT_TS, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);
	CHECK_ST(ntfsmac_journal_update(j, &e), NTFSMAC_JOURNAL_ERR_CORRUPT);
	CHECK(jt_read_file(victim, buf, sizeof(buf)) == 6 && memcmp(buf, "VICTIM", 6) == 0);
	/* Dangling symlink at the entry name: also "exists", target not created. */
	entry_path(p, sizeof(p), jdir, 0xA2);
	jt_path(cdir, sizeof(cdir), base, "not-created", NULL);
	CHECK(symlink(cdir, p) == 0);
	e = jt_entry(0xA2);
	CHECK_ST(ntfsmac_journal_create(j, &e), NTFSMAC_JOURNAL_ERR_EXISTS);
	CHECK(lstat(cdir, &st) != 0);

	/* Pre-planted symlink at the temporary name: not followed, create works. */
	for (int round = 0; round < 2; round++) {
		char tname[NTFSMAC_JOURNAL_IDTMPNAME_SIZE];

		e = jt_entry(0xA3);
		CHECK(ntfsmac_journal_format_idtmpname(tname, sizeof(tname), 0xA3, JT_TS) == 0);
		jt_path(p, sizeof(p), jdir, tname, NULL);
		CHECK(symlink(victim, p) == 0);
		if (round == 0) {
			CHECK_ST(ntfsmac_journal_create(j, &e), NTFSMAC_JOURNAL_OK);
		} else {
			e.marker = NTFSMAC_JOURNAL_MARKER_SET;
			e.updated++;
			CHECK_ST(ntfsmac_journal_update(j, &e), NTFSMAC_JOURNAL_OK);
		}
		CHECK(jt_read_file(victim, buf, sizeof(buf)) == 6 && memcmp(buf, "VICTIM", 6) == 0);
		CHECK(lstat(p, &st) != 0); /* the planted name was removed, not written through */
		entry_path(p, sizeof(p), jdir, 0xA3);
		CHECK(lstat(p, &st) == 0 && S_ISREG(st.st_mode));
		CHECK_ST(ntfsmac_journal_get(j, 0xA3, JT_TS, &g), NTFSMAC_JOURNAL_OK);
		CHECK(jt_entry_eq(&e, &g));
	}
	/* A planted tombstone name that is a symlink counts as a collision. */
	make_entry(j, 0xA4, NTFSMAC_JOURNAL_MARKER_NONE, NTFSMAC_JOURNAL_STATE_OPEN);
	CHECK(ntfsmac_journal_format_tombstone(tomb, sizeof(tomb), 0xA4, 1800000000) == 0);
	jt_path(p, sizeof(p), jdir, "cleared", tomb);
	CHECK(symlink(victim, p) == 0);
	CHECK_ST(ntfsmac_journal_remove(j, 0xA4, JT_TS, NTFSMAC_JOURNAL_REMOVE_JOURNAL_CLEAR, 1800000000),
		 NTFSMAC_JOURNAL_ERR_EXISTS);
	CHECK_ST(ntfsmac_journal_get(j, 0xA4, JT_TS, &g), NTFSMAC_JOURNAL_OK);
	CHECK(jt_read_file(victim, buf, sizeof(buf)) == 6 && memcmp(buf, "VICTIM", 6) == 0);

	ntfsmac_journal_close(j);
	jt_rmrf(base);
}

static int count_cb(const ntfsmac_journal_entry *e, void *ctx)
{
	(void)e;
	(*(int *)ctx)++;
	return 0;
}

/*
 * Rewrite the first line of an entry file ("ntfsmac-journal N") to `head`; same length. For
 * format 1 the `mount` line of format 2 is dropped as well, so the text is a format-1 entry.
 */
static void set_version(const char *path, const char *head, const char *out_path)
{
	char text[2048];
	size_t n = jt_read_file(path, text, sizeof(text));
	char *m;

	CHECK(n > 17 && n < sizeof(text) && strlen(head) == 17);
	text[n] = '\0';
	memcpy(text, head, 17);
	if (head[16] == '1' && (m = strstr(text, "\nmount ")) != NULL) {
		char *eol = strchr(m + 1, '\n');

		CHECK(eol != NULL);
		if (eol != NULL) {
			memmove(m, eol, strlen(eol) + 1);
			n = strlen(text);
		}
	}
	jt_write_file(out_path, text, n, 0600);
}

/*
 * U-JRN-ID-01..04 (T-M7-13): the journal is keyed by the volume identity (serial and
 * total_sectors). Two volumes with one serial have an entry each; a lookup, an update and a
 * removal address the right one; an entry of format 1 (by serial alone) is read, applies to
 * every volume of its serial, blocks a new entry of that serial, is updated in format 1 and can
 * be removed; the version line must match the kind of name.
 */
static void test_identity(void)
{
	char base[PATH_MAX_T], jdir[PATH_MAX_T], p[PATH_MAX_T], lp[PATH_MAX_T];
	char n[NTFSMAC_JOURNAL_IDNAME_SIZE], t[NTFSMAC_JOURNAL_IDTMPNAME_SIZE];
	ntfsmac_journal *j = open_fresh(base, sizeof(base), jdir, sizeof(jdir));
	ntfsmac_journal_entry a = jt_entry(0xC0), b = jt_entry(0xC0), g;

	/* names */
	CHECK(ntfsmac_journal_format_idname(n, sizeof(n), 0xAB, 0x10) == 0);
	CHECK(strcmp(n, "00000000000000AB-0000000000000010") == 0);
	CHECK(ntfsmac_journal_format_idname(n, sizeof(n) - 1, 0xAB, 0x10) == -1);
	CHECK(ntfsmac_journal_format_idname(NULL, sizeof(n), 0xAB, 0x10) == -1);
	CHECK(ntfsmac_journal_format_idtmpname(t, sizeof(t), UINT64_MAX, 1) == 0);
	CHECK(strcmp(t, ".FFFFFFFFFFFFFFFF-0000000000000001.tmp") == 0);
	CHECK(ntfsmac_journal_format_idtmpname(t, sizeof(t) - 1, 1, 1) == -1);

	/* U-JRN-ID-01: two volumes with one serial, each its own entry */
	b.total_sectors = JT_TS + 1;
	b.session_id = 2;
	CHECK_ST(ntfsmac_journal_create(j, &a), NTFSMAC_JOURNAL_OK);
	CHECK_ST(ntfsmac_journal_create(j, &b), NTFSMAC_JOURNAL_OK);
	CHECK_ST(ntfsmac_journal_create(j, &b), NTFSMAC_JOURNAL_ERR_EXISTS);
	CHECK_ST(ntfsmac_journal_get(j, 0xC0, JT_TS, &g), NTFSMAC_JOURNAL_OK);
	CHECK(jt_entry_eq(&a, &g) && !g.legacy);
	CHECK_ST(ntfsmac_journal_get(j, 0xC0, JT_TS + 1, &g), NTFSMAC_JOURNAL_OK);
	CHECK(jt_entry_eq(&b, &g));
	CHECK_ST(ntfsmac_journal_get(j, 0xC0, JT_TS + 2, &g), NTFSMAC_JOURNAL_ERR_NOT_FOUND);
	CHECK_ST(ntfsmac_journal_get(j, 0xC0, 0, &g), NTFSMAC_JOURNAL_ERR_NOT_FOUND);
	b.marker = NTFSMAC_JOURNAL_MARKER_SET;
	CHECK_ST(ntfsmac_journal_update(j, &b), NTFSMAC_JOURNAL_OK);
	CHECK_ST(ntfsmac_journal_get(j, 0xC0, JT_TS, &g), NTFSMAC_JOURNAL_OK);
	CHECK(g.marker == NTFSMAC_JOURNAL_MARKER_PENDING); /* a's entry did not change */
	CHECK_ST(ntfsmac_journal_remove(j, 0xC0, JT_TS + 1, NTFSMAC_JOURNAL_REMOVE_JOURNAL_CLEAR, 7000),
		 NTFSMAC_JOURNAL_OK);
	CHECK_ST(ntfsmac_journal_get(j, 0xC0, JT_TS + 1, &g), NTFSMAC_JOURNAL_ERR_NOT_FOUND);
	CHECK_ST(ntfsmac_journal_get(j, 0xC0, JT_TS, &g), NTFSMAC_JOURNAL_OK);
	CHECK(jt_entry_eq(&a, &g)); /* the other volume's entry stays */
	/* a file under a format-2 name whose total_sectors is not the name's: corrupt */
	entry_path(p, sizeof(p), jdir, 0xC0);
	CHECK(ntfsmac_journal_format_idname(n, sizeof(n), 0xC0, JT_TS + 5) == 0);
	jt_path(lp, sizeof(lp), jdir, n, NULL);
	{
		char text[2048];
		size_t len = jt_read_file(p, text, sizeof(text));

		jt_write_file(lp, text, len, 0600);
	}
	CHECK_ST(ntfsmac_journal_get(j, 0xC0, JT_TS + 5, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);

	/* U-JRN-ID-02: format 1 is read; it applies to every volume of its serial */
	a = jt_entry(0xC1);
	CHECK_ST(ntfsmac_journal_create(j, &a), NTFSMAC_JOURNAL_OK);
	entry_path(p, sizeof(p), jdir, 0xC1);
	legacy_path(lp, sizeof(lp), jdir, 0xC1);
	set_version(p, "ntfsmac-journal 1", lp);
	CHECK(unlink(p) == 0);
	CHECK_ST(ntfsmac_journal_get(j, 0xC1, JT_TS, &g), NTFSMAC_JOURNAL_OK);
	CHECK(g.legacy && g.session_id == a.session_id);
	CHECK_ST(ntfsmac_journal_get(j, 0xC1, 12345, &g), NTFSMAC_JOURNAL_OK);
	CHECK(g.legacy);
	CHECK_ST(ntfsmac_journal_get(j, 0xC1, 0, &g), NTFSMAC_JOURNAL_OK);
	/* it blocks a new entry of the serial, whatever its size */
	CHECK_ST(ntfsmac_journal_create(j, &a), NTFSMAC_JOURNAL_ERR_EXISTS);
	b = jt_entry(0xC1);
	b.total_sectors = 77;
	CHECK_ST(ntfsmac_journal_create(j, &b), NTFSMAC_JOURNAL_ERR_EXISTS);
	/* create never writes format 1 */
	b.legacy = true;
	CHECK_ST(ntfsmac_journal_create(j, &b), NTFSMAC_JOURNAL_ERR_INVALID);
	/* updated in place, in format 1 */
	g.marker = NTFSMAC_JOURNAL_MARKER_SET;
	CHECK_ST(ntfsmac_journal_update(j, &g), NTFSMAC_JOURNAL_OK);
	CHECK_ST(ntfsmac_journal_get(j, 0xC1, 99, &g), NTFSMAC_JOURNAL_OK);
	CHECK(g.legacy && g.marker == NTFSMAC_JOURNAL_MARKER_SET);
	{
		char text[32];

		CHECK(jt_read_file(lp, text, sizeof(text)) > 17 && memcmp(text, "ntfsmac-journal 1\n", 18) == 0);
	}
	CHECK(access(p, F_OK) != 0); /* no format-2 file appeared */
	/* the version line must match the name */
	set_version(lp, "ntfsmac-journal 2", lp);
	CHECK_ST(ntfsmac_journal_get(j, 0xC1, JT_TS, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);
	set_version(lp, "ntfsmac-journal 1", lp);
	CHECK_ST(ntfsmac_journal_get(j, 0xC1, JT_TS, &g), NTFSMAC_JOURNAL_OK);
	a = jt_entry(0xC2);
	CHECK_ST(ntfsmac_journal_create(j, &a), NTFSMAC_JOURNAL_OK);
	entry_path(p, sizeof(p), jdir, 0xC2);
	set_version(p, "ntfsmac-journal 1", p);
	CHECK_ST(ntfsmac_journal_get(j, 0xC2, JT_TS, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);
	CHECK_ST(ntfsmac_journal_remove(j, 0xC2, JT_TS, NTFSMAC_JOURNAL_REMOVE_JOURNAL_CLEAR, 7001),
		 NTFSMAC_JOURNAL_OK);

	/* U-JRN-ID-03: a volume's own entry comes first; format 1 is the fall-back */
	a = jt_entry(0xC3);
	CHECK_ST(ntfsmac_journal_create(j, &a), NTFSMAC_JOURNAL_OK);
	entry_path(p, sizeof(p), jdir, 0xC3);
	legacy_path(lp, sizeof(lp), jdir, 0xC3);
	set_version(p, "ntfsmac-journal 1", lp); /* planted: both exist */
	CHECK_ST(ntfsmac_journal_get(j, 0xC3, JT_TS, &g), NTFSMAC_JOURNAL_OK);
	CHECK(!g.legacy);
	CHECK_ST(ntfsmac_journal_get(j, 0xC3, JT_TS + 1, &g), NTFSMAC_JOURNAL_OK);
	CHECK(g.legacy);
	CHECK_ST(ntfsmac_journal_remove(j, 0xC3, JT_TS, NTFSMAC_JOURNAL_REMOVE_JOURNAL_CLEAR, 7002),
		 NTFSMAC_JOURNAL_OK);
	CHECK(access(p, F_OK) != 0 && access(lp, F_OK) == 0);
	CHECK_ST(ntfsmac_journal_remove(j, 0xC3, JT_TS, NTFSMAC_JOURNAL_REMOVE_JOURNAL_CLEAR, 7003),
		 NTFSMAC_JOURNAL_OK);
	CHECK(access(lp, F_OK) != 0);

	/* U-JRN-ID-04: list gives both kinds; open removes stray temporaries of both kinds */
	{
		int count = 0;
		unsigned skipped = 0;

		CHECK_ST(ntfsmac_journal_list(j, count_cb, &count, &skipped), NTFSMAC_JOURNAL_OK);
		CHECK(count == 2);   /* 0xC0 of size JT_TS, 0xC1 of format 1 */
		CHECK(skipped == 1); /* 0xC0 of size JT_TS + 5: corrupt */
	}
	ntfsmac_journal_close(j);
	jt_path(p, sizeof(p), jdir, ".00000000000000C0-0000000000000001.tmp", NULL);
	jt_write_file(p, "partial", 7, 0600);
	jt_path(p, sizeof(p), jdir, ".00000000000000C0-0000000000000000.tmp", NULL); /* size 0: not a name */
	jt_write_file(p, "keep", 4, 0600);
	CHECK_ST(ntfsmac_journal_open(jdir, &j), NTFSMAC_JOURNAL_OK);
	CHECK(jt_count_names(jdir, jt_is_tmp) == 1);
	ntfsmac_journal_close(j);
	jt_rmrf(base);
}

/*
 * U-JRN-MNT-01 (T-M7-12): the mount record of an entry. Set once by update and read back;
 * never changed or dropped afterwards; never on a format-1 entry; the `mount` line is parsed
 * strictly.
 */
static void test_mount_record(void)
{
	char base[PATH_MAX_T], jdir[PATH_MAX_T], p[PATH_MAX_T], good[2048], text[2048];
	ntfsmac_journal *j = open_fresh(base, sizeof(base), jdir, sizeof(jdir));
	ntfsmac_journal_entry e = jt_entry(0xD0), g, x;
	size_t glen;
	static const char *const bad[] = {
		"mount",
		"mount ",
		"mount  -",
		"mount 501",
		"mount 501 0000000000000AF5",
		"mount 501 0000000000000AF5 ",
		"mount 501 0000000000000af5 6162",
		"mount 0501 0000000000000AF5 6162",
		"mount 4294967296 0000000000000AF5 6162",
		"mount 501 0000000000000AF5 616",
		"mount 501 0000000000000AF5 6109",
		"mount 501 0000000000000AF5 6G62",
		"mount 501  0000000000000AF5 6162",
		"mount 501 0000000000000AF5 6162 ",
	};

	CHECK_ST(ntfsmac_journal_create(j, &e), NTFSMAC_JOURNAL_OK);
	CHECK_ST(ntfsmac_journal_get(j, 0xD0, JT_TS, &g), NTFSMAC_JOURNAL_OK);
	CHECK(!g.has_mount);
	x = g;
	x.has_mount = true;
	x.mount_owner = 501;
	x.mount_fsid = 0xAF5;
	snprintf(x.mount_from, sizeof(x.mount_from), "ntfs-3g@macfuse0");
	CHECK_ST(ntfsmac_journal_update(j, &x), NTFSMAC_JOURNAL_OK);
	CHECK_ST(ntfsmac_journal_get(j, 0xD0, JT_TS, &g), NTFSMAC_JOURNAL_OK);
	CHECK(g.has_mount && g.mount_owner == 501 && g.mount_fsid == 0xAF5 &&
	      strcmp(g.mount_from, "ntfs-3g@macfuse0") == 0);
	/* kept by later changes, never changed or dropped */
	g.marker = NTFSMAC_JOURNAL_MARKER_SET;
	CHECK_ST(ntfsmac_journal_update(j, &g), NTFSMAC_JOURNAL_OK);
	x = g;
	x.mount_owner = 502;
	CHECK_ST(ntfsmac_journal_update(j, &x), NTFSMAC_JOURNAL_ERR_IMMUTABLE);
	x = g;
	snprintf(x.mount_from, sizeof(x.mount_from), "other");
	CHECK_ST(ntfsmac_journal_update(j, &x), NTFSMAC_JOURNAL_ERR_IMMUTABLE);
	x = g;
	x.has_mount = false;
	CHECK_ST(ntfsmac_journal_update(j, &x), NTFSMAC_JOURNAL_ERR_IMMUTABLE);
	/* an unprintable or empty source is refused */
	x = g;
	x.mount_from[0] = '\t';
	CHECK_ST(ntfsmac_journal_update(j, &x), NTFSMAC_JOURNAL_ERR_INVALID);
	x.mount_from[0] = '\0';
	CHECK_ST(ntfsmac_journal_update(j, &x), NTFSMAC_JOURNAL_ERR_INVALID);
	/* a format-1 entry has no record */
	x = g;
	x.legacy = true;
	CHECK_ST(ntfsmac_journal_update(j, &x), NTFSMAC_JOURNAL_ERR_INVALID);

	/* strict parsing of the line */
	entry_path(p, sizeof(p), jdir, 0xD0);
	glen = jt_read_file(p, good, sizeof(good));
	CHECK(glen > 0 && strstr(good, "\nmount 501 0000000000000AF5 6e7466732d3367406d61636675736530\n") != NULL);
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		size_t n;

		memcpy(text, good, glen);
		n = set_line(text, glen, sizeof(text), "mount", bad[i]);
		jt_write_file(p, text, n, 0600);
		if (ntfsmac_journal_get(j, 0xD0, JT_TS, &g) != NTFSMAC_JOURNAL_ERR_CORRUPT) {
			fprintf(stderr, "FAIL: accepted: %s\n", bad[i]);
			exit(1);
		}
	}
	jt_write_file(p, good, glen, 0600);
	CHECK_ST(ntfsmac_journal_get(j, 0xD0, JT_TS, &g), NTFSMAC_JOURNAL_OK);
	ntfsmac_journal_close(j);
	jt_rmrf(base);
}

int main(void)
{
	ntfsmac_log_set_level(NTFSMAC_LOG_NONE);
	RUN(test_mount_record);
	RUN(test_identity);
	RUN(test_names);
	RUN(test_transitions);
	RUN(test_open);
	RUN(test_open_cleans_stray_temporaries);
	RUN(test_create_get);
	RUN(test_update);
	RUN(test_removal_paths);
	RUN(test_tombstones);
	RUN(test_removal_of_damaged_entries);
	RUN(test_strict_parsing);
	RUN(test_list);
	RUN(test_list_bound);
	RUN(test_naming_u_jrn_name_01);
	printf("journal.unit: %d test groups passed\n", g_tests);
	return 0;
}
