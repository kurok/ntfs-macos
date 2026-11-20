/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-DMN-DA-01: NTFS filter of ARCHITECTURE.md section 4.1 step 1.
 * U-DMN-DA-02: BSD name check.
 * U-DMN-DA-03: dissent and known-disk table.
 * U-DMN-DA-04: argument checks of the public interface that need no
 *              diskarbitrationd.
 */
#include "check.h"
#include "da_filter.h"
#include "internal.h"

#include <string.h>

static void test_filter(void)
{
	/* U-DMN-DA-01 */
	CHECK(ntfsmac_da_content_is_candidate("Windows_NTFS", false));
	CHECK(ntfsmac_da_content_is_candidate("windows_ntfs", false));
	CHECK(ntfsmac_da_content_is_candidate("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7", false));
	CHECK(ntfsmac_da_content_is_candidate("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7", false));
	CHECK(ntfsmac_da_content_is_candidate("Windows_NTFS", true));
	/* Raw file system on a whole disk: no content. */
	CHECK(ntfsmac_da_content_is_candidate(NULL, true));
	CHECK(ntfsmac_da_content_is_candidate("", true));
	/* A partition with no content is not a candidate. */
	CHECK(!ntfsmac_da_content_is_candidate(NULL, false));
	CHECK(!ntfsmac_da_content_is_candidate("", false));
	/* Partition maps and other types. */
	CHECK(!ntfsmac_da_content_is_candidate("GUID_partition_scheme", true));
	CHECK(!ntfsmac_da_content_is_candidate("FDisk_partition_scheme", true));
	CHECK(!ntfsmac_da_content_is_candidate("Apple_APFS", false));
	CHECK(!ntfsmac_da_content_is_candidate("DOS_FAT_32", false));
	CHECK(!ntfsmac_da_content_is_candidate("Microsoft Basic Data", false));
	CHECK(!ntfsmac_da_content_is_candidate("Windows_NTFS ", false));
	CHECK(!ntfsmac_da_content_is_candidate("Windows_NTF", false));
	CHECK(!ntfsmac_da_content_is_candidate("EBD0A0A2-B9E5-4433-87C0-68B6B72699C", false));
	CHECK(!ntfsmac_da_content_is_candidate("EBD0A0A2-B9E5-4433-87C0-68B6B72699C78", false));
}

static void test_macfuse_backing(void)
{
	const uint64_t mib8 = UINT64_C(8) * 1024 * 1024;

	/* U-DMN-DA-05: macFUSE's own 8 MiB virtual disk is recognised, nothing else is */
	CHECK(ntfsmac_da_is_macfuse_backing(mib8, true, "", "Disk Image", "Virtual Interface"));
	CHECK(ntfsmac_da_is_macfuse_backing(mib8, true, NULL, "Disk Image", "Virtual Interface"));
	CHECK(!ntfsmac_da_is_macfuse_backing(mib8 + 512, true, "", "Disk Image", "Virtual Interface"));
	CHECK(!ntfsmac_da_is_macfuse_backing(mib8 - 512, true, "", "Disk Image", "Virtual Interface"));
	CHECK(!ntfsmac_da_is_macfuse_backing(mib8, false, "", "Disk Image", "Virtual Interface"));
	CHECK(!ntfsmac_da_is_macfuse_backing(mib8, true, "GUID_partition_scheme", "Disk Image",
					     "Virtual Interface"));
	CHECK(!ntfsmac_da_is_macfuse_backing(mib8, true, "", "SanDisk Ultra", "USB"));
	CHECK(!ntfsmac_da_is_macfuse_backing(mib8, true, "", "Disk Image", "USB"));
	CHECK(!ntfsmac_da_is_macfuse_backing(mib8, true, "", NULL, "Virtual Interface"));
	CHECK(!ntfsmac_da_is_macfuse_backing(mib8, true, "", "Disk Image", NULL));
	CHECK(!ntfsmac_da_is_macfuse_backing(0, true, "", "Disk Image", "Virtual Interface"));
}

