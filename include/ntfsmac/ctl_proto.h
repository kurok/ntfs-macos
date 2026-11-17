/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_CTL_PROTO_H
#define NTFSMAC_CTL_PROTO_H

/*
 * Wire names of the daemon control protocol (docs/ARCHITECTURE.md section
 * 3.4), shared by the daemon (src/daemon/ctl*.c) and the CLI (src/cli/).
 * Every string here is fixed text: a reply never echoes bytes taken from a
 * request or from a disk. The name functions live in src/daemon/ctl_parse.c.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Default socket path (section 3.4). */
#define NTFSMAC_CTL_SOCKET_PATH "/var/run/ntfsmacd.sock"

/*
 * Mach service of the privileged requests (journal_clear, repair; ADR-019,
 * amendment 2026-11-16). T-M4-14 adds the listener (src/daemon/ctl_xpc.c) and T-M4-08 the
 * MachServices entry of the launchd plist. A request is one message: a dictionary with the
 * string NTFSMAC_CTL_XPC_KEY_REQUEST (the same JSON line as on the socket); the reply is a
 * dictionary with the string NTFSMAC_CTL_XPC_KEY_REPLY (the same reply line). The Unix socket
 * answers `journal_clear` and `repair` with `denied`, reason `wrong_channel`, and this service
 * answers every other command the same way. The worker and volmark sandbox
 * profiles (integration/sandbox/) deny looking it up; the spawn module hands
 * this name to them as the parameter XPC_SERVICE, so it is defined only here.
 */
#define NTFSMAC_CTL_XPC_SERVICE "org.ntfs-macos.ctl.privileged"
#define NTFSMAC_CTL_XPC_KEY_REQUEST "request"
#define NTFSMAC_CTL_XPC_KEY_REPLY "reply"

/* One request line: at most 4096 bytes without the newline. */
#define NTFSMAC_CTL_LINE_MAX 4096u

/* The reply buffer the daemon hands to every operation. */
#define NTFSMAC_CTL_REPLY_MAX (64u * 1024u)

/* Smallest reply buffer ntfsmac_ctl_handle_line() accepts. */
#define NTFSMAC_CTL_REPLY_MIN 256u

/* Authorization Services right of ADR-019, check 4. */
#define NTFSMAC_CTL_RIGHT_NAME "org.ntfs-macos.journal-modify"

/* An AuthorizationExternalForm is 32 bytes; on the wire it is 64 hex digits. */
#define NTFSMAC_CTL_AUTH_FORM_BYTES 32u

/* Request commands: X(IDENTIFIER, "wire name"). */
#define NTFSMAC_CTL_CMD_LIST(X)            \
	X(LIST, "list")                    \
	X(STATUS, "status")                \
	X(MOUNT, "mount")                  \
	X(UNMOUNT, "unmount")              \
	X(DOCTOR, "doctor")                \
	X(JOURNAL_CLEAR, "journal_clear")  \
	X(REPAIR, "repair")

/* Error codes of a failed reply: {"ok":false,"error":"<code>",...}. */
#define NTFSMAC_CTL_ERROR_LIST(X)              \
	X(BAD_JSON, "bad_json")                \
	X(LINE_TOO_LONG, "line_too_long")      \
	X(UNKNOWN_COMMAND, "unknown_command")  \
	X(BAD_FIELD, "bad_field")              \
	X(DENIED, "denied")                    \
	X(NOT_IMPLEMENTED, "not_implemented")  \
	X(FAILED, "failed")                    \
	X(INTERNAL, "internal")

/* Why a request was refused: {"ok":false,"error":"denied","reason":"<id>"}. */
#define NTFSMAC_CTL_REFUSAL_LIST(X)                          \
	X(PEER_NOT_ALLOWED, "peer_not_allowed")              \
	X(NOT_ADMIN, "not_admin")                            \
	X(SPAWNED_PROCESS, "spawned_process")                \
	X(CLIENT_NOT_SIGNED, "client_not_signed")            \
	X(NO_AUTHORIZATION, "no_authorization")              \
	X(AUTHORIZATION_REPLAYED, "authorization_replayed")  \
	X(AUTHORIZATION_DENIED, "authorization_denied")      \
	X(AUTHORIZATION_RIGHT_INVALID, "authorization_right_invalid") \
	X(CHECK_UNAVAILABLE, "check_unavailable")            \
	X(WRONG_CHANNEL, "wrong_channel")

typedef enum ntfsmac_ctl_cmd {
#define X(id, str) NTFSMAC_CTL_CMD_##id,
	NTFSMAC_CTL_CMD_LIST(X)
#undef X
	NTFSMAC_CTL_CMD_COUNT_
} ntfsmac_ctl_cmd;

typedef enum ntfsmac_ctl_error {
#define X(id, str) NTFSMAC_CTL_ERR_##id,
	NTFSMAC_CTL_ERROR_LIST(X)
#undef X
	NTFSMAC_CTL_ERR_COUNT_
} ntfsmac_ctl_error;

typedef enum ntfsmac_ctl_refusal {
#define X(id, str) NTFSMAC_CTL_REFUSE_##id,
	NTFSMAC_CTL_REFUSAL_LIST(X)
#undef X
	NTFSMAC_CTL_REFUSE_COUNT_
} ntfsmac_ctl_refusal;

/* Fixed wire names; "?" for a value out of range. Never NULL. */
const char *ntfsmac_ctl_cmd_name(ntfsmac_ctl_cmd c);
const char *ntfsmac_ctl_error_name(ntfsmac_ctl_error e);
const char *ntfsmac_ctl_refusal_name(ntfsmac_ctl_refusal r);

#ifdef __cplusplus
}
#endif

#endif /* NTFSMAC_CTL_PROTO_H */
