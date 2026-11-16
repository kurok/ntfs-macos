/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * T-M4-05: restart reconciliation (U-DMN-RC-01..05).
 *   01  which mount point belongs to which entry
 *   02  decision matrix: every marker, every state, every kind of live mount
 *   03  ambiguity: several entries or several mounts for one label
 *   04  bad arguments and capacity
 *   05  run over a real journal in a temporary directory; entries unchanged
 *
 * The CHECK macro of jtest.h stops the test at the first failure.
 */
#include "jtest.h"

#include "internal.h"

static ntfsmac_journal_entry mk_entry(uint64_t serial, const char *label,
				      ntfsmac_journal_marker marker,
				      ntfsmac_journal_state state)
{
	ntfsmac_journal_entry e = jt_entry(serial);

	snprintf(e.label, sizeof e.label, "%s", label);
	e.marker = marker;
	e.state = state;
	return e;
}

static ntfsmac_live_mount mk_mount(const char *path, bool ro, pid_t pid)
{
	ntfsmac_live_mount m;

	memset(&m, 0, sizeof m);
	snprintf(m.mount_point, sizeof m.mount_point, "%s", path);
	m.read_only = ro;
	m.worker_pid = pid;
	return m;
}

/* ---- 01 ------------------------------------------------------------ */

static void test_match(void)
{
	ntfsmac_journal_entry e = mk_entry(1, "DATA", NTFSMAC_JOURNAL_MARKER_SET,
					   NTFSMAC_JOURNAL_STATE_OPEN);
	char big[200];

	CHECK(ntfsmac_reconcile_mount_matches(&e, "/Volumes/DATA"));
	CHECK(ntfsmac_reconcile_mount_matches(&e, "/Volumes/DATA 2"));
	CHECK(ntfsmac_reconcile_mount_matches(&e, "/Volumes/DATA 9"));
	CHECK(ntfsmac_reconcile_mount_matches(&e, "/Volumes/DATA 10"));
	CHECK(ntfsmac_reconcile_mount_matches(&e, "/Volumes/DATA 99"));
	CHECK(!ntfsmac_reconcile_mount_matches(&e, "/Volumes/DATA 1"));
	CHECK(!ntfsmac_reconcile_mount_matches(&e, "/Volumes/DATA 0"));
	CHECK(!ntfsmac_reconcile_mount_matches(&e, "/Volumes/DATA 02"));
	CHECK(!ntfsmac_reconcile_mount_matches(&e, "/Volumes/DATA 100"));
	CHECK(!ntfsmac_reconcile_mount_matches(&e, "/Volumes/DATA 2x"));
	CHECK(!ntfsmac_reconcile_mount_matches(&e, "/Volumes/DATA "));
	CHECK(!ntfsmac_reconcile_mount_matches(&e, "/Volumes/DATA2"));
	CHECK(!ntfsmac_reconcile_mount_matches(&e, "/Volumes/DATAX"));
	CHECK(!ntfsmac_reconcile_mount_matches(&e, "/Volumes/DAT"));
	CHECK(!ntfsmac_reconcile_mount_matches(&e, "/Volumes/data"));
	CHECK(!ntfsmac_reconcile_mount_matches(&e, "/Volumes/"));
	CHECK(!ntfsmac_reconcile_mount_matches(&e, "/Volumes/DATA/sub"));
	CHECK(!ntfsmac_reconcile_mount_matches(&e, "/Volumes/DATA 2/sub"));
	CHECK(!ntfsmac_reconcile_mount_matches(&e, "/Volumes/../DATA"));
	CHECK(!ntfsmac_reconcile_mount_matches(&e, "/Volumes"));
	CHECK(!ntfsmac_reconcile_mount_matches(&e, "/tmp/DATA"));
	CHECK(!ntfsmac_reconcile_mount_matches(&e, "Volumes/DATA"));
	CHECK(!ntfsmac_reconcile_mount_matches(&e, "//Volumes/DATA"));
	CHECK(!ntfsmac_reconcile_mount_matches(&e, ""));
	CHECK(!ntfsmac_reconcile_mount_matches(&e, NULL));
	CHECK(!ntfsmac_reconcile_mount_matches(NULL, "/Volumes/DATA"));

	/* A label with a space inside is matched whole. */
	e = mk_entry(1, "MY DATA", NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_OPEN);
	CHECK(ntfsmac_reconcile_mount_matches(&e, "/Volumes/MY DATA"));
	CHECK(ntfsmac_reconcile_mount_matches(&e, "/Volumes/MY DATA 3"));
	CHECK(!ntfsmac_reconcile_mount_matches(&e, "/Volumes/MY"));

	/* An empty label gets the fallback leaf of ADR-016 (last 8 hex digits). */
	e = mk_entry(0x1122334455667788ull, "", NTFSMAC_JOURNAL_MARKER_SET,
		     NTFSMAC_JOURNAL_STATE_OPEN);
	CHECK(ntfsmac_reconcile_mount_matches(&e, "/Volumes/NTFS-55667788"));
	CHECK(ntfsmac_reconcile_mount_matches(&e, "/Volumes/NTFS-55667788 2"));
	CHECK(!ntfsmac_reconcile_mount_matches(&e, "/Volumes/NTFS-55667789"));

	/* A path that is too long for any leaf never matches and never overflows. */
	memset(big, 'A', sizeof big);
	memcpy(big, "/Volumes/", 9);
	big[sizeof big - 1] = '\0';
	CHECK(!ntfsmac_reconcile_mount_matches(&e, big));

	/* A 64-byte label (the cap) with the longest suffix. */
	{
		char label[65], path[160];

		memset(label, 'L', 64);
		label[64] = '\0';
		e = mk_entry(2, label, NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_OPEN);
		snprintf(path, sizeof path, "/Volumes/%s 99", label);
		CHECK(ntfsmac_reconcile_mount_matches(&e, path));
		snprintf(path, sizeof path, "/Volumes/%s", label);
		CHECK(ntfsmac_reconcile_mount_matches(&e, path));
	}
}

