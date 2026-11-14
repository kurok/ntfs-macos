/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Session journal (docs/ARCHITECTURE.md section 3.5, ADR-016, ADR-017).
 *
 * Rules kept throughout:
 *  - every file name is formatted from a parsed uint64 serial (and, for
 *    tombstones, an integer time); a name read from the disk is only parsed
 *    into those integers, never reused as a string;
 *  - every open, rename and unlink is relative to a directory descriptor that
 *    was opened with O_DIRECTORY | O_NOFOLLOW, and no path is concatenated;
 *  - every stored entry is parsed strictly and anything else is CORRUPT.
 *
 * On-disk entry (private; LF line endings, fixed order, canonical numbers):
 *   ntfsmac-journal 1
 *   serial <16 upper-case hex>
 *   label <lower-case hex of the label bytes, may be empty>
 *   total_sectors <decimal>
 *   session_id <decimal>
 *   created <decimal>
 *   updated <decimal>
 *   logfile_lsn_at_start <decimal>
 *   marker pending|set|failed|none
 *   state open|write_error
 *   end
 *
 * One process owns a journal directory (the root daemon); update and remove
 * are read-modify-write without a lock.
 */

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include "ntfsmac/journal.h"
#include "ntfsmac/log.h"

#include "journal_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#error "O_CLOEXEC is required"
#endif

#define ENTRY_MAX_BYTES 1024u
#define LIST_MAX_ITEMS 65536u
#define CLEARED_DIR "cleared"

struct ntfsmac_journal {
	int dirfd;
	int clrfd;
};

/* ------------------------------------------------------------------ */
/* Names                                                              */
/* ------------------------------------------------------------------ */

static const char HEX_UPPER[] = "0123456789ABCDEF";
static const char HEX_LOWER[] = "0123456789abcdef";

static void put_hex16(char *out, uint64_t v)
{
	for (int i = 15; i >= 0; i--) {
		out[i] = HEX_UPPER[v & 0xf];
		v >>= 4;
	}
}

static void put_dec(char *out, unsigned v, int width)
{
	for (int i = width - 1; i >= 0; i--) {
		out[i] = (char)('0' + v % 10);
		v /= 10;
	}
}

/* Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant). */
static int64_t days_from_civil(int64_t y, int m, int d)
{
	int64_t era;
	int64_t yoe, doy, doe;

	y -= m <= 2;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = y - era * 400;
	doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
	int64_t era, doe, yoe, doy, mp, yy;

	z += 719468;
	era = (z >= 0 ? z : z - 146096) / 146097;
	doe = z - era * 146097;
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	yy = yoe + era * 400;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	*d = (int)(doy - (153 * mp + 2) / 5 + 1);
	*m = (int)(mp < 10 ? mp + 3 : mp - 9);
	*y = (int)(yy + (*m <= 2));
}

int ntfsmac_journal_format_name(char *out, size_t size, uint64_t serial)
{
	if (out == NULL || size < NTFSMAC_JOURNAL_NAME_SIZE)
		return -1;
	put_hex16(out, serial);
	out[16] = '\0';
	return 0;
}

int ntfsmac_journal_format_tmpname(char *out, size_t size, uint64_t serial)
{
	if (out == NULL || size < NTFSMAC_JOURNAL_TMPNAME_SIZE)
		return -1;
	out[0] = '.';
	put_hex16(out + 1, serial);
	memcpy(out + 17, ".tmp", 5); /* with NUL */
	return 0;
}

int ntfsmac_journal_format_tombstone(char *out, size_t size, uint64_t serial,
				     int64_t when_utc)
{
	/* Years 0001..9999 only: four digits. */
	const int64_t min_t = days_from_civil(1, 1, 1) * 86400;
	const int64_t max_t = days_from_civil(9999, 12, 31) * 86400 + 86399;
	int64_t days, secs;
	int y, m, d;

	if (out == NULL || size < NTFSMAC_JOURNAL_TOMBNAME_SIZE)
		return -1;
	if (when_utc < min_t || when_utc > max_t)
		return -1;
	days = when_utc / 86400;
	secs = when_utc % 86400;
	if (secs < 0) {
		secs += 86400;
		days -= 1;
	}
	civil_from_days(days, &y, &m, &d);
	put_hex16(out, serial);
	out[16] = '-';
	put_dec(out + 17, (unsigned)y, 4);
	put_dec(out + 21, (unsigned)m, 2);
	put_dec(out + 23, (unsigned)d, 2);
	out[25] = 'T';
	put_dec(out + 26, (unsigned)(secs / 3600), 2);
	put_dec(out + 28, (unsigned)(secs % 3600 / 60), 2);
	put_dec(out + 30, (unsigned)(secs % 60), 2);
	out[32] = 'Z';
	out[33] = '\0';
	return 0;
}

/* Exactly 16 upper-case hex digits, no more, no less. */
static int parse_hex16(const char *s, uint64_t *out)
{
	uint64_t v = 0;

	for (int i = 0; i < 16; i++) {
		char c = s[i];
		unsigned digit;

		if (c >= '0' && c <= '9')
			digit = (unsigned)(c - '0');
		else if (c >= 'A' && c <= 'F')
			digit = (unsigned)(c - 'A') + 10u;
		else
			return -1;
		v = (v << 4) | digit;
	}
	*out = v;
	return 0;
}

