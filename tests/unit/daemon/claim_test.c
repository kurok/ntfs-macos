/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-DMN-CLM-01..09 (T-M7-11): the attach decision about another driver's mount (claim.c) with a
 * fake Disk Arbitration layer. The fake keeps the dissent table of da.c (a BSD name with the flag
 * "a mount was refused"), whether some driver has the disk mounted, and counts the unmounts and
 * the mounts it was asked for. Each case runs the steps of attach_run() in main.c: the dissent of
 * disk_appeared, ntfsmac_claim_take() before the probe, the state the attach ends in, and
 * ntfsmac_claim_finish() at its end.
 */
#include <string.h>

#include "check.h"
#include "da_filter.h"
#include "internal.h"

#define BSD "disk4s1"

typedef struct fake_da {
	ntfsmac_da_table dissent; /* flag: a mount was refused (on_approval) */
	bool mounted;             /* DAVolumePath present */
	bool unmount_busy;        /* DADiskUnmount fails */
	unsigned unmounts, mounts;
} fake_da;

static ntfsmac_dmn_status f_unmount(void *ctx, const char *bsd)
{
	fake_da *f = ctx;

	(void)bsd;
	f->unmounts++;
	if (!f->mounted)
		return NTFSMAC_DMN_E_STATE;
	if (f->unmount_busy)
		return NTFSMAC_DMN_E_BUSY;
	f->mounted = false;
	return NTFSMAC_DMN_OK;
}

/* ntfsmac_da_release(): drop the name; mount when a mount was refused or when asked to. */
static ntfsmac_dmn_status f_release(void *ctx, const char *bsd, bool mount_again)
{
	fake_da *f = ctx;
	bool refused = false;

	if (!ntfsmac_da_table_remove(&f->dissent, bsd, &refused))
		refused = false;
	if (refused || mount_again) {
		f->mounts++;
		f->mounted = true;
	}
	return NTFSMAC_DMN_OK;
}

static void fake_init(fake_da *f, ntfsmac_claim_ops *ops, bool mounted, bool hot_plug)
{
	memset(f, 0, sizeof *f);
	ntfsmac_da_table_init(&f->dissent);
	f->mounted = mounted;
	/* disk_appeared: the dissent is given from inside the callback */
	CHECK(ntfsmac_da_table_add(&f->dissent, BSD));
	/* a hot plug: the system's mount came after the dissent and was refused */
	if (hot_plug)
		CHECK(ntfsmac_da_table_set_flag(&f->dissent, BSD, true));
	ops->ctx = f;
	ops->unmount = f_unmount;
	ops->release = f_release;
}

/* U-DMN-CLM-01: the kind of Disk Arbitration, classified. */
static void test_classify(void)
{
	CHECK_EQ_INT(ntfsmac_kind_classify(NULL), NTFSMAC_KIND_UNKNOWN);
	CHECK_EQ_INT(ntfsmac_kind_classify(""), NTFSMAC_KIND_UNKNOWN);
	CHECK_EQ_INT(ntfsmac_kind_classify("ntfs"), NTFSMAC_KIND_NTFS);
	CHECK_EQ_INT(ntfsmac_kind_classify("ntfs3g"), NTFSMAC_KIND_OTHER_NTFS);
	CHECK_EQ_INT(ntfsmac_kind_classify("ufsd_NTFS"), NTFSMAC_KIND_OTHER_NTFS);
	CHECK_EQ_INT(ntfsmac_kind_classify("tuxera_ntfs"), NTFSMAC_KIND_OTHER_NTFS);
	CHECK_EQ_INT(ntfsmac_kind_classify("exfat"), NTFSMAC_KIND_NOT_NTFS);
	CHECK_EQ_INT(ntfsmac_kind_classify("msdos"), NTFSMAC_KIND_NOT_NTFS);
	CHECK_EQ_INT(ntfsmac_kind_classify("apfs"), NTFSMAC_KIND_NOT_NTFS);
	CHECK_EQ_INT(ntfsmac_kind_classify("hfs"), NTFSMAC_KIND_NOT_NTFS);
	CHECK_EQ_INT(ntfsmac_kind_classify("nfts"), NTFSMAC_KIND_NOT_NTFS);
	/* review: UNKNOWN is only "no kind"; a kind that could not be kept is left alone */
	CHECK_EQ_INT(ntfsmac_kind_classify(NTFSMAC_DA_KIND_UNREADABLE), NTFSMAC_KIND_OTHER);
	CHECK_EQ_INT(ntfsmac_kind_classify("ufsd_ntfs"), NTFSMAC_KIND_OTHER_NTFS);
	CHECK_EQ_INT(ntfsmac_kind_classify("com.paragon-software.filesystems.ntfs"), NTFSMAC_KIND_OTHER_NTFS);
	CHECK_EQ_INT(ntfsmac_kind_classify("com.apple.fskit.exfat"), NTFSMAC_KIND_NOT_NTFS);
}

