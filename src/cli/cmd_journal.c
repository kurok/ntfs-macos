/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Stub of `ntfsmac journal-clear`. T-M4-14 replaces this file: it prints the
 * warning of ADR-017, collects the typed label and serial, calls
 * ntfsmac_cli_authz_acquire(), builds the `journal_clear` request with the
 * `auth` field and releases the authorization after the reply.
 */
#include "cli.h"

int ntfsmac_cli_cmd_journal_clear(const ntfsmac_cli_ctx *ctx, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	fputs("ntfsmac: journal-clear is not implemented yet.\n", ctx->err);
	return NTFSMAC_CLI_EXIT_UNAVAILABLE;
}
