/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-DMN-SM-01: one test per transition of the volume state machine
 * (docs/ARCHITECTURE.md section 5, events from sections 4.1 and 4.5), and an
 * exhaustive check that the table holds these transitions and no others.
 */
#include <string.h>

#include "check.h"
#include "statemachine_common.h"

#define RW NTFSMAC_VMODE_RW
#define RO NTFSMAC_VMODE_RO
#define NOMODE NTFSMAC_VMODE_NONE

static ntfsmac_vol_result apply(ntfsmac_volume *v, ntfsmac_vol_event e, ntfsmac_vol_mode m,
				ntfsmac_vol_reason r)
{
	ntfsmac_vol_event_arg a = { m, r };

	return ntfsmac_volume_apply(v, e, &a);
}

/* The complete list of allowed (state, event, disk present) -> state. */
struct tr {
	ntfsmac_vol_state from;
	ntfsmac_vol_event ev;
	bool disk_present;
	ntfsmac_vol_state to;
};

#define S(x) NTFSMAC_VOL_##x
#define E(x) NTFSMAC_VEV_##x
static const struct tr allowed[] = {
	{ S(APPEARED), E(START), true, S(PROBING) },
	{ S(FAILED), E(START), true, S(PROBING) },
	{ S(PROBING), E(PROBE_DONE), true, S(DECIDING) },
	{ S(PROBING), E(PROBE_FAILED), true, S(FAILED) },
	{ S(DECIDING), E(DECIDED), true, S(MOUNTING) },
	{ S(DECIDING), E(REFUSED), true, S(FAILED) },
	{ S(MOUNTING), E(MOUNTED), true, S(MOUNTED) },
	{ S(MOUNTING), E(MOUNT_FAILED), true, S(FAILED) },
	{ S(MOUNTED), E(UNMOUNT), true, S(UNMOUNTING) },
	{ S(UNMOUNTING), E(UNMOUNT_BUSY), true, S(MOUNTED) },
	{ S(UNMOUNTING), E(UNMOUNTED), true, S(APPEARED) },
	{ S(UNMOUNTING), E(UNMOUNTED), false, S(GONE) },
	{ S(UNMOUNTING), E(UNMOUNT_FAILED), true, S(FAILED) },
	{ S(UNMOUNTING), E(UNMOUNT_FAILED), false, S(GONE) },
	/* DISK_GONE leaves the disk present flag false, whatever it was. */
	{ S(APPEARED), E(DISK_GONE), true, S(GONE) },
	{ S(PROBING), E(DISK_GONE), true, S(GONE) },
	{ S(DECIDING), E(DISK_GONE), true, S(GONE) },
	{ S(FAILED), E(DISK_GONE), true, S(GONE) },
	{ S(MOUNTING), E(DISK_GONE), true, S(UNMOUNTING) },
	{ S(MOUNTED), E(DISK_GONE), true, S(UNMOUNTING) },
	{ S(UNMOUNTING), E(DISK_GONE), true, S(UNMOUNTING) },
	{ S(UNMOUNTING), E(DISK_GONE), false, S(UNMOUNTING) },
};

static bool in_allowed(ntfsmac_vol_state s, ntfsmac_vol_event e, bool present, ntfsmac_vol_state *to)
{
	size_t i;

	for (i = 0; i < sizeof allowed / sizeof allowed[0]; i++)
		if (allowed[i].from == s && allowed[i].ev == e &&
		    allowed[i].disk_present == present) {
			*to = allowed[i].to;
			return true;
		}
	return false;
}

