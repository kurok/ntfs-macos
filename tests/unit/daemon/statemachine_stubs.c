/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-DMN-SM-03: the stub modules compile, link and answer "not implemented".
 * A task that fills a stub replaces the matching check here with its own tests.
 */
#include "check.h"
#include "internal.h"

int main(void)
{
	ntfsmac_probe_verdict verdict;
	ntfsmac_errwatch w;
	ntfsmac_conflict_report cr;
	ntfsmac_da *da = (ntfsmac_da *)1;
	ntfsmac_ctl *ctl = (ntfsmac_ctl *)1;
	ntfsmac_session *sess = (ntfsmac_session *)1;
	ntfsmac_vol_mode mode = NTFSMAC_VMODE_RW;
	char reply[16] = "x";
	pid_t busy = 5;

	CHECK_EQ_INT(ntfsmac_spawn_probe(NULL, &verdict), NTFSMAC_DMN_E_NOTIMPL);
	CHECK_EQ_INT(ntfsmac_da_start(NULL, NULL, &da), NTFSMAC_DMN_E_NOTIMPL);
	CHECK(da == NULL);
	CHECK_EQ_INT(ntfsmac_unmount_path("/x", NULL, &busy), NTFSMAC_DMN_E_NOTIMPL);
	CHECK_EQ_INT(busy, 0);
	CHECK_EQ_INT(ntfsmac_mountpoint_create("a", 1, 0, 0, 0, NULL), NTFSMAC_DMN_E_NOTIMPL);
	CHECK_EQ_INT(ntfsmac_ctl_start("/x", NULL, NULL, &ctl), NTFSMAC_DMN_E_NOTIMPL);
	CHECK(ctl == NULL);
	CHECK_EQ_INT(ntfsmac_ctl_handle_line(NULL, NULL, NULL, "", 0, reply, sizeof reply),
		     NTFSMAC_DMN_E_NOTIMPL);
	CHECK(reply[0] == '\0');
	CHECK_EQ_INT(ntfsmac_session_begin(NULL, NULL, NTFSMAC_POLICY_RO, &sess, &mode),
		     NTFSMAC_DMN_E_NOTIMPL);
	CHECK(sess == NULL);
	CHECK_EQ_INT(mode, NTFSMAC_VMODE_NONE);
	CHECK_EQ_INT(ntfsmac_reconcile_run(NULL, NULL, NULL), NTFSMAC_DMN_E_NOTIMPL);
	CHECK_EQ_INT(ntfsmac_conflict_detect(&cr), NTFSMAC_DMN_E_NOTIMPL);
	CHECK_EQ_INT(ntfsmac_doctor_run(NULL, NULL), NTFSMAC_DMN_E_NOTIMPL);
	CHECK_EQ_INT(ntfsmac_notify_post(NTFSMAC_NOTIFY_REFUSED, "x", "y"), NTFSMAC_DMN_E_NOTIMPL);

	/* The stub watcher fails closed: no session is ever proven clean. */
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
