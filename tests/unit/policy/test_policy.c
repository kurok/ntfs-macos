/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-POL-*: policy table, evidence rule, gates, warnings (ARCHITECTURE.md 3.2).
 */
#include "ntfsmac/policy.h"

#include <string.h>

#include "check.h"

#define R(d, a, n) { NTFSMAC_POLICY_##d, NTFSMAC_POLICY_JOURNAL_##a, NTFSMAC_POLICY_NOTE_##n }
#define FLAG(f) NTFSMAC_PROBE_VOLFLAG_BIT(NTFSMAC_PROBE_VOLFLAG_##f)

enum jcase { J_NONE, J_EVIDENCE, J_NO_EVIDENCE };

#define SERIAL UINT64_C(0x1122334455667788)
#define SECTORS UINT64_C(204800)
#define LSN_START UINT64_C(1000)

static void make_verdict(ntfsmac_probe_verdict *v, ntfsmac_probe_state st)
{
	memset(v, 0, sizeof(*v));
	v->is_ntfs = true;
	v->has_ntfs_version = true;
	v->ntfs_version_major = 3;
	v->ntfs_version_minor = 1;
	v->serial = SERIAL;
	v->total_sectors = SECTORS;
	v->cluster_size = 4096;
	v->sector_size = 512;
	v->logfile = NTFSMAC_PROBE_LOGFILE_CLEAN;
	v->hiberfile = NTFSMAC_PROBE_HIBERFILE_ABSENT;
	v->has_logfile_lsn = true;
	v->logfile_lsn = LSN_START + 1; /* differs from the entry's */
	v->state = st;
	if (st != NTFSMAC_PROBE_STATE_CLEAN)
		v->reason_codes = NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_CHECK_FAILED);
}

static void make_entry(ntfsmac_journal_entry *e)
{
	memset(e, 0, sizeof(*e));
	e->serial = SERIAL;
	e->total_sectors = SECTORS;
	e->session_id = 7;
	e->logfile_lsn_at_start = LSN_START;
	e->marker = NTFSMAC_JOURNAL_MARKER_SET;
	e->state = NTFSMAC_JOURNAL_STATE_OPEN;
}

static const ntfsmac_journal_entry *pick(enum jcase c, ntfsmac_journal_entry *e)
{
	make_entry(e);
	switch (c) {
	case J_NONE:
		return NULL;
	case J_EVIDENCE:
		return e;
	case J_NO_EVIDENCE:
		e->marker = NTFSMAC_JOURNAL_MARKER_PENDING;
		return e;
	}
	return NULL;
}

typedef struct expected {
	ntfsmac_policy_decision d;
	ntfsmac_policy_journal_action a;
	ntfsmac_policy_note n;
} expected;

static void run_one(ntfsmac_probe_state st, enum jcase jc, ntfsmac_policy_mode m, bool ev,
		    expected x)
{
	ntfsmac_probe_verdict v;
	ntfsmac_journal_entry e;
	ntfsmac_policy_input in;
	ntfsmac_policy_result r;

	make_verdict(&v, st);
	in.verdict = &v;
	in.journal = pick(jc, &e);
	in.mode = m;
	in.evidence_rule_enabled = ev;
	r = ntfsmac_policy_decide(&in);
	if (r.decision != x.d || r.journal_action != x.a || r.note != x.n)
		fprintf(stderr, "row state=%d jc=%d mode=%d ev=%d got %d/%d/%d want %d/%d/%d\n",
			(int)st, (int)jc, (int)m, (int)ev, r.decision, r.journal_action,
			r.note, x.d, x.a, x.n);
	CHECK_EQ_INT(r.decision, x.d);
	CHECK_EQ_INT(r.journal_action, x.a);
	CHECK_EQ_INT(r.note, x.n);
}

/* U-POL-TABLE: every row of section 3.2 with each journal state, mode and switch. */
static void test_table(void)
{
	static const struct {
		ntfsmac_probe_state st;
		expected none, entry; /* entry: any journal entry, evidence or not */
	} flat[] = {
		{ NTFSMAC_PROBE_STATE_DIRTY_FLAG, R(RO, KEEP, DIRTY_FLAG), R(RO, KEEP, DIRTY_FLAG_JOURNAL) },
		{ NTFSMAC_PROBE_STATE_UNCLEAN_LOG, R(RO, KEEP, UNCLEAN_LOG), R(RO, KEEP, UNCLEAN_LOG) },
		{ NTFSMAC_PROBE_STATE_HIBERNATED, R(RO, KEEP, HIBERNATED), R(RO, KEEP, HIBERNATED) },
		{ NTFSMAC_PROBE_STATE_FAST_STARTUP, R(RO, KEEP, HIBERNATED), R(RO, KEEP, HIBERNATED) },
		{ NTFSMAC_PROBE_STATE_UNKNOWN, R(RO, KEEP, UNKNOWN), R(RO, KEEP, UNKNOWN) },
		{ NTFSMAC_PROBE_STATE_CORRUPT, R(REFUSE, KEEP, REFUSED), R(REFUSE, KEEP, REFUSED) },
		{ NTFSMAC_PROBE_STATE_UNSUPPORTED, R(REFUSE, KEEP, REFUSED), R(REFUSE, KEEP, REFUSED) },
		{ NTFSMAC_PROBE_STATE_MIRROR_SPLIT, R(REFUSE, KEEP, MIRROR_SPLIT), R(REFUSE, KEEP, MIRROR_SPLIT) },
		{ NTFSMAC_PROBE_STATE_NAMES_NOT_NFC, R(RO, KEEP, NAMES_NOT_NFC), R(RO, KEEP, NAMES_NOT_NFC) },
	};
	/* Clean rows: explicit per (journal case, mode, evidence switch). */
	static const struct {
		enum jcase jc;
		ntfsmac_policy_mode m;
		bool ev;
		expected x;
	} clean[] = {
		{ J_NONE, NTFSMAC_POLICY_MODE_AUTO, false, R(RW, KEEP, NONE) },
		{ J_NONE, NTFSMAC_POLICY_MODE_AUTO, true, R(RW, KEEP, NONE) },
		{ J_NONE, NTFSMAC_POLICY_MODE_RO, false, R(RO, KEEP, MODE_RO_REQUESTED) },
		{ J_NONE, NTFSMAC_POLICY_MODE_RO, true, R(RO, KEEP, MODE_RO_REQUESTED) },
		{ J_EVIDENCE, NTFSMAC_POLICY_MODE_AUTO, true, R(RO, REMOVE, EXTERNAL_CHECK) },
		{ J_EVIDENCE, NTFSMAC_POLICY_MODE_RO, true, R(RO, KEEP, EXTERNAL_CHECK) }, /* A6 */
		{ J_EVIDENCE, NTFSMAC_POLICY_MODE_AUTO, false, R(RO, KEEP, UNCLEAN_SESSION) },
		{ J_EVIDENCE, NTFSMAC_POLICY_MODE_RO, false, R(RO, KEEP, UNCLEAN_SESSION) },
		{ J_NO_EVIDENCE, NTFSMAC_POLICY_MODE_AUTO, true, R(RO, KEEP, UNCLEAN_SESSION) },
		{ J_NO_EVIDENCE, NTFSMAC_POLICY_MODE_AUTO, false, R(RO, KEEP, UNCLEAN_SESSION) },
		{ J_NO_EVIDENCE, NTFSMAC_POLICY_MODE_RO, true, R(RO, KEEP, UNCLEAN_SESSION) },
		{ J_NO_EVIDENCE, NTFSMAC_POLICY_MODE_RO, false, R(RO, KEEP, UNCLEAN_SESSION) },
	};
	size_t i;
	int m, ev, jc;

	for (i = 0; i < sizeof(flat) / sizeof(flat[0]); i++)
		for (m = 0; m < 2; m++)
			for (ev = 0; ev < 2; ev++)
				for (jc = 0; jc < 3; jc++)
					run_one(flat[i].st, (enum jcase)jc, (ntfsmac_policy_mode)m,
						ev != 0, jc == J_NONE ? flat[i].none : flat[i].entry);
	for (i = 0; i < sizeof(clean) / sizeof(clean[0]); i++)
		run_one(NTFSMAC_PROBE_STATE_CLEAN, clean[i].jc, clean[i].m, clean[i].ev, clean[i].x);
}