static int parse_entry_name(const char *name, uint64_t *serial)
{
	return strlen(name) == 16 ? parse_hex16(name, serial) : -1;
}

static int parse_tmp_name(const char *name, uint64_t *serial)
{
	if (strlen(name) != 21 || name[0] != '.' || strcmp(name + 17, ".tmp") != 0)
		return -1;
	return parse_hex16(name + 1, serial);
}

static int digits_ok(const char *s, int n)
{
	for (int i = 0; i < n; i++)
		if (s[i] < '0' || s[i] > '9')
			return 0;
	return 1;
}

static int dec_at(const char *s, int n)
{
	int v = 0;

	for (int i = 0; i < n; i++)
		v = v * 10 + (s[i] - '0');
	return v;
}

/* Tombstone grammar; the name must round-trip through the formatter. */
static int parse_tombstone_name(const char *name, uint64_t *serial, int64_t *when)
{
	char again[NTFSMAC_JOURNAL_TOMBNAME_SIZE];
	int64_t t;

	if (strlen(name) != 33 || name[16] != '-' || name[25] != 'T' ||
	    name[32] != 'Z')
		return -1;
	if (parse_hex16(name, serial) != 0)
		return -1;
	if (!digits_ok(name + 17, 8) || !digits_ok(name + 26, 6))
		return -1;
	{
		int y = dec_at(name + 17, 4), mo = dec_at(name + 21, 2);
		int d = dec_at(name + 23, 2), h = dec_at(name + 26, 2);
		int mi = dec_at(name + 28, 2), s = dec_at(name + 30, 2);

		if (y < 1 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 ||
		    mi > 59 || s > 59)
			return -1;
		t = days_from_civil(y, mo, d) * 86400 + (int64_t)h * 3600 +
		    (int64_t)mi * 60 + s;
	}
	/* Rejects 30 February and the like. */
	if (ntfsmac_journal_format_tombstone(again, sizeof(again), *serial, t) != 0 ||
	    strcmp(again, name) != 0)
		return -1;
	*when = t;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Transitions                                                        */
/* ------------------------------------------------------------------ */

static bool marker_valid(ntfsmac_journal_marker m)
{
	return m == NTFSMAC_JOURNAL_MARKER_PENDING ||
	       m == NTFSMAC_JOURNAL_MARKER_SET ||
	       m == NTFSMAC_JOURNAL_MARKER_FAILED ||
	       m == NTFSMAC_JOURNAL_MARKER_NONE;
}

static bool state_valid(ntfsmac_journal_state s)
{
	return s == NTFSMAC_JOURNAL_STATE_OPEN ||
	       s == NTFSMAC_JOURNAL_STATE_WRITE_ERROR;
}

bool ntfsmac_journal_marker_transition_ok(ntfsmac_journal_marker from,
					  ntfsmac_journal_marker to)
{
	if (!marker_valid(from) || !marker_valid(to))
		return false;
	if (from == to)
		return true;
	return from == NTFSMAC_JOURNAL_MARKER_PENDING &&
	       (to == NTFSMAC_JOURNAL_MARKER_SET ||
		to == NTFSMAC_JOURNAL_MARKER_FAILED);
}

bool ntfsmac_journal_state_transition_ok(ntfsmac_journal_state from,
					 ntfsmac_journal_state to)
{
	if (!state_valid(from) || !state_valid(to))
		return false;
	if (from == to)
		return true;
	return from == NTFSMAC_JOURNAL_STATE_OPEN &&
	       to == NTFSMAC_JOURNAL_STATE_WRITE_ERROR;
}

/* ------------------------------------------------------------------ */
/* Entry text: validation, serialisation, strict parsing              */
/* ------------------------------------------------------------------ */

static const char *marker_text(ntfsmac_journal_marker m)
{
	switch (m) {
	case NTFSMAC_JOURNAL_MARKER_PENDING: return "pending";
	case NTFSMAC_JOURNAL_MARKER_SET:     return "set";
	case NTFSMAC_JOURNAL_MARKER_FAILED:  return "failed";
	case NTFSMAC_JOURNAL_MARKER_NONE:    return "none";
	}
	return NULL;
}

static const char *state_text(ntfsmac_journal_state s)
{
	switch (s) {
	case NTFSMAC_JOURNAL_STATE_OPEN:        return "open";
	case NTFSMAC_JOURNAL_STATE_WRITE_ERROR: return "write_error";
	}
	return NULL;
}

static bool label_byte_ok(unsigned char c)
{
	return c >= 0x20 && c != 0x7f;
}

/* Field ranges shared by create, update (input) and get (stored data). */
static bool fields_valid(const ntfsmac_journal_entry *e)
{
	const void *nul = memchr(e->label, '\0', sizeof(e->label));

	if (nul == NULL)
		return false;
	for (const unsigned char *p = (const unsigned char *)e->label; *p; p++)
		if (!label_byte_ok(*p))
			return false;
	if (!marker_valid(e->marker) || !state_valid(e->state))
		return false;
	return e->session_id != 0 && e->total_sectors != 0;
}

static int serialise(const ntfsmac_journal_entry *e, char *buf, size_t size,
		     size_t *len)
{
	char serial[NTFSMAC_JOURNAL_NAME_SIZE];
	char label_hex[2 * NTFSMAC_JOURNAL_LABEL_MAX_BYTES + 1];
	size_t n = 0;
	int w;

	if (ntfsmac_journal_format_name(serial, sizeof(serial), e->serial) != 0)
		return -1;
	for (const unsigned char *p = (const unsigned char *)e->label; *p; p++) {
		label_hex[n++] = HEX_LOWER[*p >> 4];
		label_hex[n++] = HEX_LOWER[*p & 0xf];
	}
	label_hex[n] = '\0';
	w = snprintf(buf, size,
		     "ntfsmac-journal 1\n"
		     "serial %s\n"
		     "label %s\n"
		     "total_sectors %" PRIu64 "\n"
		     "session_id %" PRIu64 "\n"
		     "created %" PRId64 "\n"
		     "updated %" PRId64 "\n"
		     "logfile_lsn_at_start %" PRIu64 "\n"
		     "marker %s\n"
		     "state %s\n"
		     "end\n",
		     serial, label_hex, e->total_sectors, e->session_id,
		     e->created, e->updated, e->logfile_lsn_at_start,
		     marker_text(e->marker), state_text(e->state));
	if (w < 0 || (size_t)w >= size)
		return -1;
	*len = (size_t)w;
	return 0;
}

struct cursor {
	const char *p;
	const char *end;
};

/* Next line without its LF; the LF must be present. */
static int next_line(struct cursor *c, const char **line, size_t *len)
{
	const char *nl = memchr(c->p, '\n', (size_t)(c->end - c->p));

	if (nl == NULL)
		return -1;
	*line = c->p;
	*len = (size_t)(nl - c->p);
	c->p = nl + 1;
	return 0;
}

/* Line must be exactly `key` followed by a space; value is the rest. */
static int line_value(struct cursor *c, const char *key, const char **val,
		      size_t *vlen)
{
	const char *line;
	size_t len, klen = strlen(key);

	if (next_line(c, &line, &len) != 0)
		return -1;
	if (len < klen + 1 || memcmp(line, key, klen) != 0 || line[klen] != ' ')
		return -1;
	*val = line + klen + 1;
	*vlen = len - klen - 1;
	return 0;
}

static int dec_u64(const char *s, size_t n, uint64_t *out)
{
	uint64_t v = 0;

	if (n == 0 || n > 20)
		return -1;
	if (n > 1 && s[0] == '0')
		return -1;
	for (size_t i = 0; i < n; i++) {
		unsigned d;

		if (s[i] < '0' || s[i] > '9')
			return -1;
		d = (unsigned)(s[i] - '0');
		if (v > (UINT64_MAX - d) / 10)
			return -1;
		v = v * 10 + d;
	}
	*out = v;
	return 0;
}

static int dec_i64(const char *s, size_t n, int64_t *out)
{
	uint64_t mag;

	if (n >= 1 && s[0] == '-') {
		if (dec_u64(s + 1, n - 1, &mag) != 0 || mag == 0)
			return -1;
		if (mag > (uint64_t)INT64_MAX + 1u)
			return -1;
		*out = mag == (uint64_t)INT64_MAX + 1u ? INT64_MIN : -(int64_t)mag;
		return 0;
	}
	if (dec_u64(s, n, &mag) != 0 || mag > (uint64_t)INT64_MAX)
		return -1;
	*out = (int64_t)mag;
	return 0;
}

static int hexval_lower(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

static int parse_entry_text(const char *buf, size_t len, ntfsmac_journal_entry *e)
{
	struct cursor c = { buf, buf + len };
	const char *line, *v;
	size_t ll, vl;
	uint64_t serial;
	ntfsmac_journal_entry t;

	memset(&t, 0, sizeof(t));
	if (next_line(&c, &line, &ll) != 0 || ll != 17 ||
	    memcmp(line, "ntfsmac-journal 1", 17) != 0)
		return -1;
	if (line_value(&c, "serial", &v, &vl) != 0 || vl != 16)
		return -1;
	{
		char tmp[17];

		memcpy(tmp, v, 16);
		tmp[16] = '\0';
		if (parse_hex16(tmp, &serial) != 0)
			return -1;
	}
	t.serial = serial;
	if (line_value(&c, "label", &v, &vl) != 0 || (vl & 1u) != 0 ||
	    vl > 2 * NTFSMAC_JOURNAL_LABEL_MAX_BYTES)
		return -1;
	for (size_t i = 0; i < vl; i += 2) {
		int hi = hexval_lower(v[i]), lo = hexval_lower(v[i + 1]);
		unsigned char b;

		if (hi < 0 || lo < 0)
			return -1;
		b = (unsigned char)(hi << 4 | lo);
		if (!label_byte_ok(b))
			return -1;
		t.label[i / 2] = (char)b;
	}
	if (line_value(&c, "total_sectors", &v, &vl) != 0 ||
	    dec_u64(v, vl, &t.total_sectors) != 0)
		return -1;
	if (line_value(&c, "session_id", &v, &vl) != 0 ||
	    dec_u64(v, vl, &t.session_id) != 0)
		return -1;
	if (line_value(&c, "created", &v, &vl) != 0 ||
	    dec_i64(v, vl, &t.created) != 0)
		return -1;
	if (line_value(&c, "updated", &v, &vl) != 0 ||
	    dec_i64(v, vl, &t.updated) != 0)
		return -1;
	if (line_value(&c, "logfile_lsn_at_start", &v, &vl) != 0 ||
	    dec_u64(v, vl, &t.logfile_lsn_at_start) != 0)
		return -1;
	if (line_value(&c, "marker", &v, &vl) != 0)
		return -1;
	if (vl == 7 && memcmp(v, "pending", 7) == 0)
		t.marker = NTFSMAC_JOURNAL_MARKER_PENDING;
	else if (vl == 3 && memcmp(v, "set", 3) == 0)
		t.marker = NTFSMAC_JOURNAL_MARKER_SET;
	else if (vl == 6 && memcmp(v, "failed", 6) == 0)
		t.marker = NTFSMAC_JOURNAL_MARKER_FAILED;
	else if (vl == 4 && memcmp(v, "none", 4) == 0)
		t.marker = NTFSMAC_JOURNAL_MARKER_NONE;
	else
		return -1;
	if (line_value(&c, "state", &v, &vl) != 0)
		return -1;
	if (vl == 4 && memcmp(v, "open", 4) == 0)
		t.state = NTFSMAC_JOURNAL_STATE_OPEN;
	else if (vl == 11 && memcmp(v, "write_error", 11) == 0)
		t.state = NTFSMAC_JOURNAL_STATE_WRITE_ERROR;
	else
		return -1;
	if (next_line(&c, &line, &ll) != 0 || ll != 3 || memcmp(line, "end", 3) != 0)
		return -1;
	if (c.p != c.end)
		return -1; /* trailing bytes */
	if (!fields_valid(&t))
		return -1;
	*e = t;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Descriptor-relative I/O                                            */
/* ------------------------------------------------------------------ */

static int write_all(int fd, const char *buf, size_t len)
{
	while (len > 0) {
		ssize_t n = write(fd, buf, len);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		buf += n;
		len -= (size_t)n;
	}
	return 0;
}

/* F_FULLFSYNC where the platform has it; fsync where the file system or
 * device rejects it (same rule as ntfsmac-volmark, section 3.6 step 5). */
static int full_fsync(int fd)
{
#if defined(F_FULLFSYNC)
	if (fcntl(fd, F_FULLFSYNC) == 0)
		return 0;
	if (errno != ENOTSUP && errno != EINVAL && errno != ENOTTY)
		return -1;
#endif
	while (fsync(fd) != 0) {
		if (errno != EINTR)
			return -1;
	}
	return 0;
}

/* Directory descriptor sanity: a directory we own that nobody else can write. */
static int check_dir(int fd)
{
	struct stat st;

	if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode))
		return -1;
	if (st.st_uid != geteuid() || (st.st_mode & (S_IWGRP | S_IWOTH)) != 0)
		return -1;
	return 0;
}

/* Rename that never replaces the target. 0, or -1 with errno (EEXIST). */
static int rename_noreplace(int ofd, const char *o, int nfd, const char *n)
{
	struct stat st;
	int r = -1;

#if defined(__APPLE__) && defined(RENAME_EXCL)
	r = renameatx_np(ofd, o, nfd, n, RENAME_EXCL);
	if (r == 0 || errno == EEXIST)
		return r;
#elif defined(__linux__) && defined(SYS_renameat2)
	r = (int)syscall(SYS_renameat2, ofd, o, nfd, n, 1u /* RENAME_NOREPLACE */);
	if (r == 0 || errno == EEXIST)
		return r;
#endif
	(void)r;
	/* File system without the atomic form: check, then plain rename. */
	if (fstatat(nfd, n, &st, AT_SYMLINK_NOFOLLOW) == 0) {
		errno = EEXIST;
		return -1;
	}
	if (errno != ENOENT)
		return -1;
	return renameat(ofd, o, nfd, n);
}

struct item {
	uint64_t serial;
	int64_t when;
};

struct vec {
	struct item *v;
	size_t n, cap;
	int failed;
};

static void vec_push(struct vec *vec, uint64_t serial, int64_t when)
{
	if (vec->failed)
		return;
	if (vec->n >= LIST_MAX_ITEMS) {
		vec->failed = 1;
		return;
	}
	if (vec->n == vec->cap) {
		size_t cap = vec->cap ? vec->cap * 2 : 16;
		struct item *nv = realloc(vec->v, cap * sizeof(*nv));

		if (nv == NULL) {
			vec->failed = 1;
			return;
		}
		vec->v = nv;
		vec->cap = cap;
	}
	vec->v[vec->n].serial = serial;
	vec->v[vec->n].when = when;
	vec->n++;
}

enum kind { KIND_ENTRY, KIND_TMP, KIND_TOMB };

/* Collect the parsed values of all names of one grammar in a directory. */
static int collect(int fd, enum kind kind, struct vec *out)
{
	DIR *d;
	struct dirent *de;
	int dupfd = fcntl(fd, F_DUPFD_CLOEXEC, 0);

	memset(out, 0, sizeof(*out));
	if (dupfd < 0)
		return -1;
	d = fdopendir(dupfd);
	if (d == NULL) {
		close(dupfd);
		return -1;
	}
	rewinddir(d);
	errno = 0;
	while ((de = readdir(d)) != NULL) {
		uint64_t serial;
		int64_t when = 0;
		int ok;

		switch (kind) {
		case KIND_ENTRY: ok = parse_entry_name(de->d_name, &serial) == 0; break;
		case KIND_TMP:   ok = parse_tmp_name(de->d_name, &serial) == 0; break;
		default:
			ok = parse_tombstone_name(de->d_name, &serial, &when) == 0;
			break;
		}
		if (ok)
			vec_push(out, serial, when);
		errno = 0;
	}
	if (errno != 0 || out->failed) {
		closedir(d);
		free(out->v);
		memset(out, 0, sizeof(*out));
		return -1;
	}
	closedir(d);
	return 0;
}

static int cmp_item_asc(const void *a, const void *b)
{
	const struct item *x = a, *y = b;

	if (x->when != y->when)
		return x->when < y->when ? -1 : 1;
	if (x->serial != y->serial)
		return x->serial < y->serial ? -1 : 1;
	return 0;
}

static int cmp_item_desc(const void *a, const void *b)
{
	return cmp_item_asc(b, a);
}

/* ------------------------------------------------------------------ */
/* Open and close                                                     */
/* ------------------------------------------------------------------ */

static void remove_stray_temporaries(ntfsmac_journal *j)
{
	struct vec v;

	if (collect(j->dirfd, KIND_TMP, &v) != 0)
		return;
	for (size_t i = 0; i < v.n; i++) {
		char name[NTFSMAC_JOURNAL_TMPNAME_SIZE];

		if (ntfsmac_journal_format_tmpname(name, sizeof(name), v.v[i].serial) == 0)
			(void)unlinkat(j->dirfd, name, 0); /* a symlink is removed, not followed */
	}
	free(v.v);
}

ntfsmac_journal_status ntfsmac_journal_open(const char *dir_path,
					    ntfsmac_journal **out)
{
	const int dflags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
	ntfsmac_journal *j;
	int fd, clr, created = 0;

	if (out == NULL)
		return NTFSMAC_JOURNAL_ERR_INVALID;
	*out = NULL;
	if (dir_path == NULL || dir_path[0] == '\0')
		return NTFSMAC_JOURNAL_ERR_INVALID;

	fd = open(dir_path, dflags);
	if (fd < 0 && errno == ENOENT) {
		if (mkdir(dir_path, 0700) != 0 && errno != EEXIST)
			return NTFSMAC_JOURNAL_ERR_IO;
		fd = open(dir_path, dflags);
	}
	if (fd < 0)
		return NTFSMAC_JOURNAL_ERR_IO;
	if (check_dir(fd) != 0) {
		close(fd);
		return NTFSMAC_JOURNAL_ERR_IO;
	}
	if (mkdirat(fd, CLEARED_DIR, 0700) == 0)
		created = 1;
	else if (errno != EEXIST) {
		close(fd);
		return NTFSMAC_JOURNAL_ERR_IO;
	}
	clr = openat(fd, CLEARED_DIR, dflags);
	if (clr < 0 || check_dir(clr) != 0) {
		if (clr >= 0)
			close(clr);
		close(fd);
		return NTFSMAC_JOURNAL_ERR_IO;
	}
	if (created && full_fsync(fd) != 0) {
		close(clr);
		close(fd);
		return NTFSMAC_JOURNAL_ERR_IO;
	}
	j = calloc(1, sizeof(*j));
	if (j == NULL) {
		close(clr);
		close(fd);
		return NTFSMAC_JOURNAL_ERR_IO;
	}
	j->dirfd = fd;
	j->clrfd = clr;
	remove_stray_temporaries(j);
	*out = j;
	return NTFSMAC_JOURNAL_OK;
}

void ntfsmac_journal_close(ntfsmac_journal *j)
{
	if (j == NULL)
		return;
	close(j->clrfd);
	close(j->dirfd);
	free(j);
}

/* ------------------------------------------------------------------ */
/* Read                                                               */
/* ------------------------------------------------------------------ */

ntfsmac_journal_status ntfsmac_journal_get(ntfsmac_journal *j, uint64_t serial,
					   ntfsmac_journal_entry *out)
{
	char name[NTFSMAC_JOURNAL_NAME_SIZE];
	char buf[ENTRY_MAX_BYTES + 1];
	ntfsmac_journal_entry e;
	struct stat st;
	size_t got = 0;
	int fd;

	if (j == NULL || out == NULL)
		return NTFSMAC_JOURNAL_ERR_INVALID;
	if (ntfsmac_journal_format_name(name, sizeof(name), serial) != 0)
		return NTFSMAC_JOURNAL_ERR_INVALID;

	/* O_NONBLOCK: a FIFO planted at the name must not hang the open. */
	fd = openat(j->dirfd, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		if (errno == ENOENT)
			return NTFSMAC_JOURNAL_ERR_NOT_FOUND;
		if (errno == ELOOP || errno == ENOTDIR || errno == EMLINK)
			return NTFSMAC_JOURNAL_ERR_CORRUPT; /* symlink at the name */
		return NTFSMAC_JOURNAL_ERR_IO;
	}
	if (fstat(fd, &st) != 0) {
		close(fd);
		return NTFSMAC_JOURNAL_ERR_IO;
	}
	if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
	    (st.st_mode & (S_IWGRP | S_IWOTH)) != 0 || st.st_size <= 0 ||
	    (uint64_t)st.st_size > ENTRY_MAX_BYTES) {
		close(fd);
		return NTFSMAC_JOURNAL_ERR_CORRUPT;
	}
	for (;;) {
		ssize_t n = read(fd, buf + got, sizeof(buf) - got);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			return NTFSMAC_JOURNAL_ERR_IO;
		}
		if (n == 0)
			break;
		got += (size_t)n;
		if (got > ENTRY_MAX_BYTES)
			break;
	}
	close(fd);
	if (got == 0 || got > ENTRY_MAX_BYTES || got != (size_t)st.st_size)
		return NTFSMAC_JOURNAL_ERR_CORRUPT;
	if (parse_entry_text(buf, got, &e) != 0 || e.serial != serial)
		return NTFSMAC_JOURNAL_ERR_CORRUPT;
	*out = e;
	return NTFSMAC_JOURNAL_OK;
}

