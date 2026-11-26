/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Directory walk for check 5 (hiberfil.sys) and check 5a (names that are not
 * NFC). See names.h.
 *
 * The walk is breadth first over a table of directories, not recursive: a
 * directory found during a listing is appended to the table and listed later,
 * so the depth of the tree costs no stack and a loop in the index structure
 * (a directory reachable twice) is cut by a set of record numbers. Every limit
 * that stops the walk (directory count, name memory, time) makes it
 * incomplete, which the caller reports as `unknown`, never as `clean`.
 *
 * Entries come from ntfs_readdir() with the volume as ntfs_device_mount() leaves
 * it: case sensitive, hidden and system names shown, so every Win32 and POSIX
 * name of the index reaches the callback. DOS (8.3) names are skipped: they are
 * ASCII, and the listing of a Mac mount skips them too.
 */
#include "posix.h"
#include "names.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <ntfs-3g/dir.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/attrib.h>
#include <ntfs-3g/layout.h>
#include <ntfs-3g/unistr.h>

#include "nfc.h"
#include "probe_core.h"
#include "text.h"

#define MAX_DIRS (1u << 22)          /* 4 Mi directories */
#define MAX_ARENA_UNITS (1u << 25)   /* 32 Mi UTF-16 units (64 MiB) of directory names */
#define MAX_HIBER_CANDIDATES 16
#define HIBER_HEADER 4096

typedef struct dir_ent {
	uint64_t rec;      /* MFT record number */
	uint32_t parent;   /* index of the parent in the table, UINT32_MAX for the root */
	uint32_t name_off; /* into the name arena */
	uint16_t name_len;
} dir_ent;

typedef struct scan {
	ntfs_volume *vol;
	names_result *r;
	int64_t budget_ms;
	struct timespec t0;
	unsigned ticks;
	bool failed;

	dir_ent *dirs;
	size_t ndirs, cap_dirs;
	uint16_t *arena;
	size_t arena_len, arena_cap;
	uint64_t *seen; /* open addressing, entries are record number + 1, 0 is empty */
	size_t seen_cap, seen_count;
	size_t cur; /* the directory being listed */

	uint64_t hib[MAX_HIBER_CANDIDATES];
	unsigned nhib;
	bool hib_overflow;
	ntfschar hibname[12];
} scan;

static const char hiberfile_name[] = "hiberfil.sys";

/* ---- time --------------------------------------------------------------- */

static bool over_budget(scan *s)
{
	struct timespec now;
	int64_t ms;

	if (s->budget_ms == 0)
		return true;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return true; /* no clock: stop rather than run unbounded */
	ms = (int64_t)(now.tv_sec - s->t0.tv_sec) * 1000 + (now.tv_nsec - s->t0.tv_nsec) / 1000000;
	return ms >= s->budget_ms;
}

/* ---- set of record numbers ---------------------------------------------- */

static size_t slot(uint64_t key, size_t cap)
{
	return (size_t)((key * UINT64_C(0x9E3779B97F4A7C15)) >> 20) & (cap - 1);
}

static bool seen_grow(scan *s)
{
	size_t ncap = s->seen_cap ? s->seen_cap * 2 : 1024, i;
	uint64_t *n = calloc(ncap, sizeof(*n));

	if (!n)
		return false;
	for (i = 0; i < s->seen_cap; i++) {
		size_t j;

		if (!s->seen[i])
			continue;
		j = slot(s->seen[i], ncap);
		while (n[j])
			j = (j + 1) & (ncap - 1);
		n[j] = s->seen[i];
	}
	free(s->seen);
	s->seen = n;
	s->seen_cap = ncap;
	return true;
}

/* 1 if inserted, 0 if already there, -1 on memory failure. */
static int seen_insert(scan *s, uint64_t rec)
{
	uint64_t key = rec + 1;
	size_t j;

	if ((s->seen_count + 1) * 2 > s->seen_cap && !seen_grow(s))
		return -1;
	j = slot(key, s->seen_cap);
	while (s->seen[j]) {
		if (s->seen[j] == key)
			return 0;
		j = (j + 1) & (s->seen_cap - 1);
	}
	s->seen[j] = key;
	s->seen_count++;
	return 1;
}

/* ---- directory table ----------------------------------------------------- */

