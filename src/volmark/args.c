/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Command line and the flag decision of ntfsmac-volmark (docs/ARCHITECTURE.md
 * section 3.6, step 3; section 3.7, step 2). Pure C, no I/O: unit tested on every host
 * (tests/unit/volmark/test_args.c).
 */
#include "volmark.h"

#include <stddef.h>
#include <string.h>

int volmark_parse_serial(const char *s, uint64_t *v)
{
	uint64_t n = 0;
	int i;

	for (i = 0; i < 16; i++) {
		char c = s[i];
		unsigned d;

		if (c >= '0' && c <= '9')
			d = (unsigned)(c - '0');
		else if (c >= 'a' && c <= 'f')
			d = (unsigned)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			d = (unsigned)(c - 'A' + 10);
		else
			return -1; /* also the terminating NUL of a short string */
		n = (n << 4) | d;
	}
	if (s[16] != '\0')
		return -1;
	*v = n;
	return 0;
}

int volmark_parse_sectors(const char *s, uint64_t *v)
{
	uint64_t n = 0;

	if (*s < '1' || *s > '9')
		return -1; /* empty, sign, leading zero or zero */
	for (; *s; s++) {
		if (*s < '0' || *s > '9')
			return -1;
		if (n > (UINT64_C(0x7fffffffffffffff) - (uint64_t)(*s - '0')) / 10)
			return -1;
		n = n * 10 + (uint64_t)(*s - '0');
	}
	*v = n;
	return 0;
}

/* Descriptor number: decimal, no leading zero, 3 .. 1023 (0 to 2 are the standard streams). */
static int parse_fd(const char *s, int *fd)
{
	long n = 0;

	if (*s < '1' || *s > '9')
		return -1;
	for (; *s; s++) {
		if (*s < '0' || *s > '9')
			return -1;
		n = n * 10 + (*s - '0');
		if (n > 1023)
			return -1;
	}
	if (n < 3)
		return -1;
	*fd = (int)n;
	return 0;
}

int volmark_parse_args(int argc, char **argv, bool test_opts, volmark_args *a)
{
	bool have_fd = false, have_serial = false, have_sectors = false;
	int i;

	memset(a, 0, sizeof(*a));
	a->fd = -1;
	if (argc < 2)
		return VOLMARK_EXIT_USAGE;
	if (!strcmp(argv[1], "set"))
		a->mode = VOLMARK_SET;
	else if (!strcmp(argv[1], "clear"))
		a->mode = VOLMARK_CLEAR;
	else if (!strcmp(argv[1], "clear-repair"))
		a->mode = VOLMARK_CLEAR_REPAIR;
	else if (!strcmp(argv[1], "sync-mirror"))
		a->mode = VOLMARK_SYNC_MIRROR;
	else
		return VOLMARK_EXIT_USAGE;

	for (i = 2; i < argc; i++) {
		const char *o = argv[i];

		if (!strcmp(o, "--fd") && i + 1 < argc && !have_fd) {
			if (parse_fd(argv[++i], &a->fd) != 0)
				return VOLMARK_EXIT_USAGE;
			have_fd = true;
		} else if (!strcmp(o, "--expect-serial") && i + 1 < argc && !have_serial) {
			if (volmark_parse_serial(argv[++i], &a->expect_serial) != 0)
				return VOLMARK_EXIT_USAGE;
			have_serial = true;
		} else if (!strcmp(o, "--expect-sectors") && i + 1 < argc && !have_sectors) {
			if (volmark_parse_sectors(argv[++i], &a->expect_sectors) != 0)
				return VOLMARK_EXIT_USAGE;
			have_sectors = true;
		} else if (!strcmp(o, "--debug")) {
			a->debug = true;
		} else if (test_opts && !strcmp(o, "--stop-before-write")) {
			a->stop_before_write = true;
		} else if (test_opts && !strcmp(o, "--no-alarm")) {
			a->no_alarm = true;
		} else {
			return VOLMARK_EXIT_USAGE;
		}
	}
	/* The binding values are not optional (ADR-020). */
	if (!have_fd || !have_serial || !have_sectors)
		return VOLMARK_EXIT_USAGE;
	return VOLMARK_EXIT_OK;
}

int volmark_next_flags(volmark_mode mode, uint16_t flags, uint16_t *out)
{
	const unsigned f = flags;

	switch (mode) {
	case VOLMARK_SET:
		/*
		 * Refuse the dirty flag and every flag but the informational ones: a bit outside
		 * the mask would be dropped by ntfs_volume_write_flags(), and chkdsk or
		 * pending-work flags mean the volume is not one to mark and write (F-06).
		 */
		if ((f & VOLMARK_VOL_DIRTY) ||
		    (f & ~(VOLMARK_VOL_MODIFIED_BY_CHKDSK | VOLMARK_VOL_MOUNTED_ON_NT4)))
			return VOLMARK_EXIT_FLAGS;
		*out = (uint16_t)(f | VOLMARK_VOL_DIRTY);
		return VOLMARK_EXIT_OK;
	case VOLMARK_CLEAR:
		/*
		 * Our own `set` refuses a bit outside the mask, so one now means somebody
		 * else changed the volume during the session: an anomaly like a missing flag.
		 */
		if (!(f & VOLMARK_VOL_DIRTY) || (f & ~VOLMARK_VOL_FLAGS_MASK))
			return VOLMARK_EXIT_ANOMALY;
		*out = (uint16_t)(f & ~VOLMARK_VOL_DIRTY);
		return VOLMARK_EXIT_OK;
	case VOLMARK_CLEAR_REPAIR:
		/* ADR-017: only the dirty flag plus the informational chkdsk and NT4 bits. */
		if (!(f & VOLMARK_VOL_DIRTY) ||
		    (f & ~(VOLMARK_VOL_DIRTY | VOLMARK_VOL_MODIFIED_BY_CHKDSK | VOLMARK_VOL_MOUNTED_ON_NT4)))
			return VOLMARK_EXIT_ANOMALY;
		*out = (uint16_t)(f & ~VOLMARK_VOL_DIRTY);
		return VOLMARK_EXIT_OK;
	case VOLMARK_SYNC_MIRROR:
		/*
		 * Section 3.7 step 2: the flag may be set (an interrupted `set` leaves it in $MFT) and
		 * the informational two may be there; any other bit is a finding the probe reports as
		 * `dirty_flag` and that this repair does not go near. Nothing is written for flags.
		 */
		if (f & ~(VOLMARK_VOL_DIRTY | VOLMARK_VOL_MODIFIED_BY_CHKDSK | VOLMARK_VOL_MOUNTED_ON_NT4))
			return VOLMARK_EXIT_ANOMALY;
		*out = (uint16_t)f;
		return VOLMARK_EXIT_OK;
	}
	return VOLMARK_EXIT_USAGE;
}