/* ---- 02 ------------------------------------------------------------ */

enum mount_kind { MK_NONE, MK_RO, MK_RW_WORKER, MK_RW_NO_WORKER, MK_RO_WORKER };

static ntfsmac_reconcile_action expected(ntfsmac_journal_marker mk, enum mount_kind kind)
{
	switch (kind) {
	case MK_RW_WORKER:
		if (mk == NTFSMAC_JOURNAL_MARKER_SET || mk == NTFSMAC_JOURNAL_MARKER_NONE)
			return NTFSMAC_RECONCILE_READOPT;
		return NTFSMAC_RECONCILE_FORCE_UNMOUNT; /* pending, failed */
	case MK_RW_NO_WORKER:
		return NTFSMAC_RECONCILE_FORCE_UNMOUNT;
	case MK_NONE:
	case MK_RO:
	case MK_RO_WORKER:
	default:
		return NTFSMAC_RECONCILE_KEEP_ENTRY;
	}
}

static void test_matrix(void)
{
	static const ntfsmac_journal_marker markers[] = {
		NTFSMAC_JOURNAL_MARKER_PENDING, NTFSMAC_JOURNAL_MARKER_SET,
		NTFSMAC_JOURNAL_MARKER_FAILED, NTFSMAC_JOURNAL_MARKER_NONE};
	static const ntfsmac_journal_state states[] = {NTFSMAC_JOURNAL_STATE_OPEN,
						       NTFSMAC_JOURNAL_STATE_WRITE_ERROR};
	unsigned mi, si, k, cases = 0;

	for (mi = 0; mi < 4; mi++)
		for (si = 0; si < 2; si++)
			for (k = MK_NONE; k <= MK_RO_WORKER; k++) {
				ntfsmac_journal_entry e = mk_entry(7, "VOL", markers[mi], states[si]);
				ntfsmac_live_mount m;
				ntfsmac_reconcile_item items[4];
				size_t n, nm = 1;

				switch (k) {
				case MK_RO:
					m = mk_mount("/Volumes/VOL", true, 0);
					break;
				case MK_RO_WORKER:
					m = mk_mount("/Volumes/VOL", true, 4242);
					break;
				case MK_RW_WORKER:
					m = mk_mount("/Volumes/VOL", false, 4242);
					break;
				case MK_RW_NO_WORKER:
					m = mk_mount("/Volumes/VOL", false, 0);
					break;
				default:
					m = mk_mount("/Volumes/OTHER", false, 4242);
					nm = k == MK_NONE ? 0 : 1;
					break;
				}
				n = ntfsmac_reconcile_plan(&e, 1, &m, nm, items, 4);
				CHECK(n == 1);
				CHECK(items[0].entry_index == 0);
				CHECK(items[0].action == expected(markers[mi], (enum mount_kind)k));
				if (items[0].action == NTFSMAC_RECONCILE_KEEP_ENTRY)
					CHECK(items[0].mount_index == -1);
				else
					CHECK(items[0].mount_index == 0);
				cases++;
			}
	CHECK(cases == 4 * 2 * 5);

	/* A writable mount of another name never touches the entry. */
	{
		ntfsmac_journal_entry e = mk_entry(7, "VOL", NTFSMAC_JOURNAL_MARKER_SET,
						   NTFSMAC_JOURNAL_STATE_OPEN);
		ntfsmac_live_mount m = mk_mount("/Volumes/OTHER", false, 4242);
		ntfsmac_reconcile_item it;

		CHECK(ntfsmac_reconcile_plan(&e, 1, &m, 1, &it, 2) == 1);
		CHECK(it.action == NTFSMAC_RECONCILE_KEEP_ENTRY);
	}

	/* No entries: nothing to decide. */
	{
		ntfsmac_live_mount m = mk_mount("/Volumes/VOL", false, 4242);
		ntfsmac_reconcile_item it;

		CHECK(ntfsmac_reconcile_plan(NULL, 0, &m, 1, &it, 4) == 0);
	}
}

