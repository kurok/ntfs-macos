/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_DAEMON_REPORT_H
#define NTFSMAC_DAEMON_REPORT_H

/*
 * Replies of `list`, `status` and `doctor` and the fixed tokens behind them (T-M4-08;
 * docs/ARCHITECTURE.md section 3.4). Pure C: the daemon (main.c) copies what it knows about a
 * volume into plain structs and these functions write one JSON line. Every string is escaped,
 * and the only strings that go in are fixed tokens, the sanitised label (ADR-016) and the path
 * display form of ADR-023, so a reply never carries a raw byte of a disk.
 */

#include <stdbool.h>
#include <stddef.h>

#include "internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One volume as `list` and `status` show it. Every pointer may be NULL (shown as ""). */
typedef struct ntfsmac_report_vol {
	const char *bsd;
	const char *label;      /* sanitised */
	const char *state;      /* ntfsmac_vol_state_name() */
	const char *mount;      /* "rw", "ro" or "none" */
	const char *mountpoint; /* /Volumes/<leaf> or "" */
	const char *reason;     /* a fixed token or "" */
} ntfsmac_report_vol;

/*
 * {"ok":true,"volumes":[{"bsd":..,"label":..,"state":..,"mount":..,"mountpoint":..,"reason":..},..]}
 * Returns the length written, or 0 if `cap` is too small (the buffer then holds an empty string).
 */
size_t ntfsmac_report_list(const ntfsmac_report_vol *vols, size_t n, char *buf, size_t cap);

/*
 * {"ok":true,"volume":{...as above plus "serial"...},"probe":{...},"journal":{...}}
 * `last` (the last probe verdict of the volume) and `je` (its journal entry) may be NULL: the
 * member is then left out. The `journal` member is the text of ntfsmac_priv_journal_member().
 * The path list is cut, whole paths only, so that the reply fits in `cap`.
 */
size_t ntfsmac_report_status(const ntfsmac_report_vol *vol, const ntfsmac_probe_verdict *last,
			     const ntfsmac_journal_entry *je, char *buf, size_t cap);

/* {"ok":true,"findings":[{"code":N,"ok":bool,"value":N},..]} from the doctor findings. */
size_t ntfsmac_report_doctor(const ntfsmac_doctor_finding *f, size_t n, char *buf, size_t cap);

/* Fixed wire tokens. Never NULL; "" for NONE. */
const char *ntfsmac_report_vreason_token(ntfsmac_vol_reason r);
const char *ntfsmac_report_policy_note_token(ntfsmac_policy_note n);
const char *ntfsmac_report_session_reason_token(ntfsmac_session_reason r);
/* The fixed text of a token of the three functions above, for a notification; "" if none. */
const char *ntfsmac_report_reason_text(const char *token);

#ifdef __cplusplus
}
#endif

#endif /* NTFSMAC_DAEMON_REPORT_H */
