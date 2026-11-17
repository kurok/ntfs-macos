/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * `ntfsmac repair --clear-dirty [--label TEXT --serial HEX8] <bsd>` (T-M4-14, ADR-017).
 * `--sync-mirror` is T-M4-16's: it is recognised here and answers "not available yet" until that
 * task adds its request.
 */
#include <string.h>

#include "cli.h"

int ntfsmac_cli_cmd_repair(const ntfsmac_cli_ctx *ctx, int argc, char **argv)
{
	if (argc >= 1 && strcmp(argv[0], "--clear-dirty") == 0)
		return ntfsmac_cli_priv_run(ctx, NTFSMAC_CTL_CMD_REPAIR, "clear_dirty", argc - 1,
					    argv + 1);
	if (argc >= 1 && strcmp(argv[0], "--sync-mirror") == 0) {
		fputs("ntfsmac: repair --sync-mirror is not available yet.\n", ctx->err);
		return NTFSMAC_CLI_EXIT_UNAVAILABLE;
	}
	fputs("usage: ntfsmac repair --clear-dirty [--label TEXT --serial HEX8] <bsd>\n", ctx->err);
	return NTFSMAC_CLI_EXIT_USAGE;
}
