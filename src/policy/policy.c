/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Policy table and gates (docs/ARCHITECTURE.md section 3.2, ADR-005, ADR-017).
 * Pure C: no I/O, no clock, no environment. Every unexpected input resolves
 * to read-only (decide) or to a refusal (gates); nothing here can produce
 * `rw` outside the one row of the table that allows it.
 */
#include "ntfsmac/policy.h"

#include <stddef.h>

/* Informational flags that leave a volume `clean` (section 3.1 check 3). */
#define INFO_FLAGS                                                  \
	(NTFSMAC_PROBE_VOLFLAG_BIT(NTFSMAC_PROBE_VOLFLAG_MODIFIED_BY_CHKDSK) | \
	 NTFSMAC_PROBE_VOLFLAG_BIT(NTFSMAC_PROBE_VOLFLAG_MOUNTED_ON_NT4))

static ntfsmac_policy_result make_result(ntfsmac_policy_decision d,
					 ntfsmac_policy_journal_action a,
					 ntfsmac_policy_note n)
{
	ntfsmac_policy_result r;

	r.decision = d;
	r.journal_action = a;
	r.note = n;
	return r;
}

/* A verdict whose enums are in range; anything else is treated as unknown. */
static bool verdict_in_range(const ntfsmac_probe_verdict *v)
{
	return (unsigned)v->state < (unsigned)NTFSMAC_PROBE_STATE_COUNT_;
}

/*
 * `clean` must agree with its cross-field rule (state clean iff no reason
 * code, section 3.1). A verdict that says `clean` with a reason code is
 * inconsistent and is never trusted for rw.
 */
static bool verdict_is_clean(const ntfsmac_probe_verdict *v)
{
	return v->state == NTFSMAC_PROBE_STATE_CLEAN && v->reason_codes == 0;
}

bool ntfsmac_policy_evidence_holds(const ntfsmac_probe_verdict *v,
				   const ntfsmac_journal_entry *e)
{
	if (v == NULL || e == NULL)
		return false;
	if (e->marker != NTFSMAC_JOURNAL_MARKER_SET)
		return false;
	if (!verdict_is_clean(v))
		return false;
	if (!v->has_logfile_lsn || v->logfile_lsn == e->logfile_lsn_at_start)
		return false;
	if (v->serial != e->serial || v->total_sectors != e->total_sectors)
		return false;
	return true;
}

ntfsmac_policy_result ntfsmac_policy_decide(const ntfsmac_policy_input *in)
{
	const ntfsmac_probe_verdict *v;
	const ntfsmac_journal_entry *e;
	bool auto_mode;

	if (in == NULL || in->verdict == NULL)
		goto unknown;
	if (in->mode != NTFSMAC_POLICY_MODE_AUTO && in->mode != NTFSMAC_POLICY_MODE_RO)
		goto unknown;
	v = in->verdict;
	e = in->journal;
	if (!verdict_in_range(v))
		goto unknown;
	auto_mode = in->mode == NTFSMAC_POLICY_MODE_AUTO;

	switch (v->state) {
	case NTFSMAC_PROBE_STATE_CLEAN:
		if (!verdict_is_clean(v))
			goto unknown;
		if (e == NULL) {
			if (auto_mode)
				return make_result(NTFSMAC_POLICY_RW,
						   NTFSMAC_POLICY_JOURNAL_KEEP,
						   NTFSMAC_POLICY_NOTE_NONE);
			return make_result(NTFSMAC_POLICY_RO,
					   NTFSMAC_POLICY_JOURNAL_KEEP,
					   NTFSMAC_POLICY_NOTE_MODE_RO_REQUESTED);
		}
		if (in->evidence_rule_enabled && ntfsmac_policy_evidence_holds(v, e))
			return make_result(NTFSMAC_POLICY_RO,
					   auto_mode ? NTFSMAC_POLICY_JOURNAL_REMOVE
						     : NTFSMAC_POLICY_JOURNAL_KEEP,
					   NTFSMAC_POLICY_NOTE_EXTERNAL_CHECK);
		return make_result(NTFSMAC_POLICY_RO, NTFSMAC_POLICY_JOURNAL_KEEP,
				   NTFSMAC_POLICY_NOTE_UNCLEAN_SESSION);
	case NTFSMAC_PROBE_STATE_DIRTY_FLAG:
		return make_result(NTFSMAC_POLICY_RO, NTFSMAC_POLICY_JOURNAL_KEEP,
				   e != NULL ? NTFSMAC_POLICY_NOTE_DIRTY_FLAG_JOURNAL
					     : NTFSMAC_POLICY_NOTE_DIRTY_FLAG);
	case NTFSMAC_PROBE_STATE_UNCLEAN_LOG:
		return make_result(NTFSMAC_POLICY_RO, NTFSMAC_POLICY_JOURNAL_KEEP,
				   NTFSMAC_POLICY_NOTE_UNCLEAN_LOG);
	case NTFSMAC_PROBE_STATE_HIBERNATED:
	case NTFSMAC_PROBE_STATE_FAST_STARTUP:
		return make_result(NTFSMAC_POLICY_RO, NTFSMAC_POLICY_JOURNAL_KEEP,
				   NTFSMAC_POLICY_NOTE_HIBERNATED);
	case NTFSMAC_PROBE_STATE_UNSUPPORTED:
	case NTFSMAC_PROBE_STATE_CORRUPT:
		return make_result(NTFSMAC_POLICY_REFUSE, NTFSMAC_POLICY_JOURNAL_KEEP,
				   NTFSMAC_POLICY_NOTE_REFUSED);
	case NTFSMAC_PROBE_STATE_UNKNOWN:
	case NTFSMAC_PROBE_STATE_COUNT_:
	default:
		break;
	}

unknown:
	return make_result(NTFSMAC_POLICY_RO, NTFSMAC_POLICY_JOURNAL_KEEP,
			   NTFSMAC_POLICY_NOTE_UNKNOWN);
}

