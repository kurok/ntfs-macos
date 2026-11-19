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

/*
 * What the privileged commands (journal-clear, repair) need from the outside, so that tests can
 * stand in for the system dialog, the XPC service and the terminal. NULL members mean the real
 * thing. The real `ntfsmac` never sets them.
 */
typedef struct ntfsmac_cli_authz ntfsmac_cli_authz;
typedef struct ntfsmac_cli_priv_hooks {
	int (*authz_acquire)(const char *prompt, char hex[2 * NTFSMAC_CTL_AUTH_FORM_BYTES + 1],
			     ntfsmac_cli_authz **out, char *err_text, size_t err_cap);
	void (*authz_release)(ntfsmac_cli_authz *h);
	int (*exchange)(const char *service, const char *line, int timeout_ms, char *reply,
			size_t cap, char *err_text, size_t err_cap);
	int (*is_root)(void);
	/* the read-only `status` request on the socket that precedes the confirmation (NULL: real) */
	int (*sock_exchange)(const char *socket_path, const char *line, int timeout_ms, char *reply,
			     size_t cap, char *err_text, size_t err_cap);
	FILE *in; /* where the typed confirmation is read from; NULL: stdin */
	/* nonzero: the daemon accepts only a root caller (check 4 of ADR-019, strategy ROOT_PEER);
	 * NULL: the choice of this build, ntfsmac_cli_root_only() */
	int (*root_only)(void);
} ntfsmac_cli_priv_hooks;