/* U-POL-EVIDENCE: each condition of the rule, negated one at a time. */
static void test_evidence(void)
{
	ntfsmac_probe_verdict v;
	ntfsmac_journal_entry e;
	int m, s;

	make_verdict(&v, NTFSMAC_PROBE_STATE_CLEAN);
	make_entry(&e);
	CHECK(ntfsmac_policy_evidence_holds(&v, &e));
	CHECK(!ntfsmac_policy_evidence_holds(NULL, &e));
	CHECK(!ntfsmac_policy_evidence_holds(&v, NULL));

	for (m = 0; m <= NTFSMAC_JOURNAL_MARKER_NONE; m++) {
		make_entry(&e);
		e.marker = (ntfsmac_journal_marker)m;
		CHECK_EQ_INT(ntfsmac_policy_evidence_holds(&v, &e), m == NTFSMAC_JOURNAL_MARKER_SET);
	}
	make_entry(&e);
	e.marker = (ntfsmac_journal_marker)99;
	CHECK(!ntfsmac_policy_evidence_holds(&v, &e));

	for (s = 0; s < NTFSMAC_PROBE_STATE_COUNT_; s++) {
		make_verdict(&v, (ntfsmac_probe_state)s);
		make_entry(&e);
		CHECK_EQ_INT(ntfsmac_policy_evidence_holds(&v, &e), s == NTFSMAC_PROBE_STATE_CLEAN);
	}
	make_verdict(&v, NTFSMAC_PROBE_STATE_CLEAN);
	make_entry(&e);
	v.logfile_lsn = e.logfile_lsn_at_start; /* unchanged LSN */
	CHECK(!ntfsmac_policy_evidence_holds(&v, &e));
	make_verdict(&v, NTFSMAC_PROBE_STATE_CLEAN);
	v.has_logfile_lsn = false;
	v.logfile = NTFSMAC_PROBE_LOGFILE_UNREADABLE;
	CHECK(!ntfsmac_policy_evidence_holds(&v, &e));
	make_verdict(&v, NTFSMAC_PROBE_STATE_CLEAN);
	v.serial ^= 1;
	CHECK(!ntfsmac_policy_evidence_holds(&v, &e));
	make_verdict(&v, NTFSMAC_PROBE_STATE_CLEAN);
	v.total_sectors++;
	CHECK(!ntfsmac_policy_evidence_holds(&v, &e));
	/* write_error entry: the rule looks at the marker only. */
	make_verdict(&v, NTFSMAC_PROBE_STATE_CLEAN);
	e.state = NTFSMAC_JOURNAL_STATE_WRITE_ERROR;
	CHECK(ntfsmac_policy_evidence_holds(&v, &e));
	/* clean with a reason code is inconsistent and never counts. */
	make_verdict(&v, NTFSMAC_PROBE_STATE_CLEAN);
	v.reason_codes = 1;
	CHECK(!ntfsmac_policy_evidence_holds(&v, &e));
}