ntfsmac_policy_gate ntfsmac_policy_gate_journal_clear(const ntfsmac_policy_journal_clear_input *in)
{
	if (in == NULL || in->probe == NULL)
		return NTFSMAC_GATE_INVALID_INPUT;
	if (in->entry == NULL)
		return NTFSMAC_GATE_NO_ENTRY;
	if (!verdict_is_clean(in->probe))
		return NTFSMAC_GATE_PROBE_NOT_CLEAN;
	/*
	 * Not a row of the gate table, but the probe must describe the volume
	 * the entry belongs to: a probe of another serial proves nothing about
	 * this entry (fail closed).
	 */
	if (in->probe->serial != in->entry->serial)
		return NTFSMAC_GATE_INVALID_INPUT;
	if (in->mounted_rw)
		return NTFSMAC_GATE_MOUNTED_RW;
	if (in->live_session_owns_entry)
		return NTFSMAC_GATE_LIVE_SESSION;
	if (!in->peer_authorised)
		return NTFSMAC_GATE_PEER;
	if (!in->confirmation_ok)
		return NTFSMAC_GATE_CONFIRMATION;
	return NTFSMAC_GATE_ALLOWED;
}

ntfsmac_policy_gate ntfsmac_policy_gate_repair(const ntfsmac_policy_repair_input *in)
{
	const ntfsmac_probe_verdict *p;
	uint32_t dirty = NTFSMAC_PROBE_VOLFLAG_BIT(NTFSMAC_PROBE_VOLFLAG_DIRTY);

	if (in == NULL || in->probe == NULL)
		return NTFSMAC_GATE_INVALID_INPUT;
	p = in->probe;
	if (p->state != NTFSMAC_PROBE_STATE_DIRTY_FLAG)
		return NTFSMAC_GATE_PROBE_NOT_DIRTY_FLAG;
	if (p->logfile != NTFSMAC_PROBE_LOGFILE_CLEAN)
		return NTFSMAC_GATE_LOG_NOT_CLEAN;
	if (p->hiberfile != NTFSMAC_PROBE_HIBERFILE_ABSENT &&
	    p->hiberfile != NTFSMAC_PROBE_HIBERFILE_INACTIVE)
		return NTFSMAC_GATE_HIBERFILE;
	if ((p->volume_flags & dirty) == 0 || (p->volume_flags & ~(dirty | INFO_FLAGS)) != 0)
		return NTFSMAC_GATE_FLAGS;
	if (in->mounted)
		return NTFSMAC_GATE_MOUNTED;
	if (!in->peer_authorised)
		return NTFSMAC_GATE_PEER;
	if (!in->confirmation_ok)
		return NTFSMAC_GATE_CONFIRMATION;
	return NTFSMAC_GATE_ALLOWED;
}

ntfsmac_policy_clear_warning ntfsmac_policy_journal_clear_warning(
	const ntfsmac_journal_entry *entry, const ntfsmac_probe_verdict *probe)
{
	if (entry == NULL || probe == NULL)
		return NTFSMAC_CLEAR_WARNING_NONE;
	if (entry->state == NTFSMAC_JOURNAL_STATE_WRITE_ERROR)
		return NTFSMAC_CLEAR_WARNING_WRITE_ERROR;
	if (entry->marker == NTFSMAC_JOURNAL_MARKER_SET &&
	    probe->state == NTFSMAC_PROBE_STATE_CLEAN && probe->has_logfile_lsn &&
	    probe->logfile_lsn == entry->logfile_lsn_at_start)
		return NTFSMAC_CLEAR_WARNING_LOST_FLUSH;
	return NTFSMAC_CLEAR_WARNING_NONE;
}

const char *ntfsmac_policy_decision_name(ntfsmac_policy_decision d)
{
	switch (d) {
	case NTFSMAC_POLICY_RW:
		return "rw";
	case NTFSMAC_POLICY_RO:
		return "ro";
	case NTFSMAC_POLICY_REFUSE:
		return "refuse";
	}
	return NULL;
}
