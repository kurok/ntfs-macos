/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The XPC Mach service of the privileged requests (T-M4-14, docs/ARCHITECTURE.md section 3.4,
 * ADR-019 amendment of 2026-11-16).
 *
 * Why a second door: on a Unix socket the system reports the last process that read or wrote
 * it, not the author of the bytes, so checks 2 and 3 could be satisfied by borrowing the
 * identity of the signed CLI. A Mach message carries the audit token of its sender. Two things
 * follow from that here:
 *
 *   - the code requirement of check 3 is set on the connection before it is resumed, so the
 *     system itself drops a client that does not satisfy it;
 *   - the peer that checks 1, 2 and 4 look at is built from the audit token of each message,
 *     never from the connection, and ntfsmac_ctl_check_client_signed() runs per message too.
 *
 * The message is a dictionary with one string, the same JSON line as on the socket
 * (NTFSMAC_CTL_XPC_KEY_REQUEST), and the reply is a dictionary with the reply line
 * (NTFSMAC_CTL_XPC_KEY_REPLY). Everything after that is ntfsmac_ctl_handle_line_env(), the same
 * code the socket uses, with channel XPC: it serves journal_clear and repair and nothing else.
 *
 * The part that can be tested without a Mach service, ntfsmac_xpc_check_request(), is plain C.
 * The listener needs launchd to know the service name (the MachServices entry of the plist,
 * T-M4-08); a process that is not started by launchd under that name receives nothing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ctl_priv.h"

ntfsmac_ctl_parse_result ntfsmac_xpc_check_request(const char *line, size_t len,
						   ntfsmac_ctl_request *out, bool *wrong_channel)
{
	ntfsmac_ctl_parse_result pr;

	if (wrong_channel != NULL)
		*wrong_channel = false;
	if (line == NULL || out == NULL)
		return NTFSMAC_CTL_PARSE_BAD_JSON;
	if (len > NTFSMAC_CTL_LINE_MAX)
		return NTFSMAC_CTL_PARSE_TOO_LONG;
	/* One message is one request: no line ends and no NUL inside. */
	if (memchr(line, '\n', len) != NULL || memchr(line, '\r', len) != NULL ||
	    memchr(line, '\0', len) != NULL)
		return NTFSMAC_CTL_PARSE_BAD_JSON;
	pr = ntfsmac_ctl_parse_request(line, len, out);
	if (pr != NTFSMAC_CTL_PARSE_OK)
		return pr;
	if (out->cmd != NTFSMAC_CTL_CMD_JOURNAL_CLEAR && out->cmd != NTFSMAC_CTL_CMD_REPAIR) {
		if (wrong_channel != NULL)
			*wrong_channel = true;
		return NTFSMAC_CTL_PARSE_UNKNOWN_COMMAND;
	}
	return NTFSMAC_CTL_PARSE_OK;
}

#if defined(__APPLE__)

#include <dispatch/dispatch.h>
#include <xpc/xpc.h>

/*
 * Exported by libxpc and used by Apple's own code and by many daemons, but not declared in the
 * public headers: the audit token of the sender of one message. There is no public call that
 * returns the whole token of a message (xpc_connection_get_pid and _get_euid are public, but a
 * pid alone can be reused, which is what ADR-019 avoids). The call is made once per message;
 * a message whose token cannot be read is refused.
 */
extern void xpc_dictionary_get_audit_token(xpc_object_t xdict, audit_token_t *token);

struct ntfsmac_xpc {
	xpc_connection_t listener;
	dispatch_queue_t queue; /* serial: one message at a time */
	const ntfsmac_ctl_ops *ops;
	void *ctx;
	ntfsmac_ctl_env env;
	char requirement[1024];
	bool has_requirement;
	bool stopped; /* set on the queue by ntfsmac_xpc_stop(): later messages are ignored */
	char reply[NTFSMAC_CTL_REPLY_MAX];
};

static void send_reply(xpc_object_t message, const char *reply_line)
{
	xpc_object_t reply = xpc_dictionary_create_reply(message);
	xpc_connection_t remote;

	if (reply == NULL)
		return;
	remote = xpc_dictionary_get_remote_connection(message);
	if (remote != NULL) {
		xpc_dictionary_set_string(reply, NTFSMAC_CTL_XPC_KEY_REPLY, reply_line);
		xpc_connection_send_message(remote, reply);
	}
	xpc_release(reply);
}

static void handle_message(struct ntfsmac_xpc *x, xpc_object_t message)
{
	ntfsmac_ctl_peer peer;
	audit_token_t token;
	const char *line;
	size_t len;

	if (xpc_get_type(message) != XPC_TYPE_DICTIONARY)
		return;
	line = xpc_dictionary_get_string(message, NTFSMAC_CTL_XPC_KEY_REQUEST);
	if (line == NULL) {
		send_reply(message, "{\"ok\":false,\"error\":\"bad_field\"}");
		return;
	}
	len = strnlen(line, NTFSMAC_CTL_LINE_MAX + 1u);
	if (x->stopped)
		return;
	{
		/* One message, one request, the right command for this door: before anything else. */
		ntfsmac_ctl_request req;
		bool wrong = false;
		ntfsmac_ctl_parse_result pr = ntfsmac_xpc_check_request(line, len, &req, &wrong);

		if (pr != NTFSMAC_CTL_PARSE_OK) {
			send_reply(message, wrong ? "{\"ok\":false,\"error\":\"denied\",\"reason\":\"wrong_channel\"}"
				   : pr == NTFSMAC_CTL_PARSE_TOO_LONG ? "{\"ok\":false,\"error\":\"line_too_long\"}"
				   : pr == NTFSMAC_CTL_PARSE_BAD_FIELD ? "{\"ok\":false,\"error\":\"bad_field\"}"
				   : pr == NTFSMAC_CTL_PARSE_UNKNOWN_COMMAND ? "{\"ok\":false,\"error\":\"unknown_command\"}"
				   : "{\"ok\":false,\"error\":\"bad_json\"}");
			return;
		}
	}

	memset(&token, 0, sizeof token);
	xpc_dictionary_get_audit_token(message, &token);
	if (x->env.sys->peer_from_token == NULL ||
	    x->env.sys->peer_from_token(x->env.sys->ctx, (const uint32_t *)&token, &peer) != 0) {
		/* Cannot tell who sent it: refuse, and say it the way every check does. */
		send_reply(message, "{\"ok\":false,\"error\":\"denied\",\"reason\":\"check_unavailable\"}");
		return;
	}
	/* A request that does not parse is answered by handle_line_env() with its own error. */
	(void)ntfsmac_ctl_handle_line_env(&x->env, x->ops, x->ctx, &peer, line, len, x->reply,
					  sizeof x->reply);
	send_reply(message, x->reply);
}