/* U-POL-FAILCLOSED */
static void test_fail_closed(void)
{
	ntfsmac_probe_verdict v;
	ntfsmac_policy_input in;
	ntfsmac_policy_result r;

	make_verdict(&v, NTFSMAC_PROBE_STATE_CLEAN);
	in.verdict = &v;
	in.journal = NULL;
	in.mode = NTFSMAC_POLICY_MODE_AUTO;
	in.evidence_rule_enabled = false;

	r = ntfsmac_policy_decide(NULL);
	CHECK_EQ_INT(r.decision, NTFSMAC_POLICY_RO);
	CHECK_EQ_INT(r.journal_action, NTFSMAC_POLICY_JOURNAL_KEEP);
	CHECK_EQ_INT(r.note, NTFSMAC_POLICY_NOTE_UNKNOWN);

	in.verdict = NULL;
	r = ntfsmac_policy_decide(&in);
	CHECK_EQ_INT(r.decision, NTFSMAC_POLICY_RO);
	CHECK_EQ_INT(r.note, NTFSMAC_POLICY_NOTE_UNKNOWN);
	in.verdict = &v;

	in.mode = (ntfsmac_policy_mode)7;
	r = ntfsmac_policy_decide(&in);
	CHECK_EQ_INT(r.decision, NTFSMAC_POLICY_RO);
	CHECK_EQ_INT(r.note, NTFSMAC_POLICY_NOTE_UNKNOWN);
	in.mode = NTFSMAC_POLICY_MODE_AUTO;

	v.state = (ntfsmac_probe_state)42;
	r = ntfsmac_policy_decide(&in);
	CHECK_EQ_INT(r.decision, NTFSMAC_POLICY_RO);
	CHECK_EQ_INT(r.note, NTFSMAC_POLICY_NOTE_UNKNOWN);
	v.state = NTFSMAC_PROBE_STATE_COUNT_;
	r = ntfsmac_policy_decide(&in);
	CHECK_EQ_INT(r.decision, NTFSMAC_POLICY_RO);

	/* clean with a reason code contradicts the cross-field rule. */
	make_verdict(&v, NTFSMAC_PROBE_STATE_CLEAN);
	v.reason_codes = 4;
	r = ntfsmac_policy_decide(&in);
	CHECK_EQ_INT(r.decision, NTFSMAC_POLICY_RO);
	CHECK_EQ_INT(r.note, NTFSMAC_POLICY_NOTE_UNKNOWN);

	/* clean with a field that contradicts it: ro, never rw (section 3.1, F-04). */
	{
		static const struct {
			const char *what;
			int field;
		} bad[] = { { "not NTFS", 0 }, { "BitLocker", 1 }, { "no version", 2 },
			    { "log unclean", 3 }, { "log v2_fast_startup", 4 },
			    { "log unreadable", 5 }, { "hiberfile hibernated", 6 },
			    { "hiberfile unreadable", 7 }, { "DIRTY", 8 },
			    { "CHKDSK_UNDERWAY", 9 }, { "RESIZE_LOG_FILE", 10 },
			    { "UPGRADE_ON_MOUNT", 11 }, { "DELETE_USN_UNDERWAY", 12 },
			    { "REPAIR_OBJECT_ID", 13 }, { "UNKNOWN_BITS", 14 } };
		ntfsmac_journal_entry je;
		size_t i;

		make_entry(&je);
		for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
			make_verdict(&v, NTFSMAC_PROBE_STATE_CLEAN);
			switch (bad[i].field) {
			case 0: v.is_ntfs = false; break;
			case 1: v.bitlocker = true; break;
			case 2: v.has_ntfs_version = false; break;
			case 3: v.logfile = NTFSMAC_PROBE_LOGFILE_UNCLEAN; break;
			case 4: v.logfile = NTFSMAC_PROBE_LOGFILE_V2_FAST_STARTUP; break;
			case 5: v.logfile = NTFSMAC_PROBE_LOGFILE_UNREADABLE; break;
			case 6: v.hiberfile = NTFSMAC_PROBE_HIBERFILE_HIBERNATED; break;
			case 7: v.hiberfile = NTFSMAC_PROBE_HIBERFILE_UNREADABLE; break;
			case 8: v.volume_flags = FLAG(DIRTY); break;
			case 9: v.volume_flags = FLAG(CHKDSK_UNDERWAY); break;
			case 10: v.volume_flags = FLAG(RESIZE_LOG_FILE); break;
			case 11: v.volume_flags = FLAG(UPGRADE_ON_MOUNT); break;
			case 12: v.volume_flags = FLAG(DELETE_USN_UNDERWAY); break;
			case 13: v.volume_flags = FLAG(REPAIR_OBJECT_ID); break;
			default: v.volume_flags = FLAG(UNKNOWN_BITS); break;
			}
			r = ntfsmac_policy_decide(&in);
			CHECK_EQ_INT(r.decision, NTFSMAC_POLICY_RO);
			CHECK_EQ_INT(r.note, NTFSMAC_POLICY_NOTE_UNKNOWN);
			CHECK(!ntfsmac_policy_evidence_holds(&v, &je));
		}
		/* The informational flags and both quiet hiberfile values stay clean. */
		make_verdict(&v, NTFSMAC_PROBE_STATE_CLEAN);
		v.volume_flags = FLAG(MODIFIED_BY_CHKDSK) | FLAG(MOUNTED_ON_NT4);
		v.hiberfile = NTFSMAC_PROBE_HIBERFILE_INACTIVE;
		r = ntfsmac_policy_decide(&in);
		CHECK_EQ_INT(r.decision, NTFSMAC_POLICY_RW);
	}

	/* The fail-closed probe verdict (probe.h: state unknown, CHECK_FAILED, rest zero). */
	memset(&v, 0, sizeof(v));
	v.state = NTFSMAC_PROBE_STATE_UNKNOWN;
	v.reason_codes = NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_CHECK_FAILED);
	v.logfile = NTFSMAC_PROBE_LOGFILE_UNREADABLE;
	v.hiberfile = NTFSMAC_PROBE_HIBERFILE_UNREADABLE;
	r = ntfsmac_policy_decide(&in);
	CHECK_EQ_INT(r.decision, NTFSMAC_POLICY_RO);
	CHECK_EQ_INT(r.note, NTFSMAC_POLICY_NOTE_UNKNOWN);
}

/*
 * U-POL-PROP: exhaustive enumeration over the enums (and a few field
 * variants). rw only for clean + no entry of this volume + auto; remove only
 * on the evidence row in auto; refuse only for corrupt, unsupported and
 * mirror_split. An entry applies when it is of format 1 (`legacy`) or has the
 * verdict's serial and total_sectors (T-M7-13).
 */
static void test_property(void)
{
	long cases = 0;
	int st, mode, ev, jc, mk, js, lsn, ser, sec, hasl, rc, leg;

	for (st = -1; st <= (int)NTFSMAC_PROBE_STATE_COUNT_ + 1; st++)
	for (mode = -1; mode <= 2; mode++)
	for (ev = 0; ev < 2; ev++)
	for (jc = 0; jc < 2; jc++)
	for (mk = -1; mk <= (int)NTFSMAC_JOURNAL_MARKER_NONE + 1; mk++)
	for (js = 0; js < 3; js++)
	for (lsn = 0; lsn < 2; lsn++)
	for (ser = 0; ser < 2; ser++)
	for (sec = 0; sec < 2; sec++)
	for (hasl = 0; hasl < 2; hasl++)
	for (leg = 0; leg < 2; leg++)
	for (rc = 0; rc < 2; rc++) {
		ntfsmac_probe_verdict v;
		ntfsmac_journal_entry e;
		ntfsmac_policy_input in;
		ntfsmac_policy_result r;
		bool valid_mode = mode == 0 || mode == 1;
		bool is_clean = st == (int)NTFSMAC_PROBE_STATE_CLEAN && rc == 0;
		bool own = jc && (leg || (!ser && !sec)); /* an entry that applies */
		bool holds;

		make_verdict(&v, NTFSMAC_PROBE_STATE_CLEAN);
		v.state = (ntfsmac_probe_state)st;
		v.reason_codes = rc ? 1u : 0u;
		v.has_logfile_lsn = hasl != 0;
		v.logfile_lsn = lsn ? LSN_START : LSN_START + 5;
		if (ser) v.serial ^= 1;
		if (sec) v.total_sectors += 1;
		make_entry(&e);
		e.marker = (ntfsmac_journal_marker)mk;
		e.state = (ntfsmac_journal_state)js;
		e.legacy = leg != 0;
		in.verdict = &v;
		in.journal = jc ? &e : NULL;
		in.mode = (ntfsmac_policy_mode)mode;
		in.evidence_rule_enabled = ev != 0;
		r = ntfsmac_policy_decide(&in);
		holds = jc && ntfsmac_policy_evidence_holds(&v, &e);
		cases++;

		/* Invariant of policy.h. */
		if (r.decision == NTFSMAC_POLICY_RW) {
			CHECK(is_clean && valid_mode && !own && mode == 0);
			CHECK_EQ_INT(r.journal_action, NTFSMAC_POLICY_JOURNAL_KEEP);
		}
		if (is_clean && valid_mode && !own && mode == 0)
			CHECK_EQ_INT(r.decision, NTFSMAC_POLICY_RW);
		if (r.journal_action == NTFSMAC_POLICY_JOURNAL_REMOVE) {
			CHECK(is_clean && mode == 0 && ev && holds);
			CHECK_EQ_INT(r.decision, NTFSMAC_POLICY_RO);
		}
		if (is_clean && mode == 0 && ev && holds)
			CHECK_EQ_INT(r.journal_action, NTFSMAC_POLICY_JOURNAL_REMOVE);
		if (r.decision == NTFSMAC_POLICY_REFUSE)
			CHECK(valid_mode && (st == (int)NTFSMAC_PROBE_STATE_CORRUPT ||
					     st == (int)NTFSMAC_PROBE_STATE_UNSUPPORTED ||
					     st == (int)NTFSMAC_PROBE_STATE_MIRROR_SPLIT));
		if (!valid_mode || st < 0 || st >= (int)NTFSMAC_PROBE_STATE_COUNT_) {
			CHECK_EQ_INT(r.decision, NTFSMAC_POLICY_RO);
			CHECK_EQ_INT(r.note, NTFSMAC_POLICY_NOTE_UNKNOWN);
		}
		/* mode=ro only narrows. */
		if (mode == 1)
			CHECK(r.decision != NTFSMAC_POLICY_RW);
		/* No row is reachable with the evidence switch off. */
		if (!ev)
			CHECK(r.note != NTFSMAC_POLICY_NOTE_EXTERNAL_CHECK &&
			      r.journal_action == NTFSMAC_POLICY_JOURNAL_KEEP);
	}
	CHECK(cases > 100000);
}

