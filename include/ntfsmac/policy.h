/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_POLICY_H
#define NTFSMAC_POLICY_H

/*
 * Policy contract (T-M3-00). Frozen: changes only through a
 * coordinator-scheduled task.
 *
 * Source of truth: docs/ARCHITECTURE.md section 3.2 (policy table, evidence
 * rule, `journal-clear` and `repair` gates), section 3.5 (evidence rule and
 * warnings), section 3.4 (the peer and confirmation checks the gates take as
 * inputs), ADR-005 (no override from an unsafe state to rw), ADR-017.
 *
 * The library is pure C: no I/O, no clock, no environment. Implemented in
 * src/policy/ (T-M3-03). Declarations only.
 *
 * Invariant (unit-tested): rw is produced only for verdict state `clean`, no
 * journal entry for the serial, and `mode=auto`. `recover` and
 * `remove_hiberfile` are not representable here.
 */

#include <stdbool.h>
#include <stdint.h>

#include "ntfsmac/journal.h"
#include "ntfsmac/probe.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Request column of the table (section 3.2): `mode=auto` or `mode=ro`. */
typedef enum ntfsmac_policy_mode {
	NTFSMAC_POLICY_MODE_AUTO = 0,
	NTFSMAC_POLICY_MODE_RO
} ntfsmac_policy_mode;

/* Decision (section 3.2). `ro` means O_RDONLY descriptor and worker `-o ro`. */
typedef enum ntfsmac_policy_decision {
	NTFSMAC_POLICY_RW = 0,
	NTFSMAC_POLICY_RO,
	NTFSMAC_POLICY_REFUSE
} ntfsmac_policy_decision;

/* Journal action of the decision (section 3.2: `keep` or `remove`). */
typedef enum ntfsmac_policy_journal_action {
	NTFSMAC_POLICY_JOURNAL_KEEP = 0,
	NTFSMAC_POLICY_JOURNAL_REMOVE /* removal path 2, section 3.5 */
} ntfsmac_policy_journal_action;

/*
 * Which row of the table fired, so that the daemon and the CLI choose their
 * own text (probe-supplied text is never shown, ADR-016). This is the
 * "Notes" column of section 3.2 as codes.
 */
typedef enum ntfsmac_policy_note {
	NTFSMAC_POLICY_NOTE_NONE = 0,             /* clean, no entry, auto: rw */
	NTFSMAC_POLICY_NOTE_MODE_RO_REQUESTED,    /* clean, no entry, ro request */
	NTFSMAC_POLICY_NOTE_EXTERNAL_CHECK,       /* clean, entry, evidence holds */
	NTFSMAC_POLICY_NOTE_UNCLEAN_SESSION,      /* clean, entry, no evidence */
	NTFSMAC_POLICY_NOTE_DIRTY_FLAG,           /* dirty_flag, no entry */
	NTFSMAC_POLICY_NOTE_DIRTY_FLAG_JOURNAL,   /* dirty_flag, entry present */
	NTFSMAC_POLICY_NOTE_UNCLEAN_LOG,          /* unclean_log */
	NTFSMAC_POLICY_NOTE_HIBERNATED,           /* hibernated or fast_startup */
	NTFSMAC_POLICY_NOTE_UNKNOWN,              /* unknown (and any invalid input) */
	NTFSMAC_POLICY_NOTE_REFUSED               /* corrupt or unsupported */
} ntfsmac_policy_note;

/*
 * Inputs of the decision.
 *
 * `journal` is the entry for the verdict's serial, or NULL if none (the
 * "Journal entry for this serial" column). `evidence_rule_enabled` is the
 * configuration switch of section 3.5 ("compiled in but disabled by
 * configuration" until U12 and U13 are resolved); when false the evidence rule
 * never holds. The evidence itself is computed here, from the verdict and the
 * entry, by ntfsmac_policy_evidence_holds().
 */
typedef struct ntfsmac_policy_input {
	const ntfsmac_probe_verdict *verdict;
	const ntfsmac_journal_entry *journal;
	ntfsmac_policy_mode mode;
	bool evidence_rule_enabled;
} ntfsmac_policy_input;

typedef struct ntfsmac_policy_result {
	ntfsmac_policy_decision decision;
	ntfsmac_policy_journal_action journal_action;
	ntfsmac_policy_note note;
} ntfsmac_policy_result;

/*
 * The external-check evidence rule (section 3.5 removal path 2), conditions
 * only, without the configuration switch. True if all hold: entry marker is
 * `set`; verdict state is exactly `clean`; the verdict has an LSN and it
 * differs from `logfile_lsn_at_start`; verdict serial and total_sectors equal
 * the entry's. NULL arguments give false.
 */
bool ntfsmac_policy_evidence_holds(const ntfsmac_probe_verdict *v,
				   const ntfsmac_journal_entry *e);