/* ---- 03 ------------------------------------------------------------ */

static void test_ambiguity(void)
{
	ntfsmac_reconcile_item it[8];
	size_t n;

	/* Same label on two entries, one writable mount with a worker: cannot
	 * tell whose it is. Force unmount once; the second entry is kept. */
	{
		ntfsmac_journal_entry e[2] = {
			mk_entry(1, "DATA", NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_OPEN),
			mk_entry(2, "DATA", NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_OPEN)};
		ntfsmac_live_mount m = mk_mount("/Volumes/DATA", false, 100);

		n = ntfsmac_reconcile_plan(e, 2, &m, 1, it, 8);
		CHECK(n == 2);
		CHECK(it[0].entry_index == 0 && it[0].action == NTFSMAC_RECONCILE_FORCE_UNMOUNT);
		CHECK(it[0].mount_index == 0);
		CHECK(it[1].entry_index == 1 && it[1].action == NTFSMAC_RECONCILE_KEEP_ENTRY);
		CHECK(it[1].mount_index == -1);
	}

	/* One entry, two writable mounts (DATA and DATA 2), both with workers:
	 * both are force-unmounted, none re-adopted. */
	{
		ntfsmac_journal_entry e = mk_entry(1, "DATA", NTFSMAC_JOURNAL_MARKER_SET,
						   NTFSMAC_JOURNAL_STATE_OPEN);
		ntfsmac_live_mount m[2] = {mk_mount("/Volumes/DATA", false, 100),
					   mk_mount("/Volumes/DATA 2", false, 101)};

		n = ntfsmac_reconcile_plan(&e, 1, m, 2, it, 8);
		CHECK(n == 2);
		CHECK(it[0].action == NTFSMAC_RECONCILE_FORCE_UNMOUNT && it[0].mount_index == 0);
		CHECK(it[1].action == NTFSMAC_RECONCILE_FORCE_UNMOUNT && it[1].mount_index == 1);
	}

	/* One entry, a writable mount and a read-only mount of the same label:
	 * not unique, so no re-adoption; the writable one is force-unmounted,
	 * the read-only one is left alone. */
	{
		ntfsmac_journal_entry e = mk_entry(1, "DATA", NTFSMAC_JOURNAL_MARKER_SET,
						   NTFSMAC_JOURNAL_STATE_OPEN);
		ntfsmac_live_mount m[2] = {mk_mount("/Volumes/DATA", true, 0),
					   mk_mount("/Volumes/DATA 2", false, 101)};

		n = ntfsmac_reconcile_plan(&e, 1, m, 2, it, 8);
		CHECK(n == 1);
		CHECK(it[0].action == NTFSMAC_RECONCILE_FORCE_UNMOUNT && it[0].mount_index == 1);
	}

	/* Different labels, each with its own writable mount and worker: both
	 * are re-adopted. Order follows the entries. */
	{
		ntfsmac_journal_entry e[2] = {
			mk_entry(1, "A", NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_OPEN),
			mk_entry(2, "B", NTFSMAC_JOURNAL_MARKER_NONE,
				 NTFSMAC_JOURNAL_STATE_WRITE_ERROR)};
		ntfsmac_live_mount m[2] = {mk_mount("/Volumes/B", false, 201),
					   mk_mount("/Volumes/A", false, 200)};

		n = ntfsmac_reconcile_plan(e, 2, m, 2, it, 8);
		CHECK(n == 2);
		CHECK(it[0].entry_index == 0 && it[0].action == NTFSMAC_RECONCILE_READOPT &&
		      it[0].mount_index == 1);
		CHECK(it[1].entry_index == 1 && it[1].action == NTFSMAC_RECONCILE_READOPT &&
		      it[1].mount_index == 0);
	}

	/* A, "A 2" as a name of its own: label "A" claims "A 2" too, so the
	 * mount "A 2" is shared by the entries "A" and "A 2" and is not
	 * re-adopted by either. */
	{
		ntfsmac_journal_entry e[2] = {
			mk_entry(1, "A", NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_OPEN),
			mk_entry(2, "A 2", NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_OPEN)};
		ntfsmac_live_mount m = mk_mount("/Volumes/A 2", false, 300);

		n = ntfsmac_reconcile_plan(e, 2, &m, 1, it, 8);
		CHECK(n == 2);
		CHECK(it[0].action == NTFSMAC_RECONCILE_FORCE_UNMOUNT && it[0].mount_index == 0);
		CHECK(it[1].action == NTFSMAC_RECONCILE_KEEP_ENTRY);
	}

	/* Every entry appears in the output at least once, in entry order. */
	{
		ntfsmac_journal_entry e[3] = {
			mk_entry(1, "X", NTFSMAC_JOURNAL_MARKER_PENDING, NTFSMAC_JOURNAL_STATE_OPEN),
			mk_entry(2, "Y", NTFSMAC_JOURNAL_MARKER_FAILED, NTFSMAC_JOURNAL_STATE_OPEN),
			mk_entry(3, "Z", NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_OPEN)};
		ntfsmac_live_mount m = mk_mount("/Volumes/Y", false, 9);

		n = ntfsmac_reconcile_plan(e, 3, &m, 1, it, 8);
		CHECK(n == 3);
		CHECK(it[0].entry_index == 0 && it[0].action == NTFSMAC_RECONCILE_KEEP_ENTRY);
		CHECK(it[1].entry_index == 1 && it[1].action == NTFSMAC_RECONCILE_FORCE_UNMOUNT);
		CHECK(it[2].entry_index == 2 && it[2].action == NTFSMAC_RECONCILE_KEEP_ENTRY);
	}
}