static ntfsmac_policy_journal_clear_input jc_ok(const ntfsmac_journal_entry *e,
						const ntfsmac_probe_verdict *p)
{
	ntfsmac_policy_journal_clear_input in;

	in.entry = e;
	in.probe = p;
	in.mounted_rw = false;
	in.live_session_owns_entry = false;
	in.peer_authorised = true;
	in.confirmation_ok = true;
	return in;
}

/* U-POL-GATE-JC */
static void test_gate_journal_clear(void)
{
	ntfsmac_probe_verdict v;
	ntfsmac_journal_entry e;
	ntfsmac_policy_journal_clear_input in;
	int s;

	make_verdict(&v, NTFSMAC_PROBE_STATE_CLEAN);
	make_entry(&e);
	in = jc_ok(&e, &v);
	CHECK_EQ_INT(ntfsmac_policy_gate_journal_clear(&in), NTFSMAC_GATE_ALLOWED);
	CHECK_EQ_INT(ntfsmac_policy_gate_journal_clear(NULL), NTFSMAC_GATE_INVALID_INPUT);

	in = jc_ok(&e, NULL);
	CHECK_EQ_INT(ntfsmac_policy_gate_journal_clear(&in), NTFSMAC_GATE_INVALID_INPUT);
	in = jc_ok(NULL, &v);
	CHECK_EQ_INT(ntfsmac_policy_gate_journal_clear(&in), NTFSMAC_GATE_NO_ENTRY);

	/* Any marker and any entry state is allowed. */
	for (s = 0; s < 4; s++) {
		in = jc_ok(&e, &v);
		e.marker = (ntfsmac_journal_marker)s;
		e.state = (ntfsmac_journal_state)(s & 1);
		CHECK_EQ_INT(ntfsmac_policy_gate_journal_clear(&in), NTFSMAC_GATE_ALLOWED);
	}
	make_entry(&e);

	/* Probe must be exactly clean. */
	for (s = 0; s < NTFSMAC_PROBE_STATE_COUNT_; s++) {
		make_verdict(&v, (ntfsmac_probe_state)s);
		in = jc_ok(&e, &v);
		CHECK_EQ_INT(ntfsmac_policy_gate_journal_clear(&in),
			     s == NTFSMAC_PROBE_STATE_CLEAN ? NTFSMAC_GATE_ALLOWED
							    : NTFSMAC_GATE_PROBE_NOT_CLEAN);
	}
	make_verdict(&v, NTFSMAC_PROBE_STATE_CLEAN);
	v.state = (ntfsmac_probe_state)77;
	in = jc_ok(&e, &v);
	CHECK_EQ_INT(ntfsmac_policy_gate_journal_clear(&in), NTFSMAC_GATE_PROBE_NOT_CLEAN);
	make_verdict(&v, NTFSMAC_PROBE_STATE_CLEAN);
	v.reason_codes = 1;
	CHECK_EQ_INT(ntfsmac_policy_gate_journal_clear(&in), NTFSMAC_GATE_PROBE_NOT_CLEAN);

	/* The probe must be of the entry's volume. */
	make_verdict(&v, NTFSMAC_PROBE_STATE_CLEAN);
	v.serial ^= 0x100;
	CHECK_EQ_INT(ntfsmac_policy_gate_journal_clear(&in), NTFSMAC_GATE_INVALID_INPUT);
	make_verdict(&v, NTFSMAC_PROBE_STATE_CLEAN);

	in = jc_ok(&e, &v);
	in.mounted_rw = true;
	CHECK_EQ_INT(ntfsmac_policy_gate_journal_clear(&in), NTFSMAC_GATE_MOUNTED_RW);
	in = jc_ok(&e, &v);
	in.live_session_owns_entry = true;
	CHECK_EQ_INT(ntfsmac_policy_gate_journal_clear(&in), NTFSMAC_GATE_LIVE_SESSION);
	in = jc_ok(&e, &v);
	in.peer_authorised = false;
	CHECK_EQ_INT(ntfsmac_policy_gate_journal_clear(&in), NTFSMAC_GATE_PEER);
	in = jc_ok(&e, &v);
	in.confirmation_ok = false;
	CHECK_EQ_INT(ntfsmac_policy_gate_journal_clear(&in), NTFSMAC_GATE_CONFIRMATION);

	/* First failing check wins, in table order. */
	in = jc_ok(&e, &v);
	in.mounted_rw = in.live_session_owns_entry = true;
	in.peer_authorised = in.confirmation_ok = false;
	CHECK_EQ_INT(ntfsmac_policy_gate_journal_clear(&in), NTFSMAC_GATE_MOUNTED_RW);
	in.mounted_rw = false;
	CHECK_EQ_INT(ntfsmac_policy_gate_journal_clear(&in), NTFSMAC_GATE_LIVE_SESSION);
	in.live_session_owns_entry = false;
	CHECK_EQ_INT(ntfsmac_policy_gate_journal_clear(&in), NTFSMAC_GATE_PEER);
	in.peer_authorised = true;
	CHECK_EQ_INT(ntfsmac_policy_gate_journal_clear(&in), NTFSMAC_GATE_CONFIRMATION);
	v.state = NTFSMAC_PROBE_STATE_DIRTY_FLAG;
	CHECK_EQ_INT(ntfsmac_policy_gate_journal_clear(&in), NTFSMAC_GATE_PROBE_NOT_CLEAN);
}

static ntfsmac_policy_repair_input rp_ok(const ntfsmac_probe_verdict *p)
{
	ntfsmac_policy_repair_input in;

	in.probe = p;
	in.mounted = false;
	in.peer_authorised = true;
	in.confirmation_ok = true;
	return in;
}

static void make_repairable(ntfsmac_probe_verdict *v)
{
	make_verdict(v, NTFSMAC_PROBE_STATE_DIRTY_FLAG);
	v->volume_flags = FLAG(DIRTY);
	v->reason_codes = NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_FLAG_DIRTY);
}

/* A verdict the sync-mirror gate accepts; `dirty`: record 3 split, VOLUME_IS_DIRTY set. */
static void make_splittable(ntfsmac_probe_verdict *v, int dirty)
{
	make_verdict(v, NTFSMAC_PROBE_STATE_MIRROR_SPLIT);
	v->reason_codes = NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_MFTMIRR_STALE);
	if (dirty) {
		v->reason_codes |= NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_FLAG_DIRTY);
		v->volume_flags = FLAG(DIRTY);
	}
}

