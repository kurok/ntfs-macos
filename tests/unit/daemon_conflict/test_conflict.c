/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-DMN-CFL-01..05: conflict rules (pure), directory scan on temporary
 * directories, passive-mode gates.
 */
#include "tmpdir.h"

#include "check.h"
#include "internal.h"

static void test_classify(void)
{
	/* U-DMN-CFL-01: the pure rule. */
	CHECK_EQ_INT(ntfsmac_conflict_classify("ufsd_NTFS.fs"), NTFSMAC_CONFLICT_PARAGON);
	CHECK_EQ_INT(ntfsmac_conflict_classify("UFSD_ntfs.FS"), NTFSMAC_CONFLICT_PARAGON);
	CHECK_EQ_INT(ntfsmac_conflict_classify("tuxera_ntfs.fs"), NTFSMAC_CONFLICT_TUXERA);
	CHECK_EQ_INT(ntfsmac_conflict_classify("Tuxera_NTFS.fs"), NTFSMAC_CONFLICT_TUXERA);
	CHECK_EQ_INT(ntfsmac_conflict_classify("ntfs-3g.fs"), NTFSMAC_CONFLICT_OTHER);
	CHECK_EQ_INT(ntfsmac_conflict_classify("AcmeNTFS.fs"), NTFSMAC_CONFLICT_OTHER);
	/* Not conflicts. */
	CHECK_EQ_INT(ntfsmac_conflict_classify("ntfs.fs"), NTFSMAC_CONFLICT_NONE);
	CHECK_EQ_INT(ntfsmac_conflict_classify("NTFS.fs"), NTFSMAC_CONFLICT_NONE);
	CHECK_EQ_INT(ntfsmac_conflict_classify("ntfsmac.fs"), NTFSMAC_CONFLICT_NONE);
	CHECK_EQ_INT(ntfsmac_conflict_classify("macfuse.fs"), NTFSMAC_CONFLICT_NONE);
	CHECK_EQ_INT(ntfsmac_conflict_classify("fuse-t.fs"), NTFSMAC_CONFLICT_NONE);
	CHECK_EQ_INT(ntfsmac_conflict_classify("ufsd_ExtFS.fs"), NTFSMAC_CONFLICT_NONE);
	CHECK_EQ_INT(ntfsmac_conflict_classify("exfat.fs"), NTFSMAC_CONFLICT_NONE);
	CHECK_EQ_INT(ntfsmac_conflict_classify("my_ntfs_notes.txt"), NTFSMAC_CONFLICT_NONE);
	CHECK_EQ_INT(ntfsmac_conflict_classify("ntfs"), NTFSMAC_CONFLICT_NONE);
	CHECK_EQ_INT(ntfsmac_conflict_classify(".fs"), NTFSMAC_CONFLICT_NONE);
	CHECK_EQ_INT(ntfsmac_conflict_classify(""), NTFSMAC_CONFLICT_NONE);
	CHECK_EQ_INT(ntfsmac_conflict_classify(NULL), NTFSMAC_CONFLICT_NONE);
	CHECK(strcmp(ntfsmac_conflict_kind_name(NTFSMAC_CONFLICT_PARAGON), "?") != 0);
	CHECK(strcmp(ntfsmac_conflict_kind_name(NTFSMAC_CONFLICT_KIND_COUNT_), "?") == 0);
}

