/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Stub of `ntfsmac repair`. T-M4-14 replaces this file (and T-M4-16 adds
 * --sync-mirror): same flow as journal-clear, with the `repair` request.
 */
#include "cli.h"

int ntfsmac_cli_cmd_repair(const ntfsmac_cli_ctx *ctx, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	fputs("ntfsmac: repair is not implemented yet.\n", ctx->err);
	return NTFSMAC_CLI_EXIT_UNAVAILABLE;
}