/* U-POL-GATE-REPAIR and U-POL-FLAGS */
static void test_gate_repair(void)
{
	ntfsmac_probe_verdict v;
	ntfsmac_policy_repair_input in;
	uint32_t f;
	int s, l, h;

	make_repairable(&v);
	in = rp_ok(&v);
	CHECK_EQ_INT(ntfsmac_policy_gate_repair(&in), NTFSMAC_GATE_ALLOWED);
	CHECK_EQ_INT(ntfsmac_policy_gate_repair(NULL), NTFSMAC_GATE_INVALID_INPUT);
	in = rp_ok(NULL);
	CHECK_EQ_INT(ntfsmac_policy_gate_repair(&in), NTFSMAC_GATE_INVALID_INPUT);

	/* A journal entry is not required: the input has none. */
	for (s = -1; s <= (int)NTFSMAC_PROBE_STATE_COUNT_; s++) {
		make_repairable(&v);
		v.state = (ntfsmac_probe_state)s;
		in = rp_ok(&v);
		CHECK_EQ_INT(ntfsmac_policy_gate_repair(&in),
			     s == (int)NTFSMAC_PROBE_STATE_DIRTY_FLAG
				     ? NTFSMAC_GATE_ALLOWED
				     : NTFSMAC_GATE_PROBE_NOT_DIRTY_FLAG);
	}
	for (l = -1; l <= (int)NTFSMAC_PROBE_LOGFILE_COUNT_; l++) {
		make_repairable(&v);
		v.logfile = (ntfsmac_probe_logfile)l;
		in = rp_ok(&v);
		CHECK_EQ_INT(ntfsmac_policy_gate_repair(&in),
			     l == (int)NTFSMAC_PROBE_LOGFILE_CLEAN ? NTFSMAC_GATE_ALLOWED
								   : NTFSMAC_GATE_LOG_NOT_CLEAN);
	}
	for (h = -1; h <= (int)NTFSMAC_PROBE_HIBERFILE_COUNT_; h++) {
		bool ok = h == (int)NTFSMAC_PROBE_HIBERFILE_ABSENT ||
			  h == (int)NTFSMAC_PROBE_HIBERFILE_INACTIVE;

		make_repairable(&v);
		v.hiberfile = (ntfsmac_probe_hiberfile)h;
		in = rp_ok(&v);
		CHECK_EQ_INT(ntfsmac_policy_gate_repair(&in),
			     ok ? NTFSMAC_GATE_ALLOWED : NTFSMAC_GATE_HIBERFILE);
	}
	/* Every volume_flags value, including bits outside the nine. */
	for (f = 0; f < 1024; f++) {
		uint32_t allowed = FLAG(DIRTY) | FLAG(MODIFIED_BY_CHKDSK) | FLAG(MOUNTED_ON_NT4);
		bool ok = (f & FLAG(DIRTY)) != 0 && (f & ~allowed) == 0;

		make_repairable(&v);
		v.volume_flags = f;
		in = rp_ok(&v);
		CHECK_EQ_INT(ntfsmac_policy_gate_repair(&in),
			     ok ? NTFSMAC_GATE_ALLOWED : NTFSMAC_GATE_FLAGS);
	}
	make_repairable(&v);
	v.volume_flags = 0xFFFFFFFFu;
	in = rp_ok(&v);
	CHECK_EQ_INT(ntfsmac_policy_gate_repair(&in), NTFSMAC_GATE_FLAGS);

	/* Reason codes: FLAG_DIRTY, and nothing else but NAMES_NOT_NFC. */
	{
		static const ntfsmac_probe_reason bad[] = {
			NTFSMAC_PROBE_REASON_CHECK_FAILED, NTFSMAC_PROBE_REASON_VERSION_UNKNOWN,
			NTFSMAC_PROBE_REASON_LOG_UNREADABLE, NTFSMAC_PROBE_REASON_HIBER_UNREADABLE,
			NTFSMAC_PROBE_REASON_MOUNT_FAILED, NTFSMAC_PROBE_REASON_MFTMIRR_STALE,
			NTFSMAC_PROBE_REASON_FLAG_CHKDSK_UNDERWAY,
		};
		unsigned i;

		for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
			make_repairable(&v);
			v.reason_codes |= NTFSMAC_PROBE_REASON_BIT(bad[i]);
			in = rp_ok(&v);
			CHECK_EQ_INT(ntfsmac_policy_gate_repair(&in), NTFSMAC_GATE_PROBE_NOT_DIRTY_FLAG);
			v.reason_codes |= NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_NAMES_NOT_NFC);
			CHECK_EQ_INT(ntfsmac_policy_gate_repair(&in), NTFSMAC_GATE_PROBE_NOT_DIRTY_FLAG);
		}
		make_repairable(&v);
		v.reason_codes = NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_FLAG_DIRTY) |
				 NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_NAMES_NOT_NFC);
		in = rp_ok(&v);
		CHECK_EQ_INT(ntfsmac_policy_gate_repair(&in), NTFSMAC_GATE_ALLOWED);
		v.reason_codes = NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_NAMES_NOT_NFC); /* FLAG_DIRTY missing */
		CHECK_EQ_INT(ntfsmac_policy_gate_repair(&in), NTFSMAC_GATE_PROBE_NOT_DIRTY_FLAG);
		v.reason_codes = 0;
		CHECK_EQ_INT(ntfsmac_policy_gate_repair(&in), NTFSMAC_GATE_PROBE_NOT_DIRTY_FLAG);
	}

	/* U-POL-FLAGS named cases. */
	make_repairable(&v);
	v.volume_flags = FLAG(DIRTY) | FLAG(MODIFIED_BY_CHKDSK);
	in = rp_ok(&v);
	CHECK_EQ_INT(ntfsmac_policy_gate_repair(&in), NTFSMAC_GATE_ALLOWED);
	v.volume_flags = FLAG(CHKDSK_UNDERWAY);
	CHECK_EQ_INT(ntfsmac_policy_gate_repair(&in), NTFSMAC_GATE_FLAGS);
	v.volume_flags = FLAG(UNKNOWN_BITS);
	CHECK_EQ_INT(ntfsmac_policy_gate_repair(&in), NTFSMAC_GATE_FLAGS);
	v.volume_flags = FLAG(DIRTY) | FLAG(UNKNOWN_BITS);
	CHECK_EQ_INT(ntfsmac_policy_gate_repair(&in), NTFSMAC_GATE_FLAGS);
	{
		/* Only informational flags: the volume is clean, journal-clear opens. */
		ntfsmac_probe_verdict c;
		ntfsmac_journal_entry e;
		ntfsmac_policy_journal_clear_input jc;

		make_verdict(&c, NTFSMAC_PROBE_STATE_CLEAN);
		c.volume_flags = FLAG(MODIFIED_BY_CHKDSK);
		make_entry(&e);
		jc = jc_ok(&e, &c);
		CHECK_EQ_INT(ntfsmac_policy_gate_journal_clear(&jc), NTFSMAC_GATE_ALLOWED);
		c.volume_flags = FLAG(MOUNTED_ON_NT4);
		CHECK_EQ_INT(ntfsmac_policy_gate_journal_clear(&jc), NTFSMAC_GATE_ALLOWED);
	}

	make_repairable(&v);
	in = rp_ok(&v);
	in.mounted = true;
	CHECK_EQ_INT(ntfsmac_policy_gate_repair(&in), NTFSMAC_GATE_MOUNTED);
	in = rp_ok(&v);
	in.peer_authorised = false;
	CHECK_EQ_INT(ntfsmac_policy_gate_repair(&in), NTFSMAC_GATE_PEER);
	in = rp_ok(&v);
	in.confirmation_ok = false;
	CHECK_EQ_INT(ntfsmac_policy_gate_repair(&in), NTFSMAC_GATE_CONFIRMATION);

	/* Order of the table. */
	in = rp_ok(&v);
	in.mounted = true;
	in.peer_authorised = in.confirmation_ok = false;
	CHECK_EQ_INT(ntfsmac_policy_gate_repair(&in), NTFSMAC_GATE_MOUNTED);
	in.mounted = false;
	CHECK_EQ_INT(ntfsmac_policy_gate_repair(&in), NTFSMAC_GATE_PEER);
	v.volume_flags = 0;
	CHECK_EQ_INT(ntfsmac_policy_gate_repair(&in), NTFSMAC_GATE_FLAGS);
	v.hiberfile = NTFSMAC_PROBE_HIBERFILE_HIBERNATED;
	CHECK_EQ_INT(ntfsmac_policy_gate_repair(&in), NTFSMAC_GATE_HIBERFILE);
	v.logfile = NTFSMAC_PROBE_LOGFILE_UNCLEAN;
	CHECK_EQ_INT(ntfsmac_policy_gate_repair(&in), NTFSMAC_GATE_LOG_NOT_CLEAN);
	v.state = NTFSMAC_PROBE_STATE_CLEAN;
	CHECK_EQ_INT(ntfsmac_policy_gate_repair(&in), NTFSMAC_GATE_PROBE_NOT_DIRTY_FLAG);
}