static void test_scan(void)
{
	char base[PATH_MAX], path[PATH_MAX], target[PATH_MAX];
	ntfsmac_conflict_report r;

	tmp_make(base, sizeof base);

	/* U-DMN-CFL-02: empty directory, then only non-conflicts. */
	CHECK_EQ_INT(ntfsmac_conflict_scan(base, &r), NTFSMAC_DMN_OK);
	CHECK(!r.passive);
	CHECK_EQ_INT(r.drivers_found, 0);
	tmp_mkdir(base, "macfuse.fs");
	tmp_mkdir(base, "fuse-t.fs");
	tmp_mkdir(base, "NetFSPlugins");
	CHECK_EQ_INT(ntfsmac_conflict_scan(base, &r), NTFSMAC_DMN_OK);
	CHECK(!r.passive);
	CHECK_EQ_INT(r.drivers_found, 0);
	CHECK_EQ_INT(r.kind_mask, 0);

	/* A stray file with a driver name is not a bundle. */
	tmp_write(base, "ufsd_NTFS.fs", "x");
	CHECK_EQ_INT(ntfsmac_conflict_scan(base, &r), NTFSMAC_DMN_OK);
	CHECK(!r.passive);
	/* A dangling link is not a bundle either. */
	snprintf(path, sizeof path, "%s/tuxera_ntfs.fs", base);
	CHECK(symlink("/nonexistent-ntfsmac-target", path) == 0);
	CHECK_EQ_INT(ntfsmac_conflict_scan(base, &r), NTFSMAC_DMN_OK);
	CHECK(!r.passive);
	CHECK(unlink(path) == 0);
	snprintf(path, sizeof path, "%s/ufsd_NTFS.fs", base);
	CHECK(unlink(path) == 0);

	/* U-DMN-CFL-03: the dummy Paragon bundle (a directory) gives passive mode. */
	tmp_mkdir(base, "ufsd_NTFS.fs");
	CHECK_EQ_INT(ntfsmac_conflict_scan(base, &r), NTFSMAC_DMN_OK);
	CHECK(r.passive);
	CHECK_EQ_INT(r.drivers_found, 1);
	CHECK_EQ_INT(r.kind_mask, 1u << NTFSMAC_CONFLICT_PARAGON);

	/* Several drivers: counted, the mask holds each kind. */
	tmp_mkdir(base, "tuxera_ntfs.fs");
	tmp_mkdir(base, "other-ntfs.fs");
	CHECK_EQ_INT(ntfsmac_conflict_scan(base, &r), NTFSMAC_DMN_OK);
	CHECK(r.passive);
	CHECK_EQ_INT(r.drivers_found, 3);
	CHECK_EQ_INT(r.kind_mask, (1u << NTFSMAC_CONFLICT_PARAGON) |
				      (1u << NTFSMAC_CONFLICT_TUXERA) |
				      (1u << NTFSMAC_CONFLICT_OTHER));
	tmp_rmrf(base);

	/* U-DMN-CFL-04: a link to a bundle directory counts; a missing
	 * directory means no driver; an unreadable one means passive and E_IO. */
	tmp_make(base, sizeof base);
	tmp_mkdir(base, "real");
	snprintf(target, sizeof target, "%s/real", base);
	snprintf(path, sizeof path, "%s/tuxera_ntfs.fs", base);
	CHECK(symlink(target, path) == 0);
	CHECK_EQ_INT(ntfsmac_conflict_scan(base, &r), NTFSMAC_DMN_OK);
	CHECK(r.passive);
	CHECK_EQ_INT(r.drivers_found, 1);
	snprintf(path, sizeof path, "%s/missing", base);
	CHECK_EQ_INT(ntfsmac_conflict_scan(path, &r), NTFSMAC_DMN_OK);
	CHECK(!r.passive);
	CHECK_EQ_INT(r.drivers_found, 0);
	tmp_write(base, "afile", "x");
	snprintf(path, sizeof path, "%s/afile", base); /* not a directory */
	CHECK_EQ_INT(ntfsmac_conflict_scan(path, &r), NTFSMAC_DMN_E_IO);
	CHECK(r.passive);
	tmp_rmrf(base);

	CHECK_EQ_INT(ntfsmac_conflict_scan(NULL, &r), NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_conflict_scan("/", NULL), NTFSMAC_DMN_E_INVAL);
}

static void test_passive_gates(void)
{
	ntfsmac_conflict_report active = {false, 0, 0};
	ntfsmac_conflict_report passive = {true, 1, 1u << NTFSMAC_CONFLICT_PARAGON};

	/* U-DMN-CFL-05: passive mode issues no dissent and no automount. */
	CHECK(ntfsmac_passive_allows_dissent(&active));
	CHECK(ntfsmac_passive_allows_automount(&active));
	CHECK(!ntfsmac_passive_allows_dissent(&passive));
	CHECK(!ntfsmac_passive_allows_automount(&passive));
	CHECK(!ntfsmac_passive_allows_dissent(NULL));
	CHECK(!ntfsmac_passive_allows_automount(NULL));

	/* `ntfsmac mount` in passive mode: only on a volume nobody has mounted. */
	CHECK_EQ_INT(ntfsmac_passive_check_manual_mount(&passive, false), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(ntfsmac_passive_check_manual_mount(&passive, true), NTFSMAC_DMN_E_DENIED);
	CHECK_EQ_INT(ntfsmac_passive_check_manual_mount(NULL, true), NTFSMAC_DMN_E_DENIED);
	CHECK_EQ_INT(ntfsmac_passive_check_manual_mount(&active, false), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(ntfsmac_passive_check_manual_mount(&active, true), NTFSMAC_DMN_OK);
}

int main(void)
{
	test_classify();
	test_scan();
	test_passive_gates();
	if (g_failures != 0) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	return 0;
}