/* Every reachable pair: the table agrees with the list above, nothing more. */
static void test_exhaustive(void)
{
	int s, e, p;

	for (s = 0; s < NTFSMAC_VOL_STATE_COUNT_; s++)
		for (e = 0; e < NTFSMAC_VEV_COUNT_; e++)
			for (p = 0; p < 2; p++) {
				ntfsmac_vol_state want, got;

				/*
				 * Without the disk only Unmounting is reachable (every other
				 * state leaves through DISK_GONE); START must still be refused.
				 */
				if (p == 0 && s != S(UNMOUNTING) && e != E(START))
					continue;
				bool ok = in_allowed((ntfsmac_vol_state)s, (ntfsmac_vol_event)e, p != 0, &want);

				got = ntfsmac_vol_next_state((ntfsmac_vol_state)s, (ntfsmac_vol_event)e,
							     p != 0);
				if (ok != (got != NTFSMAC_VOL_STATE_COUNT_) || (ok && got != want)) {
					fprintf(stderr, "FAIL table: %s + %s (disk %d) gave %s\n",
						ntfsmac_vol_state_name((ntfsmac_vol_state)s),
						ntfsmac_vol_event_name((ntfsmac_vol_event)e), p,
						ntfsmac_vol_state_name(got));
					g_failures++;
				}
			}
}

static void test_init(void)
{
	ntfsmac_volume v;

	ntfsmac_volume_init(&v);
	CHECK_EQ_INT(v.state, NTFSMAC_VOL_APPEARED);
	CHECK_EQ_INT(v.mode, NOMODE);
	CHECK_EQ_INT(v.planned_mode, NOMODE);
	CHECK_EQ_INT(v.reason, NTFSMAC_VREASON_NONE);
	CHECK(v.disk_present);
	CHECK(ntfsmac_volume_accepts_mount(&v));
}

static void t_appeared_start(void)
{
	ntfsmac_volume v = volume_in(S(APPEARED), RW, false);

	CHECK_EQ_INT(apply(&v, E(START), NOMODE, 0), NTFSMAC_VRES_OK);
	CHECK_EQ_INT(v.state, S(PROBING));
}

static void t_failed_start(void)
{
	ntfsmac_volume v = volume_in(S(FAILED), RW, false);

	CHECK(ntfsmac_volume_accepts_mount(&v));
	CHECK_EQ_INT(apply(&v, E(START), NOMODE, 0), NTFSMAC_VRES_OK);
	CHECK_EQ_INT(v.state, S(PROBING));
	CHECK_EQ_INT(v.reason, NTFSMAC_VREASON_NONE); /* the old reason is cleared */
}

static void t_probing_done(void)
{
	ntfsmac_volume v = volume_in(S(PROBING), RW, false);

	CHECK_EQ_INT(apply(&v, E(PROBE_DONE), NOMODE, 0), NTFSMAC_VRES_OK);
	CHECK_EQ_INT(v.state, S(DECIDING));
}

static void t_probing_failed(void)
{
	ntfsmac_volume v = volume_in(S(PROBING), RW, false);

	CHECK_EQ_INT(apply(&v, E(PROBE_FAILED), NOMODE, 0), NTFSMAC_VRES_OK);
	CHECK_EQ_INT(v.state, S(FAILED));
	CHECK_EQ_INT(v.reason, NTFSMAC_VREASON_PROBE_FAILED);
}

static void t_deciding_decided_rw(void)
{
	ntfsmac_volume v = volume_in(S(DECIDING), RW, false);

	CHECK_EQ_INT(apply(&v, E(DECIDED), RW, 0), NTFSMAC_VRES_OK);
	CHECK_EQ_INT(v.state, S(MOUNTING));
	CHECK_EQ_INT(v.planned_mode, RW);
	CHECK_EQ_INT(v.mode, NOMODE);
}

static void t_deciding_decided_ro(void)
{
	ntfsmac_volume v = volume_in(S(DECIDING), RW, false);

	CHECK_EQ_INT(apply(&v, E(DECIDED), RO, 0), NTFSMAC_VRES_OK);
	CHECK_EQ_INT(v.state, S(MOUNTING));
	CHECK_EQ_INT(v.planned_mode, RO);
}

static void t_deciding_refused(void)
{
	ntfsmac_volume v = volume_in(S(DECIDING), RW, false);

	CHECK_EQ_INT(apply(&v, E(REFUSED), NOMODE, 0), NTFSMAC_VRES_OK);
	CHECK_EQ_INT(v.state, S(FAILED));
	CHECK_EQ_INT(v.reason, NTFSMAC_VREASON_POLICY_REFUSED);
}

