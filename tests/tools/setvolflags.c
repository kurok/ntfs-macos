/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * setvolflags - set or clear $Volume flag bits of an NTFS IMAGE FILE.
 *
 * Usage: setvolflags IMAGE [--set FLAGS] [--clear FLAGS]
 *        setvolflags IMAGE --show
 *
 * FLAGS is a comma separated list of names or numbers (0x... hex, or decimal):
 *   DIRTY (0x0001), RESIZE_LOG_FILE (0x0002), UPGRADE_ON_MOUNT (0x0004),
 *   MOUNTED_ON_NT4 (0x0008), DELETE_USN_UNDERWAY (0x0010),
 *   REPAIR_OBJECT_ID (0x0020), CHKDSK_UNDERWAY (0x4000),
 *   MODIFIED_BY_CHKDSK (0x8000).
 * Bits outside VOLUME_FLAGS_MASK (0xc03f) are refused: the library write path
 * masks them away, and a silent no-op would give a wrong fixture.
 * --set is applied before --clear. The image must be a regular file; block
 * devices and everything else are refused.
 *
 * How: the flags live in the $VOLUME_INFORMATION attribute of MFT record 3
 * ($Volume). libntfs-3g (opened with NTFS_MNT_FORENSIC: no log, hibernation or
 * $TXF_DATA handling, so unsafe images open) changes the attribute with
 * ntfs_volume_write_flags(), which writes the record through the library's MFT
 * path: it applies the update sequence fix-ups of the record and, because
 * record 3 is below the mirror size, writes the copy in $MFTMirr too. The tool
 * does not rely on that: it re-opens the image and reads record 3 of $MFT and of
 * $MFTMirr through the library's fix-up aware attribute reader, and fails if
 * either copy does not carry the expected flags. A raw byte edit would have to
 * repeat the USA and mirror handling by hand for no gain.
 *
 * Output: "flags_before=0x.... flags_after=0x.... mft=0x.... mirror=0x...."
 * (--show prints the same line with before equal to after).
 *
 * Exit status: 0 success, 1 failure, 2 usage error.
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h> /* libntfs-3g/ntfstime.h uses time() without including it */

#include <ntfs-3g/attrib.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/layout.h>
#include <ntfs-3g/logging.h>
#include <ntfs-3g/mft.h>
#include <ntfs-3g/volume.h>

#define FLAGS_MASK 0xc03fu /* VOLUME_FLAGS_MASK */

static const struct {
	const char *name;
	unsigned bit;
} names[] = {
	{"DIRTY", 0x0001},
	{"RESIZE_LOG_FILE", 0x0002},
	{"UPGRADE_ON_MOUNT", 0x0004},
	{"MOUNTED_ON_NT4", 0x0008},
	{"DELETE_USN_UNDERWAY", 0x0010},
	{"REPAIR_OBJECT_ID", 0x0020},
	{"CHKDSK_UNDERWAY", 0x4000},
	{"MODIFIED_BY_CHKDSK", 0x8000},
};

static void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

