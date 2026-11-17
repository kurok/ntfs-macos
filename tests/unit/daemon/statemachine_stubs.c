/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-DMN-SM-03: the stub modules compile, link and answer "not implemented" (the session and notification modules are no stub any more).
 * A task that fills a stub replaces the matching check here with its own tests.
 */
#include "check.h"
#include "internal.h"

int main(void)
{
	ntfsmac_errwatch w;
	ntfsmac_session *sess = (ntfsmac_session *)1;
	ntfsmac_session_start res;

	/* session.c is real since T-M4-14 (tests/unit/daemon/session_test.c); bad arguments only. */
	CHECK_EQ_INT(ntfsmac_session_begin(NULL, NULL, NULL, NULL, &sess, &res), NTFSMAC_DMN_E_INVAL);
	CHECK(sess == NULL);
	/* notify.c is real since T-M4-08 (notify_test.c); a bad kind is refused, and no banner is
	 * ever shown from a unit test. */
	CHECK_EQ_INT(ntfsmac_notify_post((ntfsmac_notify_kind)99, "x", "y"), NTFSMAC_DMN_E_INVAL);

	/* An empty channel never proves a session clean (parser tests: errwatch_test.c). */
	ntfsmac_errwatch_init(&w);
	ntfsmac_errwatch_eof(&w);
	CHECK(!ntfsmac_errwatch_clean_proof(&w, true, false, 0));

	CHECK(ntfsmac_dmn_status_name(NTFSMAC_DMN_OK)[0] != '?');
	if (g_failures != 0) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	return 0;
}
