/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * T-M3-12: the pure part of ntfsmac-volmark (src/volmark/args.c): the command
 * line and the step-3 flag decision of docs/ARCHITECTURE.md section 3.6.
 * Runs on every host (no libntfs-3g).
 */
#include <stdio.h>
#include <string.h>

#include "volmark.h"

static int failures;

#define CHECK(cond)                                                                   \
	do {                                                                          \
		if (!(cond)) {                                                        \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                   \
		}                                                                     \
	} while (0)

static int parse(bool test_opts, volmark_args *a, const char *a0, const char *a1, const char *a2,
		 const char *a3, const char *a4, const char *a5, const char *a6, const char *a7)
{
	char *argv[10];
	const char *in[8] = { a0, a1, a2, a3, a4, a5, a6, a7 };
	int argc = 1, i;

	argv[0] = (char *)"ntfsmac-volmark";
	for (i = 0; i < 8 && in[i]; i++)
		argv[argc++] = (char *)in[i];
	argv[argc] = NULL;
	return volmark_parse_args(argc, argv, test_opts, a);
}

#define S "0123456789abcdef"

static void test_args(void)
{
	volmark_args a;

	CHECK(parse(false, &a, "set", "--fd", "3", "--expect-serial", S, "--expect-sectors", "131071", NULL) ==
	      VOLMARK_EXIT_OK);
	CHECK(a.mode == VOLMARK_SET && a.fd == 3 && a.expect_serial == 0x0123456789abcdefULL &&
	      a.expect_sectors == 131071 && !a.debug && !a.stop_before_write);
	CHECK(parse(false, &a, "clear", "--expect-sectors", "1", "--fd", "7", "--expect-serial", "FFFFFFFFFFFFFFFF",
		    NULL) == VOLMARK_EXIT_OK);
	CHECK(a.mode == VOLMARK_CLEAR && a.fd == 7 && a.expect_serial == 0xffffffffffffffffULL && a.expect_sectors == 1);
	CHECK(parse(false, &a, "clear-repair", "--fd", "3", "--expect-serial", S, "--expect-sectors", "9", "--debug") ==
	      VOLMARK_EXIT_OK);
	CHECK(a.mode == VOLMARK_CLEAR_REPAIR && a.debug);

	/* modes */
	CHECK(parse(false, &a, "sync-mirror", "--fd", "3", "--expect-serial", S, "--expect-sectors", "9", NULL) ==
	      VOLMARK_EXIT_OK);
	CHECK(a.mode == VOLMARK_SYNC_MIRROR && a.fd == 3 && a.expect_sectors == 9 && !a.stop_before_write);
	CHECK(parse(false, &a, "sync-mirror", "--fd", "3", "--expect-serial", S, NULL, NULL, NULL) == VOLMARK_EXIT_USAGE);
	CHECK(parse(false, &a, "sync-mirror", "--stop-before-write", "--fd", "3", "--expect-serial", S,
		    "--expect-sectors", "9") == VOLMARK_EXIT_USAGE);
	CHECK(parse(true, &a, "sync-mirror", "--fd", "3", "--expect-serial", S, "--expect-sectors", "9",
		    "--stop-before-write") == VOLMARK_EXIT_OK && a.stop_before_write);
	CHECK(parse(false, &a, "sync_mirror", "--fd", "3", "--expect-serial", S, "--expect-sectors", "9", NULL) ==
	      VOLMARK_EXIT_USAGE);
	CHECK(parse(false, &a, "SET", "--fd", "3", "--expect-serial", S, "--expect-sectors", "9", NULL) ==
	      VOLMARK_EXIT_USAGE);
	CHECK(parse(false, &a, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL) == VOLMARK_EXIT_USAGE);
	/* every value is required (ADR-020) */
	CHECK(parse(false, &a, "set", "--fd", "3", "--expect-serial", S, NULL, NULL, NULL) == VOLMARK_EXIT_USAGE);
	CHECK(parse(false, &a, "set", "--fd", "3", "--expect-sectors", "9", NULL, NULL, NULL) == VOLMARK_EXIT_USAGE);
	CHECK(parse(false, &a, "set", "--expect-serial", S, "--expect-sectors", "9", NULL, NULL, NULL) ==
	      VOLMARK_EXIT_USAGE);
	/* descriptor: 3 .. 1023, no leading zero, given once */
	CHECK(parse(false, &a, "set", "--fd", "2", "--expect-serial", S, "--expect-sectors", "9", NULL) ==
	      VOLMARK_EXIT_USAGE);
	CHECK(parse(false, &a, "set", "--fd", "03", "--expect-serial", S, "--expect-sectors", "9", NULL) ==
	      VOLMARK_EXIT_USAGE);
	CHECK(parse(false, &a, "set", "--fd", "1024", "--expect-serial", S, "--expect-sectors", "9", NULL) ==
	      VOLMARK_EXIT_USAGE);
	CHECK(parse(false, &a, "set", "--fd", "3", "--fd", "4", "--expect-serial", S, "--expect-sectors") ==
	      VOLMARK_EXIT_USAGE);
	/* test options only in the test build */
	CHECK(parse(false, &a, "set", "--fd", "3", "--expect-serial", S, "--expect-sectors", "9", "--no-alarm") ==
	      VOLMARK_EXIT_USAGE);
	CHECK(parse(false, &a, "set", "--fd", "3", "--expect-serial", S, "--expect-sectors", "9",
		    "--stop-before-write") == VOLMARK_EXIT_USAGE);
	CHECK(parse(true, &a, "set", "--fd", "3", "--expect-serial", S, "--expect-sectors", "9",
		    "--stop-before-write") == VOLMARK_EXIT_OK);
	CHECK(a.stop_before_write);
	CHECK(parse(false, &a, "set", "--fd", "3", "--expect-serial", S, "--expect-sectors", "9", "extra") ==
	      VOLMARK_EXIT_USAGE);
}