static void die(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "setvolflags: error: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

static void usage(void)
{
	fputs("usage: setvolflags IMAGE [--set FLAGS] [--clear FLAGS]\n"
	      "       setvolflags IMAGE --show\n"
	      "FLAGS: comma separated names (DIRTY, MODIFIED_BY_CHKDSK, ...) or numbers\n",
	      stderr);
	exit(2);
}

/* Parse "DIRTY,0x8000" into a bit set; exit 2 on anything unknown. */
static unsigned parse_flags(const char *arg)
{
	char *copy = strdup(arg), *save = NULL, *tok;
	unsigned bits = 0;

	if (!copy)
		die("out of memory");
	for (tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		size_t i;
		int found = 0;

		for (i = 0; i < sizeof names / sizeof names[0]; i++)
			if (!strcmp(tok, names[i].name)) {
				bits |= names[i].bit;
				found = 1;
			}
		if (!found) {
			char *end;
			unsigned long v;

			errno = 0;
			v = strtoul(tok, &end, 0);
			if (errno || *end || !*tok || v > 0xffffu) {
				fprintf(stderr, "setvolflags: unknown flag: %s\n", tok);
				exit(2);
			}
			bits |= (unsigned)v;
		}
	}
	free(copy);
	if (bits & ~FLAGS_MASK) {
		fprintf(stderr, "setvolflags: bits 0x%04x are outside VOLUME_FLAGS_MASK 0x%04x\n",
			bits & ~FLAGS_MASK, FLAGS_MASK);
		exit(2);
	}
	return bits;
}

static ntfs_volume *open_image(const char *path, int writable)
{
	struct stat st;
	ntfs_volume *vol;
	unsigned long flags = NTFS_MNT_FORENSIC | (writable ? 0 : NTFS_MNT_RDONLY);

	if (stat(path, &st))
		die("%s: %s", path, strerror(errno));
	if (!S_ISREG(st.st_mode))
		die("%s: not a regular file (this tool edits image files only)", path);
	vol = ntfs_mount(path, flags);
	if (!vol)
		die("%s: cannot open as NTFS (%s)", path, strerror(errno));
	return vol;
}

/*
 * Find the flags word of $VOLUME_INFORMATION in the (fix-ups already applied)
 * MFT record @rec of @size bytes. Every offset is bounds checked against the
 * record. Returns 0 and sets *flags, or -1.
 */
static int record_flags(const uint8_t *rec, uint32_t size, uint16_t *flags)
{
	uint32_t pos, used;

	if (size < 0x30 || memcmp(rec, "FILE", 4))
		return -1;
	pos = (uint32_t)(rec[0x14] | (rec[0x15] << 8));              /* attrs_offset */
	used = (uint32_t)rec[0x18] | ((uint32_t)rec[0x19] << 8) |    /* bytes_in_use */
	       ((uint32_t)rec[0x1a] << 16) | ((uint32_t)rec[0x1b] << 24);
	if (used > size)
		return -1;
	while (pos + 8 <= used) {
		uint32_t type = (uint32_t)rec[pos] | ((uint32_t)rec[pos + 1] << 8) |
				((uint32_t)rec[pos + 2] << 16) | ((uint32_t)rec[pos + 3] << 24);
		uint32_t len = (uint32_t)rec[pos + 4] | ((uint32_t)rec[pos + 5] << 8) |
			       ((uint32_t)rec[pos + 6] << 16) | ((uint32_t)rec[pos + 7] << 24);

		if (type == 0xffffffffu || len < 0x18 || len % 8 || pos + len > used)
			return -1;
		if (type == 0x70) { /* $VOLUME_INFORMATION */
			uint32_t vofs = (uint32_t)(rec[pos + 0x14] | (rec[pos + 0x15] << 8));
			uint32_t vlen = (uint32_t)rec[pos + 0x10] | ((uint32_t)rec[pos + 0x11] << 8);

			if (rec[pos + 8] /* non-resident */ || vlen < 12 || vofs + vlen > len)
				return -1;
			/* value: reserved(8) major(1) minor(1) flags(2) */
			*flags = (uint16_t)(rec[pos + vofs + 10] | (rec[pos + vofs + 11] << 8));
			return 0;
		}
		pos += len;
	}
	return -1;
}

/* Flags of record 3 in the $MFT copy (@mirror 0) or the $MFTMirr copy (1). */
static uint16_t read_copy(ntfs_volume *vol, int mirror)
{
	ntfs_attr *na = mirror ? vol->mftmirr_na : vol->mft_na;
	uint32_t size = vol->mft_record_size;
	uint8_t *rec = malloc(size);
	uint16_t flags;

	if (!na || !rec)
		die("%s unavailable", mirror ? "$MFTMirr" : "$MFT");
	/* ntfs_attr_mst_pread applies the update sequence fix-ups */
	if (ntfs_attr_mst_pread(na, (s64)FILE_Volume * size, 1, size, rec) != 1)
		die("cannot read record 3 from %s (%s)", mirror ? "$MFTMirr" : "$MFT",
		    strerror(errno));
	if (record_flags(rec, size, &flags))
		die("record 3 in %s has no usable $VOLUME_INFORMATION", mirror ? "$MFTMirr" : "$MFT");
	free(rec);
	return flags;
}

int main(int argc, char **argv)
{
	const char *path = NULL;
	unsigned set = 0, clear = 0;
	int have_set = 0, have_clear = 0, show = 0, i;
	ntfs_volume *vol;
	unsigned before, after, mft, mirr;

	ntfs_log_set_handler(ntfs_log_handler_stderr);
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--set") && i + 1 < argc) {
			set = parse_flags(argv[++i]);
			have_set = 1;
		} else if (!strcmp(argv[i], "--clear") && i + 1 < argc) {
			clear = parse_flags(argv[++i]);
			have_clear = 1;
		} else if (!strcmp(argv[i], "--show"))
			show = 1;
		else if (argv[i][0] != '-' && !path)
			path = argv[i];
		else
			usage();
	}
	if (!path || (show && (have_set || have_clear)) || (!show && !have_set && !have_clear))
		usage();

	vol = open_image(path, !show);
	before = le16_to_cpu(vol->flags);
	after = before;
	if (!show) {
		if (before & ~FLAGS_MASK)
			die("the image has flag bits outside VOLUME_FLAGS_MASK (0x%04x): the library "
			    "cannot write them back, nothing was changed", before & ~FLAGS_MASK);
		after = (before | set) & ~clear;
		if (ntfs_volume_write_flags(vol, cpu_to_le16((u16)after)))
			die("ntfs_volume_write_flags failed (%s)", strerror(errno));
	}
	if (ntfs_umount(vol, FALSE))
		die("unmounting the image failed (%s)", strerror(errno));

	/* Independent check from disk: both copies of record 3 carry the flags. */
	vol = open_image(path, 0);
	mft = read_copy(vol, 0);
	mirr = read_copy(vol, 1);
	ntfs_umount(vol, FALSE);
	if (mft != after || mirr != after)
		die("verification failed: wanted 0x%04x, $MFT has 0x%04x, $MFTMirr has 0x%04x", after,
		    mft, mirr);
	printf("flags_before=0x%04x flags_after=0x%04x mft=0x%04x mirror=0x%04x\n", before, after, mft,
	       mirr);
	return 0;
}