/* U-DMN-CLM-07: Disk Arbitration's kind, as da.c keeps it, then classified. */
static void test_kind_from_da(void)
{
	static const struct {
		const char *raw;
		bool present;
		const char *kept;
		ntfsmac_kind_class cls;
	} c[] = {
		{ NULL, false, "", NTFSMAC_KIND_UNKNOWN },
		{ "", true, "", NTFSMAC_KIND_UNKNOWN },
		{ NULL, true, "?", NTFSMAC_KIND_OTHER },      /* not a string */
		{ "ntfs", true, "ntfs", NTFSMAC_KIND_NTFS },  /* Apple (U18) */
		{ "ufsd_NTFS", true, "ufsd_ntfs", NTFSMAC_KIND_OTHER_NTFS }, /* Paragon: never unmounted */
		{ "com.apple.fskit.exfat", true, "com.apple.fskit.exfat", NTFSMAC_KIND_NOT_NTFS },
		{ "com.example.fskit.NTFS", true, "com.example.fskit.ntfs", NTFSMAC_KIND_OTHER_NTFS },
		{ "a-kind-name-that-is-far-longer-than-the-sixty-four-bytes-we-keep-for-it", true, "?",
		  NTFSMAC_KIND_OTHER },
		{ "ex fat", true, "?", NTFSMAC_KIND_OTHER },
		{ "MSDOS", true, "msdos", NTFSMAC_KIND_NOT_NTFS },
	};
	size_t i;

	for (i = 0; i < sizeof c / sizeof c[0]; i++) {
		char out[NTFSMAC_DISK_KIND_SIZE];
		fake_da f;
		ntfsmac_claim_ops ops;
		bool took = true;

		ntfsmac_da_kind_normalize(c[i].raw, c[i].present, out, sizeof out);
		CHECK(strcmp(out, c[i].kept) == 0);
		CHECK_EQ_INT(ntfsmac_kind_classify(out), c[i].cls);
		/* mounted with that kind: only Apple's kind and "no kind" are unmounted */
		fake_init(&f, &ops, true, false);
		(void)ntfsmac_claim_take(&ops, BSD, true, out, &took);
		CHECK_EQ_INT(f.unmounts, (c[i].cls == NTFSMAC_KIND_NTFS || c[i].cls == NTFSMAC_KIND_UNKNOWN) ? 1 : 0);
	}
	/* the record keeps a long kind as unreadable, never as "none" */
	{
		ntfsmac_volrec r;
		char longk[200];

		memset(&r, 0, sizeof r);
		memset(longk, 'x', sizeof longk - 1);
		longk[sizeof longk - 1] = '\0';
		ntfsmac_volrec_set_kind(&r, longk);
		CHECK(strcmp(r.kind, NTFSMAC_DA_KIND_UNREADABLE) == 0);
		ntfsmac_volrec_set_kind(&r, "exfat");
		CHECK(strcmp(r.kind, "exfat") == 0);
		ntfsmac_volrec_reset(&r);
		CHECK(r.kind[0] == '\0');
	}
}

/*
 * U-DMN-CLM-08: a failed spawn or mode mismatch whose clean-up did not prove the device free (a
 * mount of ours or a holder is left): the dissent stays and nothing is mounted by the system,
 * whatever was taken before. Proven free: released, and the removed mount asked for again.
 */