static void t_mounting_mounted_same_mode(void)
{
	ntfsmac_volume v = volume_in(S(MOUNTING), RW, false);

	CHECK_EQ_INT(apply(&v, E(MOUNTED), RW, 0), NTFSMAC_VRES_OK);
	CHECK_EQ_INT(v.state, S(MOUNTED));
	CHECK_EQ_INT(v.mode, RW);
	v = volume_in(S(MOUNTING), RO, false);
	CHECK_EQ_INT(apply(&v, E(MOUNTED), RO, 0), NTFSMAC_VRES_OK);
	CHECK_EQ_INT(v.mode, RO);
}

/* Section 4.1 step 8: a rw decision with a read-only mount is recorded as ro. */
static void t_mounting_mounted_rw_decision_ro_mount(void)
{
	ntfsmac_volume v = volume_in(S(MOUNTING), RW, false);

	CHECK_EQ_INT(apply(&v, E(MOUNTED), RO, 0), NTFSMAC_VRES_OK);
	CHECK_EQ_INT(v.state, S(MOUNTED));
	CHECK_EQ_INT(v.mode, RO);
	CHECK_EQ_INT(v.planned_mode, RW);
}

static void t_mounting_failed(void)
{
	ntfsmac_volume v = volume_in(S(MOUNTING), RW, false);

	CHECK_EQ_INT(apply(&v, E(MOUNT_FAILED), NOMODE, NTFSMAC_VREASON_MOUNT_TIMEOUT), NTFSMAC_VRES_OK);
	CHECK_EQ_INT(v.state, S(FAILED));
	CHECK_EQ_INT(v.reason, NTFSMAC_VREASON_MOUNT_TIMEOUT);
	CHECK_EQ_INT(v.planned_mode, NOMODE);
	v = volume_in(S(MOUNTING), RW, false);
	CHECK_EQ_INT(apply(&v, E(MOUNT_FAILED), NOMODE, 0), NTFSMAC_VRES_OK);
	CHECK_EQ_INT(v.reason, NTFSMAC_VREASON_SPAWN); /* default reason */
}

static void t_mounted_unmount(void)
{
	ntfsmac_volume v = volume_in(S(MOUNTED), RW, false);

	CHECK(!ntfsmac_volume_accepts_mount(&v)); /* section 5: mount is rejected here */
	CHECK_EQ_INT(apply(&v, E(UNMOUNT), NOMODE, 0), NTFSMAC_VRES_OK);
	CHECK_EQ_INT(v.state, S(UNMOUNTING));
	CHECK_EQ_INT(v.mode, RW); /* still mounted until the cleanup ends */
}

static void t_unmounting_busy(void)
{
	ntfsmac_volume v = volume_in(S(UNMOUNTING), RW, false);

	CHECK_EQ_INT(apply(&v, E(UNMOUNT_BUSY), NOMODE, 0), NTFSMAC_VRES_OK);
	CHECK_EQ_INT(v.state, S(MOUNTED));
	CHECK_EQ_INT(v.mode, RW);
}

static void t_unmounting_unmounted_disk_present(void)
{
	ntfsmac_volume v = volume_in(S(UNMOUNTING), RW, false);

	CHECK_EQ_INT(apply(&v, E(UNMOUNTED), NOMODE, 0), NTFSMAC_VRES_OK);
	CHECK_EQ_INT(v.state, S(APPEARED));
	CHECK_EQ_INT(v.mode, NOMODE);
	CHECK(ntfsmac_volume_accepts_mount(&v));
}

static void t_unmounting_unmounted_disk_gone(void)
{
	ntfsmac_volume v = volume_in(S(UNMOUNTING), RW, true);

	CHECK_EQ_INT(apply(&v, E(UNMOUNTED), NOMODE, 0), NTFSMAC_VRES_OK);
	CHECK_EQ_INT(v.state, S(GONE));
	CHECK_EQ_INT(v.mode, NOMODE);
}

