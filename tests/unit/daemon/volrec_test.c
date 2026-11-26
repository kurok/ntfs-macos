/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-DMN-REC-01..02 (T-M7-16): the part of a volume record that describes one disk (volrec.c).
 * Disk A is probed and goes; disk B appears under the same BSD name and its attach stops before
 * the probe. `status` for the name must show nothing of A, a record without a verdict is never
 * reported clean, and B is a candidate that accepts a mount whatever A was.
 */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "internal.h"
#include "ntfsmac/ctl_proto.h"
#include "report.h"

#define SERIAL_A 0x1122334455667788ull

static void verdict_of_a(ntfsmac_probe_verdict *pv)
{
	memset(pv, 0, sizeof *pv);
	ntfsmac_probe_verdict_unknown(pv);
	pv->is_ntfs = true;
	pv->serial = SERIAL_A;
	pv->total_sectors = 131071;
	pv->state = NTFSMAC_PROBE_STATE_NAMES_NOT_NFC;
	pv->reason_codes = NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_NAMES_NOT_NFC);
	pv->volume_flags = NTFSMAC_PROBE_VOLFLAG_BIT(NTFSMAC_PROBE_VOLFLAG_DIRTY);
	pv->nonnfc_count = 1;
	pv->nonnfc_path_count = 1;
	snprintf(pv->nonnfc_paths[0], sizeof pv->nonnfc_paths[0], "/secret-of-disk-a");
}

/* The status reply the daemon builds from a record (main.c op_status): the verdict or none. */
static size_t status_of(const ntfsmac_volume *sm, const ntfsmac_volrec *r, char *buf, size_t cap)
{
	ntfsmac_report_vol rv = {"disk4s1", "", ntfsmac_vol_state_name(sm->state), "none", "", ""};

	return ntfsmac_report_status(&rv, ntfsmac_volrec_verdict(r), NULL, buf, cap);
}

/* U-DMN-REC-01: A probed and gone, B under the same name before its probe: nothing of A. */
static void test_name_reuse(void)
{
	static char buf[NTFSMAC_CTL_REPLY_MAX];
	static ntfsmac_probe_verdict pv;
	ntfsmac_volume sm;
	ntfsmac_volrec r;
	ntfsmac_vol_event_arg arg;

	memset(&r, 0, sizeof r);
	CHECK(ntfsmac_volrec_verdict(&r) == NULL);
	ntfsmac_volume_init(&sm);

	/* disk A: probed, refused, the verdict kept */
	verdict_of_a(&pv);
	CHECK_EQ_INT(ntfsmac_volume_apply(&sm, NTFSMAC_VEV_START, NULL), NTFSMAC_VRES_OK);
	CHECK_EQ_INT(ntfsmac_volume_apply(&sm, NTFSMAC_VEV_PROBE_DONE, NULL), NTFSMAC_VRES_OK);
	CHECK_EQ_INT(ntfsmac_volrec_set_verdict(&r, &pv), NTFSMAC_DMN_OK);
	memset(&arg, 0, sizeof arg);
	arg.reason = NTFSMAC_VREASON_POLICY_REFUSED;
	CHECK_EQ_INT(ntfsmac_volume_apply(&sm, NTFSMAC_VEV_REFUSED, &arg), NTFSMAC_VRES_OK);
	CHECK(status_of(&sm, &r, buf, sizeof buf) > 0);
	CHECK(strstr(buf, "0x1122334455667788") != NULL);
	CHECK(strstr(buf, "secret-of-disk-a") != NULL);
	/* the stored copy is a copy: changing the caller's verdict does not change it */
	pv.serial = 1;
	CHECK(ntfsmac_volrec_verdict(&r) != NULL && ntfsmac_volrec_verdict(&r)->serial == SERIAL_A);

	/* A goes; disk B appears under the same name (main.c vol_reset_for_appearance) */
	CHECK_EQ_INT(ntfsmac_volume_apply(&sm, NTFSMAC_VEV_DISK_GONE, NULL), NTFSMAC_VRES_OK);
	CHECK_EQ_INT(sm.state, NTFSMAC_VOL_GONE);
	ntfsmac_volrec_reset(&r);
	ntfsmac_volume_init(&sm);
	/* B's attach stops before the probe (no console user, the open fails, an unmount fails) */
	CHECK_EQ_INT(ntfsmac_volume_apply(&sm, NTFSMAC_VEV_START, NULL), NTFSMAC_VRES_OK);
	memset(&arg, 0, sizeof arg);
	arg.reason = NTFSMAC_VREASON_PROBE_FAILED;
	CHECK_EQ_INT(ntfsmac_volume_apply(&sm, NTFSMAC_VEV_PROBE_FAILED, &arg), NTFSMAC_VRES_OK);

	CHECK(ntfsmac_volrec_verdict(&r) == NULL);
	CHECK(status_of(&sm, &r, buf, sizeof buf) > 0);
	CHECK(strstr(buf, "\"probe\"") == NULL);       /* no verdict member at all */
	CHECK(strstr(buf, "1122334455667788") == NULL); /* not A's serial */
	CHECK(strstr(buf, "secret-of-disk-a") == NULL); /* not A's paths */
	CHECK(strstr(buf, "DIRTY") == NULL);            /* not A's flags */
	CHECK(strstr(buf, "clean") == NULL);            /* never reported clean */
	CHECK(strstr(buf, "\"journal\"") == NULL);
	/* B is a candidate that a `mount` request may retry */
	CHECK(ntfsmac_volume_accepts_mount(&sm));
}

/* U-DMN-REC-02: the helpers refuse NULL and keep working after a reset. */
static void test_helpers(void)
{
	static ntfsmac_probe_verdict pv;
	ntfsmac_volrec r;

	memset(&r, 0, sizeof r);
	verdict_of_a(&pv);
	CHECK_EQ_INT(ntfsmac_volrec_set_verdict(NULL, &pv), NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_volrec_set_verdict(&r, NULL), NTFSMAC_DMN_E_INVAL);
	CHECK(ntfsmac_volrec_verdict(NULL) == NULL);
	ntfsmac_volrec_reset(NULL);
	ntfsmac_volrec_reset(&r); /* nothing stored: fine */
	CHECK_EQ_INT(ntfsmac_volrec_set_verdict(&r, &pv), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(ntfsmac_volrec_set_verdict(&r, &pv), NTFSMAC_DMN_OK); /* replaces, no leak */
	ntfsmac_volrec_reset(&r);
	CHECK(ntfsmac_volrec_verdict(&r) == NULL);
	CHECK_EQ_INT(ntfsmac_volrec_set_verdict(&r, &pv), NTFSMAC_DMN_OK);
	ntfsmac_volrec_reset(&r);
}

int main(void)
{
	test_name_reuse();
	test_helpers();
	if (g_failures != 0) {
		fprintf(stderr, "volrec_test: %d failure(s)\n", g_failures);
		return 1;
	}
	printf("volrec_test: OK\n");
	return 0;
}
