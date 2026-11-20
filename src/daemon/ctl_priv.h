/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_DAEMON_CTL_PRIV_H
#define NTFSMAC_DAEMON_CTL_PRIV_H

/*
 * Private interfaces of the control socket (T-M4-04). Three parts, in three
 * files, so that each can be tested alone:
 *
 *   ctl_parse.c  strict request parser (no system calls)
 *   ctl_peer.c   the peer checks of ADR-019, one function each; every system
 *                fact comes in through ntfsmac_ctl_sys, so tests can fake it
 *   ctl_sys.c    the real ntfsmac_ctl_sys (the only file that uses Security
 *                .framework and the membership API)
 *   ctl.c        dispatch (handle_line) and the socket server
 */

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "internal.h"
#include "ntfsmac/ctl_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Request parser (ctl_parse.c)                                        */
/* ------------------------------------------------------------------ */

#define NTFSMAC_CTL_BSD_MAX 31u
#define NTFSMAC_CTL_LABEL_MAX 255u /* decoded bytes of confirm_label */
#define NTFSMAC_CTL_SERIAL_DIGITS 8u

typedef enum ntfsmac_ctl_parse_result {
	NTFSMAC_CTL_PARSE_OK = 0,
	NTFSMAC_CTL_PARSE_BAD_JSON,        /* not one flat JSON object of the grammar below */
	NTFSMAC_CTL_PARSE_TOO_LONG,        /* line longer than NTFSMAC_CTL_LINE_MAX */
	NTFSMAC_CTL_PARSE_UNKNOWN_COMMAND, /* `cmd` missing or not in the list */
	NTFSMAC_CTL_PARSE_BAD_FIELD        /* unknown, duplicate, missing, misplaced or malformed field */
} ntfsmac_ctl_parse_result;

typedef enum ntfsmac_ctl_repair_action {
	NTFSMAC_CTL_REPAIR_NONE = 0,
	NTFSMAC_CTL_REPAIR_CLEAR_DIRTY,
	NTFSMAC_CTL_REPAIR_SYNC_MIRROR
} ntfsmac_ctl_repair_action;

typedef struct ntfsmac_ctl_request {
	ntfsmac_ctl_cmd cmd;
	char bsd[NTFSMAC_CTL_BSD_MAX + 1];
	bool read_only;                    /* mount: mode is "ro" */
	bool force;                        /* unmount */
	ntfsmac_ctl_repair_action action;  /* repair */
	char confirm_label[NTFSMAC_CTL_LABEL_MAX + 1];
	char confirm_serial[NTFSMAC_CTL_SERIAL_DIGITS + 1];
	bool has_auth;                     /* the `auth` field was present */
	uint8_t auth[NTFSMAC_CTL_AUTH_FORM_BYTES];
} ntfsmac_ctl_request;

/*
 * Grammar: one JSON object, string keys, values that are strings or true or
 * false, nothing after the closing brace but white space. `\uXXXX` escapes
 * are decoded (surrogate pairs joined; a lone surrogate or \u0000 is
 * rejected); raw control bytes and invalid UTF-8 are rejected. A decoded
 * value (other than `cmd`) with a code point below U+0020, U+007F or in
 * U+0080..U+009F is BAD_FIELD. Keys:
 * cmd, bsd, mode ("auto" or "ro"), force, action, confirm_label,
 * confirm_serial, auth. A key may appear once. Each command takes exactly the
 * fields of docs/ARCHITECTURE.md section 3.4 (mode and force are optional);
 * any other field is BAD_FIELD. `out` is zeroed first and is meaningful only
 * on NTFSMAC_CTL_PARSE_OK. `line` need not be NUL terminated.
 */
ntfsmac_ctl_parse_result ntfsmac_ctl_parse_request(const char *line, size_t len,
						   ntfsmac_ctl_request *out);

