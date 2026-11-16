/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_DAEMON_DA_FILTER_H
#define NTFSMAC_DAEMON_DA_FILTER_H

/*
 * Pure helpers of the Disk Arbitration module (da.c, T-M4-02): the NTFS
 * filter of docs/ARCHITECTURE.md section 4.1 step 1, the BSD name check and
 * a small fixed-size table of BSD names. No Disk Arbitration, no
 * CoreFoundation, no allocation, no locking. They build and are tested on
 * every host.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "diskN" or "diskNsM", digits only, N and M at most 6 digits. */
bool ntfsmac_da_bsd_name_valid(const char *name);

/*
 * The filter of section 4.1 step 1. `content` is DAMediaContent (may be
 * NULL); `whole` is DAMediaWhole. A disk is a candidate if its content is
 * "Windows_NTFS" (MBR type 07) or the GPT basic-data GUID, compared without
 * regard to case, or if it is a whole disk whose content is empty or absent
 * (a raw file system without a partition map). A whole disk that carries a
 * partition map is not a candidate: its partitions are.
 */
bool ntfsmac_da_content_is_candidate(const char *content, bool whole);

/* Table of BSD names, each with one flag. Fixed size; the caller locks. */
#define NTFSMAC_DA_TABLE_MAX 128
#define NTFSMAC_DA_NAME_MAX 32

typedef struct ntfsmac_da_table {
	unsigned count;
	char names[NTFSMAC_DA_TABLE_MAX][NTFSMAC_DA_NAME_MAX];
	bool flags[NTFSMAC_DA_TABLE_MAX];
} ntfsmac_da_table;

void ntfsmac_da_table_init(ntfsmac_da_table *t);
/*
 * Adds `name` with flag false. True if it is in the table afterwards, false
 * if the name is invalid or the table is full. Adding a name twice keeps one
 * entry and its flag.
 */
bool ntfsmac_da_table_add(ntfsmac_da_table *t, const char *name);
/* True if the name was present and is now removed; *flag gets its flag. */
bool ntfsmac_da_table_remove(ntfsmac_da_table *t, const char *name, bool *flag);
bool ntfsmac_da_table_contains(const ntfsmac_da_table *t, const char *name);
/* Sets the flag of a present name; false if the name is absent. */
bool ntfsmac_da_table_set_flag(ntfsmac_da_table *t, const char *name, bool flag);
/* Reads the flag of a present name; false if the name is absent. */
bool ntfsmac_da_table_get_flag(const ntfsmac_da_table *t, const char *name, bool *flag);

#ifdef __cplusplus
}
#endif

#endif /* NTFSMAC_DAEMON_DA_FILTER_H */
