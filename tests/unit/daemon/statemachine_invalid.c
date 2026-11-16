/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-DMN-SM-02: invalid transitions, bad arguments and the record staying
 * unchanged after a refusal (docs/ARCHITECTURE.md section 5: a `mount` request
 * for a volume not in Appeared or Failed is rejected).
 */
#include <string.h>

#include "check.h"
#include "statemachine_common.h"

#define S(x) NTFSMAC_VOL_##x
#define E(x) NTFSMAC_VEV_##x

static ntfsmac_volume make(ntfsmac_vol_state s)
{
	return volume_in(s, NTFSMAC_VMODE_RW, false);
}

/* Events that carry an argument get a valid one, so only the state is wrong. */
static ntfsmac_vol_result try_event(ntfsmac_volume *v, ntfsmac_vol_event e)
{
	ntfsmac_vol_event_arg a = { NTFSMAC_VMODE_RW, NTFSMAC_VREASON_NONE };

	return ntfsmac_volume_apply(v, e, &a);
}

/* Every event not on the allowed list is refused and leaves the record alone. */
static void test_refused_leaves_record(void)
{
	int s, e;

	for (s = 0; s < NTFSMAC_VOL_STATE_COUNT_; s++)
		for (e = 0; e < NTFSMAC_VEV_COUNT_; e++) {
			ntfsmac_volume v = make((ntfsmac_vol_state)s);
			ntfsmac_volume before = v;

			if (ntfsmac_vol_next_state(v.state, (ntfsmac_vol_event)e, v.disk_present) !=
			    NTFSMAC_VOL_STATE_COUNT_)
				continue;
			CHECK_EQ_INT(try_event(&v, (ntfsmac_vol_event)e), NTFSMAC_VRES_INVALID_TRANSITION);
			CHECK(memcmp(&v, &before, sizeof v) == 0);
		}
}

/* Named cases that matter most. */
static void test_named(void)
{
	ntfsmac_volume v;

	/* No mount while a mount is under way or done: section 5. */
	v = make(S(PROBING));
	CHECK(!ntfsmac_volume_accepts_mount(&v));
	CHECK_EQ_INT(try_event(&v, E(START)), NTFSMAC_VRES_INVALID_TRANSITION);
	v = make(S(MOUNTED));
	CHECK_EQ_INT(try_event(&v, E(START)), NTFSMAC_VRES_INVALID_TRANSITION);
	v = make(S(UNMOUNTING));
	CHECK(!ntfsmac_volume_accepts_mount(&v));
	CHECK_EQ_INT(try_event(&v, E(START)), NTFSMAC_VRES_INVALID_TRANSITION);

	/* Steps cannot be skipped: no mounting without a decision, no decision without a probe. */
	v = make(S(APPEARED));
	CHECK_EQ_INT(try_event(&v, E(DECIDED)), NTFSMAC_VRES_INVALID_TRANSITION);
	CHECK_EQ_INT(try_event(&v, E(MOUNTED)), NTFSMAC_VRES_INVALID_TRANSITION);
	v = make(S(PROBING));
	CHECK_EQ_INT(try_event(&v, E(DECIDED)), NTFSMAC_VRES_INVALID_TRANSITION);
	v = make(S(DECIDING));
	CHECK_EQ_INT(try_event(&v, E(MOUNTED)), NTFSMAC_VRES_INVALID_TRANSITION);

	/* Failed does not go straight to a mount, and a mount cannot be "unmounted" before it exists. */
	v = make(S(FAILED));
	CHECK_EQ_INT(try_event(&v, E(DECIDED)), NTFSMAC_VRES_INVALID_TRANSITION);
	CHECK_EQ_INT(try_event(&v, E(UNMOUNT)), NTFSMAC_VRES_INVALID_TRANSITION);
	v = make(S(MOUNTING));
	CHECK_EQ_INT(try_event(&v, E(UNMOUNT)), NTFSMAC_VRES_INVALID_TRANSITION);

	/* Gone is terminal: every event is refused, DISK_GONE included. */
	v = make(S(GONE));
	CHECK(!ntfsmac_volume_accepts_mount(&v));
	{
		int e;

		for (e = 0; e < NTFSMAC_VEV_COUNT_; e++)
			CHECK_EQ_INT(try_event(&v, (ntfsmac_vol_event)e), NTFSMAC_VRES_INVALID_TRANSITION);
	}

	/* A forced-unmount retry (BUSY) is for a present disk only. */
	v = volume_in(S(UNMOUNTING), NTFSMAC_VMODE_RW, true);
	CHECK_EQ_INT(try_event(&v, E(UNMOUNT_BUSY)), NTFSMAC_VRES_INVALID_TRANSITION);
}

