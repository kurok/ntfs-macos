/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * T-M4-14: a stand-in daemon for the XPC door, started by launchd under the Mach service name of
 * the product in a disposable guest (xpc_vm.sh). It uses the real listener (ctl_xpc.c), the real
 * system facts (ctl_sys.c: code requirement from the installer's file, audit token to peer) and
 * the real dispatcher, with fake operations that only write down what they were called with:
 *
 *   xpc_vm LOGFILE (external|root)
 *
 * `external` is the default of the product (check 4 is the right and the external form), `root`
 * is the ADR-019 fallback (a root peer). The operations answer `cleared` or `repaired`; the
 * log line holds the uid and pid the daemon took from the audit token, the BSD name, the typed
 * label and serial and whether a form came with the request.
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ctl_priv.h"

static const char *g_log;

static void note(const char *what, const ntfsmac_ctl_peer *peer, const char *bsd, const char *action,
		 const char *label, const char *serial)
{
	FILE *f = fopen(g_log, "a");

	if (f == NULL)
		return;
	fprintf(f, "%s uid=%u pid=%d admin=%d bsd=%s action=%s label=%s serial=%s\n", what,
		(unsigned)peer->uid, (int)peer->pid, peer->admin ? 1 : 0, bsd, action, label, serial);
	fclose(f);
}

static ntfsmac_dmn_status op_jc(void *ctx, const ntfsmac_ctl_peer *peer, const char *bsd,
				const char *label, const char *serial, char *reply, size_t cap)
{
	(void)ctx;
	note("journal_clear", peer, bsd, "-", label, serial);
	snprintf(reply, cap, "{\"ok\":true,\"result\":\"cleared\",\"warning\":\"none\"}");
	return NTFSMAC_DMN_OK;
}

static ntfsmac_dmn_status op_rp(void *ctx, const ntfsmac_ctl_peer *peer, const char *bsd,
				const char *action, const char *label, const char *serial, char *reply,
				size_t cap)
{
	(void)ctx;
	note("repair", peer, bsd, action, label, serial);
	snprintf(reply, cap, "{\"ok\":true,\"result\":\"repaired\",\"journal_entry\":\"none\",\"warning\":\"none\"}");
	return NTFSMAC_DMN_OK;
}

static bool op_is_spawned(void *ctx, pid_t pid)
{
	(void)ctx;
	(void)pid;
	return false;
}

int main(int argc, char **argv)
{
	ntfsmac_ctl_ops ops;
	ntfsmac_ctl_env env;
	ntfsmac_xpc *x = NULL;
	ntfsmac_dmn_status st;

	if (argc != 3) {
		fprintf(stderr, "usage: xpc_vm LOGFILE (external|root)\n");
		return 2;
	}
	g_log = argv[1];
	memset(&ops, 0, sizeof(ops));
	ops.journal_clear = op_jc;
	ops.repair = op_rp;
	ops.is_spawned = op_is_spawned;
	env = ntfsmac_ctl_default_env_xpc();
	env.authz = strcmp(argv[2], "root") == 0 ? NTFSMAC_CTL_AUTHZ_ROOT_PEER : NTFSMAC_CTL_AUTHZ_EXTERNAL_FORM;
	st = ntfsmac_xpc_start(NTFSMAC_CTL_XPC_SERVICE, &ops, NULL, &env, &x);
	if (st != NTFSMAC_DMN_OK) {
		fprintf(stderr, "xpc_vm: ntfsmac_xpc_start failed: %s\n", ntfsmac_dmn_status_name(st));
		return 1;
	}
	for (;;)
		pause();
}