/* ------------------------------------------------------------------ */
/* Write                                                              */
/* ------------------------------------------------------------------ */

/*
 * Durable write: temporary file in the journal directory, F_FULLFSYNC, rename
 * to the entry name, directory sync. With `noreplace` an existing entry name
 * makes the call fail with EXISTS.
 */
static ntfsmac_journal_status write_entry(ntfsmac_journal *j,
					  const ntfsmac_journal_entry *e,
					  bool noreplace)
{
	char name[NTFSMAC_JOURNAL_NAME_SIZE];
	char tmp[NTFSMAC_JOURNAL_TMPNAME_SIZE];
	char buf[ENTRY_MAX_BYTES];
	size_t len;
	int fd = -1, rc;

	if (ntfsmac_journal_format_name(name, sizeof(name), e->serial) != 0 ||
	    ntfsmac_journal_format_tmpname(tmp, sizeof(tmp), e->serial) != 0 ||
	    serialise(e, buf, sizeof(buf), &len) != 0)
		return NTFSMAC_JOURNAL_ERR_INVALID;

	JRN_POINT("write.begin");
	for (int attempt = 0; attempt < 2 && fd < 0; attempt++) {
		fd = openat(j->dirfd, tmp,
			    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
		if (fd < 0 && errno == EEXIST && attempt == 0) {
			/* Leftover or planted name: remove the name itself (a
			 * symlink is unlinked, never followed) and retry once. */
			if (unlinkat(j->dirfd, tmp, 0) != 0 && errno != ENOENT)
				return NTFSMAC_JOURNAL_ERR_IO;
		}
	}
	if (fd < 0)
		return NTFSMAC_JOURNAL_ERR_IO;
	JRN_POINT("write.tmp_created");
	if (fchmod(fd, 0600) != 0 || write_all(fd, buf, len) != 0)
		goto fail;
	JRN_POINT("write.tmp_written");
	if (full_fsync(fd) != 0)
		goto fail;
	JRN_POINT("write.tmp_synced");
	if (close(fd) != 0) {
		fd = -1;
		goto fail;
	}
	fd = -1;
	if (noreplace)
		rc = rename_noreplace(j->dirfd, tmp, j->dirfd, name);
	else
		rc = renameat(j->dirfd, tmp, j->dirfd, name);
	if (rc != 0) {
		int err = errno;

		(void)unlinkat(j->dirfd, tmp, 0);
		return noreplace && err == EEXIST ? NTFSMAC_JOURNAL_ERR_EXISTS
						  : NTFSMAC_JOURNAL_ERR_IO;
	}
	JRN_POINT("write.renamed");
	if (full_fsync(j->dirfd) != 0)
		return NTFSMAC_JOURNAL_ERR_IO;
	JRN_POINT("write.dir_synced");
	return NTFSMAC_JOURNAL_OK;

fail:
	if (fd >= 0)
		close(fd);
	(void)unlinkat(j->dirfd, tmp, 0);
	return NTFSMAC_JOURNAL_ERR_IO;
}

ntfsmac_journal_status ntfsmac_journal_create(ntfsmac_journal *j,
					      const ntfsmac_journal_entry *e)
{
	char name[NTFSMAC_JOURNAL_NAME_SIZE];
	struct stat st;

	if (j == NULL || e == NULL || !fields_valid(e))
		return NTFSMAC_JOURNAL_ERR_INVALID;
	/* Section 3.5: created with state=open and marker pending (or none). */
	if (e->state != NTFSMAC_JOURNAL_STATE_OPEN ||
	    (e->marker != NTFSMAC_JOURNAL_MARKER_PENDING &&
	     e->marker != NTFSMAC_JOURNAL_MARKER_NONE))
		return NTFSMAC_JOURNAL_ERR_INVALID;
	if (ntfsmac_journal_format_name(name, sizeof(name), e->serial) != 0)
		return NTFSMAC_JOURNAL_ERR_INVALID;
	/* Anything at the name, even a dangling symlink, counts as existing. */
	if (fstatat(j->dirfd, name, &st, AT_SYMLINK_NOFOLLOW) == 0)
		return NTFSMAC_JOURNAL_ERR_EXISTS;
	if (errno != ENOENT)
		return NTFSMAC_JOURNAL_ERR_IO;
	return write_entry(j, e, true);
}

ntfsmac_journal_status ntfsmac_journal_update(ntfsmac_journal *j,
					      const ntfsmac_journal_entry *e)
{
	ntfsmac_journal_entry old;
	ntfsmac_journal_status st;

	if (j == NULL || e == NULL || !fields_valid(e))
		return NTFSMAC_JOURNAL_ERR_INVALID;
	st = ntfsmac_journal_get(j, e->serial, &old);
	if (st != NTFSMAC_JOURNAL_OK)
		return st; /* NOT_FOUND, CORRUPT (never repaired silently) or IO */
	if (old.serial != e->serial || strcmp(old.label, e->label) != 0 ||
	    old.total_sectors != e->total_sectors ||
	    old.session_id != e->session_id || old.created != e->created ||
	    old.logfile_lsn_at_start != e->logfile_lsn_at_start)
		return NTFSMAC_JOURNAL_ERR_IMMUTABLE;
	if (!ntfsmac_journal_marker_transition_ok(old.marker, e->marker) ||
	    !ntfsmac_journal_state_transition_ok(old.state, e->state))
		return NTFSMAC_JOURNAL_ERR_TRANSITION;
	return write_entry(j, e, false);
}

/* ------------------------------------------------------------------ */
/* Removal                                                            */
/* ------------------------------------------------------------------ */

static const char *removal_text(ntfsmac_journal_removal p)
{
	switch (p) {
	case NTFSMAC_JOURNAL_REMOVE_CLEAN_SESSION:  return "clean-session";
	case NTFSMAC_JOURNAL_REMOVE_EXTERNAL_CHECK: return "external-check";
	case NTFSMAC_JOURNAL_REMOVE_JOURNAL_CLEAR:  return "journal-clear";
	case NTFSMAC_JOURNAL_REMOVE_REPAIR:         return "repair";
	}
	return NULL;
}

static bool removal_precondition(ntfsmac_journal_removal p,
				 const ntfsmac_journal_entry *e)
{
	switch (p) {
	case NTFSMAC_JOURNAL_REMOVE_CLEAN_SESSION:
		return e->state == NTFSMAC_JOURNAL_STATE_OPEN &&
		       (e->marker == NTFSMAC_JOURNAL_MARKER_SET ||
			e->marker == NTFSMAC_JOURNAL_MARKER_NONE);
	case NTFSMAC_JOURNAL_REMOVE_EXTERNAL_CHECK:
		return e->marker == NTFSMAC_JOURNAL_MARKER_SET;
	case NTFSMAC_JOURNAL_REMOVE_JOURNAL_CLEAR:
	case NTFSMAC_JOURNAL_REMOVE_REPAIR:
		return true;
	}
	return false;
}

/* Keep at most MAX_TOMBSTONES; the one just created is never a candidate. */
static void prune_tombstones(ntfsmac_journal *j, uint64_t new_serial,
			     int64_t new_when)
{
	struct vec v;
	size_t excess, cand = 0;

	if (collect(j->clrfd, KIND_TOMB, &v) != 0) {
		NTFSMAC_LOGW("journal: cannot list cleared/ for pruning");
		return;
	}
	if (v.n <= NTFSMAC_JOURNAL_MAX_TOMBSTONES) {
		free(v.v);
		return;
	}
	qsort(v.v, v.n, sizeof(*v.v), cmp_item_asc);
	/* Drop the new tombstone from the candidate list (it sorts anywhere). */
	for (size_t i = 0; i < v.n; i++) {
		if (v.v[i].serial == new_serial && v.v[i].when == new_when)
			continue;
		v.v[cand++] = v.v[i];
	}
	excess = v.n - NTFSMAC_JOURNAL_MAX_TOMBSTONES;
	if (excess > cand)
		excess = cand;
	for (size_t i = 0; i < excess; i++) {
		char name[NTFSMAC_JOURNAL_TOMBNAME_SIZE];

		if (ntfsmac_journal_format_tombstone(name, sizeof(name),
						     v.v[i].serial, v.v[i].when) != 0)
			continue;
		if (unlinkat(j->clrfd, name, 0) != 0 && errno != ENOENT)
			NTFSMAC_LOGW("journal: cannot prune tombstone %s", name);
		JRN_POINT("remove.pruned");
	}
	free(v.v);
	if (full_fsync(j->clrfd) != 0)
		NTFSMAC_LOGW("journal: sync of cleared/ after pruning failed");
	JRN_POINT("remove.prune_synced");
}

ntfsmac_journal_status ntfsmac_journal_remove(ntfsmac_journal *j, uint64_t serial,
					      ntfsmac_journal_removal path,
					      int64_t now_utc)
{
	char name[NTFSMAC_JOURNAL_NAME_SIZE];
	char tomb[NTFSMAC_JOURNAL_TOMBNAME_SIZE];
	ntfsmac_journal_entry e;
	ntfsmac_journal_status st;
	struct stat sb;

	if (j == NULL || removal_text(path) == NULL)
		return NTFSMAC_JOURNAL_ERR_INVALID;
	if (ntfsmac_journal_format_name(name, sizeof(name), serial) != 0 ||
	    ntfsmac_journal_format_tombstone(tomb, sizeof(tomb), serial, now_utc) != 0)
		return NTFSMAC_JOURNAL_ERR_INVALID;

	JRN_POINT("remove.begin");
	st = ntfsmac_journal_get(j, serial, &e);
	if (st == NTFSMAC_JOURNAL_OK) {
		if (!removal_precondition(path, &e))
			return NTFSMAC_JOURNAL_ERR_PATH_CONDITION;
	} else if (st == NTFSMAC_JOURNAL_ERR_CORRUPT &&
		   (path == NTFSMAC_JOURNAL_REMOVE_JOURNAL_CLEAR ||
		    path == NTFSMAC_JOURNAL_REMOVE_REPAIR)) {
		/* Paths 3 and 4 have no precondition on marker or state: a
		 * damaged file must not trap the volume read-only forever.
		 * Only a directory at the name is left alone. */
		if (fstatat(j->dirfd, name, &sb, AT_SYMLINK_NOFOLLOW) != 0 ||
		    S_ISDIR(sb.st_mode))
			return NTFSMAC_JOURNAL_ERR_CORRUPT;
	} else {
		return st;
	}

	if (fstatat(j->clrfd, tomb, &sb, AT_SYMLINK_NOFOLLOW) == 0)
		return NTFSMAC_JOURNAL_ERR_EXISTS; /* same second; entry stays */
	if (errno != ENOENT)
		return NTFSMAC_JOURNAL_ERR_IO;
	if (rename_noreplace(j->dirfd, name, j->clrfd, tomb) != 0)
		return errno == EEXIST ? NTFSMAC_JOURNAL_ERR_EXISTS
				       : NTFSMAC_JOURNAL_ERR_IO;
	JRN_POINT("remove.renamed");
	if (full_fsync(j->clrfd) != 0)
		return NTFSMAC_JOURNAL_ERR_IO;
	JRN_POINT("remove.cleared_synced");
	if (full_fsync(j->dirfd) != 0)
		return NTFSMAC_JOURNAL_ERR_IO;
	JRN_POINT("remove.dir_synced");
	NTFSMAC_LOGI("journal: entry %s removed by path %d (%s), tombstone %s",
		     name, (int)path, removal_text(path), tomb);
	prune_tombstones(j, serial, now_utc);
	JRN_POINT("remove.done");
	return NTFSMAC_JOURNAL_OK;
}

/* ------------------------------------------------------------------ */
/* Listing                                                            */
/* ------------------------------------------------------------------ */

ntfsmac_journal_status ntfsmac_journal_list(ntfsmac_journal *j,
					    ntfsmac_journal_entry_cb cb, void *ctx,
					    unsigned *skipped)
{
	struct vec v;

	if (skipped != NULL)
		*skipped = 0;
	if (j == NULL || cb == NULL)
		return NTFSMAC_JOURNAL_ERR_INVALID;
	if (collect(j->dirfd, KIND_ENTRY, &v) != 0)
		return NTFSMAC_JOURNAL_ERR_IO;
	/* Collected first so a callback may change the directory. */
	qsort(v.v, v.n, sizeof(*v.v), cmp_item_asc);
	for (size_t i = 0; i < v.n; i++) {
		ntfsmac_journal_entry e;
		ntfsmac_journal_status st = ntfsmac_journal_get(j, v.v[i].serial, &e);

		if (st == NTFSMAC_JOURNAL_ERR_NOT_FOUND)
			continue; /* removed meanwhile */
		if (st != NTFSMAC_JOURNAL_OK) {
			if (skipped != NULL)
				(*skipped)++;
			continue;
		}
		if (cb(&e, ctx) != 0)
			break;
	}
	free(v.v);
	return NTFSMAC_JOURNAL_OK;
}

ntfsmac_journal_status ntfsmac_journal_list_tombstones(ntfsmac_journal *j,
						       ntfsmac_journal_tombstone_cb cb,
						       void *ctx)
{
	struct vec v;

	if (j == NULL || cb == NULL)
		return NTFSMAC_JOURNAL_ERR_INVALID;
	if (collect(j->clrfd, KIND_TOMB, &v) != 0)
		return NTFSMAC_JOURNAL_ERR_IO;
	qsort(v.v, v.n, sizeof(*v.v), cmp_item_desc);
	for (size_t i = 0; i < v.n; i++)
		if (cb(v.v[i].serial, v.v[i].when, ctx) != 0)
			break;
	free(v.v);
	return NTFSMAC_JOURNAL_OK;
}

#if defined(NTFSMAC_JOURNAL_TESTHOOKS)
void (*ntfsmac_journal_testhook)(const char *point);
#endif
