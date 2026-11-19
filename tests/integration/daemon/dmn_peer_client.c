/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * T-M4-11, I-DMN-19 "a request sent from inside the worker's process group": a client of the
 * privileged XPC service that does not refuse a caller that is not root. It is the CLI's
 * `ntfsmac_cli_priv_run` with the test hooks of src/cli/cli.h set so that the CLI believes it is
 * root and that the daemon wants no authorization form, so the request goes out as the user that
 * runs the client (the console user, an administrator), without sudo and without a dialog. The
 * daemon then decides on the peer checks alone: the gate and check 1 pass, check 2 (a spawned
 * process or its descendant) must refuse. dmn_priv_guest.sh runs it from a stand-in worker.
 * Disposable guest only; never part of the product.
 *
 * Usage: dmn-peer-client journal-clear|repair-clear-dirty|repair-sync-mirror [--label L --serial S] BSD
 */
#include <stdio.h>
#include <string.h>

#include "cli.h"

static int yes(void)
{
	return 1;
}

int main(int argc, char **argv)
{
	ntfsmac_cli_priv_hooks h;
	ntfsmac_cli_ctx ctx;
	ntfsmac_ctl_cmd cmd = NTFSMAC_CTL_CMD_JOURNAL_CLEAR;
	const char *action = NULL;

	if (argc < 3)
		return 2;
	memset(&h, 0, sizeof h);
	h.is_root = yes;
	h.root_only = yes;
	memset(&ctx, 0, sizeof ctx);
	ctx.socket_path = NTFSMAC_CTL_SOCKET_PATH;
	ctx.out = stdout;
	ctx.err = stderr;
	ctx.hooks = &h;
	if (strcmp(argv[1], "repair-clear-dirty") == 0) {
		cmd = NTFSMAC_CTL_CMD_REPAIR;
		action = "clear_dirty";
	} else if (strcmp(argv[1], "repair-sync-mirror") == 0) {
		cmd = NTFSMAC_CTL_CMD_REPAIR;
		action = "sync_mirror";
	} else if (strcmp(argv[1], "journal-clear") != 0) {
		return 2;
	}
	return ntfsmac_cli_priv_run(&ctx, cmd, action, argc - 2, argv + 2);
}
