/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Command line of ntfsmac-volmark (docs/ARCHITECTURE.md section 3.6):
 *
 *   ntfsmac-volmark set|clear|clear-repair|sync-mirror --fd N --expect-serial <16 hex digits>
 *                   --expect-sectors <decimal> [--debug]
 *
 * stdout carries only the progress marks `w` (a write is about to start) and
 * `d` (it returned success); diagnostics go to stderr. Exit statuses are in
 * volmark.h. The process opens no path: the device is descriptor N.
 *
 * The test build (volmark_test) also takes --stop-before-write and --no-alarm.
 */
#include "posix.h"
#include "volmark.h"

#include <signal.h>
#include <stdio.h>
#include <unistd.h>

/*
 * The daemon allows 20 s until `w` (section 3.6); stop a little earlier on our
 * own. The alarm is cancelled right before `w`: it can never end a write.
 */
#define PRE_WRITE_LIMIT_SECONDS 19u

static void on_alarm(int sig)
{
	(void)sig;
	_exit(VOLMARK_EXIT_TOOL);
}

static int usage(void)
{
	fputs("usage: ntfsmac-volmark set|clear|clear-repair|sync-mirror --fd N --expect-serial <16 hex digits> "
	      "--expect-sectors <decimal> [--debug]\n",
	      stderr);
	return VOLMARK_EXIT_USAGE;
}

int volmark_main(int argc, char **argv, bool test_opts)
{
	volmark_args a;

	if (volmark_parse_args(argc, argv, test_opts, &a) != VOLMARK_EXIT_OK)
		return usage();
	/* A closed reader of `w` must not kill the process; progress() sees the error instead. */
	signal(SIGPIPE, SIG_IGN);
	if (!a.no_alarm) {
		signal(SIGALRM, on_alarm);
		alarm(PRE_WRITE_LIMIT_SECONDS);
	}
	return volmark_run(&a);
}
