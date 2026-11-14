/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * T-M3-00: the four contract headers compile together under the project flags
 * (-Wall -Wextra -Werror, C17) and their constants and tables are consistent.
 * No function is called: the implementations belong to T-M3-02, -03, -13, -19.
 */
#include "ntfsmac/journal.h"
#include "ntfsmac/options.h"
#include "ntfsmac/policy.h"
#include "ntfsmac/probe.h"

#include <stdio.h>
#include <string.h>

#define CHECK(cond)                                                      \
	do {                                                             \
		if (!(cond)) {                                           \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,    \
				__LINE__, #cond);                        \
			return 1;                                        \
		}                                                        \
	} while (0)

/* Section 3.1 counts. */
_Static_assert(NTFSMAC_PROBE_STATE_COUNT_ == 8, "8 states");
_Static_assert(NTFSMAC_PROBE_LOGFILE_COUNT_ == 4, "4 logfile values");
_Static_assert(NTFSMAC_PROBE_HIBERFILE_COUNT_ == 4, "4 hiberfile values");
_Static_assert(NTFSMAC_PROBE_VOLFLAG_COUNT_ == NTFSMAC_PROBE_MAX_VOLUME_FLAGS, "9 volume flags");
_Static_assert(NTFSMAC_PROBE_REASON_COUNT_ == NTFSMAC_PROBE_MAX_REASON_CODES, "16 reason codes");
_Static_assert(NTFSMAC_PROBE_REASON_COUNT_ <= 32, "reason set fits uint32_t");

/* Section 3.5 names: 16 hex; ".<16>.tmp"; "<16>-<YYYYMMDDTHHMMSSZ>". */
_Static_assert(NTFSMAC_JOURNAL_NAME_SIZE == 16 + 1, "entry name");
_Static_assert(NTFSMAC_JOURNAL_TMPNAME_SIZE == 1 + 16 + 4 + 1, "tmp name");
_Static_assert(NTFSMAC_JOURNAL_TOMBNAME_SIZE == 16 + 1 + 16 + 1, "tombstone name");

/* Section 3.3 rule 1: five elements. */
_Static_assert(NTFSMAC_WORKER_ARGC == 5, "five argv elements");

/* Every prototype parses with the documented shape. */
typedef __typeof__(ntfsmac_probe_parse) probe_parse_fn;
typedef __typeof__(ntfsmac_policy_decide) policy_decide_fn;
typedef __typeof__(ntfsmac_policy_gate_journal_clear) gate_clear_fn;
typedef __typeof__(ntfsmac_policy_gate_repair) gate_repair_fn;
typedef __typeof__(ntfsmac_journal_open) journal_open_fn;
typedef __typeof__(ntfsmac_journal_create) journal_create_fn;
typedef __typeof__(ntfsmac_journal_update) journal_update_fn;
typedef __typeof__(ntfsmac_journal_remove) journal_remove_fn;
typedef __typeof__(ntfsmac_options_build) options_build_fn;
typedef __typeof__(ntfsmac_argv_check) argv_check_fn;
typedef __typeof__(ntfsmac_argv_check_text) argv_check_text_fn;

static const char *const k_state_names[] = {
#define X(id, str) str,
	NTFSMAC_PROBE_STATE_LIST(X)
#undef X
};
static const char *const k_reason_names[] = {
#define X(id, str) str,
	NTFSMAC_PROBE_REASON_LIST(X)
#undef X
};

int main(void)
{
	ntfsmac_probe_verdict v;
	ntfsmac_policy_input in;
	ntfsmac_policy_result r;
	ntfsmac_journal_entry e;
	ntfsmac_options_input oi;
	ntfsmac_worker_argv wa;
	ntfsmac_argv_expect ex;
	size_t i;

	CHECK(sizeof(k_state_names) / sizeof(k_state_names[0]) == NTFSMAC_PROBE_STATE_COUNT_);
	CHECK(sizeof(k_reason_names) / sizeof(k_reason_names[0]) == NTFSMAC_PROBE_REASON_COUNT_);
	CHECK(strcmp(k_state_names[NTFSMAC_PROBE_STATE_CLEAN], "clean") == 0);
	CHECK(strcmp(k_state_names[NTFSMAC_PROBE_STATE_UNKNOWN], "unknown") == 0);
	CHECK(strcmp(k_reason_names[NTFSMAC_PROBE_REASON_CHECK_FAILED], "CHECK_FAILED") == 0);

	/* Reason names are distinct and the bit set covers exactly the list. */
	for (i = 0; i + 1 < NTFSMAC_PROBE_REASON_COUNT_; i++)
		CHECK(strcmp(k_reason_names[i], k_reason_names[i + 1]) != 0);
	CHECK(NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_CHECK_FAILED) == UINT32_C(1) << 15);
	CHECK(NTFSMAC_PROBE_VOLFLAG_BIT(NTFSMAC_PROBE_VOLFLAG_UNKNOWN_BITS) == UINT32_C(1) << 8);

	memset(&v, 0, sizeof(v));
	v.state = NTFSMAC_PROBE_STATE_CLEAN;
	v.logfile = NTFSMAC_PROBE_LOGFILE_CLEAN;
	CHECK(sizeof(v.label) == NTFSMAC_PROBE_LABEL_MAX_BYTES + 1);

	memset(&e, 0, sizeof(e));
	e.marker = NTFSMAC_JOURNAL_MARKER_PENDING;
	e.state = NTFSMAC_JOURNAL_STATE_OPEN;
	CHECK(sizeof(e.label) == NTFSMAC_JOURNAL_LABEL_MAX_BYTES + 1);

	in.verdict = &v;
	in.journal = &e;
	in.mode = NTFSMAC_POLICY_MODE_AUTO;
	in.evidence_rule_enabled = false;
	r.decision = NTFSMAC_POLICY_REFUSE;
	r.journal_action = NTFSMAC_POLICY_JOURNAL_KEEP;
	r.note = NTFSMAC_POLICY_NOTE_NONE;
	CHECK(in.verdict == &v && r.decision == NTFSMAC_POLICY_REFUSE);
	CHECK(NTFSMAC_GATE_ALLOWED == 0 && NTFSMAC_ARGV_OK == 0 && NTFSMAC_OPTIONS_OK == 0);
	CHECK(NTFSMAC_PROBE_OK == 0 && NTFSMAC_JOURNAL_OK == 0);

	memset(&oi, 0, sizeof(oi));
	memset(&wa, 0, sizeof(wa));
	memset(&ex, 0, sizeof(ex));
	CHECK(sizeof(wa.devpath) >= sizeof("/dev/fd/255"));
	CHECK(sizeof(wa.mountpoint) >= sizeof(NTFSMAC_VOLUMES_PREFIX) + NTFSMAC_LEAF_MAX_BYTES);
	CHECK(sizeof(wa.optstr) == NTFSMAC_OPTSTR_MAX_BYTES + 1);
	CHECK(oi.decision == NTFSMAC_POLICY_RW && ex.decision == NTFSMAC_POLICY_RW);

	puts("contracts headers: ok");
	return 0;
}
