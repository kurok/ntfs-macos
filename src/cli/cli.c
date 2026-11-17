/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Command line of `ntfsmac` (T-M4-04). */
#include <string.h>

#include "cli.h"

/* mount waits up to 30 s for the mount after a 20 s probe (section 4.1). */
#define EXCHANGE_TIMEOUT_MS 120000

static void usage(FILE *f)
{
	fputs("usage: ntfsmac [--socket PATH] <command>\n"
	      "\n"
	      "commands:\n"
	      "  list                         show NTFS volumes\n"
	      "  status <bsd>                 show one volume and its last check\n"
	      "  mount [--ro] <bsd>           mount a volume (read-only with --ro)\n"
	      "  unmount [--force] <bsd>      unmount a volume\n"
	      "  doctor                       check the installation\n"
	      "  journal-clear [--label TEXT --serial HEX8] <bsd>\n"
	      "                               clear a session journal entry (administrator)\n"
	      "  repair --clear-dirty [--label TEXT --serial HEX8] <bsd>\n"
	      "                               clear the dirty flag (administrator)\n"
	      "  repair --sync-mirror [--label TEXT --serial HEX8] <bsd>\n"
	      "                               repair a stale MFT mirror (administrator)\n"
	      "\n"
	      "journal-clear and repair ask for the label shown by `list` and the last 8 hex digits\n"
	      "of the serial, then for an administrator password in a system dialog. They use the\n"
	      "built-in service and refuse --socket.\n",
	      f);
}

int ntfsmac_cli_run_request(const ntfsmac_cli_ctx *ctx, ntfsmac_ctl_cmd cmd,
			    const ntfsmac_cli_req *req)
{
	char reply[NTFSMAC_CTL_REPLY_MAX];
	char why[128];

	if (req->overflow) {
		fputs("ntfsmac: the request is too long.\n", ctx->err);
		return NTFSMAC_CLI_EXIT_USAGE;
	}
	if (ntfsmac_cli_exchange(ctx->socket_path, req->buf, EXCHANGE_TIMEOUT_MS, reply,
				 sizeof reply, why, sizeof why) != 0) {
		fprintf(ctx->err, "ntfsmac: %s.\n", why);
		return NTFSMAC_CLI_EXIT_UNAVAILABLE;
	}
	return ntfsmac_cli_render(ctx, cmd, reply);
}

static int bad_usage(const ntfsmac_cli_ctx *ctx, const char *msg)
{
	fprintf(ctx->err, "ntfsmac: %s\n", msg);
	usage(ctx->err);
	return NTFSMAC_CLI_EXIT_USAGE;
}

/* A BSD name as the user types it; the daemon checks it again. */
static bool plausible_bsd(const char *s)
{
	size_t n = strlen(s);

	if (n > 5 && strncmp(s, "/dev/", 5) == 0)
		return false; /* name only, never a path */
	return n > 0 && n <= NTFSMAC_CTL_LINE_MAX;
}

int ntfsmac_cli_main(int argc, char **argv, FILE *out, FILE *err)
{
	return ntfsmac_cli_main_hooks(argc, argv, out, err, NULL);
}

int ntfsmac_cli_main_hooks(int argc, char **argv, FILE *out, FILE *err,
			   const ntfsmac_cli_priv_hooks *hooks)
{
	ntfsmac_cli_ctx ctx = {.socket_path = NTFSMAC_CTL_SOCKET_PATH,
			       .out = out,
			       .err = err,
			       .hooks = hooks};
	ntfsmac_cli_req req;
	const char *cmd;
	bool socket_given = false;
	int i = 1;

	if (i < argc && strcmp(argv[i], "--socket") == 0) {
		if (i + 1 >= argc)
			return bad_usage(&ctx, "--socket needs a path");
		ctx.socket_path = argv[i + 1];
		socket_given = true;
		i += 2;
	}
	if (i >= argc)
		return bad_usage(&ctx, "no command given");
	if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0 ||
	    strcmp(argv[i], "help") == 0) {
		usage(out);
		return NTFSMAC_CLI_EXIT_OK;
	}
	cmd = argv[i++];

	/*
	 * An administrator authorization must only ever go to the daemon at the
	 * built-in path. Another socket could be a fake daemon that keeps the
	 * authorization form and uses it. So the privileged commands refuse
	 * --socket, before anything is asked or sent.
	 */
	if (socket_given && (strcmp(cmd, "journal-clear") == 0 || strcmp(cmd, "repair") == 0))
		return bad_usage(&ctx, "--socket cannot be used with this command; "
				       "it always uses the built-in socket");

	if (strcmp(cmd, "list") == 0 || strcmp(cmd, "doctor") == 0) {
		ntfsmac_ctl_cmd c = cmd[0] == 'l' ? NTFSMAC_CTL_CMD_LIST : NTFSMAC_CTL_CMD_DOCTOR;

		if (i != argc)
			return bad_usage(&ctx, "this command takes no arguments");
		ntfsmac_cli_req_begin(&req, c);
		ntfsmac_cli_req_end(&req);
		return ntfsmac_cli_run_request(&ctx, c, &req);
	}
	if (strcmp(cmd, "status") == 0) {
		if (i + 1 != argc || !plausible_bsd(argv[i]))
			return bad_usage(&ctx, "status needs one BSD name, for example disk4s1");
		ntfsmac_cli_req_begin(&req, NTFSMAC_CTL_CMD_STATUS);
		ntfsmac_cli_req_str(&req, "bsd", argv[i]);
		ntfsmac_cli_req_end(&req);
		return ntfsmac_cli_run_request(&ctx, NTFSMAC_CTL_CMD_STATUS, &req);
	}
	if (strcmp(cmd, "mount") == 0 || strcmp(cmd, "unmount") == 0) {
		bool is_mount = cmd[0] == 'm';
		bool flag = false;
		const char *bsd = NULL;

		for (; i < argc; i++) {
			if (strcmp(argv[i], is_mount ? "--ro" : "--force") == 0 && !flag)
				flag = true;
			else if (bsd == NULL && argv[i][0] != '-')
				bsd = argv[i];
			else
				return bad_usage(&ctx, "unexpected argument");
		}
		if (bsd == NULL || !plausible_bsd(bsd))
			return bad_usage(&ctx, "a BSD name is needed, for example disk4s1");
		if (is_mount) {
			ntfsmac_cli_req_begin(&req, NTFSMAC_CTL_CMD_MOUNT);
			ntfsmac_cli_req_str(&req, "bsd", bsd);
			ntfsmac_cli_req_str(&req, "mode", flag ? "ro" : "auto");
		} else {
			ntfsmac_cli_req_begin(&req, NTFSMAC_CTL_CMD_UNMOUNT);
			ntfsmac_cli_req_str(&req, "bsd", bsd);
			ntfsmac_cli_req_bool(&req, "force", flag);
		}
		ntfsmac_cli_req_end(&req);
		return ntfsmac_cli_run_request(&ctx, is_mount ? NTFSMAC_CTL_CMD_MOUNT
							       : NTFSMAC_CTL_CMD_UNMOUNT,
					       &req);
	}
	if (strcmp(cmd, "journal-clear") == 0)
		return ntfsmac_cli_cmd_journal_clear(&ctx, argc - i, argv + i);
	if (strcmp(cmd, "repair") == 0)
		return ntfsmac_cli_cmd_repair(&ctx, argc - i, argv + i);
	return bad_usage(&ctx, "unknown command");
}