/* "disk" digits, optionally "s" digits (at most 9 digits each); at most 31 bytes. */
bool ntfsmac_ctl_bsd_valid(const char *s);

/* ------------------------------------------------------------------ */
/* System facts and the peer checks (ctl_peer.c)                       */
/* ------------------------------------------------------------------ */

typedef enum ntfsmac_ctl_sys_proc {
	NTFSMAC_CTL_PROC_OK = 0,
	NTFSMAC_CTL_PROC_GONE,  /* no such process */
	NTFSMAC_CTL_PROC_ERROR  /* anything else */
} ntfsmac_ctl_sys_proc;

typedef enum ntfsmac_ctl_sys_answer {
	NTFSMAC_CTL_SYS_YES = 0, /* the fact holds */
	NTFSMAC_CTL_SYS_NO,      /* the fact does not hold */
	NTFSMAC_CTL_SYS_UNAVAILABLE /* could not be determined; the caller refuses */
} ntfsmac_ctl_sys_answer;

/* Everything the checks ask the operating system. Fakes in tests. */
typedef struct ntfsmac_ctl_sys {
	void *ctx;
	/* Fill uid, pid, console_user, admin and the audit token from the socket. 0 on success. */
	int (*peer_from_fd)(void *ctx, int fd, ntfsmac_ctl_peer *out);
	/* Parent pid and process group id of `pid` (proc_pidinfo). */
	ntfsmac_ctl_sys_proc (*proc_parent)(void *ctx, pid_t pid, pid_t *ppid, pid_t *pgid);
	/* YES: signed, satisfies the installed requirement, runs with the hardened runtime. */
	ntfsmac_ctl_sys_answer (*client_signed)(void *ctx, const ntfsmac_ctl_peer *peer);
	/* YES: the daemon itself holds the right for this external form (AuthorizationCopyRights). */
	ntfsmac_ctl_sys_answer (*authz_verify)(void *ctx, const uint8_t form[NTFSMAC_CTL_AUTH_FORM_BYTES]);
	/*
	 * YES: the stored definition of the right is the intended rule (class
	 * user, group admin, shared false, timeout 0, nothing that changes
	 * who may get it). NO: the right is missing or its definition differs.
	 * UNAVAILABLE: the definition could not be read.
	 */
	ntfsmac_ctl_sys_answer (*right_definition)(void *ctx);
	/*
	 * T-M4-14, XPC channel: fill uid, pid, console_user, admin and the audit token of a
	 * request from the audit token of its message (the identity belongs to the sender of
	 * that message, ADR-019 amendment). 0 on success. NULL: the XPC channel refuses
	 * everything.
	 */
	int (*peer_from_token)(void *ctx, const uint32_t token[8], ntfsmac_ctl_peer *out);
	/*
	 * The code requirement of check 3 as text, for the XPC connection
	 * (xpc_connection_set_peer_code_signing_requirement). false if it cannot be read.
	 */
	bool (*requirement)(void *ctx, char *buf, size_t cap);
} ntfsmac_ctl_sys;

/* The real implementation (ctl_sys.c). Valid for the life of the process. */
const ntfsmac_ctl_sys *ntfsmac_ctl_sys_default(void);

/* Forms already seen. The daemon refuses a form it has seen before. */
#define NTFSMAC_CTL_REPLAY_SLOTS 64u
typedef struct ntfsmac_ctl_replay {
	pthread_mutex_t mu;
	uint8_t form[NTFSMAC_CTL_REPLAY_SLOTS][NTFSMAC_CTL_AUTH_FORM_BYTES];
	unsigned count; /* used slots, at most NTFSMAC_CTL_REPLAY_SLOTS */
	unsigned next;  /* slot the next form overwrites (oldest first) */
} ntfsmac_ctl_replay;

void ntfsmac_ctl_replay_init(ntfsmac_ctl_replay *r);
/* True if `form` was already recorded. Otherwise records it and returns false. */
bool ntfsmac_ctl_replay_seen(ntfsmac_ctl_replay *r,
			     const uint8_t form[NTFSMAC_CTL_AUTH_FORM_BYTES]);