/*
 * U-POL-NAMES (ADR-023): `names_not_nfc` is `ro` in both columns, with and
 * without a journal entry, with the evidence switch on or off, and never
 * `remove`. NAMES_NOT_NFC beside another state does not change that state's
 * row, and the `repair --clear-dirty` and `repair --sync-mirror` gates ignore
 * it (the second one in test_names() below and in U-POL-SYNCMIRROR).
 */
static void test_names(void)
{
	static const ntfsmac_probe_state with_code[] = {
		NTFSMAC_PROBE_STATE_DIRTY_FLAG, NTFSMAC_PROBE_STATE_UNCLEAN_LOG,
		NTFSMAC_PROBE_STATE_HIBERNATED, NTFSMAC_PROBE_STATE_FAST_STARTUP,
		NTFSMAC_PROBE_STATE_UNKNOWN,    NTFSMAC_PROBE_STATE_MIRROR_SPLIT,
		NTFSMAC_PROBE_STATE_CORRUPT,    NTFSMAC_PROBE_STATE_UNSUPPORTED,
	};
	ntfsmac_probe_verdict v;
	ntfsmac_journal_entry e;
	ntfsmac_policy_input in;
	ntfsmac_policy_result r;
	ntfsmac_policy_repair_input rp;
	size_t i;
	int m, ev, jc;

	/* The state itself. */
	for (m = 0; m < 2; m++)
		for (ev = 0; ev < 2; ev++)
			for (jc = 0; jc < 3; jc++) {
				make_verdict(&v, NTFSMAC_PROBE_STATE_NAMES_NOT_NFC);
				v.reason_codes = NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_NAMES_NOT_NFC);
				v.nonnfc_count = 22;
				v.nonnfc_path_count = 16;
				in.verdict = &v;
				in.journal = pick((enum jcase)jc, &e);
				in.mode = (ntfsmac_policy_mode)m;
				in.evidence_rule_enabled = ev != 0;
				r = ntfsmac_policy_decide(&in);
				CHECK_EQ_INT(r.decision, NTFSMAC_POLICY_RO);
				CHECK_EQ_INT(r.journal_action, NTFSMAC_POLICY_JOURNAL_KEEP);
				CHECK_EQ_INT(r.note, NTFSMAC_POLICY_NOTE_NAMES_NOT_NFC);
			}

	/* The code beside any other state: that state's row. */
	for (i = 0; i < sizeof(with_code) / sizeof(with_code[0]); i++)
		for (m = 0; m < 2; m++)
			for (jc = 0; jc < 3; jc++) {
				ntfsmac_policy_result plain, coded;

				make_verdict(&v, with_code[i]);
				in.verdict = &v;
				in.journal = pick((enum jcase)jc, &e);
				in.mode = (ntfsmac_policy_mode)m;
				in.evidence_rule_enabled = true;
				plain = ntfsmac_policy_decide(&in);
				v.reason_codes |= NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_NAMES_NOT_NFC);
				v.nonnfc_count = 1;
				v.nonnfc_path_count = 1;
				coded = ntfsmac_policy_decide(&in);
				CHECK_EQ_INT(coded.decision, plain.decision);
				CHECK_EQ_INT(coded.journal_action, plain.journal_action);
				CHECK_EQ_INT(coded.note, plain.note);
			}
	/* mirror_split with MFTMIRR_STALE and NAMES_NOT_NFC: still refused (ADR-021). */
	make_verdict(&v, NTFSMAC_PROBE_STATE_MIRROR_SPLIT);
	v.reason_codes = NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_MFTMIRR_STALE) |
			 NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_NAMES_NOT_NFC);
	in.verdict = &v;
	in.journal = NULL;
	in.mode = NTFSMAC_POLICY_MODE_AUTO;
	in.evidence_rule_enabled = true;
	r = ntfsmac_policy_decide(&in);
	CHECK_EQ_INT(r.decision, NTFSMAC_POLICY_REFUSE);
	CHECK_EQ_INT(r.note, NTFSMAC_POLICY_NOTE_MIRROR_SPLIT);

	/* repair --clear-dirty: dirty_flag with NAMES_NOT_NFC is repairable. */
	make_repairable(&v);
	rp = rp_ok(&v);
	CHECK_EQ_INT(ntfsmac_policy_gate_repair(&rp), NTFSMAC_GATE_ALLOWED);
	v.reason_codes = NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_FLAG_DIRTY) |
			 NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_NAMES_NOT_NFC);
	v.nonnfc_count = 22;
	v.nonnfc_path_count = 16;
	CHECK_EQ_INT(ntfsmac_policy_gate_repair(&rp), NTFSMAC_GATE_ALLOWED);
	/* ... and the gate still looks at everything else. */
	v.logfile = NTFSMAC_PROBE_LOGFILE_UNCLEAN;
	CHECK_EQ_INT(ntfsmac_policy_gate_repair(&rp), NTFSMAC_GATE_LOG_NOT_CLEAN);
	v.logfile = NTFSMAC_PROBE_LOGFILE_CLEAN;
	rp.mounted = true;
	CHECK_EQ_INT(ntfsmac_policy_gate_repair(&rp), NTFSMAC_GATE_MOUNTED);
	/* A names_not_nfc verdict is not a dirty_flag verdict: no repair. */
	rp.mounted = false;
	v.state = NTFSMAC_PROBE_STATE_NAMES_NOT_NFC;
	CHECK_EQ_INT(ntfsmac_policy_gate_repair(&rp), NTFSMAC_GATE_PROBE_NOT_DIRTY_FLAG);

	/* repair --sync-mirror: mirror_split with both codes is repairable (U-POL-NAMES). */
	make_splittable(&v, 0);
	v.reason_codes |= NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_NAMES_NOT_NFC);
	v.nonnfc_count = 22;
	v.nonnfc_path_count = 16;
	rp = rp_ok(&v);
	CHECK_EQ_INT(ntfsmac_policy_gate_sync_mirror(&rp), NTFSMAC_GATE_ALLOWED);
}

