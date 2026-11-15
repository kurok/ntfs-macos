/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_PROBE_CORE_H
#define NTFSMAC_PROBE_CORE_H

/*
 * ntfsmac-probe engine (docs/ARCHITECTURE.md section 3.1). Reads one device
 * descriptor, never writes to it, and fills a verdict. The command line is in
 * cli.c, the JSON form in json.c.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ntfsmac/probe.h"

/* Exit statuses of the command (section 3.1). */
#define NTFSMAC_PROBE_EXIT_OK 0
#define NTFSMAC_PROBE_EXIT_USAGE 2
#define NTFSMAC_PROBE_EXIT_IO 3
#define NTFSMAC_PROBE_EXIT_INTERNAL 4

/*
 * The daemon kills a probe after 20 s (section 3.1 exit 4). The name scan of
 * check 5a stops at this budget and reports `unknown`, so a slow volume costs
 * the verdict, not the process.
 */
#define NTFSMAC_PROBE_DEFAULT_SCAN_BUDGET_MS 15000

typedef struct ntfsmac_probe_opts {
	/* Time budget of the name scan in milliseconds; negative: the default.
	 * 0 means "already spent" (test-only: the scan cannot finish). */
	int64_t scan_budget_ms;
	/* Send libntfs-3g's log to stderr (the command's --debug). */
	bool debug;
	/* CLOCK_MONOTONIC milliseconds at the start of main(), or negative if unknown. The default
	 * scan budget is what remains of NTFSMAC_PROBE_DEFAULT_SCAN_BUDGET_MS since then, so the
	 * boot sector, the mount, the mirror rule, the log and the walk share one deadline. */
	int64_t start_ms;
} ntfsmac_probe_opts;

/* CLOCK_MONOTONIC in milliseconds (0 if the clock fails). */
int64_t ntfsmac_probe_now_ms(void);

/*
 * Run checks 1 to 6 on descriptor `fd`. Returns NTFSMAC_PROBE_EXIT_OK with *v
 * filled (a valid schema-1 verdict that passes the cross-field rules),
 * NTFSMAC_PROBE_EXIT_IO if the descriptor cannot be used or the boot sector
 * cannot be read, or NTFSMAC_PROBE_EXIT_INTERNAL.
 */
int ntfsmac_probe_run(int fd, const ntfsmac_probe_opts *opts, ntfsmac_probe_verdict *v);

/*
 * One JSON object plus a newline, keys in the order of the example in section
 * 3.1. Returns the length written (without NUL), or 0 if cap is too small.
 */
size_t ntfsmac_probe_json(const ntfsmac_probe_verdict *v, char *buf, size_t cap);

/* Command line (cli.c). `test_opts` enables --scan-budget-ms and --no-alarm. */
int ntfsmac_probe_cli(int argc, char **argv, bool test_opts);

#endif /* NTFSMAC_PROBE_CORE_H */
