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

static void entry_path(char *out, size_t size, const char *jdir, uint64_t serial)
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

	CHECK_ST(ntfsmac_journal_get(j, 0xAB, &g), NTFSMAC_JOURNAL_ERR_NOT_FOUND);
	CHECK_ST(ntfsmac_journal_create(j, &e), NTFSMAC_JOURNAL_OK);
	CHECK_ST(ntfsmac_journal_get(j, 0xAB, &g), NTFSMAC_JOURNAL_OK);
	CHECK(jt_entry_eq(&e, &g));
	entry_path(p, sizeof(p), jdir, 0xAB);
	CHECK(stat(p, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0777) == 0600);
	CHECK(jt_count_names(jdir, jt_is_tmp) == 0);

	/* Create never replaces. */
	e.label[0] = 'X';
	CHECK_ST(ntfsmac_journal_create(j, &e), NTFSMAC_JOURNAL_ERR_EXISTS);
	CHECK_ST(ntfsmac_journal_get(j, 0xAB, &g), NTFSMAC_JOURNAL_OK);
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
		CHECK_ST(ntfsmac_journal_get(j, serials[i], &g), NTFSMAC_JOURNAL_OK);
		CHECK(jt_entry_eq(&x, &g));
	}
	{ /* Empty label, 64-byte label with UTF-8. */
		ntfsmac_journal_entry x = jt_entry(0x77);

		x.label[0] = '\0';
		CHECK_ST(ntfsmac_journal_create(j, &x), NTFSMAC_JOURNAL_OK);
		CHECK_ST(ntfsmac_journal_get(j, 0x77, &g), NTFSMAC_JOURNAL_OK);
		CHECK(jt_entry_eq(&x, &g));
		x = jt_entry(0x78);
		memset(x.label, 'a', 62);
		x.label[62] = (char)0xC3;
		x.label[63] = (char)0xA9; /* 64 bytes */
		x.label[64] = '\0';
		CHECK_ST(ntfsmac_journal_create(j, &x), NTFSMAC_JOURNAL_OK);
		CHECK_ST(ntfsmac_journal_get(j, 0x78, &g), NTFSMAC_JOURNAL_OK);
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
	CHECK_ST(ntfsmac_journal_get(j, 0x90, &g), NTFSMAC_JOURNAL_ERR_NOT_FOUND);
	CHECK_ST(ntfsmac_journal_create(NULL, &e), NTFSMAC_JOURNAL_ERR_INVALID);
	CHECK_ST(ntfsmac_journal_create(j, NULL), NTFSMAC_JOURNAL_ERR_INVALID);
	CHECK_ST(ntfsmac_journal_get(j, 1, NULL), NTFSMAC_JOURNAL_ERR_INVALID);
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
	CHECK_ST(ntfsmac_journal_get(j, 0xC1, &g), NTFSMAC_JOURNAL_OK);
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
		for (int i = 0; i < 5; i++)
			CHECK_ST(ntfsmac_journal_update(j, &m[i]), NTFSMAC_JOURNAL_ERR_IMMUTABLE);
		CHECK_ST(ntfsmac_journal_get(j, 0xC1, &g), NTFSMAC_JOURNAL_OK);
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
	CHECK_ST(ntfsmac_journal_get(j, 0xC1, &g), NTFSMAC_JOURNAL_OK);
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
				CHECK_ST(ntfsmac_journal_get(j, serial, &before), NTFSMAC_JOURNAL_OK);
				tombs(j, &t);
				{
					unsigned t0 = t.n;
					ntfsmac_journal_status st =
						ntfsmac_journal_remove(j, serial, P[p], now);

					if (ok) {
						CHECK(st == NTFSMAC_JOURNAL_OK);
						CHECK_ST(ntfsmac_journal_get(j, serial, &after),
							 NTFSMAC_JOURNAL_ERR_NOT_FOUND);
						tombs(j, &t);
						CHECK(t.n == t0 + 1 && has_tomb(&t, serial, now));
						allowed++;
					} else {
						CHECK(st == NTFSMAC_JOURNAL_ERR_PATH_CONDITION);
						CHECK_ST(ntfsmac_journal_get(j, serial, &after),
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
	CHECK_ST(ntfsmac_journal_remove(j, 0xDEAD, NTFSMAC_JOURNAL_REMOVE_JOURNAL_CLEAR, now),
		 NTFSMAC_JOURNAL_ERR_NOT_FOUND);
	make_entry(j, 0xBEEF, NTFSMAC_JOURNAL_MARKER_NONE, NTFSMAC_JOURNAL_STATE_OPEN);
	CHECK_ST(ntfsmac_journal_remove(j, 0xBEEF, (ntfsmac_journal_removal)0, now),
		 NTFSMAC_JOURNAL_ERR_INVALID);
	CHECK_ST(ntfsmac_journal_remove(j, 0xBEEF, (ntfsmac_journal_removal)5, now),
		 NTFSMAC_JOURNAL_ERR_INVALID);
	CHECK_ST(ntfsmac_journal_remove(j, 0xBEEF, NTFSMAC_JOURNAL_REMOVE_REPAIR, INT64_MAX),
		 NTFSMAC_JOURNAL_ERR_INVALID);
	CHECK_ST(ntfsmac_journal_remove(NULL, 0xBEEF, NTFSMAC_JOURNAL_REMOVE_REPAIR, now),
		 NTFSMAC_JOURNAL_ERR_INVALID);
	{
		ntfsmac_journal_entry g;

		CHECK_ST(ntfsmac_journal_get(j, 0xBEEF, &g), NTFSMAC_JOURNAL_OK);
	}

	/* The removal is logged with the path. */
	ntfsmac_log_set_sink(log_capture, NULL);
	ntfsmac_log_set_level(NTFSMAC_LOG_INFO);
	g_log[0] = '\0';
	CHECK_ST(ntfsmac_journal_remove(j, 0xBEEF, NTFSMAC_JOURNAL_REMOVE_REPAIR, now + 1000),
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
	CHECK_ST(ntfsmac_journal_remove(j, 0x42, NTFSMAC_JOURNAL_REMOVE_CLEAN_SESSION, t0),
		 NTFSMAC_JOURNAL_OK);
	make_entry(j, 0x42, NTFSMAC_JOURNAL_MARKER_NONE, NTFSMAC_JOURNAL_STATE_OPEN);
	CHECK_ST(ntfsmac_journal_remove(j, 0x42, NTFSMAC_JOURNAL_REMOVE_CLEAN_SESSION, t0),
		 NTFSMAC_JOURNAL_ERR_EXISTS);
	CHECK_ST(ntfsmac_journal_get(j, 0x42, &g), NTFSMAC_JOURNAL_OK);
	tombs(j, &t);
	CHECK(t.n == 1);
	CHECK_ST(ntfsmac_journal_remove(j, 0x42, NTFSMAC_JOURNAL_REMOVE_CLEAN_SESSION, t0 + 1),
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
		CHECK_ST(ntfsmac_journal_remove(j, 100 + (uint64_t)i,
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
	CHECK_ST(ntfsmac_journal_remove(j, 500, NTFSMAC_JOURNAL_REMOVE_CLEAN_SESSION, 1000),
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
	CHECK_ST(ntfsmac_journal_get(j, 0x55, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);
	CHECK_ST(ntfsmac_journal_remove(j, 0x55, NTFSMAC_JOURNAL_REMOVE_CLEAN_SESSION, 5000),
		 NTFSMAC_JOURNAL_ERR_CORRUPT);
	CHECK_ST(ntfsmac_journal_remove(j, 0x55, NTFSMAC_JOURNAL_REMOVE_EXTERNAL_CHECK, 5000),
		 NTFSMAC_JOURNAL_ERR_CORRUPT);
	CHECK(access(p, F_OK) == 0);
	CHECK_ST(ntfsmac_journal_remove(j, 0x55, NTFSMAC_JOURNAL_REMOVE_JOURNAL_CLEAR, 5000),
		 NTFSMAC_JOURNAL_OK);
	CHECK(access(p, F_OK) != 0);
	entry_path(p, sizeof(p), jdir, 0x56);
	jt_write_file(p, "garbage", 7, 0600);
	CHECK_ST(ntfsmac_journal_remove(j, 0x56, NTFSMAC_JOURNAL_REMOVE_REPAIR, 5000),
		 NTFSMAC_JOURNAL_OK);

	/* A symlink at the entry name is moved, never followed. */
	jt_path(victim, sizeof(victim), base, "victim", NULL);
	jt_write_file(victim, "VICTIM", 6, 0600);
	entry_path(p, sizeof(p), jdir, 0x57);
	CHECK(symlink(victim, p) == 0);
	CHECK_ST(ntfsmac_journal_remove(j, 0x57, NTFSMAC_JOURNAL_REMOVE_JOURNAL_CLEAR, 5000),
		 NTFSMAC_JOURNAL_OK);
	CHECK(lstat(p, &st) != 0);
	CHECK(ntfsmac_journal_format_tombstone(tomb, sizeof(tomb), 0x57, 5000) == 0);
	jt_path(tp, sizeof(tp), jdir, "cleared", tomb);
	CHECK(lstat(tp, &st) == 0 && S_ISLNK(st.st_mode));
	CHECK(jt_read_file(victim, buf, sizeof(buf)) == 6 && memcmp(buf, "VICTIM", 6) == 0);

	/* A directory at the name stays. */
	entry_path(p, sizeof(p), jdir, 0x58);
	CHECK(mkdir(p, 0700) == 0);
	CHECK_ST(ntfsmac_journal_remove(j, 0x58, NTFSMAC_JOURNAL_REMOVE_JOURNAL_CLEAR, 5000),
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
		{ "ntfsmac-journal", "ntfsmac-journal 2" },
		{ "ntfsmac-journal", "ntfsmac-journal  1" },
		{ "ntfsmac-journal", "Ntfsmac-journal 1" },
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
		CHECK_ST(ntfsmac_journal_get(j, BASE_SERIAL, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);
	}
	jt_write_file(p, good, glen, 0600);
	CHECK_ST(ntfsmac_journal_get(j, BASE_SERIAL, &g), NTFSMAC_JOURNAL_OK);

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		size_t n;

		memcpy(text, good, glen);
		n = set_line(text, glen, sizeof(text), cases[i].key, cases[i].line);
		jt_write_file(p, text, n, 0600);
		if (ntfsmac_journal_get(j, BASE_SERIAL, &g) != NTFSMAC_JOURNAL_ERR_CORRUPT) {
			fprintf(stderr, "FAIL: accepted: %s\n", cases[i].line);
			exit(1);
		}
	}

	/* Structure: trailing data, CRLF, missing LF, NUL, doubled body. */
	memcpy(text, good, glen);
	text[glen] = 'x';
	jt_write_file(p, text, glen + 1, 0600);
	CHECK_ST(ntfsmac_journal_get(j, BASE_SERIAL, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);
	text[glen] = '\n';
	jt_write_file(p, text, glen + 1, 0600);
	CHECK_ST(ntfsmac_journal_get(j, BASE_SERIAL, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);
	memcpy(text, good, glen);
	memcpy(text + glen, good, glen);
	jt_write_file(p, text, 2 * glen, 0600);
	CHECK_ST(ntfsmac_journal_get(j, BASE_SERIAL, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);
	{ /* CRLF */
		size_t o = 0;

		for (size_t i = 0; i < glen; i++) {
			if (good[i] == '\n')
				text[o++] = '\r';
			text[o++] = good[i];
		}
		jt_write_file(p, text, o, 0600);
		CHECK_ST(ntfsmac_journal_get(j, BASE_SERIAL, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);
	}
	memcpy(text, good, glen);
	text[10] = '\0';
	jt_write_file(p, text, glen, 0600);
	CHECK_ST(ntfsmac_journal_get(j, BASE_SERIAL, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);
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
		CHECK_ST(ntfsmac_journal_get(j, BASE_SERIAL, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);
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
		CHECK_ST(ntfsmac_journal_get(j, BASE_SERIAL, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);
	}
	/* A corrupt entry is never repaired by update, and create sees it as existing. */
	e.marker = NTFSMAC_JOURNAL_MARKER_SET;
	CHECK_ST(ntfsmac_journal_update(j, &e), NTFSMAC_JOURNAL_ERR_CORRUPT);
	e.marker = NTFSMAC_JOURNAL_MARKER_PENDING;
	CHECK_ST(ntfsmac_journal_create(j, &e), NTFSMAC_JOURNAL_ERR_EXISTS);

	/* Restore: valid again. */
	jt_write_file(p, good, glen, 0600);
	CHECK_ST(ntfsmac_journal_get(j, BASE_SERIAL, &g), NTFSMAC_JOURNAL_OK);

	/* File properties: mode, size, type. */
	CHECK(chmod(p, 0666) == 0);
	CHECK_ST(ntfsmac_journal_get(j, BASE_SERIAL, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);
	CHECK(chmod(p, 0620) == 0);
	CHECK_ST(ntfsmac_journal_get(j, BASE_SERIAL, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);
	CHECK(chmod(p, 0600) == 0);
	CHECK_ST(ntfsmac_journal_get(j, BASE_SERIAL, &g), NTFSMAC_JOURNAL_OK);
	{
		char big[4096];

		memset(big, 'a', sizeof(big));
		memcpy(big, good, glen);
		jt_write_file(p, big, sizeof(big), 0600);
		CHECK_ST(ntfsmac_journal_get(j, BASE_SERIAL, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);
	}
	CHECK(unlink(p) == 0);
	CHECK(mkfifo(p, 0600) == 0); /* must not hang the open */
	CHECK_ST(ntfsmac_journal_get(j, BASE_SERIAL, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);
	CHECK(unlink(p) == 0);
	CHECK(mkdir(p, 0700) == 0);
	CHECK_ST(ntfsmac_journal_get(j, BASE_SERIAL, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);
	CHECK(rmdir(p) == 0);
	{ /* symlink at the name to a perfectly valid entry file */
		char target[PATH_MAX_T];

		jt_path(target, sizeof(target), base, "valid-copy", NULL);
		jt_write_file(target, good, glen, 0600);
		CHECK(symlink(target, p) == 0);
		CHECK_ST(ntfsmac_journal_get(j, BASE_SERIAL, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);
	}

	ntfsmac_journal_close(j);
	jt_rmrf(base);
}

struct list_ctx {
	unsigned n;
	uint64_t serial[16];
	unsigned stop_after;
};

static int list_cb(const ntfsmac_journal_entry *e, void *ctx)
{
	struct list_ctx *c = ctx;

	if (c->n < 16)
		c->serial[c->n] = e->serial;
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

static int is_entry_or_cleared(const char *name)
{
	if (!jt_is_any(name))
		return 1;
	return strcmp(name, "cleared") == 0 || is_hex16_upper(name);
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
		CHECK_ST(ntfsmac_journal_get(j, weird[i], &g), NTFSMAC_JOURNAL_OK);
	}
	CHECK(jt_count_names(jdir, jt_is_any) == (int)(sizeof(weird) / sizeof(weird[0])) + 1);
	CHECK(jt_count_names(jdir, not_entry_or_cleared) == 0);
	for (size_t i = 0; i < sizeof(weird) / sizeof(weird[0]); i++) {
		CHECK_ST(ntfsmac_journal_remove(j, weird[i], NTFSMAC_JOURNAL_REMOVE_JOURNAL_CLEAR,
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
	CHECK_ST(ntfsmac_journal_get(j, 0xA1, &g), NTFSMAC_JOURNAL_ERR_CORRUPT);
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
		e = jt_entry(0xA3);
		jt_path(p, sizeof(p), jdir, ".00000000000000A3.tmp", NULL);
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
		CHECK_ST(ntfsmac_journal_get(j, 0xA3, &g), NTFSMAC_JOURNAL_OK);
		CHECK(jt_entry_eq(&e, &g));
	}
	/* A planted tombstone name that is a symlink counts as a collision. */
	make_entry(j, 0xA4, NTFSMAC_JOURNAL_MARKER_NONE, NTFSMAC_JOURNAL_STATE_OPEN);
	CHECK(ntfsmac_journal_format_tombstone(tomb, sizeof(tomb), 0xA4, 1800000000) == 0);
	jt_path(p, sizeof(p), jdir, "cleared", tomb);
	CHECK(symlink(victim, p) == 0);
	CHECK_ST(ntfsmac_journal_remove(j, 0xA4, NTFSMAC_JOURNAL_REMOVE_JOURNAL_CLEAR, 1800000000),
		 NTFSMAC_JOURNAL_ERR_EXISTS);
	CHECK_ST(ntfsmac_journal_get(j, 0xA4, &g), NTFSMAC_JOURNAL_OK);
	CHECK(jt_read_file(victim, buf, sizeof(buf)) == 6 && memcmp(buf, "VICTIM", 6) == 0);

	ntfsmac_journal_close(j);
	jt_rmrf(base);
}

int main(void)
{
	ntfsmac_log_set_level(NTFSMAC_LOG_NONE);
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
	RUN(test_naming_u_jrn_name_01);
	printf("journal.unit: %d test groups passed\n", g_tests);
	return 0;
}