static void t_unmounting_failed_disk_present(void)
{
	ntfsmac_volume v = volume_in(S(UNMOUNTING), RW, false);

	CHECK_EQ_INT(apply(&v, E(UNMOUNT_FAILED), NOMODE, 0), NTFSMAC_VRES_OK);
	CHECK_EQ_INT(v.state, S(FAILED));
	CHECK_EQ_INT(v.reason, NTFSMAC_VREASON_UNMOUNT);
}

static void t_unmounting_failed_disk_gone(void)
{
	ntfsmac_volume v = volume_in(S(UNMOUNTING), RW, true);

	CHECK_EQ_INT(apply(&v, E(UNMOUNT_FAILED), NOMODE, 0), NTFSMAC_VRES_OK);
	CHECK_EQ_INT(v.state, S(GONE));
	CHECK_EQ_INT(v.reason, NTFSMAC_VREASON_NONE);
}

static void t_disk_gone_direct(void)
{
	static const ntfsmac_vol_state from[] = { S(APPEARED), S(PROBING), S(DECIDING), S(FAILED) };
	size_t i;

	for (i = 0; i < sizeof from / sizeof from[0]; i++) {
		ntfsmac_volume v = volume_in(from[i], RW, false);

		CHECK_EQ_INT(apply(&v, E(DISK_GONE), NOMODE, 0), NTFSMAC_VRES_OK);
		CHECK_EQ_INT(v.state, S(GONE));
		CHECK(!v.disk_present);
		CHECK(!ntfsmac_volume_accepts_mount(&v));
	}
}

static void t_disk_gone_needs_cleanup(void)
{
	ntfsmac_volume v = volume_in(S(MOUNTING), RW, false);

	CHECK_EQ_INT(apply(&v, E(DISK_GONE), NOMODE, 0), NTFSMAC_VRES_OK);
	CHECK_EQ_INT(v.state, S(UNMOUNTING));
	CHECK(!v.disk_present);
	v = volume_in(S(MOUNTED), RW, false);
	CHECK_EQ_INT(apply(&v, E(DISK_GONE), NOMODE, 0), NTFSMAC_VRES_OK);
	CHECK_EQ_INT(v.state, S(UNMOUNTING));
	CHECK(!v.disk_present);
}

static void t_disk_gone_during_unmount(void)
{
	ntfsmac_volume v = volume_in(S(UNMOUNTING), RW, false);

	CHECK_EQ_INT(apply(&v, E(DISK_GONE), NOMODE, 0), NTFSMAC_VRES_OK);
	CHECK_EQ_INT(v.state, S(UNMOUNTING));
	CHECK(!v.disk_present);
	/* Then the cleanup ends in Gone, and a retry cannot leave Unmounting. */
	CHECK_EQ_INT(apply(&v, E(UNMOUNT_BUSY), NOMODE, 0), NTFSMAC_VRES_INVALID_TRANSITION);
	CHECK_EQ_INT(apply(&v, E(UNMOUNTED), NOMODE, 0), NTFSMAC_VRES_OK);
	CHECK_EQ_INT(v.state, S(GONE));
}

/* The path of section 4.1 followed by section 4.5, end to end. */
static void t_full_lifecycle(void)
{
	ntfsmac_volume v;

	ntfsmac_volume_init(&v);
	must(&v, E(START), NOMODE);
	must(&v, E(PROBE_DONE), NOMODE);
	must(&v, E(DECIDED), RW);
	must(&v, E(MOUNTED), RW);
	must(&v, E(UNMOUNT), NOMODE);
	must(&v, E(UNMOUNTED), NOMODE);
	CHECK_EQ_INT(v.state, S(APPEARED));
	must(&v, E(START), NOMODE); /* a second attach of the same disk */
	must(&v, E(DISK_GONE), NOMODE);
	CHECK_EQ_INT(v.state, S(GONE));
}

/*
 * T-M7-11: "not NTFS" (the probe's verdict, or Disk Arbitration's volume kind) is final for this
 * appearance of the disk: `mount` is refused with its own reason and `list` leaves it out. Every
 * other Failed still takes a retry. A new appearance starts over.
 */