/*
 * Section 3.2 table. Fail closed: a NULL input or verdict, or an enum value
 * out of range, gives the `unknown` row (decision ro, journal keep, note
 * UNKNOWN), never rw and never remove.
 *
 * Choice (ambiguity A6): journal action `remove` is produced only in the
 * `mode=auto` column of the evidence row, as the table is written; a
 * `mode=ro` request on the same row gives ro with `keep`.
 */
ntfsmac_policy_result ntfsmac_policy_decide(const ntfsmac_policy_input *in);

/*
 * Gate results. The first failing check in the order of the gate table in
 * section 3.2 is returned; ALLOWED is 0.
 */
typedef enum ntfsmac_policy_gate {
	NTFSMAC_GATE_ALLOWED = 0,
	NTFSMAC_GATE_INVALID_INPUT,     /* NULL or out-of-range argument */
	NTFSMAC_GATE_NO_ENTRY,          /* journal-clear: no journal entry */
	NTFSMAC_GATE_PROBE_NOT_CLEAN,   /* journal-clear: state is not exactly clean */
	NTFSMAC_GATE_MOUNTED_RW,        /* journal-clear: volume mounted read/write */
	NTFSMAC_GATE_LIVE_SESSION,      /* journal-clear: a live session owns the entry */
	NTFSMAC_GATE_PROBE_NOT_DIRTY_FLAG, /* repair: state is not exactly dirty_flag */
	NTFSMAC_GATE_LOG_NOT_CLEAN,     /* repair: logfile is not `clean` */
	NTFSMAC_GATE_HIBERFILE,         /* repair: hiberfile not absent or inactive */
	NTFSMAC_GATE_FLAGS,             /* repair: DIRTY missing or another flag set */
	NTFSMAC_GATE_MOUNTED,           /* repair: volume is mounted (any mode) */
	NTFSMAC_GATE_PEER,              /* peer checks 1 to 4 of section 3.4 failed */
	NTFSMAC_GATE_CONFIRMATION       /* typed label or serial does not match */
} ntfsmac_policy_gate;

/*
 * `journal-clear` gate (section 3.2). The daemon computes the booleans: it
 * ran the probe for this request (`probe`), knows the volume's mount mode and
 * every volume's session ids, and ran the peer checks of section 3.4 (checks
 * 1 to 4: admin, not a spawned process, signed CLI, administrator
 * authorisation) and compared the typed label and serial (check 5 second
 * half). Choice (ambiguity A7): confirmation is an input so that every
 * condition of an accepted request is in one tested place.
 */
typedef struct ntfsmac_policy_journal_clear_input {
	const ntfsmac_journal_entry *entry;  /* NULL: no entry */
	const ntfsmac_probe_verdict *probe;  /* probe run for this request */
	bool mounted_rw;                     /* mounted read/write now (ro is fine) */
	bool live_session_owns_entry;        /* any volume Mounting or Mounted with entry->session_id */
	bool peer_authorised;
	bool confirmation_ok;
} ntfsmac_policy_journal_clear_input;

ntfsmac_policy_gate ntfsmac_policy_gate_journal_clear(const ntfsmac_policy_journal_clear_input *in);

/*
 * `repair --clear-dirty` gate (section 3.2, ADR-017). Allowed only when the
 * probe state is exactly `dirty_flag`, logfile `clean`, hiberfile `absent` or
 * `inactive`, volume_flags contains DIRTY and otherwise only
 * MODIFIED_BY_CHKDSK or MOUNTED_ON_NT4, the volume is not mounted in any
 * mode, the peer checks passed and the confirmation matches. A journal entry
 * is not required.
 */
typedef struct ntfsmac_policy_repair_input {
	const ntfsmac_probe_verdict *probe;  /* probe run for this request */
	bool mounted;                        /* mounted in any mode */
	bool peer_authorised;
	bool confirmation_ok;
} ntfsmac_policy_repair_input;

ntfsmac_policy_gate ntfsmac_policy_gate_repair(const ntfsmac_policy_repair_input *in);

/*
 * The stronger warnings of section 3.5 path 3, for the CLI text. Pure.
 *   WRITE_ERROR  entry state is write_error
 *   LOST_FLUSH   marker `set`, fresh probe `clean` and its LSN equals
 *                logfile_lsn_at_start ("nothing shows this volume was checked")
 * WRITE_ERROR wins if both hold (choice A8: the section says "a stronger
 * warning" for each and does not rank them).
 */
typedef enum ntfsmac_policy_clear_warning {
	NTFSMAC_CLEAR_WARNING_NONE = 0,
	NTFSMAC_CLEAR_WARNING_WRITE_ERROR,
	NTFSMAC_CLEAR_WARNING_LOST_FLUSH
} ntfsmac_policy_clear_warning;

ntfsmac_policy_clear_warning ntfsmac_policy_journal_clear_warning(
	const ntfsmac_journal_entry *entry, const ntfsmac_probe_verdict *probe);

/* Short stable names for logs and tests ("rw", "ro", "refuse"); NULL if out of range. */
const char *ntfsmac_policy_decision_name(ntfsmac_policy_decision d);

#ifdef __cplusplus
}
#endif

#endif /* NTFSMAC_POLICY_H */
