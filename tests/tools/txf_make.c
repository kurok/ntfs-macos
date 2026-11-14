/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * txf_make - give the root directory of an NTFS IMAGE FILE a NON-RESIDENT
 * $TXF_DATA stream.
 *
 * Usage: txf_make IMAGE
 *
 * $TXF_DATA is the LOGGED_UTILITY_STREAM (0x100) named "$TXF_DATA" that
 * Windows keeps on files and directories touched by transactional NTFS.
 * NTFS-3G's fix_txf_data() in libntfs-3g/volume.c makes a non-resident one on
 * the root directory resident again on every read/write mount (Vista cannot
 * mount a volume otherwise) and logs "$TXF_DATA made resident". The fixture
 * made here exercises that path: a read/write mount changes the volume, a
 * read-only or forensic one does not.
 *
 * How: libntfs-3g, opened with NTFS_MNT_FORENSIC (no fix_txf_data(), no log or
 * hibernation checks), does the hard part: allocating a cluster in $Bitmap,
 * building the data run and re-packing the root's MFT record with its update
 * sequence fix-ups. The library refuses to make a stream NAMED "$TXF_DATA"
 * non-resident (ntfs_attr_can_be_non_resident(): Windows Vista cannot mount
 * such a volume, the very reason fix_txf_data() exists), so the stream is built
 * under the name "$TXF_DATB" and the one name character is changed to 'A' in
 * the in-memory MFT record afterwards; the library writes the record back. The
 * name is the only thing changed that way: same length, same sort position.
 *
 * Steps: add a resident 56 byte pattern stream "$TXF_DATB" (ntfs_attr_add()),
 * ntfs_attr_force_non_resident() moves it into one allocated cluster, rename.
 * A root that already has a $TXF_DATA is refused (a real one is not touched).
 * The tool re-opens the image read-only afterwards and checks that "$TXF_DATA"
 * is non-resident with the pattern as its content. On an error the image may be
 * half edited: discard it.
 *
 * The image must be a regular file. Output: "txf_nonresident=1 size=N allocated=M".
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
#include <ntfs-3g/volume.h>

#define TXF_NAME_LEN 9 /* strlen("$TXF_DATA") */
#define TXF_SIZE 56

static void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

static void die(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "txf_make: error: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
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
 * Change the stream name @tmpname ("$TXF_DATB") to "$TXF_DATA" in the root's
 * in-memory MFT record and mark the inode dirty; closing the inode writes the
 * record (update sequence fix-ups applied by the library). The attribute record
 * keeps its length and position: only the last of its nine UTF-16 name
 * characters changes.
 */
static void rename_to_txf_data(ntfs_inode *ni, ntfschar *tmpname)
{
	ntfs_attr_search_ctx *ctx = ntfs_attr_get_search_ctx(ni, NULL);
	ATTR_RECORD *a;
	ntfschar *chars;

	if (!ctx)
		die("out of memory");
	if (ntfs_attr_lookup(AT_LOGGED_UTILITY_STREAM, tmpname, TXF_NAME_LEN, CASE_SENSITIVE, 0,
			     NULL, 0, ctx))
		die("cannot find the stream to rename (%s)", strerror(errno));
	if (ctx->ntfs_ino != ni)
		die("the stream is in an extent record; refusing to rename it");
	a = ctx->attr;
	/* The record is in memory and was parsed by the library: still check. */
	if (a->name_length != TXF_NAME_LEN ||
	    (uint32_t)le16_to_cpu(a->name_offset) + 2u * TXF_NAME_LEN > le32_to_cpu(a->length))
		die("unexpected attribute name layout");
	chars = (ntfschar *)((u8 *)a + le16_to_cpu(a->name_offset));
	if (memcmp(chars, tmpname, sizeof(ntfschar) * TXF_NAME_LEN))
		die("attribute name differs from what was added");
	chars[TXF_NAME_LEN - 1] = cpu_to_le16('A');
	ntfs_inode_mark_dirty(ni);
	ntfs_attr_put_search_ctx(ctx);
}

int main(int argc, char **argv)
{
	uint8_t pattern[TXF_SIZE], got[TXF_SIZE];
	ntfschar tmpname[TXF_NAME_LEN];
	ntfs_volume *vol;
	ntfs_inode *ni;
	ntfs_attr *na;
	s64 size, alloc;
	int i;

	ntfs_log_set_handler(ntfs_log_handler_stderr);
	if (argc != 2 || argv[1][0] == '-') {
		fputs("usage: txf_make IMAGE\n", stderr);
		return 2;
	}
	for (i = 0; i < TXF_SIZE; i++)
		pattern[i] = (uint8_t)(3 * i + 1);

	vol = open_image(argv[1], 1);
	ni = ntfs_inode_open(vol, FILE_root);
	if (!ni)
		die("cannot open the root directory (%s)", strerror(errno));
	if (NInoAttrList(ni))
		die("the root directory has an $ATTRIBUTE_LIST; refusing to touch it");
	na = ntfs_attr_open(ni, AT_LOGGED_UTILITY_STREAM, TXF_DATA, TXF_NAME_LEN);
	if (na)
		die("the root directory already has a $TXF_DATA; refusing to touch it");
	memcpy(tmpname, TXF_DATA, sizeof tmpname);
	tmpname[TXF_NAME_LEN - 1] = cpu_to_le16('B'); /* "$TXF_DATB" */
	if (ntfs_attr_add(ni, AT_LOGGED_UTILITY_STREAM, tmpname, TXF_NAME_LEN, pattern, TXF_SIZE))
		die("cannot add the stream (%s)", strerror(errno));
	na = ntfs_attr_open(ni, AT_LOGGED_UTILITY_STREAM, tmpname, TXF_NAME_LEN);
	if (!na)
		die("cannot open the new stream (%s)", strerror(errno));
	if (ntfs_attr_force_non_resident(na))
		die("cannot make the stream non-resident (%s)", strerror(errno));
	ntfs_attr_close(na);
	rename_to_txf_data(ni, tmpname);
	if (ntfs_inode_close(ni))
		die("closing the root inode failed (%s)", strerror(errno));
	if (ntfs_umount(vol, FALSE))
		die("unmounting the image failed (%s)", strerror(errno));

	/* Verify from disk with a fresh read-only open. */
	vol = open_image(argv[1], 0);
	ni = ntfs_inode_open(vol, FILE_root);
	na = ni ? ntfs_attr_open(ni, AT_LOGGED_UTILITY_STREAM, TXF_DATA, TXF_NAME_LEN) : NULL;
	if (!na)
		die("verification: $TXF_DATA not found on the root directory");
	if (!NAttrNonResident(na))
		die("verification: $TXF_DATA is still resident");
	size = na->data_size;
	alloc = na->allocated_size;
	if (size > TXF_SIZE || ntfs_attr_pread(na, 0, size, got) != size ||
	    size != TXF_SIZE || memcmp(got, pattern, (size_t)size))
		die("verification: $TXF_DATA content differs");
	ntfs_attr_close(na);
	ntfs_inode_close(ni);
	ntfs_umount(vol, FALSE);
	printf("txf_nonresident=1 size=%lld allocated=%lld\n", (long long)size, (long long)alloc);
	return 0;
}