static void t_not_ntfs_is_final(void)
{
	static const ntfsmac_vol_reason retry[] = { NTFSMAC_VREASON_PROBE_FAILED,
						    NTFSMAC_VREASON_POLICY_REFUSED,
						    NTFSMAC_VREASON_SPAWN, NTFSMAC_VREASON_MOUNT_TIMEOUT };
	ntfsmac_volume v;
	size_t i;

	/* the probe said not NTFS */
	ntfsmac_volume_init(&v);
	CHECK_EQ_INT(apply(&v, E(START), NOMODE, 0), NTFSMAC_VRES_OK);
	CHECK_EQ_INT(apply(&v, E(PROBE_DONE), NOMODE, 0), NTFSMAC_VRES_OK);
	CHECK_EQ_INT(apply(&v, E(REFUSED), NOMODE, NTFSMAC_VREASON_NOT_NTFS), NTFSMAC_VRES_OK);
	CHECK_EQ_INT(v.state, S(FAILED));
	CHECK(!ntfsmac_volume_accepts_mount(&v));
	CHECK(strcmp(ntfsmac_volume_mount_refusal(&v), "not_ntfs") == 0);
	CHECK(!ntfsmac_volume_listed(&v));
	/* Disk Arbitration's kind said it, before any probe */
	ntfsmac_volume_init(&v);
	CHECK_EQ_INT(apply(&v, E(START), NOMODE, 0), NTFSMAC_VRES_OK);
	CHECK_EQ_INT(apply(&v, E(PROBE_FAILED), NOMODE, NTFSMAC_VREASON_NOT_NTFS), NTFSMAC_VRES_OK);
	CHECK(strcmp(ntfsmac_volume_mount_refusal(&v), "not_ntfs") == 0);
	CHECK(!ntfsmac_volume_listed(&v));
	/* the disk goes and appears again: a new disk, a candidate */
	CHECK_EQ_INT(apply(&v, E(DISK_GONE), NOMODE, 0), NTFSMAC_VRES_OK);
	CHECK(!ntfsmac_volume_listed(&v));
	CHECK(strcmp(ntfsmac_volume_mount_refusal(&v), "wrong_state") == 0);
	ntfsmac_volume_init(&v);
	CHECK(ntfsmac_volume_mount_refusal(&v) == NULL);
	CHECK(ntfsmac_volume_listed(&v));
	/* a probe or spawn failure, a refusal by the policy: retry allowed, listed */
	for (i = 0; i < sizeof retry / sizeof retry[0]; i++) {
		v = volume_in(S(FAILED), NOMODE, false);
		v.reason = retry[i];
		CHECK(ntfsmac_volume_accepts_mount(&v));
		CHECK(ntfsmac_volume_mount_refusal(&v) == NULL);
		CHECK(ntfsmac_volume_listed(&v));
	}
	/* our own volume: wrong state, listed */
	v = volume_in(S(MOUNTED), RW, false);
	CHECK(strcmp(ntfsmac_volume_mount_refusal(&v), "wrong_state") == 0);
	CHECK(ntfsmac_volume_listed(&v));
	CHECK(strcmp(ntfsmac_volume_mount_refusal(NULL), "wrong_state") == 0);
	CHECK(!ntfsmac_volume_listed(NULL));
}

int main(void)
{
	test_exhaustive();
	t_not_ntfs_is_final();
	test_init();
	t_appeared_start();
	t_failed_start();
	t_probing_done();
	t_probing_failed();
	t_deciding_decided_rw();
	t_deciding_decided_ro();
	t_deciding_refused();
	t_mounting_mounted_same_mode();
	t_mounting_mounted_rw_decision_ro_mount();
	t_mounting_failed();
	t_mounted_unmount();
	t_unmounting_busy();
	t_unmounting_unmounted_disk_present();
	t_unmounting_unmounted_disk_gone();
	t_unmounting_failed_disk_present();
	t_unmounting_failed_disk_gone();
	t_disk_gone_direct();
	t_disk_gone_needs_cleanup();
	t_disk_gone_during_unmount();
	t_full_lifecycle();
	if (g_failures != 0) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	return 0;
}
