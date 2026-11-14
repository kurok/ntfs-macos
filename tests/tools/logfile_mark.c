/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * logfile_mark - edit the $LogFile restart pages of an NTFS IMAGE FILE.
 *
 * Usage: logfile_mark IMAGE [--init] [--unclean] [--v2] [--bump-lsn [--steps N]]
 *        logfile_mark IMAGE --dump
 *        logfile_mark --selftest
 *
 * Test tool for the unsafe-state fixtures (T-M3-04). It never touches a block
 * device or anything that is not a regular file, and it only writes the two
 * restart pages at the start of $LogFile (bytes 0..2*page_size of that file).
 *
 * Modes (several may be combined; they run in the order listed here):
 *
 *   --init       mkntfs leaves $LogFile as all 0xff ("empty"): NTFS-3G treats an
 *                empty log as clean and there is nothing to edit. --init writes a
 *                synthetic pair of Windows-style version 1.1 restart pages with a
 *                clean volume (client in use, RESTART_VOLUME_IS_CLEAN set).
 *                Refused unless the first two log pages are all 0xff.
 *   --unclean    clear RESTART_VOLUME_IS_CLEAN in both restart areas while the
 *                log client stays in use: the engine then reports an unclean
 *                shutdown (EOPNOTSUPP in ntfs_volume_check_logfile()).
 *   --v2         set the restart page version to 2.0 in both pages (Windows fast
 *                restart; the engine refuses it with EPERM even with "recover").
 *   --bump-lsn   advance current_lsn in both restart areas to
 *                max(lsn0, lsn1) + N * 2^(64 - seq_number_bits), i.e. N steps of
 *                the log sequence number (N from --steps, default 1). This is what
 *                a Windows check that mounts and dismounts cleanly does to the
 *                log. Nothing else changes: the clean/unclean state, version and
 *                client records are kept, so a clean log stays clean.
 *   --dump       print the state, change nothing (also printed after an edit).
 *   --selftest   test the update-sequence-array code against NTFS-3G's own
 *                ntfs_mst_*_fixup(); needs no image.
 *
 * Output (stdout, key=value, one record per line) is meant for scripts:
 *   page0 off=0 magic=RSTR version=1.1 usn=3 flags=0x0002 clients_in_use=0 \
 *         current_lsn=0x0000000000100400 seq_bits=45 file_size=2097152
 *   page1 ...
 *   log=clean|unclean|v2|empty|invalid   (the engine's verdict, see below)
 *   current_lsn=0x...                    (the larger of the two pages)
 *
 * How the edit works, and why it is done this way
 * -----------------------------------------------
 * A restart page is an NTFS "multi sector transfer" (MST) record. Every 512
 * byte sector of the page ends with a 16 bit word that the writer replaces by
 * the update sequence number (USN); the 16 bit words it displaced are saved in
 * the update sequence array (USA) in the page header, entry 0 being the USN
 * itself. A reader checks that each sector tail still equals the USN (else the
 * write was torn) and puts the saved words back. This tool therefore edits only
 * an UNPROTECTED copy of the page:
 *
 *   read page -> usa_unprotect() -> edit the fields -> usa_protect() -> write
 *
 * usa_protect() bumps the USN (skipping 0 and 0xffff, like ntfs_mst_pre_write_
 * fixup()), saves the real sector tails in the USA and stamps the new USN into
 * the tails. NTFS restart pages carry no other checksum, so recomputing the USA
 * is the whole integrity step. --selftest compares the two functions byte for
 * byte with the library's ntfs_mst_pre_write_fixup() and ntfs_mst_post_read_
 * fixup(), including USN wrap-around and a torn-sector case.
 *
 * Why libntfs-3g for the rest: locating $LogFile means reading MFT record 2,
 * applying its own fix-ups and decoding the data runs of a non-resident
 * attribute. The library does that and is the code the engine itself uses, so the
 * image is read exactly as ntfs-3g reads it. The page layout (offsets below) is
 * parsed by this tool, not by library structs, so that every check that guards a
 * buffer access is visible here. After every edit the tool re-opens the image
 * and asks the engine (ntfs_check_logfile(), ntfs_is_logfile_clean(), the 2.0
 * rule of ntfs_volume_check_logfile()) for its verdict and fails if that is not
 * what the mode promised.
 *
 * Mounting uses NTFS_MNT_FORENSIC: no hibernation or log check, no $TXF_DATA
 * fix-up, no log reset, so images with an unsafe state can be opened. Edits use
 * ntfs_attr_pwrite() on the $LogFile data attribute; dumps open the image
 * read-only.
 *
 * An image whose verification fails is left edited: discard it (as with
 * txf_make). --unclean is refused on a version 2.0 log (the engine reports v2
 * whatever the clean bit says).
 *
 * The --init pages are a LIBNTFS-3G/PROBE FIXTURE ONLY. They are valid for the
 * engine's checks, but the client's oldest_lsn and client_restart_lsn equal
 * current_lsn and point into a log page that is still all 0xff (no RCRD page, no
 * client restart record), and last_lsn_data_length is 0. Windows would most
 * likely call such a log corrupt. Do not use these images as a Windows-clean
 * baseline (H-USB-08, H-USB-09, T-M5-10).
 *
 * Exit status: 0 success, 1 failure (message on stderr), 2 usage error.
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
#include <ntfs-3g/logfile.h>
#include <ntfs-3g/logging.h>
#include <ntfs-3g/mst.h>
#include <ntfs-3g/volume.h>

/* ---- on-disk layout, byte offsets (see include/ntfs-3g/logfile.h) ---- */

#define SECTOR 512u /* USA granularity: fixed by the format, not the device */
#define PAGE_MIN 512u
#define PAGE_MAX 65536u

/* RESTART_PAGE_HEADER */
#define RP_MAGIC 0
#define RP_USA_OFS 4
#define RP_USA_COUNT 6
#define RP_CHKDSK_LSN 8
#define RP_SYSTEM_PAGE_SIZE 16
#define RP_LOG_PAGE_SIZE 20
#define RP_RESTART_AREA_OFS 24
#define RP_MINOR_VER 26 /* signed 16 bit */
#define RP_MAJOR_VER 28 /* signed 16 bit */
#define RP_USN 30       /* first word of the USA */
#define RP_HEADER_SIZE 32

/* RESTART_AREA, relative to the restart area */
#define RA_CURRENT_LSN 0
#define RA_LOG_CLIENTS 8
#define RA_CLIENT_FREE_LIST 10
#define RA_CLIENT_IN_USE_LIST 12
#define RA_FLAGS 14
#define RA_SEQ_NUMBER_BITS 16
#define RA_RESTART_AREA_LENGTH 20
#define RA_CLIENT_ARRAY_OFS 22
#define RA_FILE_SIZE 24
#define RA_LAST_LSN_DATA_LENGTH 32
#define RA_LOG_RECORD_HEADER_LENGTH 36
#define RA_LOG_PAGE_DATA_OFFSET 38
#define RA_RESTART_LOG_OPEN_COUNT 40
#define RA_SIZE 48

/* LOG_CLIENT_RECORD, relative to the record */
#define LC_OLDEST_LSN 0
#define LC_CLIENT_RESTART_LSN 8
#define LC_PREV_CLIENT 16
#define LC_NEXT_CLIENT 18
#define LC_SEQ_NUMBER 20
#define LC_CLIENT_NAME_LENGTH 28
#define LC_CLIENT_NAME 32
#define LC_SIZE 160

#define NO_CLIENT 0xffffu
#define VOLUME_IS_CLEAN 0x0002u /* RESTART_VOLUME_IS_CLEAN */
#define MAGIC_RSTR 0x52545352u  /* "RSTR" little endian */
#define LOG_PAGE_SIZE 4096u     /* what mkntfs and Windows use */

/* ---- little endian access (the image is little endian on every host) ---- */

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static uint64_t get64(const uint8_t *p)
{
	return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
	put16(p, (uint16_t)v);
	put16(p + 2, (uint16_t)(v >> 16));
}

static void put64(uint8_t *p, uint64_t v)
{
	put32(p, (uint32_t)v);
	put32(p + 4, (uint32_t)(v >> 32));
}

static void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

static void die(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "logfile_mark: error: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

/* ======================================================================== */
/* Update sequence array (multi sector transfer protection)                 */
/* ======================================================================== */

/*
 * Check the USA geometry of a page of @size bytes whose header says
 * (@usa_ofs, @usa_count). Same rule as is_valid_record() in libntfs-3g/mst.c:
 * one USA entry per 512 byte sector plus the USN itself, and the whole array
 * lies in the first sector below its last word (so the USA never overlaps a
 * sector tail it protects). Returns 0 if valid, -1 if not.
 */
static int usa_geometry_ok(uint32_t size, uint32_t usa_ofs, uint32_t usa_count)
{
	if (size % SECTOR == 0 && usa_ofs % 2 == 0 && usa_ofs >= RP_USN &&
	    usa_count == 1 + size / SECTOR && usa_ofs + usa_count * 2 <= SECTOR - 2)
		return 0;
	return -1;
}

/*
 * Undo the protection of @page in place, after a read.
 *
 * Layout: the USA starts at @usa_ofs; entry 0 is the USN, entry i (1..count-1)
 * holds the word that sector i-1 had in its last two bytes before the writer
 * overwrote them with the USN.
 *
 *   1. every sector tail must equal the USN, otherwise the page was written
 *      only in part (a torn write): fail and leave the page untouched;
 *   2. copy USA[i] back over the tail of sector i-1.
 *
 * The USN field and the USA stay in the buffer: they are header bytes, not
 * data. Returns 0, or -1 for a bad geometry or a torn page.
 */
static int usa_unprotect(uint8_t *page, uint32_t size)
{
	uint32_t usa_ofs = get16(page + RP_USA_OFS);
	uint32_t usa_count = get16(page + RP_USA_COUNT);
	uint16_t usn;
	uint32_t i;

	if (usa_geometry_ok(size, usa_ofs, usa_count))
		return -1;
	usn = get16(page + usa_ofs);
	for (i = 1; i < usa_count; i++)
		if (get16(page + i * SECTOR - 2) != usn)
			return -1; /* torn: this sector does not carry the USN */
	for (i = 1; i < usa_count; i++)
		memcpy(page + i * SECTOR - 2, page + usa_ofs + 2 * i, 2);
	return 0;
}

/*
 * Protect @page in place, before a write: the inverse of usa_unprotect().
 *
 *   1. new USN = old USN + 1, skipping 0 and 0xffff (0xffff is the value of an
 *      erased sector, 0 means "never written"); store it in USA[0];
 *   2. for each sector i-1: save its last word in USA[i], then put the new USN
 *      there.
 *
 * Only USA[0..count-1] and the last word of each sector change; every other
 * byte of the page is as the caller left it. Returns 0 or -1 for a bad geometry.
 */
static int usa_protect(uint8_t *page, uint32_t size)
{
	uint32_t usa_ofs = get16(page + RP_USA_OFS);
	uint32_t usa_count = get16(page + RP_USA_COUNT);
	uint16_t usn;
	uint32_t i;

	if (usa_geometry_ok(size, usa_ofs, usa_count))
		return -1;
	usn = (uint16_t)(get16(page + usa_ofs) + 1);
	if (usn == 0xffff || usn == 0)
		usn = 1;
	put16(page + usa_ofs, usn);
	for (i = 1; i < usa_count; i++) {
		memcpy(page + usa_ofs + 2 * i, page + i * SECTOR - 2, 2);
		put16(page + i * SECTOR - 2, usn);
	}
	return 0;
}

/* ======================================================================== */
/* Restart pages                                                            */
/* ======================================================================== */

struct rpage {
	int64_t pos;   /* byte offset in $LogFile */
	uint32_t size; /* system_page_size, also the size of buf */
	uint8_t buf[PAGE_MAX]; /* UNPROTECTED page: edit here */
};

static uint32_t ra_ofs(const struct rpage *pg) { return get16(pg->buf + RP_RESTART_AREA_OFS); }
static uint8_t *ra(struct rpage *pg) { return pg->buf + ra_ofs(pg); }
static const uint8_t *ra_c(const struct rpage *pg) { return pg->buf + ra_ofs(pg); }

/*
 * Check the fields of an UNPROTECTED page that the editing code dereferences.
 * Mirrors ntfs_check_restart_page_header() and ntfs_check_restart_area() (the
 * engine has the last word: the result is re-checked by engine_verdict() after
 * every edit), so that no offset taken from the image can point outside buf or
 * into a sector tail. The USA geometry was already checked by usa_unprotect().
 */
static int page_check(const struct rpage *pg, const char **why)
{
	uint32_t ofs = ra_ofs(pg);
	uint32_t usa_end = get16(pg->buf + RP_USA_OFS) + 2u * get16(pg->buf + RP_USA_COUNT);
	uint32_t ca, ra_len, clients;

	if (ofs % 8 || ofs < usa_end || ofs + RA_SIZE > SECTOR - 2) {
		*why = "restart area offset out of range";
		return -1;
	}
	ca = get16(pg->buf + ofs + RA_CLIENT_ARRAY_OFS);
	clients = get16(pg->buf + ofs + RA_LOG_CLIENTS);
	ra_len = ca + clients * LC_SIZE;
	if (ca % 8 || ofs + ca > SECTOR - 2 || ofs + ra_len > pg->size ||
	    ra_len > get16(pg->buf + ofs + RA_RESTART_AREA_LENGTH)) {
		*why = "client array out of range";
		return -1;
	}
	return 0;
}

/*
 * Read the restart page at @pos from $LogFile attribute @na, check it and
 * remove the update sequence protection. @expect_size is 0 for the first page
 * (the page learns its size from the header) and the first page's
 * system_page_size for the second.
 */
static int page_load(ntfs_attr *na, int64_t pos, uint32_t expect_size, struct rpage *pg,
		     const char **why)
{
	uint8_t head[SECTOR];
	uint32_t size, log_size;
	int16_t major, minor;

	if (ntfs_attr_pread(na, pos, SECTOR, head) != SECTOR) {
		*why = "cannot read restart page header";
		return -1;
	}
	if (get32(head + RP_MAGIC) != MAGIC_RSTR) {
		*why = "no RSTR magic (all-0xff, CHKD or damaged page)";
		return -1;
	}
	size = get32(head + RP_SYSTEM_PAGE_SIZE);
	if (size < PAGE_MIN || size > PAGE_MAX || (size & (size - 1))) {
		*why = "unsupported system_page_size";
		return -1;
	}
	if (expect_size ? size != expect_size : pos != 0) {
		*why = "restart page at an unexpected position";
		return -1;
	}
	log_size = get32(head + RP_LOG_PAGE_SIZE);
	if (log_size < PAGE_MIN || (log_size & (log_size - 1))) {
		*why = "unsupported log_page_size";
		return -1;
	}
	minor = (int16_t)get16(head + RP_MINOR_VER);
	major = (int16_t)get16(head + RP_MAJOR_VER);
	if (!((major == 1 && minor == 1) || (major == 2 && minor == 0))) {
		*why = "restart page version is neither 1.1 nor 2.0";
		return -1;
	}
	if (usa_geometry_ok(size, get16(head + RP_USA_OFS), get16(head + RP_USA_COUNT))) {
		*why = "bad update sequence array geometry";
		return -1;
	}
	if (ntfs_attr_pread(na, pos, size, pg->buf) != (s64)size) {
		*why = "cannot read the whole restart page";
		return -1;
	}
	pg->pos = pos;
	pg->size = size;
	if (usa_unprotect(pg->buf, size)) {
		*why = "torn restart page (a sector tail does not carry the USN)";
		return -1;
	}
	return page_check(pg, why);
}

/* Protect a copy of @pg and write it back; @pg stays unprotected. */
static int page_store(ntfs_attr *na, const struct rpage *pg)
{
	static uint8_t out[PAGE_MAX];

	memcpy(out, pg->buf, pg->size);
	if (usa_protect(out, pg->size))
		return -1;
	return ntfs_attr_pwrite(na, pg->pos, pg->size, out) == (s64)pg->size ? 0 : -1;
}

/* Number of bits needed to hold @v (bit length). */
static unsigned bit_length(uint64_t v)
{
	unsigned n = 0;

	while (v) {
		v >>= 1;
		n++;
	}
	return n;
}

/* ---- the three edits, on unprotected pages ---- */

/* Is a log client in use (the log is "open")? */
static int client_in_use(const struct rpage *pg)
{
	return get16(ra_c(pg) + RA_CLIENT_IN_USE_LIST) != NO_CLIENT;
}

static void edit_unclean(struct rpage *pg)
{
	uint16_t f = get16(ra(pg) + RA_FLAGS);

	put16(ra(pg) + RA_FLAGS, (uint16_t)(f & ~VOLUME_IS_CLEAN));
}

static void edit_v2(struct rpage *pg)
{
	put16(pg->buf + RP_MAJOR_VER, 2);
	put16(pg->buf + RP_MINOR_VER, 0);
}

static void edit_lsn(struct rpage *pg, uint64_t lsn)
{
	put64(ra(pg) + RA_CURRENT_LSN, lsn);
}

/*
 * Build the page of a clean, open, version 1.1 log in @pg. The page is
 * unprotected on return, with USN 0 (usa_protect() makes it @usn_seed + 1).
 * Everything outside the header, the restart area and the one client record is
 * zero. NTFS-3G requires the zeros: an all-0xff 512 byte block inside the page
 * would end its search for the second restart page.
 */
static void page_build(struct rpage *pg, int64_t pos, uint64_t file_size, uint64_t lsn,
		       uint16_t usn_seed)
{
	uint8_t *p, *c;

	memset(pg->buf, 0, LOG_PAGE_SIZE);
	pg->pos = pos;
	pg->size = LOG_PAGE_SIZE;
	p = pg->buf;
	put32(p + RP_MAGIC, MAGIC_RSTR);
	put16(p + RP_USA_OFS, RP_USN);
	put16(p + RP_USA_COUNT, 1 + LOG_PAGE_SIZE / SECTOR);
	put32(p + RP_SYSTEM_PAGE_SIZE, LOG_PAGE_SIZE);
	put32(p + RP_LOG_PAGE_SIZE, LOG_PAGE_SIZE);
	put16(p + RP_RESTART_AREA_OFS, 0x30); /* right after the 9 word USA */
	put16(p + RP_MINOR_VER, 1);
	put16(p + RP_MAJOR_VER, 1);
	put16(p + RP_USN, usn_seed);

	p = pg->buf + 0x30; /* the restart area */
	put64(p + RA_CURRENT_LSN, lsn);
	put16(p + RA_LOG_CLIENTS, 1);
	put16(p + RA_CLIENT_FREE_LIST, NO_CLIENT);
	put16(p + RA_CLIENT_IN_USE_LIST, 0);
	put16(p + RA_FLAGS, VOLUME_IS_CLEAN);
	put32(p + RA_SEQ_NUMBER_BITS, 67 - bit_length(file_size)); /* engine's own check */
	put16(p + RA_CLIENT_ARRAY_OFS, 0x40); /* WinXP and later: 16 bytes after the area */
	put16(p + RA_RESTART_AREA_LENGTH, 0x40 + LC_SIZE);
	put64(p + RA_FILE_SIZE, file_size);
	put32(p + RA_LAST_LSN_DATA_LENGTH, 0);
	put16(p + RA_LOG_RECORD_HEADER_LENGTH, 0x30);
	put16(p + RA_LOG_PAGE_DATA_OFFSET, 0x40);
	put32(p + RA_RESTART_LOG_OPEN_COUNT, 1);

	c = p + 0x40; /* the single client, "NTFS" */
	put64(c + LC_OLDEST_LSN, lsn);
	put64(c + LC_CLIENT_RESTART_LSN, lsn);
	put16(c + LC_PREV_CLIENT, NO_CLIENT);
	put16(c + LC_NEXT_CLIENT, NO_CLIENT);
	put16(c + LC_SEQ_NUMBER, 0);
	put32(c + LC_CLIENT_NAME_LENGTH, 8);
	put16(c + LC_CLIENT_NAME + 0, 'N');
	put16(c + LC_CLIENT_NAME + 2, 'T');
	put16(c + LC_CLIENT_NAME + 4, 'F');
	put16(c + LC_CLIENT_NAME + 6, 'S');
}

/* ======================================================================== */
/* Volume access and the engine's verdict                                   */
/* ======================================================================== */

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

static ntfs_attr *open_logfile(ntfs_volume *vol, ntfs_inode **nip)
{
	ntfs_attr *na;

	*nip = ntfs_inode_open(vol, FILE_LogFile);
	if (!*nip)
		die("cannot open $LogFile inode (%s)", strerror(errno));
	na = ntfs_attr_open(*nip, AT_DATA, AT_UNNAMED, 0);
	if (!na)
		die("cannot open $LogFile $DATA (%s)", strerror(errno));
	if (!NAttrNonResident(na))
		die("$LogFile $DATA is resident");
	return na;
}

static void close_logfile(ntfs_volume *vol, ntfs_inode *ni, ntfs_attr *na)
{
	ntfs_attr_close(na);
	if (ntfs_inode_close(ni))
		die("closing $LogFile inode failed (%s)", strerror(errno));
	if (ntfs_umount(vol, FALSE))
		die("unmounting the image failed (%s)", strerror(errno));
}

/*
 * The engine's verdict, computed as ntfs_volume_check_logfile() in volume.c
 * does it: "invalid" is what makes it fail with EOPNOTSUPP because the check
 * itself fails; "v2" is the version 2.0 rule (EPERM); "unclean" is
 * ntfs_is_logfile_clean() being false (EOPNOTSUPP); "empty" is the all-0xff log
 * (NVolLogFileEmpty, clean). Order of precedence as in the engine: v2 wins.
 */
static const char *engine_verdict(ntfs_attr *na, int *have_lsn, uint64_t *lsn)
{
	RESTART_PAGE_HEADER *rp = NULL;
	const char *v;

	*have_lsn = 0;
	if (!ntfs_check_logfile(na, &rp))
		return "invalid";
	if (rp) {
		/* the page the engine chose: the newer valid one (page 0 on a tie) */
		const u8 *area = (const u8 *)rp + le16_to_cpu(rp->restart_area_offset);

		*lsn = get64(area + RA_CURRENT_LSN);
		*have_lsn = 1;
	}
	if (rp && sle16_to_cpu(rp->major_ver) == 2 && sle16_to_cpu(rp->minor_ver) == 0)
		v = "v2";
	else if (!ntfs_is_logfile_clean(na, rp))
		v = "unclean";
	else
		v = rp ? "clean" : "empty";
	free(rp);
	return v;
}

/* ---- state report ---- */

struct state {
	int have_pages;
	struct rpage pg[2];
	const char *verdict;
	int have_lsn;      /* the engine returned a restart page ... */
	uint64_t engine_lsn; /* ... and this is its current_lsn */
};

static void state_load(ntfs_attr *na, struct state *st)
{
	const char *why = "";

	st->verdict = engine_verdict(na, &st->have_lsn, &st->engine_lsn);
	st->have_pages = 0;
	if (!strcmp(st->verdict, "empty") || !strcmp(st->verdict, "invalid"))
		return;
	if (page_load(na, 0, 0, &st->pg[0], &why))
		die("restart page 0: %s", why);
	if (page_load(na, st->pg[0].size, st->pg[0].size, &st->pg[1], &why))
		die("restart page 1: %s", why);
	st->have_pages = 1;
}

static uint64_t page_lsn(const struct rpage *pg) { return get64(ra_c(pg) + RA_CURRENT_LSN); }

static void state_print(const struct state *st)
{
	int i;

	if (st->have_pages) {
		for (i = 0; i < 2; i++) {
			const struct rpage *pg = &st->pg[i];

			printf("page%d off=%lld magic=RSTR version=%d.%d usn=%u flags=0x%04x "
			       "clients_in_use=%d current_lsn=0x%016llx seq_bits=%u file_size=%llu\n",
			       i, (long long)pg->pos, (int)(int16_t)get16(pg->buf + RP_MAJOR_VER),
			       (int)(int16_t)get16(pg->buf + RP_MINOR_VER), get16(pg->buf + RP_USN),
			       get16(ra_c(pg) + RA_FLAGS), client_in_use(pg),
			       (unsigned long long)page_lsn(pg), get32(ra_c(pg) + RA_SEQ_NUMBER_BITS),
			       (unsigned long long)get64(ra_c(pg) + RA_FILE_SIZE));
		}
		/* the probe contract: current_lsn of the newer VALID page, as the engine picks it */
		if (st->have_lsn)
			printf("current_lsn=0x%016llx\n", (unsigned long long)st->engine_lsn);
	}
	printf("log=%s\n", st->verdict);
}

/* Re-open the image read-only, load the state fresh from disk. */
static void state_from_image(const char *path, struct state *st)
{
	ntfs_volume *vol = open_image(path, 0);
	ntfs_inode *ni;
	ntfs_attr *na = open_logfile(vol, &ni);

	state_load(na, st);
	close_logfile(vol, ni, na);
}

/* ======================================================================== */
/* Selftest of the USA code against the library                             */
/* ======================================================================== */

static void st_fail(const char *what)
{
	fprintf(stderr, "logfile_mark: selftest FAILED: %s\n", what);
	exit(1);
}

static void st_check(int ok, const char *what)
{
	if (!ok)
		st_fail(what);
}

/* One page with non-zero junk in every sector tail, so the saved words matter. */
static void st_page(uint8_t *page, uint32_t size, uint16_t usn)
{
	uint32_t i, x = 0x12345678u + usn;

	for (i = 0; i < size; i++) {
		x = x * 1664525u + 1013904223u;
		page[i] = (uint8_t)(x >> 24);
	}
	memset(page, 0, RP_HEADER_SIZE);
	memcpy(page, "RSTR", 4);
	put16(page + RP_USA_OFS, RP_USN);
	put16(page + RP_USA_COUNT, (uint16_t)(1 + size / SECTOR));
	put16(page + RP_USN, usn);
}

static int selftest(void)
{
	static const uint16_t usns[] = {0, 1, 7, 0x1234, 0xfffd, 0xfffe, 0xffff};
	static const uint32_t sizes[] = {512, 1024, 4096, 8192};
	uint8_t orig[8192], mine[8192], lib[8192], back[8192];
	size_t ui, si;
	uint32_t i, ti, size, usa_bytes;

	ntfs_log_set_handler(ntfs_log_handler_null); /* the torn cases make the library complain */

	for (si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
		size = sizes[si];
		for (ui = 0; ui < sizeof usns / sizeof usns[0]; ui++) {
			uint16_t want_usn = (uint16_t)(usns[ui] + 1);

			if (want_usn == 0xffff || want_usn == 0)
				want_usn = 1;
			st_page(orig, size, usns[ui]);

			/* protect: byte-identical to the library's pre-write fix-up */
			memcpy(mine, orig, size);
			memcpy(lib, orig, size);
			st_check(usa_protect(mine, size) == 0, "usa_protect rejected a valid page");
			st_check(ntfs_mst_pre_write_fixup((NTFS_RECORD *)lib, size) == 0,
				 "library pre-write fix-up failed");
			st_check(memcmp(mine, lib, size) == 0, "usa_protect differs from the library");
			st_check(get16(mine + RP_USN) == want_usn, "USN not incremented as specified");
			for (i = 1; i < 1u + size / SECTOR; i++) {
				st_check(get16(mine + i * SECTOR - 2) == want_usn,
					 "sector tail does not carry the USN");
				st_check(memcmp(mine + RP_USN + 2 * i, orig + i * SECTOR - 2, 2) == 0,
					 "saved word is not the original sector tail");
			}

			/*
			 * unprotect: restores every data byte. The USN word and the USA
			 * are header bytes that stay in the buffer, so compare the rest.
			 */
			memcpy(back, mine, size);
			st_check(usa_unprotect(back, size) == 0, "usa_unprotect rejected a protected page");
			usa_bytes = 2 * (1 + size / SECTOR);
			st_check(memcmp(back, orig, RP_USN) == 0 &&
					 memcmp(back + RP_USN + usa_bytes, orig + RP_USN + usa_bytes,
						size - RP_USN - usa_bytes) == 0,
				 "usa_unprotect did not restore the page");

			/* and agrees with the library's post-read fix-up */
			memcpy(back, mine, size);
			memcpy(lib, mine, size);
			st_check(usa_unprotect(back, size) == 0, "usa_unprotect failed");
			st_check(ntfs_mst_post_read_fixup((NTFS_RECORD *)lib, size) == 0,
				 "library post-read fix-up failed");
			st_check(memcmp(back, lib, size) == 0, "usa_unprotect differs from the library");

			/*
			 * a torn page (one sector tail not the USN) is detected and left
			 * untouched, for the first, the second and the last sector tail
			 * (a 512 byte page has only the first), as the library does
			 */
			for (ti = 0; ti < 3; ti++) {
				i = ti == 0 ? 1 : ti == 1 ? 2 : size / SECTOR; /* sector i-1's tail */
				if (i > size / SECTOR)
					continue;
				memcpy(back, mine, size);
				put16(back + i * SECTOR - 2, (uint16_t)(want_usn ^ 0x5555));
				memcpy(lib, back, size);
				st_check(usa_unprotect(back, size) == -1, "torn page not detected");
				st_check(memcmp(back, lib, size) == 0, "torn page was modified");
				st_check(ntfs_mst_post_read_fixup_warn((NTFS_RECORD *)lib, size, FALSE) == -1,
					 "library accepts the torn page");
			}
		}
	}
	/*
	 * a USA that does not start at offset 30 (4096 byte page, USA at 40), and the
	 * largest page we accept (65536 bytes, 129 USA words, USA at 30): both must
	 * match the library byte for byte
	 */
	{
		static const uint32_t psz[] = {4096, PAGE_MAX};
		static const uint32_t uofs[] = {40, RP_USN};
		size_t k;

		for (k = 0; k < 2; k++) {
			uint8_t *a = malloc(PAGE_MAX), *b = malloc(PAGE_MAX), *c = malloc(PAGE_MAX);

			st_check(a && b && c, "out of memory");
			st_page(a, psz[k], 5);
			put16(a + RP_USA_OFS, (uint16_t)uofs[k]);
			put16(a + uofs[k], 5); /* the USN word moves with the USA */
			memcpy(b, a, psz[k]);
			memcpy(c, a, psz[k]);
			st_check(usa_protect(b, psz[k]) == 0, "usa_protect rejected page");
			st_check(ntfs_mst_pre_write_fixup((NTFS_RECORD *)c, psz[k]) == 0,
				 "library pre-write fix-up failed");
			st_check(memcmp(b, c, psz[k]) == 0, "usa_protect differs from the library");
			memcpy(c, b, psz[k]);
			st_check(usa_unprotect(b, psz[k]) == 0, "usa_unprotect failed");
			st_check(ntfs_mst_post_read_fixup((NTFS_RECORD *)c, psz[k]) == 0,
				 "library post-read fix-up failed");
			st_check(memcmp(b, c, psz[k]) == 0, "usa_unprotect differs from the library");
			free(a);
			free(b);
			free(c);
		}
	}
	/* geometry guards, protect and unprotect */
	st_page(orig, 4096, 1);
	put16(orig + RP_USA_COUNT, 8);
	st_check(usa_protect(orig, 4096) == -1, "wrong usa_count accepted");
	st_page(orig, 4096, 1);
	put16(orig + RP_USA_OFS, 504);
	st_check(usa_protect(orig, 4096) == -1, "usa reaching the sector tail accepted");
	st_check(usa_unprotect(orig, 4096) == -1, "usa reaching the sector tail accepted (unprotect)");
	st_page(orig, 4096, 1);
	put16(orig + RP_USA_COUNT, 8);
	st_check(usa_unprotect(orig, 4096) == -1, "wrong usa_count accepted (unprotect)");
	st_page(orig, 4096, 1);
	put16(orig + RP_USA_OFS, 29);
	st_check(usa_protect(orig, 4096) == -1 && usa_unprotect(orig, 4096) == -1,
		 "odd usa_ofs accepted");
	puts("selftest ok");
	return 0;
}

/* ======================================================================== */
/* main                                                                     */
/* ======================================================================== */

static void usage(void)
{
	fputs("usage: logfile_mark IMAGE [--init] [--unclean] [--v2] [--bump-lsn [--steps N]]\n"
	      "       logfile_mark IMAGE --dump\n"
	      "       logfile_mark --selftest\n",
	      stderr);
	exit(2);
}

int main(int argc, char **argv)
{
	const char *path = NULL;
	int do_init = 0, do_unclean = 0, do_v2 = 0, do_bump = 0, do_dump = 0;
	unsigned long steps = 1;
	int steps_given = 0;
	int i;

	ntfs_log_set_handler(ntfs_log_handler_stderr);
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--selftest"))
			return argc == 2 ? selftest() : (usage(), 2);
		else if (!strcmp(argv[i], "--init"))
			do_init = 1;
		else if (!strcmp(argv[i], "--unclean"))
			do_unclean = 1;
		else if (!strcmp(argv[i], "--v2"))
			do_v2 = 1;
		else if (!strcmp(argv[i], "--bump-lsn"))
			do_bump = 1;
		else if (!strcmp(argv[i], "--dump"))
			do_dump = 1;
		else if (!strcmp(argv[i], "--steps") && i + 1 < argc) {
			char *end;

			errno = 0;
			steps_given = 1;
			steps = strtoul(argv[++i], &end, 10);
			if (errno || *end || !steps || steps > 1000000)
				usage();
		} else if (argv[i][0] != '-' && !path)
			path = argv[i];
		else
			usage();
	}
	if (steps_given && !do_bump)
		usage();
	if (!path || (do_dump && (do_init || do_unclean || do_v2 || do_bump)) ||
	    (!do_dump && !do_init && !do_unclean && !do_v2 && !do_bump))
		usage();

	if (do_dump) {
		struct state st;

		state_from_image(path, &st);
		state_print(&st);
		return 0;
	}

	{
		ntfs_volume *vol = open_image(path, 1);
		ntfs_inode *ni;
		ntfs_attr *na = open_logfile(vol, &ni);
		struct state st;
		const char *want_verdict;
		char before[16];
		uint64_t want_lsn[2] = {0, 0};
		int k, empty;

		state_load(na, &st);
		empty = !strcmp(st.verdict, "empty");
		snprintf(before, sizeof before, "%s", st.verdict);

		if (do_init) {
			uint8_t *z;
			uint64_t file_size = (uint64_t)na->data_size & ~(uint64_t)(LOG_PAGE_SIZE - 1);
			unsigned shift;

			if (!empty)
				die("--init needs an empty ($LogFile all 0xff) log, found: %s", st.verdict);
			if (file_size < (2 + 48) * (uint64_t)LOG_PAGE_SIZE)
				die("$LogFile is too small for a restart page pair");
			z = malloc(2 * LOG_PAGE_SIZE);
			if (!z || ntfs_attr_pread(na, 0, 2 * LOG_PAGE_SIZE, z) != 2 * LOG_PAGE_SIZE)
				die("cannot read the first two log pages");
			for (k = 0; k < 2 * (int)LOG_PAGE_SIZE; k++)
				if (z[k] != 0xff)
					die("first two log pages are not all 0xff");
			free(z);
			shift = 64 - (67 - bit_length(file_size)); /* bits of the offset part */
			/* sequence number 2, offset of the first log record page (page 2) */
			want_lsn[0] = want_lsn[1] = (2ull << shift) | ((2ull * LOG_PAGE_SIZE) >> 3);
			page_build(&st.pg[0], 0, file_size, want_lsn[0], 0);
			page_build(&st.pg[1], LOG_PAGE_SIZE, file_size, want_lsn[1], 1);
			st.have_pages = 1;
		} else if (!st.have_pages) {
			die("$LogFile has no restart pages (log is %s); use --init first", st.verdict);
		}

		if (do_unclean) {
			if (!strcmp(before, "v2") && !do_v2)
				die("the log is version 2.0 (the engine reports v2 whatever the clean "
				    "bit says); refusing --unclean");
			if (!client_in_use(&st.pg[0]) || !client_in_use(&st.pg[1]))
				die("no log client in use: clearing the clean bit would not make the "
				    "log unclean");
			edit_unclean(&st.pg[0]);
			edit_unclean(&st.pg[1]);
		}
		if (do_v2) {
			edit_v2(&st.pg[0]);
			edit_v2(&st.pg[1]);
		}
		if (do_bump) {
			/*
			 * Each page moves by the same number of sequence steps from ITS OWN
			 * LSN, so their order (which page the engine picks, and with it the
			 * verdict) does not change. Equal LSNs would make the engine pick
			 * page 0 whatever the pages say.
			 */
			uint64_t l0 = page_lsn(&st.pg[0]), l1 = page_lsn(&st.pg[1]);
			uint64_t top = l0 > l1 ? l0 : l1, step;
			unsigned bits = get32(ra_c(&st.pg[0]) + RA_SEQ_NUMBER_BITS);

			if (bits < 2 || bits > 63 || bits != get32(ra_c(&st.pg[1]) + RA_SEQ_NUMBER_BITS))
				die("unusable seq_number_bits");
			step = 1ull << (64 - bits); /* one step of the sequence number */
			if (steps > (uint64_t)INT64_MAX / step ||
			    top > (uint64_t)INT64_MAX - steps * step)
				die("current_lsn would overflow");
			want_lsn[0] = l0 + steps * step;
			want_lsn[1] = l1 + steps * step;
			edit_lsn(&st.pg[0], want_lsn[0]);
			edit_lsn(&st.pg[1], want_lsn[1]);
		}

		/* verdict the edits promise */
		want_verdict = (do_v2 || !strcmp(before, "v2")) ? "v2"
			     : do_unclean ? "unclean" : do_init ? "clean" : before;

		for (k = 0; k < 2; k++)
			if (page_store(na, &st.pg[k]))
				die("writing restart page %d failed (%s)", k, strerror(errno));
		close_logfile(vol, ni, na);

		/* read everything back from the image and let the engine judge */
		{
			struct state after;

			state_from_image(path, &after);
			if (!after.have_pages)
				die("verification: engine finds no usable log (%s)", after.verdict);
			if (strcmp(after.verdict, want_verdict))
				die("verification: engine says %s, wanted %s", after.verdict, want_verdict);
			for (k = 0; k < 2; k++)
				if (want_lsn[k] && page_lsn(&after.pg[k]) != want_lsn[k])
					die("verification: page %d current_lsn is not 0x%016llx", k,
					    (unsigned long long)want_lsn[k]);
			state_print(&after);
		}
	}
	return 0;
}