static void test_numbers(void)
{
	uint64_t v;

	CHECK(volmark_parse_serial("0000000000000000", &v) == 0 && v == 0);
	CHECK(volmark_parse_serial("A1b2C3d4E5f60718", &v) == 0 && v == 0xa1b2c3d4e5f60718ULL);
	CHECK(volmark_parse_serial("123456789abcdef", &v) == -1);   /* 15 digits */
	CHECK(volmark_parse_serial("123456789abcdef01", &v) == -1); /* 17 */
	CHECK(volmark_parse_serial("0x23456789abcdef", &v) == -1);
	CHECK(volmark_parse_serial("123456789abcdeg0", &v) == -1);
	CHECK(volmark_parse_serial("", &v) == -1);

	CHECK(volmark_parse_sectors("131071", &v) == 0 && v == 131071);
	CHECK(volmark_parse_sectors("9223372036854775807", &v) == 0 && v == 9223372036854775807ULL);
	CHECK(volmark_parse_sectors("9223372036854775808", &v) == -1);
	CHECK(volmark_parse_sectors("18446744073709551616", &v) == -1);
	CHECK(volmark_parse_sectors("0", &v) == -1);
	CHECK(volmark_parse_sectors("01", &v) == -1);
	CHECK(volmark_parse_sectors("+1", &v) == -1);
	CHECK(volmark_parse_sectors("-1", &v) == -1);
	CHECK(volmark_parse_sectors("1 ", &v) == -1);
	CHECK(volmark_parse_sectors("", &v) == -1);
}

/* Step 3 over every 16-bit flags value: only VOLUME_IS_DIRTY ever changes. */
static void test_flags(void)
{
	unsigned f;

	for (f = 0; f <= 0xffffu; f++) {
		uint16_t out = 0xa5a5;
		int rc;
		unsigned outside = f & ~VOLMARK_VOL_FLAGS_MASK;

		rc = volmark_next_flags(VOLMARK_SET, (uint16_t)f, &out);
		if ((f & VOLMARK_VOL_DIRTY) ||
		    (f & ~(VOLMARK_VOL_MODIFIED_BY_CHKDSK | VOLMARK_VOL_MOUNTED_ON_NT4))) {
			CHECK(rc == VOLMARK_EXIT_FLAGS);
		} else {
			CHECK(rc == VOLMARK_EXIT_OK);
			CHECK(out == (f | VOLMARK_VOL_DIRTY));
		}

		rc = volmark_next_flags(VOLMARK_CLEAR, (uint16_t)f, &out);
		if (!(f & VOLMARK_VOL_DIRTY) || outside) {
			CHECK(rc == VOLMARK_EXIT_ANOMALY);
		} else {
			CHECK(rc == VOLMARK_EXIT_OK);
			CHECK(out == (f & ~VOLMARK_VOL_DIRTY));
		}

		rc = volmark_next_flags(VOLMARK_CLEAR_REPAIR, (uint16_t)f, &out);
		if (f == 0x0001 || f == 0x8001 || f == 0x0009 || f == 0x8009) {
			CHECK(rc == VOLMARK_EXIT_OK);
			CHECK(out == (f & ~VOLMARK_VOL_DIRTY));
		} else {
			CHECK(rc == VOLMARK_EXIT_ANOMALY);
		}

		/* sync-mirror (section 3.7 step 2): DIRTY and the informational two may be set, nothing else; no bit changes. */
		out = 0xa5a5;
		rc = volmark_next_flags(VOLMARK_SYNC_MIRROR, (uint16_t)f, &out);
		if (f & ~(VOLMARK_VOL_DIRTY | VOLMARK_VOL_MODIFIED_BY_CHKDSK | VOLMARK_VOL_MOUNTED_ON_NT4)) {
			CHECK(rc == VOLMARK_EXIT_ANOMALY);
		} else {
			CHECK(rc == VOLMARK_EXIT_OK);
			CHECK(out == f);
		}
		if (failures > 20)
			return;
	}
}

int main(void)
{
	test_args();
	test_numbers();
	test_flags();
	if (failures) {
		fprintf(stderr, "volmark.args: %d failure(s)\n", failures);
		return 1;
	}
	printf("volmark.args: ok\n");
	return 0;
}