static void test_device_not_free(void)
{
	fake_da f;
	ntfsmac_claim_ops ops;
	bool took = false;

	fake_init(&f, &ops, true, false);
	CHECK_EQ_INT(ntfsmac_claim_take(&ops, BSD, true, "ntfs", &took), NTFSMAC_CLAIM_FREE);
	CHECK(took);
	CHECK(!ntfsmac_claim_finish(&ops, BSD, NTFSMAC_VOL_FAILED, took, false));
	CHECK(ntfsmac_da_table_contains(&f.dissent, BSD));
	CHECK_EQ_INT(f.mounts, 0);
	/* a hot plug (a refused system mount), not free: still no mount */
	fake_init(&f, &ops, false, true);
	CHECK(!ntfsmac_claim_finish(&ops, BSD, NTFSMAC_VOL_FAILED, false, false));
	CHECK_EQ_INT(f.mounts, 0);
	CHECK(ntfsmac_da_table_contains(&f.dissent, BSD));
	/* free: released and mounted again */
	CHECK(ntfsmac_claim_finish(&ops, BSD, NTFSMAC_VOL_FAILED, false, true));
	CHECK_EQ_INT(f.mounts, 1);
	CHECK(!ntfsmac_da_table_contains(&f.dissent, BSD));
}

/*
 * U-DMN-CLM-09: the device busy state is sticky. A clean-up that did not prove the device free
 * keeps it busy for every later attach of the same disk, also one that ends before a worker
 * starts (its own proof is "free"), so the dissent stays and the system is never asked to mount;
 * a later device check that returns OK with no mount left, or the disk going, clears it. A check
 * that knows no device (E_STATE) changes nothing.
 */
static void test_device_busy_sticky(void)
{
	fake_da f;
	ntfsmac_claim_ops ops;
	bool busy = false, took = false;

	fake_init(&f, &ops, true, false);
	CHECK_EQ_INT(ntfsmac_claim_take(&ops, BSD, true, "ntfs", &took), NTFSMAC_CLAIM_FREE);
	/* attach 1: the worker failed and a process still holds the device */
	ntfsmac_devbusy_note(&busy, NTFSMAC_DMN_E_BUSY, false);
	CHECK(busy);
	CHECK(!ntfsmac_claim_finish(&ops, BSD, NTFSMAC_VOL_FAILED, took,
				    ntfsmac_devbusy_free(busy, false)));
	/* attach 2 ends early (no console user, or ignored by state): its own proof says free */
	CHECK(!ntfsmac_devbusy_free(busy, true));
	CHECK(!ntfsmac_claim_finish(&ops, BSD, NTFSMAC_VOL_FAILED, false, ntfsmac_devbusy_free(busy, true)));
	CHECK(ntfsmac_da_table_contains(&f.dissent, BSD));
	CHECK_EQ_INT(f.mounts, 0);
	/* no device known: still busy */
	ntfsmac_devbusy_note(&busy, NTFSMAC_DMN_E_STATE, false);
	CHECK(busy);
	/* a check OK but our mount is still there: busy */
	ntfsmac_devbusy_note(&busy, NTFSMAC_DMN_OK, true);
	CHECK(busy);
	/* a later check returns OK, nothing mounted: free; the removed mount is asked for again */
	ntfsmac_devbusy_note(&busy, NTFSMAC_DMN_OK, false);
	CHECK(!busy);
	CHECK(ntfsmac_claim_finish(&ops, BSD, NTFSMAC_VOL_FAILED, took, ntfsmac_devbusy_free(busy, true)));
	CHECK_EQ_INT(f.mounts, 1);
	/* the disk going clears it too */
	ntfsmac_devbusy_note(&busy, NTFSMAC_DMN_E_IO, false);
	CHECK(busy);
	ntfsmac_devbusy_reset(&busy);
	CHECK(!busy && ntfsmac_devbusy_free(busy, true));
	ntfsmac_devbusy_note(NULL, NTFSMAC_DMN_E_IO, true); /* no crash */
	ntfsmac_devbusy_reset(NULL);
}

