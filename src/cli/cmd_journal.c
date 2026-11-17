/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * `ntfsmac journal-clear [--label TEXT --serial HEX8] <bsd>` (T-M4-14, ADR-017, ADR-019). The
 * flow, the warning text and the authorization are in priv.c.
 */
#include "cli.h"

int ntfsmac_cli_cmd_journal_clear(const ntfsmac_cli_ctx *ctx, int argc, char **argv)
{
	return ntfsmac_cli_priv_run(ctx, NTFSMAC_CTL_CMD_JOURNAL_CLEAR, NULL, argc, argv);
}