static void test_names(void)
{
	/* U-DMN-DA-02 */
	CHECK(ntfsmac_da_bsd_name_valid("disk0"));
	CHECK(ntfsmac_da_bsd_name_valid("disk12"));
	CHECK(ntfsmac_da_bsd_name_valid("disk4s1"));
	CHECK(ntfsmac_da_bsd_name_valid("disk123456s123456"));
	CHECK(!ntfsmac_da_bsd_name_valid(NULL));
	CHECK(!ntfsmac_da_bsd_name_valid(""));
	CHECK(!ntfsmac_da_bsd_name_valid("disk"));
	CHECK(!ntfsmac_da_bsd_name_valid("disks1"));
	CHECK(!ntfsmac_da_bsd_name_valid("disk4s"));
	CHECK(!ntfsmac_da_bsd_name_valid("disk4s1s2"));
	CHECK(!ntfsmac_da_bsd_name_valid("disk4s1 "));
	CHECK(!ntfsmac_da_bsd_name_valid("/dev/disk4"));
	CHECK(!ntfsmac_da_bsd_name_valid("rdisk4"));
	CHECK(!ntfsmac_da_bsd_name_valid("disk4x"));
	CHECK(!ntfsmac_da_bsd_name_valid("disk-1"));
	CHECK(!ntfsmac_da_bsd_name_valid("disk1234567"));
	CHECK(!ntfsmac_da_bsd_name_valid("disk4s1234567"));
	CHECK(!ntfsmac_da_bsd_name_valid("disk4\n"));
	CHECK(!ntfsmac_da_bsd_name_valid("../disk4"));
}

static void test_table(void)
{
	/* U-DMN-DA-03 */
	static ntfsmac_da_table t;
	bool flag = true;
	char name[32];
	unsigned i;

	ntfsmac_da_table_init(&t);
	CHECK(!ntfsmac_da_table_contains(&t, "disk4"));
	CHECK(!ntfsmac_da_table_add(&t, "bad name"));
	CHECK(!ntfsmac_da_table_add(&t, NULL));
	CHECK(ntfsmac_da_table_add(&t, "disk4"));
	CHECK(ntfsmac_da_table_add(&t, "disk4s1"));
	CHECK(ntfsmac_da_table_contains(&t, "disk4"));
	CHECK(!ntfsmac_da_table_contains(&t, "disk5"));
	CHECK(ntfsmac_da_table_get_flag(&t, "disk4", &flag));
	CHECK(!flag);
	CHECK(ntfsmac_da_table_set_flag(&t, "disk4", true));
	CHECK(ntfsmac_da_table_add(&t, "disk4")); /* adding again keeps the flag */
	CHECK(t.count == 2);
	CHECK(ntfsmac_da_table_get_flag(&t, "disk4", &flag));
	CHECK(flag);
	CHECK(!ntfsmac_da_table_set_flag(&t, "disk9", true));
	CHECK(!ntfsmac_da_table_get_flag(&t, "disk9", &flag));
	/* Remove the first entry: the last one moves into its place with its flag. */
	CHECK(ntfsmac_da_table_set_flag(&t, "disk4s1", true));
	flag = false;
	CHECK(ntfsmac_da_table_remove(&t, "disk4", &flag));
	CHECK(flag);
	CHECK(!ntfsmac_da_table_contains(&t, "disk4"));
	CHECK(ntfsmac_da_table_get_flag(&t, "disk4s1", &flag));
	CHECK(flag);
	CHECK(!ntfsmac_da_table_remove(&t, "disk4", NULL));
	CHECK(ntfsmac_da_table_remove(&t, "disk4s1", NULL));
	CHECK(t.count == 0);

	/* Full table: the next name is refused and nothing else is lost. */
	for (i = 0; i < NTFSMAC_DA_TABLE_MAX; i++) {
		snprintf(name, sizeof name, "disk%u", i);
		CHECK(ntfsmac_da_table_add(&t, name));
	}
	CHECK(!ntfsmac_da_table_add(&t, "disk999"));
	CHECK(ntfsmac_da_table_add(&t, "disk0")); /* present: still fine */
	CHECK(ntfsmac_da_table_remove(&t, "disk5", NULL));
	CHECK(ntfsmac_da_table_add(&t, "disk999"));
	CHECK(t.count == NTFSMAC_DA_TABLE_MAX);
}