/* ---- 04 ------------------------------------------------------------ */

static void test_args(void)
{
	ntfsmac_journal_entry e = mk_entry(1, "A", NTFSMAC_JOURNAL_MARKER_SET,
					   NTFSMAC_JOURNAL_STATE_OPEN);
	ntfsmac_live_mount m = mk_mount("/Volumes/A", false, 5);
	ntfsmac_reconcile_item it[4];

	CHECK(ntfsmac_reconcile_plan(&e, 1, &m, 1, it, 1) == 0); /* cap below bound 2 */
	CHECK(ntfsmac_reconcile_plan(&e, 1, &m, 1, NULL, 4) == 0);
	CHECK(ntfsmac_reconcile_plan(NULL, 1, &m, 1, it, 4) == 0);
	CHECK(ntfsmac_reconcile_plan(&e, 1, NULL, 1, it, 4) == 0);
	CHECK(ntfsmac_reconcile_plan(&e, 1, NULL, 0, it, 4) == 1);
	CHECK(it[0].action == NTFSMAC_RECONCILE_KEEP_ENTRY);
	CHECK(ntfsmac_reconcile_plan(&e, 1, &m, 1, it, 2) == 1);

	CHECK(ntfsmac_reconcile_run_with(NULL, &m, 1, NULL, NULL) == NTFSMAC_DMN_E_INVAL);
	CHECK(ntfsmac_reconcile_run(NULL, NULL, NULL) == NTFSMAC_DMN_E_INVAL);
}