/* Longest parent chain the walk of check 2 follows; a longer chain is refused. */
#define NTFSMAC_CTL_CHAIN_MAX 64u

/* Result of one check: NTFSMAC_CTL_REFUSE_COUNT_ means "passed". */
#define NTFSMAC_CTL_PASS NTFSMAC_CTL_REFUSE_COUNT_

/* Gate for every request: the peer is the console user or in group admin. */
ntfsmac_ctl_refusal ntfsmac_ctl_check_peer_allowed(const ntfsmac_ctl_peer *peer);

/* Check 1 of ADR-019: the peer uid is in group admin. */
ntfsmac_ctl_refusal ntfsmac_ctl_check_admin(const ntfsmac_ctl_peer *peer);

/*
 * Check 2: neither the peer, nor any process on its parent chain, nor the
 * process group of any of them, is a live spawned process. `is_spawned` is
 * ops->is_spawned; NULL refuses.
 */
ntfsmac_ctl_refusal ntfsmac_ctl_check_not_spawned(const ntfsmac_ctl_sys *sys,
						  bool (*is_spawned)(void *, pid_t),
						  void *is_spawned_ctx, pid_t peer_pid);

/* Check 3: the peer is the installed signed CLI with the hardened runtime. */
ntfsmac_ctl_refusal ntfsmac_ctl_check_client_signed(const ntfsmac_ctl_sys *sys,
						    const ntfsmac_ctl_peer *peer);

/*
 * The right must be defined as intended before any form is trusted: a right
 * that is missing, or whose rule was loosened (a longer timeout, a shared
 * grant, another class or group), would make the form worth less than the
 * ADR says. Refuses with authorization_right_invalid (missing or different)
 * or check_unavailable (could not be read).
 */
ntfsmac_ctl_refusal ntfsmac_ctl_check_right(const ntfsmac_ctl_sys *sys);

/*
 * Check 4: the right is defined as intended, then a fresh administrator
 * authorization. `form` is NULL when the request carried none. A form is
 * recorded as used before it is verified, so a second request with the same
 * form is refused as replayed.
 */
ntfsmac_ctl_refusal ntfsmac_ctl_check_authorization(const ntfsmac_ctl_sys *sys,
						    ntfsmac_ctl_replay *replay,
						    const uint8_t *form);

/*
 * Check 4 has two implementations behind one function (T-M4-14; U17 answered 2026-11-17):
 *
 *   EXTERNAL_FORM  the design of ADR-019: the right and the external form above.
 *   ROOT_PEER      the fallback of ADR-019: no right and no form; the request is accepted only
 *                  from a root peer (euid 0 taken from the audit token, for example the CLI run
 *                  with sudo). Checks 1 to 3 still apply.
 *
 * One switch picks one for the whole daemon: the macro NTFSMAC_CTL_AUTHZ_FALLBACK_ROOT (the CMake
 * option NTFSMAC_AUTHZ_FALLBACK_ROOT, ON by default and in the product preset) sets the default
 * of ntfsmac_ctl_default_authz(); only main.c may set ntfsmac_ctl_env.authz. The product uses
 * ROOT_PEER: U17 showed on macOS 27.2 that a password grant of the CLI fails the daemon's
 * AuthorizationCopyRights with -60007, so EXTERNAL_FORM cannot pass there.
 */
typedef enum ntfsmac_ctl_authz_strategy {
	NTFSMAC_CTL_AUTHZ_EXTERNAL_FORM = 0,
	NTFSMAC_CTL_AUTHZ_ROOT_PEER
} ntfsmac_ctl_authz_strategy;

ntfsmac_ctl_authz_strategy ntfsmac_ctl_default_authz(void);
/* Fixed name for the log: "external_form" or "root_peer". */
const char *ntfsmac_ctl_authz_name(ntfsmac_ctl_authz_strategy a);