static bool add_dir(scan *s, uint64_t rec, uint32_t parent, const uint16_t *name, size_t len)
{
	dir_ent *d;
	int ins = seen_insert(s, rec);

	if (ins < 0)
		return false;
	if (ins == 0)
		return true; /* listed already or queued: nothing to add */
	if (s->ndirs >= MAX_DIRS || s->arena_len + len > MAX_ARENA_UNITS)
		return false;
	if (s->ndirs == s->cap_dirs) {
		size_t ncap = s->cap_dirs ? s->cap_dirs * 2 : 256;
		dir_ent *n = realloc(s->dirs, ncap * sizeof(*n));

		if (!n)
			return false;
		s->dirs = n;
		s->cap_dirs = ncap;
	}
	if (s->arena_len + len > s->arena_cap) {
		size_t ncap = s->arena_cap ? s->arena_cap * 2 : 4096;
		uint16_t *n;

		while (ncap < s->arena_len + len)
			ncap *= 2;
		n = realloc(s->arena, ncap * sizeof(*n));
		if (!n)
			return false;
		s->arena = n;
		s->arena_cap = ncap;
	}
	d = &s->dirs[s->ndirs++];
	d->rec = rec;
	d->parent = parent;
	d->name_off = (uint32_t)s->arena_len;
	d->name_len = (uint16_t)len;
	if (len)
		memcpy(s->arena + s->arena_len, name, len * sizeof(*name));
	s->arena_len += len;
	return true;
}

/*
 * "/a/b/name" of an entry in directory `dir`, as UTF-8 cut at 512 bytes on a
 * code point boundary. The chain of parents is a tree built by the walk, so
 * it ends at the root.
 */
static void build_path(const scan *s, size_t dir, const uint16_t *name, size_t len, char *out)
{
	size_t depth = 0, i, n = 0, outlen = 0;
	uint32_t *chain;
	char tmp[NTFSMAC_NFC_MAX_UNITS * 4 + 1];

	for (i = dir; s->dirs[i].parent != UINT32_MAX; i = s->dirs[i].parent)
		depth++;
	chain = depth ? malloc(depth * sizeof(*chain)) : NULL;
	out[0] = '\0';
	if (depth && !chain) { /* out of memory: say what we know */
		probe_utf8_append(out, 0, NTFSMAC_PROBE_NONNFC_PATH_MAX_BYTES + 1, "/", 1);
		return;
	}
	for (i = dir; s->dirs[i].parent != UINT32_MAX; i = s->dirs[i].parent)
		chain[n++] = (uint32_t)i;
	while (n > 0 && outlen < NTFSMAC_PROBE_NONNFC_PATH_MAX_BYTES) {
		const dir_ent *d = &s->dirs[chain[--n]];
		size_t k = probe_utf16_to_utf8(s->arena + d->name_off, d->name_len, tmp, sizeof(tmp));

		outlen = probe_utf8_append(out, outlen, NTFSMAC_PROBE_NONNFC_PATH_MAX_BYTES + 1, "/", 1);
		outlen = probe_utf8_append(out, outlen, NTFSMAC_PROBE_NONNFC_PATH_MAX_BYTES + 1, tmp, k);
	}
	free(chain);
	if (outlen < NTFSMAC_PROBE_NONNFC_PATH_MAX_BYTES) {
		size_t k = probe_utf16_to_utf8(name, len, tmp, sizeof(tmp));

		outlen = probe_utf8_append(out, outlen, NTFSMAC_PROBE_NONNFC_PATH_MAX_BYTES + 1, "/", 1);
		(void)probe_utf8_append(out, outlen, NTFSMAC_PROBE_NONNFC_PATH_MAX_BYTES + 1, tmp, k);
	}
}

/* ---- the listing callback ------------------------------------------------ */

