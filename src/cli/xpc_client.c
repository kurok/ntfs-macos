/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Client side of the XPC service of the privileged requests (T-M4-14, ADR-019 amendment): one
 * request message, one reply message, with a time limit of our own (xpc offers none for a
 * reply). The system attaches the audit token of this process to the message; the daemon
 * judges the sender by it. Off macOS there is no such service and the call fails.
 *
 * The connection is made with XPC_CONNECTION_MACH_SERVICE_PRIVILEGED: the name is looked up in
 * the system domain only, so a service that a user registered under the same name in a user
 * domain is never the one that gets the authorization form.
 *
 * The reply block may run after the wait gave up. It therefore writes only into a heap record
 * that it owns; the caller copies out of the record only when the wait succeeded. On a timeout
 * the record, the semaphore and the connection are left to the process (it exits right after).
 */
#include <stdlib.h>
#include <string.h>

#include "cli.h"

#if defined(__APPLE__)

#include <dispatch/dispatch.h>
#include <xpc/xpc.h>

struct reply_slot {
	int rc;
	const char *why; /* a fixed string */
	char *text;      /* heap copy of the reply line */
};

int ntfsmac_cli_privileged_exchange(const char *service, const char *line, int timeout_ms,
				    char *reply, size_t cap, char *err_text, size_t err_cap)
{
	xpc_connection_t conn;
	xpc_object_t msg;
	dispatch_semaphore_t sem = dispatch_semaphore_create(0);
	struct reply_slot *slot = calloc(1, sizeof *slot);
	int rc;

	if (cap < 2 || sem == NULL || slot == NULL) {
		snprintf(err_text, err_cap, "internal error: no memory or reply buffer too small");
		free(slot);
		if (sem != NULL)
			dispatch_release(sem);
		return -1;
	}
	slot->rc = -1;
	slot->why = "the ntfsmac daemon did not answer";
	conn = xpc_connection_create_mach_service(service, NULL, XPC_CONNECTION_MACH_SERVICE_PRIVILEGED);
	if (conn == NULL) {
		snprintf(err_text, err_cap, "cannot reach the ntfsmac daemon");
		free(slot); /* no block was queued: nothing else can touch the record */
		dispatch_release(sem);
		return -1;
	}
	xpc_connection_set_event_handler(conn, ^(xpc_object_t event) {
		(void)event; /* errors arrive as the reply of the message below */
	});
	xpc_connection_resume(conn);

	msg = xpc_dictionary_create(NULL, NULL, 0);
	xpc_dictionary_set_string(msg, NTFSMAC_CTL_XPC_KEY_REQUEST, line);
	xpc_connection_send_message_with_reply(conn, msg, dispatch_get_global_queue(0, 0),
					       ^(xpc_object_t r) {
		if (xpc_get_type(r) == XPC_TYPE_DICTIONARY) {
			const char *text = xpc_dictionary_get_string(r, NTFSMAC_CTL_XPC_KEY_REPLY);

			if (text != NULL && strlen(text) < NTFSMAC_CTL_REPLY_MAX) {
				slot->text = strdup(text);
				slot->rc = slot->text != NULL ? 0 : -1;
			} else {
				slot->why = "the daemon sent a reply that is not valid";
			}
		} else if (r == XPC_ERROR_CONNECTION_INVALID) {
			slot->why = "the ntfsmac daemon is not running (its service is not registered)";
		} else if (r == XPC_ERROR_CONNECTION_INTERRUPTED) {
			slot->why = "the ntfsmac daemon closed the connection without a reply "
				    "(is this the installed, signed ntfsmac?)";
		}
		dispatch_semaphore_signal(sem);
	});
	/*
	 * A one-shot CLI turns the reply into a synchronous result with its own time limit; the
	 * reply block runs on a global queue, not on this thread, and the wait is bounded.
	 */
	/* NOLINTNEXTLINE(clang-analyzer-optin.performance.GCDAntipattern) */
	if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)timeout_ms *
								  (int64_t)NSEC_PER_MSEC)) != 0) {
		/* the block may still run and owns `slot` and `sem`: nothing is freed here */
		snprintf(err_text, err_cap, "the daemon did not answer in time");
		xpc_connection_cancel(conn);
		/* cppcheck-suppress memleak ; deliberate: the late reply block still owns `slot` */
		return -1;
	}
	rc = slot->rc;
	if (rc == 0 && strlen(slot->text) < cap)
		memcpy(reply, slot->text, strlen(slot->text) + 1);
	else if (rc == 0)
		rc = -1, slot->why = "the daemon's reply is too long";
	if (rc != 0)
		snprintf(err_text, err_cap, "%s", slot->why);
	free(slot->text);
	free(slot);
	xpc_connection_cancel(conn);
	xpc_release(conn);
	xpc_release(msg);
	dispatch_release(sem);
	return rc;
}

#else /* not macOS */

int ntfsmac_cli_privileged_exchange(const char *service, const char *line, int timeout_ms,
				    char *reply, size_t cap, char *err_text, size_t err_cap)
{
	(void)service;
	(void)line;
	(void)timeout_ms;
	(void)reply;
	(void)cap;
	snprintf(err_text, err_cap, "the privileged requests are only available on macOS");
	return -1;
}

#endif
