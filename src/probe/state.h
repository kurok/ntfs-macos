/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_PROBE_STATE_H
#define NTFSMAC_PROBE_STATE_H

/*
 * The `state` of a verdict, derived from its reason codes (docs/ARCHITECTURE.md
 * section 3.1): the most severe finding in the order
 *   unsupported > corrupt > mirror_split > hibernated > fast_startup >
 *   unclean_log > dirty_flag > names_not_nfc > unknown > clean.
 * Deriving it, instead of tracking it beside the codes, makes the cross-field
 * rule hold by construction: `clean` if and only if no code is set.
 * Pure C. The severity table itself lives in src/common/state_rank.c
 * (ntfsmac_probe_state_severity()).
 */

#include <stdint.h>

#include "ntfsmac/probe.h"

/* Severity rank of a state, higher is more severe; -1 if out of range.
 * Same as ntfsmac_probe_state_severity(). */
int probe_state_rank(ntfsmac_probe_state s);

/* The state a single reason code stands for. */
ntfsmac_probe_state probe_state_of_reason(ntfsmac_probe_reason r);

/* The most severe state over a set of NTFSMAC_PROBE_REASON_BIT() bits; clean for none. */
ntfsmac_probe_state probe_state_of_reasons(uint32_t reasons);

#endif /* NTFSMAC_PROBE_STATE_H */