/* U-POL-SYNCMIRROR: the `repair --sync-mirror` gate (section 3.2, ADR-021), each condition alone. */
static void test_gate_sync_mirror(void)
{
	static const int other_codes[] = {
		NTFSMAC_PROBE_REASON_LOG_UNCLEAN,   NTFSMAC_PROBE_REASON_HIBER_SIGNATURE,
		NTFSMAC_PROBE_REASON_CHECK_FAILED,  NTFSMAC_PROBE_REASON_VERSION_UNKNOWN,
		NTFSMAC_PROBE_REASON_MOUNT_FAILED,
	};
	ntfsmac_probe_verdict v;
	ntfsmac_policy_repair_input in;
	size_t i;
	int s;

	/* Allowed: MFTMIRR_STALE alone, and with FLAG_DIRTY (a split of record 3). */
	make_splittable(&v, 0);
	in = rp_ok(&v);
	CHECK_EQ_INT(ntfsmac_policy_gate_sync_mirror(&in), NTFSMAC_GATE_ALLOWED);
	make_splittable(&v, 1);
	CHECK_EQ_INT(ntfsmac_policy_gate_sync_mirror(&in), NTFSMAC_GATE_ALLOWED);
	/* The informational volume flags do not matter to the gate. */
	v.volume_flags |= FLAG(MODIFIED_BY_CHKDSK);
	CHECK_EQ_INT(ntfsmac_policy_gate_sync_mirror(&in), NTFSMAC_GATE_ALLOWED);

	/* Bad input. */
	CHECK_EQ_INT(ntfsmac_policy_gate_sync_mirror(NULL), NTFSMAC_GATE_INVALID_INPUT);
	in.probe = NULL;
	CHECK_EQ_INT(ntfsmac_policy_gate_sync_mirror(&in), NTFSMAC_GATE_INVALID_INPUT);

	/* Every state but mirror_split is refused, with the code set that fits it or not. */
	for (s = 0; s < (int)NTFSMAC_PROBE_STATE_COUNT_; s++) {
		if (s == (int)NTFSMAC_PROBE_STATE_MIRROR_SPLIT)
			continue;
		make_splittable(&v, 0);
		in = rp_ok(&v);
		v.state = (ntfsmac_probe_state)s;
		CHECK_EQ_INT(ntfsmac_policy_gate_sync_mirror(&in), NTFSMAC_GATE_PROBE_NOT_MIRROR_SPLIT);
	}
	/* The state with no MFTMIRR_STALE code, and with no code at all. */
	make_splittable(&v, 0);
	in = rp_ok(&v);
	v.reason_codes = 0;
	CHECK_EQ_INT(ntfsmac_policy_gate_sync_mirror(&in), NTFSMAC_GATE_PROBE_NOT_MIRROR_SPLIT);
	v.reason_codes = NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_FLAG_DIRTY);
	CHECK_EQ_INT(ntfsmac_policy_gate_sync_mirror(&in), NTFSMAC_GATE_PROBE_NOT_MIRROR_SPLIT);
	/* Any one other code beside MFTMIRR_STALE. */
	for (i = 0; i < sizeof(other_codes) / sizeof(other_codes[0]); i++) {
		make_splittable(&v, 0);
		v.reason_codes |= NTFSMAC_PROBE_REASON_BIT(other_codes[i]);
		CHECK_EQ_INT(ntfsmac_policy_gate_sync_mirror(&in), NTFSMAC_GATE_PROBE_NOT_MIRROR_SPLIT);
	}

	/* The log, the hibernation file, mounted, peer, confirmation: each alone. */
	make_splittable(&v, 0);
	v.logfile = NTFSMAC_PROBE_LOGFILE_UNCLEAN;
	CHECK_EQ_INT(ntfsmac_policy_gate_sync_mirror(&in), NTFSMAC_GATE_LOG_NOT_CLEAN);
	v.logfile = NTFSMAC_PROBE_LOGFILE_UNREADABLE;
	CHECK_EQ_INT(ntfsmac_policy_gate_sync_mirror(&in), NTFSMAC_GATE_LOG_NOT_CLEAN);
	make_splittable(&v, 0);
	v.hiberfile = NTFSMAC_PROBE_HIBERFILE_HIBERNATED;
	CHECK_EQ_INT(ntfsmac_policy_gate_sync_mirror(&in), NTFSMAC_GATE_HIBERFILE);
	v.hiberfile = NTFSMAC_PROBE_HIBERFILE_INACTIVE;
	CHECK_EQ_INT(ntfsmac_policy_gate_sync_mirror(&in), NTFSMAC_GATE_ALLOWED);
	make_splittable(&v, 0);
	in = rp_ok(&v);
	in.mounted = true;
	CHECK_EQ_INT(ntfsmac_policy_gate_sync_mirror(&in), NTFSMAC_GATE_MOUNTED);
	in = rp_ok(&v);
	in.peer_authorised = false;
	CHECK_EQ_INT(ntfsmac_policy_gate_sync_mirror(&in), NTFSMAC_GATE_PEER);
	in = rp_ok(&v);
	in.confirmation_ok = false;
	CHECK_EQ_INT(ntfsmac_policy_gate_sync_mirror(&in), NTFSMAC_GATE_CONFIRMATION);

	/* Order of the table: state, log, hibernation file, mounted, peer, confirmation. */
	in = rp_ok(&v);
	in.mounted = true;
	in.peer_authorised = in.confirmation_ok = false;
	CHECK_EQ_INT(ntfsmac_policy_gate_sync_mirror(&in), NTFSMAC_GATE_MOUNTED);
	in.mounted = false;
	CHECK_EQ_INT(ntfsmac_policy_gate_sync_mirror(&in), NTFSMAC_GATE_PEER);
	in.peer_authorised = true;
	CHECK_EQ_INT(ntfsmac_policy_gate_sync_mirror(&in), NTFSMAC_GATE_CONFIRMATION);
	in.mounted = true;
	v.hiberfile = NTFSMAC_PROBE_HIBERFILE_HIBERNATED;
	CHECK_EQ_INT(ntfsmac_policy_gate_sync_mirror(&in), NTFSMAC_GATE_HIBERFILE);
	v.logfile = NTFSMAC_PROBE_LOGFILE_UNCLEAN;
	CHECK_EQ_INT(ntfsmac_policy_gate_sync_mirror(&in), NTFSMAC_GATE_LOG_NOT_CLEAN);
	v.state = NTFSMAC_PROBE_STATE_CLEAN;
	CHECK_EQ_INT(ntfsmac_policy_gate_sync_mirror(&in), NTFSMAC_GATE_PROBE_NOT_MIRROR_SPLIT);

	/* A journal entry is neither needed nor looked at: the input has no member for it. */
	make_splittable(&v, 1);
	in = rp_ok(&v);
	CHECK_EQ_INT(ntfsmac_policy_gate_sync_mirror(&in), NTFSMAC_GATE_ALLOWED);
}

