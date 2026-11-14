/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_JOURNAL_INTERNAL_H
#define NTFSMAC_JOURNAL_INTERNAL_H

/*
 * Private to src/journal/. Kill points for the crash tests (U-JRN-*, criterion
 * 3.12): they exist only when the file is compiled with
 * NTFSMAC_JOURNAL_TESTHOOKS, which only the ntfsmac_journal_testhooks library
 * target (linked by the crash test alone) defines. The production library has
 * no hook, no global and no branch.
 */

#if defined(NTFSMAC_JOURNAL_TESTHOOKS)
extern void (*ntfsmac_journal_testhook)(const char *point);
#define JRN_POINT(name)                                                   \
	do {                                                              \
		if (ntfsmac_journal_testhook)                             \
			ntfsmac_journal_testhook(name);                   \
	} while (0)
#else
#define JRN_POINT(name) ((void)0)
#endif

#endif /* NTFSMAC_JOURNAL_INTERNAL_H */
