/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-DMN-NT-01..04: notification texts (src/daemon/notify.c; ARCHITECTURE.md sections 3.2, 3.6, 3.7,
 * 4.1 step 9, 4.5 step 3, 7). The texts are pure; delivery goes to a sink here, so a unit test never
 * shows a banner.
 */
#include <string.h>

#include "check.h"
#include "internal.h"

static char g_last[600];
static ntfsmac_notify_kind g_kind;
static int g_calls;

static void sink(ntfsmac_notify_kind kind, const char *text, void *ctx)
{
	(void)ctx;
	g_kind = kind;
	snprintf(g_last, sizeof g_last, "%s", text);
	g_calls++;
}

static bool has(ntfsmac_notify_kind k, const char *label, const char *reason, const char *want)
{
	char buf[512];

	return ntfsmac_notify_text(k, label, reason, buf, sizeof buf) > 0 && strstr(buf, want) != NULL;
}

/* U-DMN-NT-01: the wordings the architecture fixes. */
static void test_wording(void)
{
	/* section 4.5 step 3 */
	CHECK(has(NTFSMAC_NOTIFY_EJECTED_CLEAN, "Disk", NULL, "ejected, all data written."));
	CHECK(has(NTFSMAC_NOTIFY_EJECTED_NO_FLUSH_CONFIRM, "Disk", NULL,
		  "ejected, all data written; this drive did not confirm that its cache was flushed"));
	CHECK(has(NTFSMAC_NOTIFY_SESSION_UNCLEAN, "Disk", NULL, "read-only next time"));
	/* section 3.6, interrupted marking */
	CHECK(has(NTFSMAC_NOTIFY_VOLMARK_INTERRUPTED, "Disk", "Run chkdsk.",
		  "this Mac was interrupted while marking the volume; data was not changed"));
	CHECK(has(NTFSMAC_NOTIFY_VOLMARK_INTERRUPTED, "Disk", "Run chkdsk.", "Run chkdsk."));
	/* sections 3.2 and 3.7, mirror_split without an entry */
	CHECK(has(NTFSMAC_NOTIFY_MIRROR_SPLIT, "Disk", NULL,
		  "this volume was interrupted while NTFS-3G updated its file table; the data region was "
		  "not touched"));
	CHECK(has(NTFSMAC_NOTIFY_MIRROR_SPLIT, "Disk", NULL, "ntfsmac repair --sync-mirror"));
	/* section 7, first E record */
	CHECK(has(NTFSMAC_NOTIFY_WRITE_ERROR, "Disk", "14:02", "Write error on Disk: unmounting"));
	CHECK(has(NTFSMAC_NOTIFY_WRITE_ERROR, "Disk", "14:02", "Data written after 14:02 may be lost."));
	CHECK(has(NTFSMAC_NOTIFY_WRITE_ERROR, "Disk", NULL, "Data written in this session may be lost."));
	/* section 4.1 step 9 */
	CHECK(has(NTFSMAC_NOTIFY_MOUNTED_RW, "Disk", NULL, "Disk is mounted read/write."));
	CHECK(has(NTFSMAC_NOTIFY_MOUNTED_RO, "Disk", "the engine mounted read-only",
		  "mounted read-only: the engine mounted read-only."));
	CHECK(has(NTFSMAC_NOTIFY_MOUNTED_RO, "Disk", NULL, "Disk is mounted read-only."));
	CHECK(has(NTFSMAC_NOTIFY_REFUSED, "Disk", "damaged", "was not mounted: damaged."));
	CHECK(has(NTFSMAC_NOTIFY_STRAY_MOUNT, "Disk", NULL, "mounted another file system"));
}

/* U-DMN-NT-02: no label, control bytes, limits. */
static void test_safety(void)
{
	char buf[512], small[10];

	CHECK(has(NTFSMAC_NOTIFY_MOUNTED_RW, NULL, NULL, "the volume is mounted read/write."));
	CHECK(has(NTFSMAC_NOTIFY_MOUNTED_RW, "", NULL, "the volume is mounted"));
	CHECK(ntfsmac_notify_text(NTFSMAC_NOTIFY_MOUNTED_RW, "a\nb\x1b" "c", NULL, buf, sizeof buf) > 0);
	CHECK(strchr(buf, '\n') == NULL && strchr(buf, '\x1b') == NULL && strstr(buf, "a?b?c") != NULL);
	CHECK_EQ_INT(ntfsmac_notify_text((ntfsmac_notify_kind)99, "x", NULL, buf, sizeof buf), 0);
	CHECK(buf[0] == '\0');
	CHECK_EQ_INT(ntfsmac_notify_text(NTFSMAC_NOTIFY_MOUNTED_RW, "Disk", NULL, small, sizeof small), 0);
	CHECK_EQ_INT(ntfsmac_notify_text(NTFSMAC_NOTIFY_MOUNTED_RW, "Disk", NULL, NULL, 10), 0);
	CHECK_EQ_INT(ntfsmac_notify_text(NTFSMAC_NOTIFY_MOUNTED_RW, "Disk", NULL, buf, 0), 0);
}

/* U-DMN-NT-03: every kind has a text. */
static void test_all_kinds(void)
{
	char buf[512];
	int k;

	for (k = 0; k <= (int)NTFSMAC_NOTIFY_STRAY_MOUNT; k++) {
		CHECK(ntfsmac_notify_text((ntfsmac_notify_kind)k, "Disk", "x", buf, sizeof buf) > 0);
		CHECK(!strstr(buf, "(null)"));
	}
}

/* U-DMN-NT-04: the sink gets the text and the kind; post refuses a bad kind. */
static void test_sink(void)
{
	ntfsmac_notify_set_sink(sink, NULL);
	CHECK_EQ_INT(ntfsmac_notify_post(NTFSMAC_NOTIFY_EJECTED_CLEAN, "Disk", NULL), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(g_calls, 1);
	CHECK_EQ_INT(g_kind, NTFSMAC_NOTIFY_EJECTED_CLEAN);
	CHECK(strcmp(g_last, "Disk ejected, all data written.") == 0);
	CHECK_EQ_INT(ntfsmac_notify_post((ntfsmac_notify_kind)99, "Disk", NULL), NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(g_calls, 1);
	ntfsmac_notify_set_sink(NULL, NULL);
}

int main(void)
{
	test_wording();
	test_safety();
	test_all_kinds();
	test_sink();
	if (g_failures != 0) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	return 0;
}