static void test_bad_args(void)
{
	ntfsmac_volume v = make(S(DECIDING));
	ntfsmac_volume before = v;
	ntfsmac_vol_event_arg none_mode = { NTFSMAC_VMODE_NONE, NTFSMAC_VREASON_NONE };
	ntfsmac_vol_event_arg bad_mode = { (ntfsmac_vol_mode)99, NTFSMAC_VREASON_NONE };
	ntfsmac_vol_event_arg bad_reason = { NTFSMAC_VMODE_RW, (ntfsmac_vol_reason)99 };

	CHECK_EQ_INT(ntfsmac_volume_apply(NULL, E(START), NULL), NTFSMAC_VRES_BAD_ARG);
	CHECK_EQ_INT(ntfsmac_volume_apply(&v, (ntfsmac_vol_event)99, NULL), NTFSMAC_VRES_BAD_ARG);
	CHECK_EQ_INT(ntfsmac_volume_apply(&v, (ntfsmac_vol_event)-1, NULL), NTFSMAC_VRES_BAD_ARG);
	/* DECIDED needs a mode: none, NULL and out of range are refused. */
	CHECK_EQ_INT(ntfsmac_volume_apply(&v, E(DECIDED), NULL), NTFSMAC_VRES_BAD_ARG);
	CHECK_EQ_INT(ntfsmac_volume_apply(&v, E(DECIDED), &none_mode), NTFSMAC_VRES_BAD_ARG);
	CHECK_EQ_INT(ntfsmac_volume_apply(&v, E(DECIDED), &bad_mode), NTFSMAC_VRES_BAD_ARG);
	CHECK(memcmp(&v, &before, sizeof v) == 0);

	v = make(S(MOUNTING));
	before = v;
	CHECK_EQ_INT(ntfsmac_volume_apply(&v, E(MOUNTED), NULL), NTFSMAC_VRES_BAD_ARG);
	CHECK_EQ_INT(ntfsmac_volume_apply(&v, E(MOUNTED), &none_mode), NTFSMAC_VRES_BAD_ARG);
	CHECK_EQ_INT(ntfsmac_volume_apply(&v, E(MOUNT_FAILED), &bad_reason), NTFSMAC_VRES_BAD_ARG);
	CHECK(memcmp(&v, &before, sizeof v) == 0);

	/* A bad record state is refused too. */
	v = make(S(APPEARED));
	v.state = (ntfsmac_vol_state)42;
	CHECK_EQ_INT(ntfsmac_volume_apply(&v, E(START), NULL), NTFSMAC_VRES_BAD_ARG);
	CHECK(!ntfsmac_volume_accepts_mount(&v));
	CHECK(!ntfsmac_volume_accepts_mount(NULL));
	CHECK_EQ_INT(ntfsmac_vol_next_state((ntfsmac_vol_state)42, E(START), true), NTFSMAC_VOL_STATE_COUNT_);
	CHECK_EQ_INT(ntfsmac_vol_next_state(S(APPEARED), (ntfsmac_vol_event)42, true), NTFSMAC_VOL_STATE_COUNT_);
}

/* ro decision, writable mount: refused with its own result, record unchanged (step 8). */
static void test_mode_mismatch(void)
{
	ntfsmac_volume v = volume_in(S(MOUNTING), NTFSMAC_VMODE_RO, false);
	ntfsmac_volume before = v;
	ntfsmac_vol_event_arg rw = { NTFSMAC_VMODE_RW, NTFSMAC_VREASON_NONE };

	CHECK_EQ_INT(ntfsmac_volume_apply(&v, E(MOUNTED), &rw), NTFSMAC_VRES_MODE_MISMATCH);
	CHECK(memcmp(&v, &before, sizeof v) == 0);
	/* The caller reports it as a failed mount instead. */
	rw.reason = NTFSMAC_VREASON_MODE_MISMATCH;
	CHECK_EQ_INT(ntfsmac_volume_apply(&v, E(MOUNT_FAILED), &rw), NTFSMAC_VRES_OK);
	CHECK_EQ_INT(v.state, S(FAILED));
	CHECK_EQ_INT(v.reason, NTFSMAC_VREASON_MODE_MISMATCH);
}

static void test_names(void)
{
	int i;

	for (i = 0; i < NTFSMAC_VOL_STATE_COUNT_; i++)
		CHECK(strcmp(ntfsmac_vol_state_name((ntfsmac_vol_state)i), "?") != 0);
	for (i = 0; i < NTFSMAC_VEV_COUNT_; i++)
		CHECK(strcmp(ntfsmac_vol_event_name((ntfsmac_vol_event)i), "?") != 0);
	CHECK(strcmp(ntfsmac_vol_state_name((ntfsmac_vol_state)99), "?") == 0);
	CHECK(strcmp(ntfsmac_vol_result_name(NTFSMAC_VRES_INVALID_TRANSITION), "?") != 0);
}

int main(void)
{
	test_refused_leaves_record();
	test_named();
	test_bad_args();
	test_mode_mismatch();
	test_names();
	if (g_failures != 0) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	return 0;
}
