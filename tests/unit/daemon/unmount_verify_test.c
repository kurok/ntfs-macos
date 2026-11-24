/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-DMN-UMV-*: the verify-after rule for unmount answers (main.c unmount_run and its callers have
 * no seam of their own, so the rule is the pure helper ntfsmac_unmount_verify() and this test
 * replays the decision of unmount_run with the unmount hooks, a fake mount list and the real
 * volume state machine), and the 30 s bound of the Unmounting safety net.
 */
#include <errno.h>
#include <string.h>

#include "check.h"
#include "statemachine_common.h"
#include "unmount_impl.h"

typedef struct fake {
	int err;         /* errno of unmount(2); 0 = it returns 0 */
	bool mounted;    /* the fake mount table still lists the mount */
	bool drop_on_ok; /* a successful unmount(2) removes the mount */
	unsigned calls;
} fake;

static int f_unmount(void *ctx, const char *path, bool force)
{
	fake *f = ctx;

	(void)path;
	(void)force;
	f->calls++;
	if (f->err != 0) {
		errno = f->err;
		return -1;
	}
	if (f->drop_on_ok)
		f->mounted = false;
	return 0;
}

/* The decision of unmount_run: Mounted -> Unmounting, the call, the verified answer, UNMOUNT_BUSY. */
static ntfsmac_dmn_status replay(fake *f, ntfsmac_volume *v)
{
	ntfsmac_unmount_hooks h = { f_unmount, NULL, NULL, f };
	ntfsmac_unmount_opts o = { false, 0 };
	ntfsmac_vol_event_arg a = { NTFSMAC_VMODE_NONE, NTFSMAC_VREASON_NONE };
	ntfsmac_dmn_status st;

	must(v, NTFSMAC_VEV_UNMOUNT, NTFSMAC_VMODE_NONE);
	st = ntfsmac_unmount_verify(ntfsmac_unmount_run("/Volumes/X", &o, NULL, &h), f->mounted);
	if (st != NTFSMAC_DMN_OK)
		CHECK_EQ_INT(ntfsmac_volume_apply(v, NTFSMAC_VEV_UNMOUNT_BUSY, &a), NTFSMAC_VRES_OK);
	return st;
}

int main(void)
{
	fake f;
	ntfsmac_volume v;

	/* the helper */
	CHECK_EQ_INT(ntfsmac_unmount_verify(NTFSMAC_DMN_OK, false), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(ntfsmac_unmount_verify(NTFSMAC_DMN_OK, true), NTFSMAC_DMN_E_BUSY);
	CHECK_EQ_INT(ntfsmac_unmount_verify(NTFSMAC_DMN_E_STATE, false), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(ntfsmac_unmount_verify(NTFSMAC_DMN_E_STATE, true), NTFSMAC_DMN_E_BUSY);
	CHECK_EQ_INT(ntfsmac_unmount_verify(NTFSMAC_DMN_E_BUSY, true), NTFSMAC_DMN_E_BUSY);
	CHECK_EQ_INT(ntfsmac_unmount_verify(NTFSMAC_DMN_E_BUSY, false), NTFSMAC_DMN_E_BUSY);
	CHECK_EQ_INT(ntfsmac_unmount_verify(NTFSMAC_DMN_E_TIMEOUT, false), NTFSMAC_DMN_E_TIMEOUT);
	CHECK_EQ_INT(ntfsmac_unmount_verify(NTFSMAC_DMN_E_DENIED, true), NTFSMAC_DMN_E_DENIED);

	/* (a) EINVAL while the mount is still listed: back to Mounted; a later unmount works */
	memset(&f, 0, sizeof f);
	f.err = EINVAL;
	f.mounted = true;
	v = volume_in(NTFSMAC_VOL_MOUNTED, NTFSMAC_VMODE_RW, false);
	CHECK_EQ_INT(replay(&f, &v), NTFSMAC_DMN_E_BUSY);
	CHECK_EQ_INT(v.state, NTFSMAC_VOL_MOUNTED);
	f.err = 0;
	f.drop_on_ok = true;
	CHECK_EQ_INT(replay(&f, &v), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(v.state, NTFSMAC_VOL_UNMOUNTING);
	CHECK(!f.mounted);

	/* (b) unmount(2) returns 0 but the mount stays: the same */
	memset(&f, 0, sizeof f);
	f.mounted = true;
	v = volume_in(NTFSMAC_VOL_MOUNTED, NTFSMAC_VMODE_RW, false);
	CHECK_EQ_INT(replay(&f, &v), NTFSMAC_DMN_E_BUSY);
	CHECK_EQ_INT(v.state, NTFSMAC_VOL_MOUNTED);
	f.drop_on_ok = true;
	CHECK_EQ_INT(replay(&f, &v), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(v.state, NTFSMAC_VOL_UNMOUNTING);

	/* (c) EINVAL with the mount gone: Unmounting, as before */
	memset(&f, 0, sizeof f);
	f.err = EINVAL;
	v = volume_in(NTFSMAC_VOL_MOUNTED, NTFSMAC_VMODE_RW, false);
	CHECK_EQ_INT(replay(&f, &v), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(v.state, NTFSMAC_VOL_UNMOUNTING);

	/* (d) the safety net's bound */
	CHECK(!ntfsmac_unmount_stuck(0, 100000, 30000)); /* not tracked */
	CHECK(!ntfsmac_unmount_stuck(1000, 30999, 30000));
	CHECK(ntfsmac_unmount_stuck(1000, 31000, 30000));
	CHECK(!ntfsmac_unmount_stuck(5000, 1000, 30000)); /* a clock that went back */

	/* rule 3: the evidence takes the raw answer (main.c: answer == OK ? verified : answer) */
	{
		ntfsmac_unmount_ev e;
		ntfsmac_dmn_status raw;

		ntfsmac_uev_begin(&e, 7);
		raw = NTFSMAC_DMN_E_STATE; /* EINVAL, mount gone: verified OK, but the mount merely vanished */
		CHECK_EQ_INT(ntfsmac_unmount_verify(raw, false), NTFSMAC_DMN_OK);
		ntfsmac_uev_note_unmount_call(&e, raw == NTFSMAC_DMN_OK ? NTFSMAC_DMN_OK : raw, false);
		CHECK(!e.requested);
		raw = NTFSMAC_DMN_OK; /* a real OK with the mount gone */
		ntfsmac_uev_note_unmount_call(&e, ntfsmac_unmount_verify(raw, false), false);
		CHECK(e.requested);
		ntfsmac_uev_note_unmount_call(&e, ntfsmac_unmount_verify(raw, true), true); /* OK, mount stays */
		CHECK(!e.requested);
	}

	if (g_failures != 0)
		return 1;
	printf("unmount_verify_test: ok\n");
	return 0;
}