/* ---- 05 ------------------------------------------------------------ */

typedef struct seen {
	unsigned n;
	uint64_t serial[8];
	ntfsmac_reconcile_action action[8];
	char mount[8][128];
	unsigned stop_after;
} seen;

static int record(const ntfsmac_journal_entry *e, ntfsmac_reconcile_action a,
		  const ntfsmac_live_mount *m, void *ctx)
{
	seen *s = ctx;

	CHECK(s->n < 8);
	s->serial[s->n] = e->serial;
	s->action[s->n] = a;
	if (m != NULL)
		snprintf(s->mount[s->n], sizeof s->mount[s->n], "%s", m->mount_point);
	else
		s->mount[s->n][0] = '\0';
	s->n++;
	return s->stop_after != 0 && s->n >= s->stop_after;
}

/* create allows only open/pending or open/none; the rest goes through update. */
static void put_entry(ntfsmac_journal *j, const ntfsmac_journal_entry *e)
{
	ntfsmac_journal_entry c = *e;

	c.state = NTFSMAC_JOURNAL_STATE_OPEN;
	if (c.marker != NTFSMAC_JOURNAL_MARKER_NONE)
		c.marker = NTFSMAC_JOURNAL_MARKER_PENDING;
	CHECK_ST(ntfsmac_journal_create(j, &c), NTFSMAC_JOURNAL_OK);
	if (c.marker != e->marker || c.state != e->state)
		CHECK_ST(ntfsmac_journal_update(j, e), NTFSMAC_JOURNAL_OK);
}

static unsigned find_serial(const seen *s, uint64_t serial)
{
	unsigned i;

	for (i = 0; i < s->n; i++)
		if (s->serial[i] == serial)
			return i;
	CHECK(0);
	return 0;
}