typedef struct ntfsmac_cli_ctx {
	const char *socket_path;
	FILE *out;
	FILE *err;
	const ntfsmac_cli_priv_hooks *hooks; /* NULL in the real program */
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
/* Text for the reason of a refused journal_clear or repair ("probe_not_clean"), or NULL. */
const char *ntfsmac_cli_gate_text(const char *id);
/* Text for an error code such as "bad_json", or NULL. */
const char *ntfsmac_cli_error_text(const char *code);
/*
 * The codes of `doctor` findings as the daemon sends them, and the oldest macFUSE it accepts.
 * They are the values of ntfsmac_doctor_code and NTFSMAC_MACFUSE_MIN_* in src/daemon/internal.h;
 * tests/unit/ctl/ctl_cli.c asserts at compile time that the two agree.
 */
enum {
	NTFSMAC_CLI_DOCTOR_MACFUSE_INSTALLED = 1,
	NTFSMAC_CLI_DOCTOR_MACFUSE_VERSION,
	NTFSMAC_CLI_DOCTOR_MACFUSE_EXTENSION,
	NTFSMAC_CLI_DOCTOR_CONFLICT_NONE,
	NTFSMAC_CLI_DOCTOR_CONFLICT_PARAGON,
	NTFSMAC_CLI_DOCTOR_CONFLICT_TUXERA,
	NTFSMAC_CLI_DOCTOR_CONFLICT_OTHER,
	NTFSMAC_CLI_DOCTOR_CONFLICT_UNREADABLE,
	NTFSMAC_CLI_DOCTOR_JOURNAL_ENTRIES,
	NTFSMAC_CLI_DOCTOR_JOURNAL_CORRUPT,
	NTFSMAC_CLI_DOCTOR_JOURNAL_CLEARED,
	NTFSMAC_CLI_DOCTOR_JOURNAL_UNREADABLE,
	NTFSMAC_CLI_DOCTOR_EXT_NOT_APPROVED = 2 /* the `value` of EXTENSION: ntfsmac_doctor_ext_state */
};
#define NTFSMAC_CLI_MACFUSE_MIN_MAJOR 5
#define NTFSMAC_CLI_MACFUSE_MIN_MINOR 5

/*
 * Text for one `doctor` finding (the code numbers of ntfsmac_doctor_code in src/daemon/internal.h),
 * or NULL for a code this CLI does not know. `buf` takes the texts that carry a number.
 */
const char *ntfsmac_cli_doctor_text(int code, bool ok, long value, char *buf, size_t cap);

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

/*
 * Ask the system for the right org.ntfs-macos.journal-modify (shows the
 * password prompt) and return the external form as 64 hex digits plus NUL.
 * Keep the handle until the daemon has answered, then release it, which
 * destroys the right. Returns 0 on success, -1 with text in `err_text`.
 */
int ntfsmac_cli_authz_acquire(const char *prompt, char hex[2 * NTFSMAC_CTL_AUTH_FORM_BYTES + 1],
			      ntfsmac_cli_authz **out, char *err_text, size_t err_cap);
/*
 * The prompt call of Authorization Services does not fail without a GUI session: it waits (U17,
 * finding 2). ntfsmac_cli_authz_acquire() therefore gives up after NTFSMAC_CLI_PROMPT_TIMEOUT_S
 * seconds. The timeout is applied by this helper, which runs `fn(arg)` on its own thread and
 * waits at most `timeout_ms`: true if `fn` returned in time. A thread that is still blocked is
 * left behind (the program exits soon after). Portable; tested on every host.
 */
#define NTFSMAC_CLI_PROMPT_TIMEOUT_S 120
bool ntfsmac_cli_run_with_timeout(void (*fn)(void *), void *arg, unsigned timeout_ms);
void ntfsmac_cli_authz_release(ntfsmac_cli_authz *h);

/* ------------------------------------------------------------------ */
/* Commands (cli.c, cmd_journal.c, cmd_repair.c)                       */
/* ------------------------------------------------------------------ */

/* Send `req` (already ended), render the reply. */
int ntfsmac_cli_run_request(const ntfsmac_cli_ctx *ctx, ntfsmac_ctl_cmd cmd,
			    const ntfsmac_cli_req *req);

/* `ntfsmac [--socket PATH] <command> ...`; argv[0] is the program name. */
int ntfsmac_cli_main(int argc, char **argv, FILE *out, FILE *err);

/* `ntfsmac journal-clear [--label TEXT --serial HEX8] <bsd>` (cmd_journal.c). */
int ntfsmac_cli_cmd_journal_clear(const ntfsmac_cli_ctx *ctx, int argc, char **argv);
/* `ntfsmac repair --clear-dirty [--label TEXT --serial HEX8] <bsd>` (cmd_repair.c). */
int ntfsmac_cli_cmd_repair(const ntfsmac_cli_ctx *ctx, int argc, char **argv);

/*
 * Same as ntfsmac_cli_main() with the hooks of the privileged commands (tests). `hooks` may be
 * NULL.
 */
int ntfsmac_cli_main_hooks(int argc, char **argv, FILE *out, FILE *err,
			   const ntfsmac_cli_priv_hooks *hooks);

/* ------------------------------------------------------------------ */
/* Privileged requests (priv.c, xpc_client.c)                          */
/* ------------------------------------------------------------------ */

/*
 * One request to the XPC service `service` (NTFSMAC_CTL_XPC_SERVICE for the real program) and
 * its reply line, within `timeout_ms`. 0, or -1 with text in `err_text`. macOS only.
 */
int ntfsmac_cli_privileged_exchange(const char *service, const char *line, int timeout_ms,
				    char *reply, size_t cap, char *err_text, size_t err_cap);

/*
 * The flow of journal-clear and repair (ADR-017, ADR-019): print what the action does and does
 * not do, collect the typed label and serial, get the administrator authorization (a system
 * password dialog; a root caller has none and sends no form), send the request on the XPC
 * service, release the authorization, show the reply. `action` is "clear_dirty" for repair and
 * NULL for journal-clear. The authorization form only ever goes to NTFSMAC_CTL_XPC_SERVICE.
 */
/*
 * Nonzero when this build's daemon accepts journal-clear and repair only from a root caller:
 * U17 showed on macOS 27.2 that a password grant of the CLI does not satisfy the daemon's
 * check, so the product is built with NTFSMAC_AUTHZ_FALLBACK_ROOT (ARCHITECTURE section 3.4).
 */
int ntfsmac_cli_root_only(void);

int ntfsmac_cli_priv_run(const ntfsmac_cli_ctx *ctx, ntfsmac_ctl_cmd cmd, const char *action,
			 int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* NTFSMAC_CLI_H */