/* Which door a request came through. The privileged requests are served on XPC only. */
typedef enum ntfsmac_ctl_channel {
	NTFSMAC_CTL_CHAN_SOCKET = 0, /* refuses journal_clear and repair (wrong_channel) */
	NTFSMAC_CTL_CHAN_XPC         /* serves journal_clear and repair only */
} ntfsmac_ctl_channel;

/* What handle_line needs from the outside. */
typedef struct ntfsmac_ctl_env {
	const ntfsmac_ctl_sys *sys;
	ntfsmac_ctl_replay *replay;
	/*
	 * Check 4. Only main.c sets this, from ntfsmac_ctl_default_authz() (the build option); there is
	 * no run-time switch from a configuration file, so a config file can never weaken the check.
	 * Tests set it directly.
	 */
	ntfsmac_ctl_authz_strategy authz;
	ntfsmac_ctl_channel channel;      /* zero: the Unix socket */
	/*
	 * Test seam of the stale-socket takeover: called between socket_in_use() and the second
	 * lstat(), so a test can replace the path at the worst moment. NULL in production.
	 */
	void (*before_unlink)(void *ctx, const char *path);
	void *before_unlink_ctx;
} ntfsmac_ctl_env;

/* Check 4, the one seam: both strategies, chosen by `env->authz`. */
ntfsmac_ctl_refusal ntfsmac_ctl_check_admin_authorization(const ntfsmac_ctl_env *env,
							  const ntfsmac_ctl_peer *peer,
							  const ntfsmac_ctl_request *req);

/* Check 4, strategy ROOT_PEER: the peer's euid is 0. */
ntfsmac_ctl_refusal ntfsmac_ctl_check_root_peer(const ntfsmac_ctl_peer *peer);

/* Checks 1 to 4 in order; the first refusal is returned. */
ntfsmac_ctl_refusal ntfsmac_ctl_authorise_privileged(const ntfsmac_ctl_env *env,
						     const ntfsmac_ctl_ops *ops, void *ctx,
						     const ntfsmac_ctl_peer *peer,
						     const ntfsmac_ctl_request *req);

/* ------------------------------------------------------------------ */
/* Dispatch and server (ctl.c)                                         */
/* ------------------------------------------------------------------ */

/* ntfsmac_ctl_handle_line() with an explicit environment. */
ntfsmac_dmn_status ntfsmac_ctl_handle_line_env(const ntfsmac_ctl_env *env,
					       const ntfsmac_ctl_ops *ops, void *ctx,
					       const ntfsmac_ctl_peer *peer, const char *line,
					       size_t len, char *reply, size_t cap);

/* The environment of the XPC door: system facts, the shared replay table, channel XPC. */
ntfsmac_ctl_env ntfsmac_ctl_default_env_xpc(void);

/*
 * Stale-socket decision of ntfsmac_ctl_start() (ctl.c, M4 F-06), pure. Given the device and
 * inode of the socket that was examined and the result of a second lstat() of the path, say
 * whether the path may be unlinked (REMOVE), is already gone so that bind() may proceed (GONE),
 * or was replaced or cannot be examined and must be left alone (REFUSE).
 */
typedef enum ntfsmac_ctl_stale_action {
	NTFSMAC_CTL_STALE_REMOVE = 0,
	NTFSMAC_CTL_STALE_GONE,
	NTFSMAC_CTL_STALE_REFUSE
} ntfsmac_ctl_stale_action;

typedef struct ntfsmac_ctl_stale_stat {
	bool ok;      /* the second lstat() succeeded */
	int err;      /* errno of the failed lstat(), when !ok */
	dev_t dev;    /* st_dev, when ok */
	ino_t ino;    /* st_ino, when ok */
	mode_t mode;  /* st_mode, when ok */
} ntfsmac_ctl_stale_stat;

