/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_CLI_H
#define NTFSMAC_CLI_H

/*
 * The `ntfsmac` command line tool (T-M4-04, docs/ARCHITECTURE.md section 3.4).
 * It talks to the daemon over the control socket and shows the reply. It
 * never opens a device and never prints a string that came from a disk other
 * than the sanitised label the daemon sends (every string it prints passes
 * ntfsmac_cli_print_safe()).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "ntfsmac/ctl_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NTFSMAC_CLI_EXIT_OK 0
#define NTFSMAC_CLI_EXIT_FAILED 1      /* the daemon refused or failed the request */
#define NTFSMAC_CLI_EXIT_USAGE 2       /* bad command line */
#define NTFSMAC_CLI_EXIT_UNAVAILABLE 3 /* no daemon, no answer, not implemented */

typedef struct ntfsmac_cli_ctx {
	const char *socket_path;
	FILE *out;
	FILE *err;
} ntfsmac_cli_ctx;

/* ------------------------------------------------------------------ */
/* Request builder (req.c)                                             */
/* ------------------------------------------------------------------ */

typedef struct ntfsmac_cli_req {
	char buf[NTFSMAC_CTL_LINE_MAX + 1];
	size_t len;
	bool overflow;
} ntfsmac_cli_req;

/* `{"cmd":"<cmd>"`; then any number of fields; then end. */
void ntfsmac_cli_req_begin(ntfsmac_cli_req *r, ntfsmac_ctl_cmd cmd);
void ntfsmac_cli_req_str(ntfsmac_cli_req *r, const char *key, const char *value);
void ntfsmac_cli_req_bool(ntfsmac_cli_req *r, const char *key, bool value);
/* Closes the object. 0 if the whole request fit, -1 if it did not (nothing must be sent). */
int ntfsmac_cli_req_end(ntfsmac_cli_req *r);

/* ------------------------------------------------------------------ */
/* Text tables (reasons.c)                                             */
/* ------------------------------------------------------------------ */

/* Text for a probe reason code such as "FLAG_DIRTY", or NULL if the code is unknown. */
const char *ntfsmac_cli_reason_text(const char *code);
/* Text for a refusal id such as "not_admin", or NULL. */
const char *ntfsmac_cli_refusal_text(const char *id);
/* Text for an error code such as "bad_json", or NULL. */
const char *ntfsmac_cli_error_text(const char *code);

/* ------------------------------------------------------------------ */
/* Output (render.c)                                                   */
/* ------------------------------------------------------------------ */

/*
 * Print `s` with every byte that could act on a terminal written as \xNN:
 * control bytes, DEL, the C1 range, bidirectional controls and invisible
 * format characters (see risky_sequence() in render.c). Valid UTF-8
 * text passes unchanged; invalid bytes are escaped. Returns the number of
 * display columns written (code points, not bytes).
 */
size_t ntfsmac_cli_print_safe(FILE *f, const char *s);

/*
 * Show one reply line. Returns NTFSMAC_CLI_EXIT_OK for {"ok":true,...},
 * NTFSMAC_CLI_EXIT_FAILED for {"ok":false,...}, NTFSMAC_CLI_EXIT_UNAVAILABLE
 * for text that is not a reply.
 */
int ntfsmac_cli_render(const ntfsmac_cli_ctx *ctx, ntfsmac_ctl_cmd cmd, const char *reply);

/* ------------------------------------------------------------------ */
/* Socket client (client.c)                                            */
/* ------------------------------------------------------------------ */

/*
 * Send one request line and read one reply line into `reply` (NUL
 * terminated, no newline). `timeout_ms` bounds the whole exchange.
 * Returns 0, or -1 with a text for the user in `err_text`.
 */
int ntfsmac_cli_exchange(const char *socket_path, const char *line, int timeout_ms, char *reply,
			 size_t cap, char *err_text, size_t err_cap);

/* ------------------------------------------------------------------ */
/* Administrator authorization (authz.c)                               */
/* ------------------------------------------------------------------ */

typedef struct ntfsmac_cli_authz ntfsmac_cli_authz;

/*
 * Ask the system for the right org.ntfs-macos.journal-modify (shows the
 * password prompt) and return the external form as 64 hex digits plus NUL.
 * Keep the handle until the daemon has answered, then release it, which
 * destroys the right. Returns 0 on success, -1 with text in `err_text`.
 */
int ntfsmac_cli_authz_acquire(const char *prompt, char hex[2 * NTFSMAC_CTL_AUTH_FORM_BYTES + 1],
			      ntfsmac_cli_authz **out, char *err_text, size_t err_cap);
void ntfsmac_cli_authz_release(ntfsmac_cli_authz *h);

/* ------------------------------------------------------------------ */
/* Commands (cli.c, cmd_journal.c, cmd_repair.c)                       */
/* ------------------------------------------------------------------ */

/* Send `req` (already ended), render the reply. */
int ntfsmac_cli_run_request(const ntfsmac_cli_ctx *ctx, ntfsmac_ctl_cmd cmd,
			    const ntfsmac_cli_req *req);

/* `ntfsmac [--socket PATH] <command> ...`; argv[0] is the program name. */
int ntfsmac_cli_main(int argc, char **argv, FILE *out, FILE *err);

/* T-M4-14 replaces these two files. Until then they answer "not implemented". */
int ntfsmac_cli_cmd_journal_clear(const ntfsmac_cli_ctx *ctx, int argc, char **argv);
int ntfsmac_cli_cmd_repair(const ntfsmac_cli_ctx *ctx, int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* NTFSMAC_CLI_H */