/* U-POL-WARN */
static void test_warning(void)
{
	ntfsmac_probe_verdict v;
	ntfsmac_journal_entry e;

	make_verdict(&v, NTFSMAC_PROBE_STATE_CLEAN);
	make_entry(&e);
	CHECK_EQ_INT(ntfsmac_policy_journal_clear_warning(&e, &v), NTFSMAC_CLEAR_WARNING_NONE);
	CHECK_EQ_INT(ntfsmac_policy_journal_clear_warning(NULL, &v), NTFSMAC_CLEAR_WARNING_NONE);
	CHECK_EQ_INT(ntfsmac_policy_journal_clear_warning(&e, NULL), NTFSMAC_CLEAR_WARNING_NONE);

	v.logfile_lsn = e.logfile_lsn_at_start;
	CHECK_EQ_INT(ntfsmac_policy_journal_clear_warning(&e, &v), NTFSMAC_CLEAR_WARNING_LOST_FLUSH);
	v.has_logfile_lsn = false;
	CHECK_EQ_INT(ntfsmac_policy_journal_clear_warning(&e, &v), NTFSMAC_CLEAR_WARNING_NONE);
	v.has_logfile_lsn = true;
	e.marker = NTFSMAC_JOURNAL_MARKER_FAILED;
	CHECK_EQ_INT(ntfsmac_policy_journal_clear_warning(&e, &v), NTFSMAC_CLEAR_WARNING_NONE);
	e.marker = NTFSMAC_JOURNAL_MARKER_SET;
	v.state = NTFSMAC_PROBE_STATE_UNKNOWN;
	CHECK_EQ_INT(ntfsmac_policy_journal_clear_warning(&e, &v), NTFSMAC_CLEAR_WARNING_NONE);
	v.state = NTFSMAC_PROBE_STATE_CLEAN;

	/* write_error wins (A8), also when nothing else holds. */
	e.state = NTFSMAC_JOURNAL_STATE_WRITE_ERROR;
	CHECK_EQ_INT(ntfsmac_policy_journal_clear_warning(&e, &v), NTFSMAC_CLEAR_WARNING_WRITE_ERROR);
	v.logfile_lsn = e.logfile_lsn_at_start + 9;
	e.marker = NTFSMAC_JOURNAL_MARKER_PENDING;
	CHECK_EQ_INT(ntfsmac_policy_journal_clear_warning(&e, &v), NTFSMAC_CLEAR_WARNING_WRITE_ERROR);

	CHECK(strcmp(ntfsmac_policy_decision_name(NTFSMAC_POLICY_RW), "rw") == 0);
	CHECK(strcmp(ntfsmac_policy_decision_name(NTFSMAC_POLICY_RO), "ro") == 0);
	CHECK(strcmp(ntfsmac_policy_decision_name(NTFSMAC_POLICY_REFUSE), "refuse") == 0);
	CHECK(ntfsmac_policy_decision_name((ntfsmac_policy_decision)9) == NULL);
}

/*
 * U-POL-IDENT (T-M7-13): an entry applies only to the volume it was written for. Serial S with
 * total_sectors A in the entry, a clean probed volume S with B: the decision of "no entry". Same
 * identity, or an entry of format 1 (identity unknown), or a stand-in without total_sectors:
 * read-only with UNCLEAN_SESSION, as before.
 */
static void test_identity(void)
{
	ntfsmac_probe_verdict v;
	ntfsmac_journal_entry e;
	ntfsmac_policy_input in;
	ntfsmac_policy_result r;
	int mode;

	make_verdict(&v, NTFSMAC_PROBE_STATE_CLEAN);
	for (mode = 0; mode < 2; mode++) {
		ntfsmac_policy_result none;

		memset(&in, 0, sizeof(in));
		in.verdict = &v;
		in.mode = (ntfsmac_policy_mode)mode;
		none = ntfsmac_policy_decide(&in);

		/* another volume with the same serial: as if there were no entry */
		make_entry(&e);
		e.total_sectors = SECTORS + 1;
		CHECK(!ntfsmac_policy_entry_applies(&v, &e));
		in.journal = &e;
		r = ntfsmac_policy_decide(&in);
		CHECK_EQ_INT(r.decision, none.decision);
		CHECK_EQ_INT(r.note, none.note);
		CHECK_EQ_INT(r.journal_action, NTFSMAC_POLICY_JOURNAL_KEEP);
		CHECK_EQ_INT(r.decision, mode == 0 ? NTFSMAC_POLICY_RW : NTFSMAC_POLICY_RO);

		/* same identity: read-only, unclean session */
		make_entry(&e);
		CHECK(ntfsmac_policy_entry_applies(&v, &e));
		r = ntfsmac_policy_decide(&in);
		CHECK_EQ_INT(r.decision, NTFSMAC_POLICY_RO);
		CHECK_EQ_INT(r.note, NTFSMAC_POLICY_NOTE_UNCLEAN_SESSION);

		/* format 1 (no identity), even with another total_sectors: read-only as before */
		make_entry(&e);
		e.legacy = true;
		e.total_sectors = SECTORS + 1;
		CHECK(ntfsmac_policy_entry_applies(&v, &e));
		r = ntfsmac_policy_decide(&in);
		CHECK_EQ_INT(r.decision, NTFSMAC_POLICY_RO);
		CHECK_EQ_INT(r.note, NTFSMAC_POLICY_NOTE_UNCLEAN_SESSION);

		/* the stand-in of a corrupt entry (no total_sectors): applies */
		make_entry(&e);
		e.total_sectors = 0;
		CHECK(ntfsmac_policy_entry_applies(&v, &e));
		r = ntfsmac_policy_decide(&in);
		CHECK_EQ_INT(r.decision, NTFSMAC_POLICY_RO);
	}
	/* a foreign entry does not change the note of an unsafe state either */
	make_verdict(&v, NTFSMAC_PROBE_STATE_DIRTY_FLAG);
	v.reason_codes = NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_FLAG_DIRTY);
	make_entry(&e);
	e.total_sectors = SECTORS + 1;
	memset(&in, 0, sizeof(in));
	in.verdict = &v;
	in.journal = &e;
	r = ntfsmac_policy_decide(&in);
	CHECK_EQ_INT(r.decision, NTFSMAC_POLICY_RO);
	CHECK_EQ_INT(r.note, NTFSMAC_POLICY_NOTE_DIRTY_FLAG);
	CHECK(!ntfsmac_policy_entry_applies(NULL, &e));
	CHECK(!ntfsmac_policy_entry_applies(&v, NULL));
}

int main(void)
{
	test_identity();
	test_table();
	test_evidence();
	test_fail_closed();
	test_property();
	test_gate_journal_clear();
	test_gate_repair();
	test_names();
	test_gate_sync_mirror();
	test_warning();
	if (g_failures != 0) {
		fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	printf("policy: all checks passed\n");
	return 0;
}