/* U-DMN-CLM-02: a mounted exFAT or FAT disk at daemon start is not unmounted, and is released. */
static void test_other_fs_at_start(void)
{
	static const char *const kinds[] = { "exfat", "msdos" };
	size_t i;

	for (i = 0; i < 2; i++) {
		fake_da f;
		ntfsmac_claim_ops ops;
		bool took = true;

		fake_init(&f, &ops, true, false);
		CHECK_EQ_INT(ntfsmac_claim_take(&ops, BSD, true, kinds[i], &took), NTFSMAC_CLAIM_NOT_NTFS);
		CHECK(!took);
		CHECK_EQ_INT(f.unmounts, 0);
		CHECK(f.mounted);
		/* attach_run: PROBE_FAILED, reason NOT_NTFS -> Failed */
		CHECK(ntfsmac_claim_finish(&ops, BSD, NTFSMAC_VOL_FAILED, took, true));
		CHECK(!ntfsmac_da_table_contains(&f.dissent, BSD));
		CHECK_EQ_INT(f.mounts, 0); /* nothing was taken, nothing to give back */
		CHECK(f.mounted);
	}
	/* a hot-plugged exFAT disk (the dissent refused its mount): not probed, mounted by the system */
	{
		fake_da f;
		ntfsmac_claim_ops ops;
		bool took = true;

		fake_init(&f, &ops, false, true);
		CHECK_EQ_INT(ntfsmac_claim_take(&ops, BSD, false, "exfat", &took), NTFSMAC_CLAIM_NOT_NTFS);
		CHECK(ntfsmac_claim_finish(&ops, BSD, NTFSMAC_VOL_FAILED, took, true));
		CHECK_EQ_INT(f.unmounts, 0);
		CHECK_EQ_INT(f.mounts, 1);
		CHECK(!ntfsmac_da_table_contains(&f.dissent, BSD));
	}
}

/* U-DMN-CLM-03: Apple's read-only NTFS mount is still taken over; the dissent stays. */
static void test_apple_ntfs_taken_over(void)
{
	static const char *const kinds[] = { "ntfs", "" }; /* "": kind unknown, as before */
	size_t i;

	for (i = 0; i < 2; i++) {
		fake_da f;
		ntfsmac_claim_ops ops;
		bool took = false;

		fake_init(&f, &ops, true, false);
		CHECK_EQ_INT(ntfsmac_claim_take(&ops, BSD, true, kinds[i], &took), NTFSMAC_CLAIM_FREE);
		CHECK(took);
		CHECK_EQ_INT(f.unmounts, 1);
		CHECK(!f.mounted);
		/* the volume is mounted by our worker */
		CHECK(!ntfsmac_claim_finish(&ops, BSD, NTFSMAC_VOL_MOUNTED, took, true));
		CHECK(ntfsmac_da_table_contains(&f.dissent, BSD));
		CHECK_EQ_INT(f.mounts, 0);
		CHECK(!ntfsmac_claim_finish(&ops, BSD, NTFSMAC_VOL_MOUNTING, took, true));
		CHECK(!ntfsmac_claim_finish(&ops, BSD, NTFSMAC_VOL_UNMOUNTING, took, true));
		CHECK(ntfsmac_da_table_contains(&f.dissent, BSD));
	}
	/* not mounted: no unmount at all */
	{
		fake_da f;
		ntfsmac_claim_ops ops;
		bool took = true;

		fake_init(&f, &ops, false, true);
		CHECK_EQ_INT(ntfsmac_claim_take(&ops, BSD, false, "ntfs", &took), NTFSMAC_CLAIM_FREE);
		CHECK(!took);
		CHECK_EQ_INT(f.unmounts, 0);
	}
	/* the mount was gone already (E_STATE): free, but nothing of ours to give back */
	{
		fake_da f;
		ntfsmac_claim_ops ops;
		bool took = true;

		fake_init(&f, &ops, false, false);
		CHECK_EQ_INT(ntfsmac_claim_take(&ops, BSD, true, "ntfs", &took), NTFSMAC_CLAIM_FREE);
		CHECK(!took);
		CHECK_EQ_INT(f.unmounts, 1);
	}
}