static int fill(void *ctx, const ntfschar *name, const int name_len, const int name_type,
		const s64 pos, const MFT_REF mref, const unsigned dt_type)
{
	scan *s = ctx;
	uint16_t u[NTFSMAC_NFC_MAX_UNITS];
	int i, nfc;
	unsigned type;

	(void)pos;
	if (name_len <= 0 || name_len > (int)NTFSMAC_NFC_MAX_UNITS)
		return 0; /* the library never delivers these; nothing to compare */
	for (i = 0; i < name_len; i++)
		u[i] = le16_to_cpu(name[i]);
	if ((name_len == 1 && u[0] == '.') || (name_len == 2 && u[0] == '.' && u[1] == '.'))
		return 0; /* the "." and ".." entries ntfs_readdir() makes up */

	if ((++s->ticks & 0xFFu) == 0 && over_budget(s)) {
		s->failed = true;
		return -1;
	}

	/*
	 * Check 5: hiberfil.sys in the root directory, any case, in every namespace. It must find
	 * every file that ntfs_volume_check_hiberfile() finds (volmark and the engine use it): that
	 * one looks the name up by collation in the root index whatever namespace the entry has, so
	 * a DOS (8.3) name counts here too (T-M7-18). Its lookup is case sensitive on a mount
	 * without ignore_case; this check ignores case, so it finds more, never less. A record with
	 * a Win32 and a DOS name is one candidate, not two.
	 */
	if (s->cur == 0 && name_len == (int)(sizeof(hiberfile_name) - 1) &&
	    ntfs_names_are_equal(name, (size_t)name_len, s->hibname, sizeof(hiberfile_name) - 1,
				 IGNORE_CASE, s->vol->upcase, s->vol->upcase_len)) {
		unsigned k = 0;

		while (k < s->nhib && s->hib[k] != MREF(mref))
			k++;
		if (k < s->nhib)
			; /* the other name of a record that is listed already */
		else if (s->nhib < MAX_HIBER_CANDIDATES)
			s->hib[s->nhib++] = MREF(mref);
		else
			s->hib_overflow = true;
	}

	/*
	 * A DOS name is the short alias of a Win32 name (or of nothing, on a crafted volume). It is
	 * not a name a user sees: checks 5a and later, and the directory walk, take the Win32 or POSIX
	 * name of the record, so a DOS name is neither counted in names_seen nor queued.
	 */
	if (name_type == FILE_NAME_DOS)
		return 0;

	/* Check 5a. */
	s->r->names_seen++;
	nfc = ntfsmac_nfc_is_normalized(u, (size_t)name_len);
	if (nfc == 0) {
		names_result *r = s->r;

		if (r->nonnfc_count < UINT32_MAX)
			r->nonnfc_count++;
		if (r->path_count < NTFSMAC_PROBE_MAX_NONNFC_PATHS)
			build_path(s, s->cur, u, (size_t)name_len, r->paths[r->path_count++]);
	}

	/*
	 * Which entries are directories. ntfs_readdir() gives DT_DIR for a plain directory. For an
	 * entry with the reparse or system attribute it opens the inode and answers DT_LNK or
	 * DT_REPARSE (a junction or symbolic link, which can still be a directory with an index of
	 * its own), or DT_UNKNOWN. DT_UNKNOWN is what it says when the record cannot be opened, but
	 * also, normally, for the view indexes in $Extend ($ObjId, $Quota, $Reparse), which are not
	 * directories. So for all three the record decides: it must open (else the walk fails, the
	 * volume cannot be said to have no more names); a record with MFT_RECORD_IS_DIRECTORY is
	 * queued and its on-disk index walked, never the reparse target; anything else is a leaf.
	 */
	type = dt_type;
	if (type == NTFS_DT_UNKNOWN || type == NTFS_DT_LNK || type == NTFS_DT_REPARSE) {
		ntfs_inode *ni = ntfs_inode_open(s->vol, MK_MREF(MREF(mref), 0));
		bool is_dir;

		if (!ni) {
			s->failed = true;
			return -1;
		}
		is_dir = (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) != 0;
		ntfs_inode_close(ni);
		type = is_dir ? NTFS_DT_DIR : NTFS_DT_REG;
	}
	if (type == NTFS_DT_DIR && !add_dir(s, MREF(mref), (uint32_t)s->cur, u, (size_t)name_len)) {
		s->failed = true;
		return -1;
	}
	return 0;
}

/* ---- hiberfil.sys -------------------------------------------------------- */

static int hiber_rank(ntfsmac_probe_hiberfile h)
{
	switch (h) {
	case NTFSMAC_PROBE_HIBERFILE_HIBERNATED:
		return 3;
	case NTFSMAC_PROBE_HIBERFILE_UNREADABLE:
		return 2;
	case NTFSMAC_PROBE_HIBERFILE_INACTIVE:
		return 1;
	default:
		return 0;
	}
}

static void hiber_note(names_result *r, ntfsmac_probe_hiberfile h, uint32_t reasons)
{
	if (hiber_rank(h) > hiber_rank(r->hiberfile))
		r->hiberfile = h;
	r->hiber_reasons |= reasons;
}

/*
 * The header rule of ntfs_volume_check_hiberfile(): fewer than 4096 bytes, or
 * `hibr`/`HIBR`, is hibernated; the probe also treats `wake`/`WAKE` (a resume
 * in progress) as hibernated, and so is any other non-zero value in the first
 * four bytes (an unknown signature, F-05; the engine's own comment says "all
 * header bytes are zero" for the safe case but it only tests `hibr`/`HIBR`).
 * Only four zero bytes make an inactive file.
 */