static void accept_connection(struct ntfsmac_xpc *x, xpc_connection_t conn)
{
	/* A requirement the system refuses to set means no identity check: drop the connection. */
	if (x->has_requirement &&
	    xpc_connection_set_peer_code_signing_requirement(conn, x->requirement) != 0) {
		xpc_connection_cancel(conn);
		return;
	}
	xpc_connection_set_target_queue(conn, x->queue);
	xpc_connection_set_event_handler(conn, ^(xpc_object_t event) {
		if (xpc_get_type(event) == XPC_TYPE_DICTIONARY)
			handle_message(x, event);
		/* errors (invalidated, interrupted, requirement not met): nothing to do, the
		 * connection goes away by itself */
	});
	xpc_connection_resume(conn);
}

ntfsmac_dmn_status ntfsmac_xpc_start(const char *service, const ntfsmac_ctl_ops *ops, void *ctx,
				     const ntfsmac_ctl_env *env, ntfsmac_xpc **out)
{
	struct ntfsmac_xpc *x;

	if (out != NULL)
		*out = NULL;
	if (service == NULL || ops == NULL || env == NULL || env->sys == NULL ||
	    env->replay == NULL || out == NULL)
		return NTFSMAC_DMN_E_INVAL;
	x = calloc(1, sizeof *x);
	if (x == NULL)
		return NTFSMAC_DMN_E_IO;
	x->ops = ops;
	x->ctx = ctx;
	x->env = *env;
	x->env.channel = NTFSMAC_CTL_CHAN_XPC; /* this door serves the privileged requests only */
	/*
	 * Check 3 on the connection. Without a readable requirement file there is no way to say
	 * who the CLI is, so the service does not start: a service that accepted every client
	 * and relied on the per-message check alone would still be safe, but the connection
	 * level check is part of the decision, so it is not optional.
	 */
	if (env->sys->requirement == NULL ||
	    !env->sys->requirement(env->sys->ctx, x->requirement, sizeof x->requirement)) {
		free(x);
		return NTFSMAC_DMN_E_DENIED;
	}
	x->has_requirement = true;
	/* Which implementation of check 4 is active, once, so that a log shows a fallback build. */
	fprintf(stderr, "ntfsmacd: privileged requests on %s, check 4 of ADR-019: %s\n", service,
		ntfsmac_ctl_authz_name(env->authz));
	x->queue = dispatch_queue_create("org.ntfs-macos.ctl.privileged", DISPATCH_QUEUE_SERIAL);
	x->listener = xpc_connection_create_mach_service(service, x->queue,
							 XPC_CONNECTION_MACH_SERVICE_LISTENER);
	if (x->listener == NULL) {
		dispatch_release(x->queue);
		free(x);
		return NTFSMAC_DMN_E_IO;
	}
	xpc_connection_set_event_handler(x->listener, ^(xpc_object_t event) {
		if (xpc_get_type(event) == XPC_TYPE_CONNECTION)
			accept_connection(x, (xpc_connection_t)event);
	});
	xpc_connection_resume(x->listener);
	*out = x;
	return NTFSMAC_DMN_OK;
}

/*
 * Stops accepting and makes every later message a no-op. The object is NOT freed: handlers of
 * connections that were accepted earlier hold a pointer to it and may still be queued, so
 * freeing it would leave them a dangling pointer. It lives until the process ends; the daemon
 * calls this once, at shutdown, and may free its own `ops` and `ctx` after it returns.
 */
void ntfsmac_xpc_stop(ntfsmac_xpc *x)
{
	if (x == NULL)
		return;
	xpc_connection_cancel(x->listener);
	/* On the serial queue: everything queued before this runs first, nothing after sees `ops`. */
	dispatch_sync(x->queue, ^(void) {
		x->stopped = true;
	});
}

#else /* not macOS */

ntfsmac_dmn_status ntfsmac_xpc_start(const char *service, const ntfsmac_ctl_ops *ops, void *ctx,
				     const ntfsmac_ctl_env *env, ntfsmac_xpc **out)
{
	(void)ctx;
	if (out != NULL)
		*out = NULL;
	if (service == NULL || ops == NULL || env == NULL || env->sys == NULL || env->replay == NULL ||
	    out == NULL)
		return NTFSMAC_DMN_E_INVAL; /* the same arguments are refused as on macOS */
	return NTFSMAC_DMN_E_NOTIMPL;
}

void ntfsmac_xpc_stop(ntfsmac_xpc *x)
{
	(void)x;
}

#endif
