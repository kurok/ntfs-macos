/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_VOLMARK_H
#define NTFSMAC_VOLMARK_H

/*
 * ntfsmac-volmark: the on-disk dirty marker (docs/ARCHITECTURE.md section 3.6,
 * ADR-006, ADR-017, ADR-020). Modes `set`, `clear` and `clear-repair`; the
 * stale-mirror repair `sync-mirror` (section 3.7, ADR-021) is T-M4-16 and gets
 * its own file (syncmirror.c) and mode value here.
 */

#include <stdbool.h>
#include <stdint.h>

/* Exit statuses (section 3.6). 0 means every step succeeded. */
#define VOLMARK_EXIT_OK 0
#define VOLMARK_EXIT_USAGE 2    /* bad command line; nothing opened */
#define VOLMARK_EXIT_TOOL 3     /* descriptor not usable, out of memory, `w` not delivered; nothing written */
#define VOLMARK_EXIT_REFUSED 10 /* step 1: mount, hibernation or $LogFile refusal; nothing written */
#define VOLMARK_EXIT_FLAGS 11   /* step 3 of `set`: dirty flag set, or any flag but MODIFIED_BY_CHKDSK, MOUNTED_ON_NT4 */
#define VOLMARK_EXIT_IO 12      /* step 4 or 5: the flag write, the unmount sync or fsync failed */
#define VOLMARK_EXIT_ANOMALY 13 /* step 3 of `clear` / `clear-repair`: flags not as expected */
#define VOLMARK_EXIT_READBACK 14 /* step 6: read-back mount failed or the flag differs */
#define VOLMARK_EXIT_BINDING 15 /* step 2: serial or sector count differ from the expected values */

/* $Volume flags (layout.h VOLUME_FLAGS), numeric as in section 3.1. */
#define VOLMARK_VOL_DIRTY 0x0001u
#define VOLMARK_VOL_MOUNTED_ON_NT4 0x0008u
#define VOLMARK_VOL_MODIFIED_BY_CHKDSK 0x8000u
#define VOLMARK_VOL_FLAGS_MASK 0xc03fu /* what ntfs_volume_write_flags() keeps */

typedef enum volmark_mode {
	VOLMARK_SET = 0,
	VOLMARK_CLEAR,
	VOLMARK_CLEAR_REPAIR
	/* VOLMARK_SYNC_MIRROR: T-M4-16 (syncmirror.c) */
} volmark_mode;

typedef struct volmark_args {
	volmark_mode mode;
	int fd;
	uint64_t expect_serial;
	uint64_t expect_sectors;
	bool debug;
	/* test build only (volmark_test): stop with SIGSTOP right before `w` is printed */
	bool stop_before_write;
	/* test build only: no 20 s alarm of our own */
	bool no_alarm;
} volmark_args;

/*
 * Parse the command line (args.c, pure):
 *   ntfsmac-volmark set|clear|clear-repair --fd N --expect-serial <16 hex digits>
 *                   --expect-sectors <decimal> [--debug]
 * `test_opts` also accepts --stop-before-write and --no-alarm. Returns
 * VOLMARK_EXIT_OK and fills *a, or VOLMARK_EXIT_USAGE.
 */
int volmark_parse_args(int argc, char **argv, bool test_opts, volmark_args *a);

/* Exactly 16 hex digits (either case) -> value. 0 on success, -1 if not. */
int volmark_parse_serial(const char *s, uint64_t *v);

/* Decimal, no sign, no leading zero, 1 .. INT64_MAX. 0 on success, -1 if not. */
int volmark_parse_sectors(const char *s, uint64_t *v);

/*
 * Step 3: may the flags read from the volume be changed by this mode?
 * Returns VOLMARK_EXIT_OK and the flags to write in *out, else
 * VOLMARK_EXIT_FLAGS (set) or VOLMARK_EXIT_ANOMALY (clear, clear-repair).
 * Pure; the only place that decides which bit changes.
 */
int volmark_next_flags(volmark_mode mode, uint16_t flags, uint16_t *out);

/* Steps 1 to 6 on descriptor a->fd (volmark.c). Returns an exit status. */
int volmark_run(const volmark_args *a);

/* main() of both binaries (cli.c). */
int volmark_main(int argc, char **argv, bool test_opts);

#endif /* NTFSMAC_VOLMARK_H */