ntfsmac_ctl_stale_action ntfsmac_ctl_stale_socket_decide(dev_t want_dev, ino_t want_ino,
							 const ntfsmac_ctl_stale_stat *now);

/* ntfsmac_ctl_start() with an explicit environment (tests). */
ntfsmac_dmn_status ntfsmac_ctl_start_env(const char *socket_path, const ntfsmac_ctl_ops *ops,
					 void *ctx, const ntfsmac_ctl_env *env,
					 ntfsmac_ctl **out);

/* ------------------------------------------------------------------ */
/* XPC listener of the privileged requests (ctl_xpc.c, T-M4-14)        */
/* ------------------------------------------------------------------ */

/*
 * Pure part, testable on every host: one XPC request message is a dictionary that holds the
 * request line. `line` is the string value taken from the dictionary (NULL when the key is
 * missing or not a string); `len` its length. Accepts a request line only when it is within
 * the size limit, contains no newline and no NUL (a message is one request), and parses
 * (ntfsmac_ctl_parse_request) to journal_clear or repair. Anything else is refused:
 * NTFSMAC_CTL_PARSE_OK or the parser's result; a request that parses to another command is
 * reported through `*wrong_channel`. `out` is meaningful only when the result is OK.
 */
ntfsmac_ctl_parse_result ntfsmac_xpc_check_request(const char *line, size_t len,
						   ntfsmac_ctl_request *out, bool *wrong_channel);

typedef struct ntfsmac_xpc ntfsmac_xpc; /* opaque */

/*
 * Start the listener for `service` (NTFSMAC_CTL_XPC_SERVICE in production; tests use another
 * name). Every connection gets the code requirement of check 3 (`sys->requirement`) before it
 * is resumed, so a client that does not satisfy it is dropped by the system; each message is
 * then handled by ntfsmac_ctl_handle_line_env() with `env->channel` forced to
 * NTFSMAC_CTL_CHAN_XPC and the peer taken from the audit token of that message. Messages are
 * handled one at a time. Not implemented off macOS (E_NOTIMPL). T-M4-08 calls this after
 * ntfsmac_ctl_start() and stops it before exit; the launchd plist needs the MachServices entry.
 */
ntfsmac_dmn_status ntfsmac_xpc_start(const char *service, const ntfsmac_ctl_ops *ops, void *ctx,
				     const ntfsmac_ctl_env *env, ntfsmac_xpc **out);
/*
 * Stop the listener and make later messages no-ops. The object is not freed (connections that
 * were accepted earlier may still have handlers queued); call it once at shutdown.
 */
void ntfsmac_xpc_stop(ntfsmac_xpc *x);

/* ------------------------------------------------------------------ */
/* macOS system implementation details (ctl_sys.c)                     */
/* ------------------------------------------------------------------ */

/*
 * Build a system object for the real calls with other file names. Production
 * uses ntfsmac_ctl_sys_default(); the VM probe of T-M4-04 passes a requirement
 * file and a right name of its own. `requirement_file` holds one line with a
 * code requirement string. It must be a regular file owned by root and not
 * writable by group or other, unless `allow_any_owner` (tests only).
 */
typedef struct ntfsmac_ctl_sys_real {
	ntfsmac_ctl_sys sys;
	const char *requirement_file;
	const char *right_name;
	bool allow_any_owner;
} ntfsmac_ctl_sys_real;

void ntfsmac_ctl_sys_real_init(ntfsmac_ctl_sys_real *r, const char *requirement_file,
			       const char *right_name);

/* Where the installer records the requirement of the CLI (ADR-019 check 3). */
#define NTFSMAC_CTL_REQUIREMENT_FILE \
	"/Library/Application Support/ntfs-macos/cli-requirement"

#ifdef __cplusplus
}
#endif

#endif /* NTFSMAC_DAEMON_CTL_PRIV_H */