/* U-DMN-CLM-04: a failed unmount ends the attach with no dissent left behind. */
static void test_unmount_fails(void)
{
	fake_da f;
	ntfsmac_claim_ops ops;
	bool took = true;

	fake_init(&f, &ops, true, false);
	f.unmount_busy = true;
	CHECK_EQ_INT(ntfsmac_claim_take(&ops, BSD, true, "ntfs", &took), NTFSMAC_CLAIM_UNMOUNT_FAILED);
	CHECK(!took);
	CHECK(f.mounted);
	CHECK(ntfsmac_claim_finish(&ops, BSD, NTFSMAC_VOL_FAILED, took, true));
	CHECK(!ntfsmac_da_table_contains(&f.dissent, BSD));
	CHECK_EQ_INT(f.mounts, 0);
	/* the next system mount of the disk is no longer refused */
	CHECK(!ntfsmac_da_table_contains(&f.dissent, BSD));

	/* another NTFS driver's mount: left alone, released */
	fake_init(&f, &ops, true, false);
	CHECK_EQ_INT(ntfsmac_claim_take(&ops, BSD, true, "ntfs3g", &took), NTFSMAC_CLAIM_OTHER_DRIVER);
	CHECK_EQ_INT(f.unmounts, 0);
	CHECK(ntfsmac_claim_finish(&ops, BSD, NTFSMAC_VOL_FAILED, took, true));
	CHECK(!ntfsmac_da_table_contains(&f.dissent, BSD));
}

/* U-DMN-CLM-05: the daemon removed Apple's mount and then does not mount: the mount is asked for. */
static void test_give_back(void)
{
	static const ntfsmac_vol_state ends[] = { NTFSMAC_VOL_FAILED, NTFSMAC_VOL_APPEARED,
						  NTFSMAC_VOL_PROBING, NTFSMAC_VOL_DECIDING };
	size_t i;

	for (i = 0; i < sizeof ends / sizeof ends[0]; i++) {
		fake_da f;
		ntfsmac_claim_ops ops;
		bool took = false;

		fake_init(&f, &ops, true, false);
		CHECK_EQ_INT(ntfsmac_claim_take(&ops, BSD, true, "ntfs", &took), NTFSMAC_CLAIM_FREE);
		CHECK(took);
		/* the probe says corrupt or not NTFS, the policy refuses, a spawn fails, ... */
		CHECK(ntfsmac_claim_finish(&ops, BSD, ends[i], took, true));
		CHECK_EQ_INT(f.mounts, 1);
		CHECK(f.mounted);
		CHECK(!ntfsmac_da_table_contains(&f.dissent, BSD));
	}
}

/* U-DMN-CLM-06: the verdict "not NTFS", and bad arguments. */
static void test_verdict_and_args(void)
{
	static ntfsmac_probe_verdict pv;
	bool took = true;

	ntfsmac_probe_verdict_unknown(&pv); /* a probe that failed: unknown, not "not NTFS" */
	CHECK(!ntfsmac_verdict_not_ntfs(&pv));
	pv.reason_codes = NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_NOT_NTFS);
	pv.state = NTFSMAC_PROBE_STATE_UNSUPPORTED;
	CHECK(ntfsmac_verdict_not_ntfs(&pv));
	pv.bitlocker = true; /* BitLocker is NTFS inside: stays visible, may be retried */
	CHECK(!ntfsmac_verdict_not_ntfs(&pv));
	pv.bitlocker = false;
	pv.is_ntfs = true;
	CHECK(!ntfsmac_verdict_not_ntfs(&pv));
	CHECK(!ntfsmac_verdict_not_ntfs(NULL));

	CHECK_EQ_INT(ntfsmac_claim_take(NULL, BSD, true, "ntfs", &took), NTFSMAC_CLAIM_UNMOUNT_FAILED);
	CHECK(!took);
	CHECK_EQ_INT(ntfsmac_claim_take(NULL, BSD, true, "exfat", NULL), NTFSMAC_CLAIM_NOT_NTFS);
	CHECK(!ntfsmac_claim_finish(NULL, BSD, NTFSMAC_VOL_FAILED, false, true));
}

int main(void)
{
	test_classify();
	test_kind_from_da();
	test_device_not_free();
	test_device_busy_sticky();
	test_other_fs_at_start();
	test_apple_ntfs_taken_over();
	test_unmount_fails();
	test_give_back();
	test_verdict_and_args();
	if (g_failures != 0) {
		fprintf(stderr, "claim_test: %d failure(s)\n", g_failures);
		return 1;
	}
	printf("claim_test: OK\n");
	return 0;
}
