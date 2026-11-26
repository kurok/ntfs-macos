/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_PROBE_NAMES_H
#define NTFSMAC_PROBE_NAMES_H

/*
 * One walk of every directory index from the root (docs/ARCHITECTURE.md
 * section 3.1). It serves two checks:
 *   check 5   /hiberfil.sys in the root, matched ignoring case (U19);
 *   check 5a  names that are not NFC (ADR-023).
 */

#include <stdbool.h>
#include <stdint.h>

#include <ntfs-3g/volume.h>

#include "ntfsmac/probe.h"

typedef struct names_result {
	/* check 5 */
	ntfsmac_probe_hiberfile hiberfile;
	uint32_t hiber_reasons; /* NTFSMAC_PROBE_REASON_BIT() of HIBER_* */
	/* check 5a */
	uint32_t nonnfc_count;
	uint32_t path_count;
	char paths[NTFSMAC_PROBE_MAX_NONNFC_PATHS][NTFSMAC_PROBE_NONNFC_PATH_MAX_BYTES + 1];
	/* false: the walk did not finish (I/O error, damaged index, limit, time budget) */
	bool complete;
	/* what the walk saw: directories listed and names compared (for the debug log) */
	uint64_t dirs_listed, names_seen;
	/* records in the root with a name that is hiberfil.sys, any case and namespace (debug log) */
	unsigned hiber_records;
} names_result;

/*
 * Walk the volume. `budget_ms` is the time allowed for the walk (negative: the
 * default of probe_core.h; 0: already spent). The volume is only read.
 */
void names_scan(ntfs_volume *vol, int64_t budget_ms, names_result *r);

#endif /* NTFSMAC_PROBE_NAMES_H */