static void hiber_eval(scan *s, uint64_t rec)
{
	names_result *r = s->r;
	ntfs_inode *ni;
	ntfs_attr *na;
	char buf[HIBER_HEADER];
	s64 got;

	ni = ntfs_inode_open(s->vol, MK_MREF(rec, 0));
	if (!ni) {
		hiber_note(r, NTFSMAC_PROBE_HIBERFILE_UNREADABLE,
			   NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_HIBER_UNREADABLE));
		return;
	}
	na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
	if (!na) {
		hiber_note(r, NTFSMAC_PROBE_HIBERFILE_UNREADABLE,
			   NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_HIBER_UNREADABLE));
		ntfs_inode_close(ni);
		return;
	}
	got = ntfs_attr_pread(na, 0, HIBER_HEADER, buf);
	if (got < 0) {
		hiber_note(r, NTFSMAC_PROBE_HIBERFILE_UNREADABLE,
			   NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_HIBER_UNREADABLE));
	} else if (got < HIBER_HEADER) {
		hiber_note(r, NTFSMAC_PROBE_HIBERFILE_HIBERNATED,
			   NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_HIBER_SHORT));
	} else if (!memcmp(buf, "hibr", 4) || !memcmp(buf, "HIBR", 4)) {
		hiber_note(r, NTFSMAC_PROBE_HIBERFILE_HIBERNATED,
			   NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_HIBER_SIGNATURE));
	} else if (!memcmp(buf, "wake", 4) || !memcmp(buf, "WAKE", 4)) {
		hiber_note(r, NTFSMAC_PROBE_HIBERFILE_HIBERNATED,
			   NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_HIBER_WAKE));
	} else if (buf[0] != 0 || buf[1] != 0 || buf[2] != 0 || buf[3] != 0) {
		/* Not zeros and not a signature above: unknown, so not inactive (F-05). */
		hiber_note(r, NTFSMAC_PROBE_HIBERFILE_HIBERNATED,
			   NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_HIBER_SIGNATURE));
	} else {
		hiber_note(r, NTFSMAC_PROBE_HIBERFILE_INACTIVE, 0);
	}
	ntfs_attr_close(na);
	ntfs_inode_close(ni);
}

/* ---- the walk ------------------------------------------------------------ */

void names_scan(ntfs_volume *vol, int64_t budget_ms, names_result *r)
{
	scan s;
	unsigned i;

	memset(&s, 0, sizeof(s));
	memset(r, 0, sizeof(*r));
	r->hiberfile = NTFSMAC_PROBE_HIBERFILE_UNREADABLE; /* until the root has been listed */
	s.vol = vol;
	s.r = r;
	s.budget_ms = budget_ms < 0 ? NTFSMAC_PROBE_DEFAULT_SCAN_BUDGET_MS : budget_ms;
	if (clock_gettime(CLOCK_MONOTONIC, &s.t0) != 0)
		goto out;
	for (i = 0; i < sizeof(hiberfile_name) - 1; i++)
		s.hibname[i] = cpu_to_le16((u16)hiberfile_name[i]);
	/* A volume that is not what ntfs_device_mount() leaves would hide names. */
	if (!NVolCaseSensitive(vol) || !NVolShowHidFiles(vol) || !NVolShowSysFiles(vol))
		goto out;
	if (!add_dir(&s, FILE_root, UINT32_MAX, NULL, 0))
		goto out;

	for (s.cur = 0; s.cur < s.ndirs; s.cur++) {
		ntfs_inode *ni;
		s64 pos = 0;
		int rc;

		if (over_budget(&s))
			goto out;
		ni = ntfs_inode_open(vol, MK_MREF(s.dirs[s.cur].rec, 0));
		if (!ni)
			goto out;
		rc = ntfs_readdir(ni, &pos, &s, fill);
		if (ntfs_inode_close(ni) != 0 || rc != 0 || s.failed)
			goto out;
		r->dirs_listed++;
		if (s.cur == 0) {
			/* The root is listed: check 5 can answer. */
			unsigned k;

			r->hiberfile = NTFSMAC_PROBE_HIBERFILE_ABSENT;
			r->hiber_records = s.nhib;
			if (s.hib_overflow)
				hiber_note(r, NTFSMAC_PROBE_HIBERFILE_UNREADABLE,
					   NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_HIBER_UNREADABLE));
			for (k = 0; k < s.nhib; k++)
				hiber_eval(&s, s.hib[k]);
		}
	}
	r->complete = true;
out:
	if (!r->complete && r->hiberfile == NTFSMAC_PROBE_HIBERFILE_UNREADABLE)
		r->hiber_reasons |= NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_HIBER_UNREADABLE);
	free(s.dirs);
	free(s.arena);
	free(s.seen);
}