static void test_api_args(void)
{
	/* U-DMN-DA-04: bad arguments never succeed, on any platform. */
	ntfsmac_da *da = (ntfsmac_da *)1;
	ntfsmac_da_callbacks cb;
	ntfsmac_disk_info info;

	memset(&cb, 0, sizeof cb);
	/* Without disk_appeared the module has nothing to report to. */
	CHECK(ntfsmac_da_start(&cb, NULL, &da) != NTFSMAC_DMN_OK);
	CHECK(da == NULL);
	CHECK(ntfsmac_da_start(NULL, NULL, &da) != NTFSMAC_DMN_OK);
	ntfsmac_da_stop(NULL);
	CHECK(ntfsmac_da_dissent(NULL, "disk4") != NTFSMAC_DMN_OK);
	CHECK(ntfsmac_da_withdraw(NULL, "disk4") != NTFSMAC_DMN_OK);
	CHECK(ntfsmac_da_describe(NULL, "disk4", &info) != NTFSMAC_DMN_OK);
	CHECK(ntfsmac_da_unmount(NULL, "disk4") != NTFSMAC_DMN_OK);
	CHECK(ntfsmac_da_eject(NULL, "disk4") != NTFSMAC_DMN_OK);
#if defined(__APPLE__)
	CHECK_EQ_INT(ntfsmac_da_start(&cb, NULL, &da), NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_da_dissent(NULL, "disk4"), NTFSMAC_DMN_E_INVAL);
#else
	CHECK_EQ_INT(ntfsmac_da_describe(NULL, "disk4", &info), NTFSMAC_DMN_E_NOTIMPL);
#endif
}

static void test_fuse_table(void)
{
	/* U-DMN-DA-05: macFUSE's disks of live mounts, kept with the mount path (T-M5-05). */
	static ntfsmac_da_fuse_table t;
	char path[NTFSMAC_DA_FUSE_PATH_MAX], small[8], name[32], longp[NTFSMAC_DA_FUSE_PATH_MAX + 8];
	unsigned i;

	ntfsmac_da_fuse_init(&t);
	CHECK(!ntfsmac_da_fuse_take(&t, "disk7s1", path, sizeof path));
	CHECK(!ntfsmac_da_fuse_add(&t, "bad name", "/Volumes/X"));
	CHECK(!ntfsmac_da_fuse_add(&t, NULL, "/Volumes/X"));
	CHECK(!ntfsmac_da_fuse_add(&t, "disk7s1", NULL));
	CHECK(!ntfsmac_da_fuse_add(&t, "disk7s1", ""));
	memset(longp, 'a', sizeof longp - 1);
	longp[0] = '/';
	longp[sizeof longp - 1] = '\0';
	CHECK(!ntfsmac_da_fuse_add(&t, "disk7s1", longp));
	CHECK(t.count == 0);
	CHECK(ntfsmac_da_fuse_add(&t, "disk7s1", "/Volumes/X"));
	CHECK(ntfsmac_da_fuse_add(&t, "disk8s1", "/Volumes/Y"));
	CHECK(ntfsmac_da_fuse_add(&t, "disk7s1", "/Volumes/X2")); /* the same disk again: updated */
	CHECK(t.count == 2);
	/* the disk that disappears gives back its path once; another disk is not touched */
	CHECK(ntfsmac_da_fuse_take(&t, "disk7s1", path, sizeof path));
	CHECK(strcmp(path, "/Volumes/X2") == 0);
	CHECK(!ntfsmac_da_fuse_take(&t, "disk7s1", path, sizeof path));
	CHECK(!ntfsmac_da_fuse_take(&t, "disk7", path, sizeof path)); /* the whole disk was never tracked */
	CHECK(ntfsmac_da_fuse_take(&t, "disk8s1", small, sizeof small));
	CHECK(strcmp(small, "/Volume") == 0); /* truncated to the buffer, terminated */
	CHECK(t.count == 0);
	for (i = 0; i < NTFSMAC_DA_TABLE_MAX; i++) {
		snprintf(name, sizeof name, "disk%us1", i);
		CHECK(ntfsmac_da_fuse_add(&t, name, "/Volumes/Z"));
	}
	CHECK(!ntfsmac_da_fuse_add(&t, "disk999s1", "/Volumes/Z")); /* full: refused, nothing lost */
	CHECK(ntfsmac_da_fuse_take(&t, "disk0s1", NULL, 0));
	CHECK(ntfsmac_da_fuse_add(&t, "disk999s1", "/Volumes/Z"));
}

int main(void)
{
	test_filter();
	test_fuse_table();
	test_macfuse_backing();
	test_names();
	test_table();
	test_api_args();
	if (g_failures != 0) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	return 0;
}