static void test_run(void)
{
	char base[PATH_MAX_T], jdir[PATH_MAX_T];
	ntfsmac_journal *j = NULL;
	ntfsmac_journal_entry e1, e2, e3, g1, g2, g3;
	ntfsmac_live_mount m[2];
	seen s;
	unsigned i;

	jt_jdir(base, sizeof base, jdir, sizeof jdir);
	CHECK_ST(ntfsmac_journal_open(jdir, &j), NTFSMAC_JOURNAL_OK);

	/* Empty journal: OK, no callback. */
	memset(&s, 0, sizeof s);
	CHECK(ntfsmac_reconcile_run_with(j, NULL, 0, record, &s) == NTFSMAC_DMN_OK);
	CHECK(s.n == 0);

	e1 = mk_entry(0xA1, "ONE", NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_OPEN);
	e2 = mk_entry(0xA2, "TWO", NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_WRITE_ERROR);
	e3 = mk_entry(0xA3, "THREE", NTFSMAC_JOURNAL_MARKER_FAILED, NTFSMAC_JOURNAL_STATE_OPEN);
	put_entry(j, &e1);
	put_entry(j, &e2);
	put_entry(j, &e3);

	/* No live mounts at all: every entry is kept. */
	memset(&s, 0, sizeof s);
	CHECK(ntfsmac_reconcile_run_with(j, NULL, 0, record, &s) == NTFSMAC_DMN_OK);
	CHECK(s.n == 3);
	for (i = 0; i < 3; i++)
		CHECK(s.action[i] == NTFSMAC_RECONCILE_KEEP_ENTRY && s.mount[i][0] == '\0');

	/* ONE: writable, worker alive: re-adopt. TWO: writable, no worker:
	 * force unmount. THREE: no mount: kept. */
	m[0] = mk_mount("/Volumes/ONE", false, 77);
	m[1] = mk_mount("/Volumes/TWO", false, 0);
	memset(&s, 0, sizeof s);
	CHECK(ntfsmac_reconcile_run_with(j, m, 2, record, &s) == NTFSMAC_DMN_OK);
	CHECK(s.n == 3);
	CHECK(s.action[find_serial(&s, 0xA1)] == NTFSMAC_RECONCILE_READOPT);
	CHECK(strcmp(s.mount[find_serial(&s, 0xA1)], "/Volumes/ONE") == 0);
	CHECK(s.action[find_serial(&s, 0xA2)] == NTFSMAC_RECONCILE_FORCE_UNMOUNT);
	CHECK(strcmp(s.mount[find_serial(&s, 0xA2)], "/Volumes/TWO") == 0);
	CHECK(s.action[find_serial(&s, 0xA3)] == NTFSMAC_RECONCILE_KEEP_ENTRY);

	/* The callback can stop the run. */
	memset(&s, 0, sizeof s);
	s.stop_after = 1;
	CHECK(ntfsmac_reconcile_run_with(j, m, 2, record, &s) == NTFSMAC_DMN_OK);
	CHECK(s.n == 1);

	/* The journal is untouched by reconciliation, whatever the actions. */
	CHECK_ST(ntfsmac_journal_get(j, 0xA1, &g1), NTFSMAC_JOURNAL_OK);
	CHECK_ST(ntfsmac_journal_get(j, 0xA2, &g2), NTFSMAC_JOURNAL_OK);
	CHECK_ST(ntfsmac_journal_get(j, 0xA3, &g3), NTFSMAC_JOURNAL_OK);
	CHECK(jt_entry_eq(&g1, &e1));
	CHECK(jt_entry_eq(&g2, &e2));
	CHECK(jt_entry_eq(&g3, &e3));

	/* The real lister (macOS) works and finds no mount of these names;
	 * elsewhere it reports "not implemented". */
	memset(&s, 0, sizeof s);
	{
		ntfsmac_dmn_status st = ntfsmac_reconcile_run(j, record, &s);
#if defined(__APPLE__)
		CHECK(st == NTFSMAC_DMN_OK);
		CHECK(s.n == 3);
		for (i = 0; i < 3; i++)
			CHECK(s.action[i] == NTFSMAC_RECONCILE_KEEP_ENTRY);
#else
		CHECK(st == NTFSMAC_DMN_E_NOTIMPL);
#endif
	}

	ntfsmac_journal_close(j);
	jt_rmrf(base);
}

int main(void)
{
	test_match();
	test_matrix();
	test_ambiguity();
	test_args();
	test_run();
	puts("ok");
	return 0;
}
