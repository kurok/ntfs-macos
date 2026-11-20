/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "usermap.h"

#include <stdlib.h>
#include <string.h>

#define SID_MAX_SUBAUTH 8u    /* the engine reads at most 8 (cnt < 8) */
#define SID_AUTH_NT 5u        /* S-1-5: NT authority */
#define SID_SUB_DOMAIN 21u    /* S-1-5-21-...: a domain user or group */
#define SID_PATTERN_MIN 1000u /* ntfs_valid_pattern() */
#define SID_PATTERN_MAX 0x7fffffffu
#define ID_MAX 0x7fffffffu    /* atoi() of the engine is an int */

/* Digits only, 1 to 10 of them, value up to `max`. */
static bool number(const char *s, size_t n, uint64_t max, uint64_t *out)
{
	uint64_t v = 0;
	size_t i;

	if (n == 0 || n > 10u)
		return false;
	for (i = 0; i < n; i++) {
		if (s[i] < '0' || s[i] > '9')
			return false;
		v = v * 10u + (uint64_t)(s[i] - '0');
	}
	if (v > max)
		return false;
	*out = v;
	return true;
}

/* uid or gid: empty, or 1..ID_MAX (0 is root, which the engine drops). */
static bool idfield(const char *s, size_t n, bool *present, uint64_t *value)
{
	*present = n != 0;
	*value = 0;
	if (n == 0)
		return true;
	return number(s, n, ID_MAX, value) && *value >= 1u;
}

typedef struct sid {
	uint64_t auth;
	uint64_t sub[SID_MAX_SUBAUTH];
	unsigned nsub;
} sid;

static bool sid_parse(const char *s, size_t n, sid *out)
{
	size_t i = 4, start = 4;
	uint64_t v;

	if (n < 4u || memcmp(s, "S-1-", 4) != 0)
		return false;
	memset(out, 0, sizeof(*out));
	for (;;) {
		while (i < n && s[i] != '-')
			i++;
		if (!number(s + start, i - start, 0xffffffffu, &v))
			return false;
		if (start == 4u)
			out->auth = v;
		else if (out->nsub < SID_MAX_SUBAUTH)
			out->sub[out->nsub++] = v;
		else
			return false;
		if (i >= n)
			break;
		i++; /* the dash */
		start = i;
	}
	return out->nsub >= 1u;
}

/* ntfs_is_user_sid(): S-1-5-21-a-b-c-rid. */
static bool sid_is_user(const sid *s)
{
	return s->nsub == 5u && s->auth == SID_AUTH_NT && s->sub[0] == SID_SUB_DOMAIN;
}

/* ntfs_known_group_sid(): S-1-5-n, n 1 to 6. */
static bool sid_is_known_group(const sid *s)
{
	return s->nsub == 1u && s->auth == SID_AUTH_NT && s->sub[0] >= 1u && s->sub[0] <= 6u;
}

/* One checked mapping line. */
typedef struct record {
	bool has_uid, has_gid;
	uint64_t uid, gid;
	sid s;
} record;

static ntfsmac_usermap_parse mapping_line(const char *l, size_t n, record *rec)
{
	const char *c1, *c2;
	size_t u, g;

	memset(rec, 0, sizeof(*rec));
	c1 = memchr(l, ':', n);
	if (c1 == NULL)
		return NTFSMAC_USERMAP_P_SYNTAX;
	c2 = memchr(c1 + 1, ':', n - (size_t)(c1 + 1 - l));
	if (c2 == NULL || memchr(c2 + 1, ':', n - (size_t)(c2 + 1 - l)) != NULL)
		return NTFSMAC_USERMAP_P_SYNTAX; /* the engine ignores a fourth field; we do not */
	u = (size_t)(c1 - l);
	g = (size_t)(c2 - c1) - 1u;
	if (!idfield(l, u, &rec->has_uid, &rec->uid))
		return NTFSMAC_USERMAP_P_UID;
	if (!idfield(c1 + 1, g, &rec->has_gid, &rec->gid))
		return NTFSMAC_USERMAP_P_GID;
	if (!sid_parse(c2 + 1, n - (size_t)(c2 + 1 - l), &rec->s))
		return NTFSMAC_USERMAP_P_SID;
	if (!rec->has_uid && !rec->has_gid) {
		/* The implicit pattern: ntfs_valid_pattern() looks at the last sub-authority. */
		if (!sid_is_user(&rec->s))
			return NTFSMAC_USERMAP_P_SID_KIND;
		if (rec->s.sub[rec->s.nsub - 1u] < SID_PATTERN_MIN
		    || rec->s.sub[rec->s.nsub - 1u] > SID_PATTERN_MAX)
			return NTFSMAC_USERMAP_P_PATTERN;
		return NTFSMAC_USERMAP_P_OK;
	}
	if (rec->has_uid)
		return sid_is_user(&rec->s) ? NTFSMAC_USERMAP_P_OK : NTFSMAC_USERMAP_P_SID_KIND;
	return (sid_is_user(&rec->s) || sid_is_known_group(&rec->s)) ? NTFSMAC_USERMAP_P_OK
								     : NTFSMAC_USERMAP_P_SID_KIND;
}

/* A value that must not appear twice: a uid, a gid of a group-only line, or a SID. */
enum { KEY_UID = 0, KEY_GID = 1, KEY_SID = 2 };

typedef struct key {
	unsigned kind;
	uint64_t v[7]; /* uid or gid in v[0]; a SID: authority, count, sub-authorities */
	unsigned line;
} key;

static int key_cmp(const void *pa, const void *pb)
{
	const key *a = pa, *b = pb;
	size_t i;

	if (a->kind != b->kind)
		return a->kind < b->kind ? -1 : 1;
	for (i = 0; i < 7; i++)
		if (a->v[i] != b->v[i])
			return a->v[i] < b->v[i] ? -1 : 1;
	return a->line < b->line ? -1 : (a->line > b->line ? 1 : 0);
}

typedef struct keys {
	key *k;
	size_t n, cap;
} keys;

static bool key_add(keys *ks, unsigned kind, unsigned line, const uint64_t *v, size_t nv)
{
	key *k;

	if (ks->n == ks->cap) {
		size_t cap = ks->cap == 0 ? 64u : ks->cap * 2u;
		key *p = realloc(ks->k, cap * sizeof(key));

		if (p == NULL)
			return false;
		ks->k = p;
		ks->cap = cap;
	}
	k = &ks->k[ks->n++];
	memset(k, 0, sizeof(*k));
	k->kind = kind;
	k->line = line;
	memcpy(k->v, v, nv * sizeof(uint64_t));
	return true;
}

static bool keys_add_record(keys *ks, const record *r, unsigned line)
{
	uint64_t v[7];
	unsigned i;

	if (r->has_uid) {
		v[0] = r->uid;
		if (!key_add(ks, KEY_UID, line, v, 1))
			return false;
	}
	if (r->has_gid && !r->has_uid) {
		v[0] = r->gid;
		if (!key_add(ks, KEY_GID, line, v, 1))
			return false;
	}
	memset(v, 0, sizeof(v));
	v[0] = r->s.auth;
	v[1] = r->s.nsub;
	for (i = 0; i < r->s.nsub && i < 5u; i++)
		v[2 + i] = r->s.sub[i];
	return key_add(ks, KEY_SID, line, v, 7);
}

/* Line of the second occurrence of the earliest repeated value, or 0 if there is none. */
static unsigned keys_first_duplicate(keys *ks)
{
	size_t i;
	unsigned best = 0;

	if (ks->n < 2u)
		return 0;
	qsort(ks->k, ks->n, sizeof(key), key_cmp);
	for (i = 1; i < ks->n; i++) {
		key *a = &ks->k[i - 1u], *b = &ks->k[i];

		if (a->kind == b->kind && memcmp(a->v, b->v, sizeof(a->v)) == 0
		    && (best == 0 || b->line < best))
			best = b->line;
	}
	return best;
}

ntfsmac_usermap_parse ntfsmac_usermap_parse_text(const char *buf, size_t len, unsigned *bad_line)
{
	size_t pos = 0, i;
	unsigned line = 0, records = 0, users = 0, groups = 0, implicit_line = 0, blank_line = 0, dup;
	ntfsmac_usermap_parse r = NTFSMAC_USERMAP_P_OK;
	keys ks = { NULL, 0, 0 };

	if (bad_line != NULL)
		*bad_line = 0;
	if (buf == NULL && len != 0)
		return NTFSMAC_USERMAP_P_INVAL;
	if (len > NTFSMAC_USERMAP_MAX_SIZE)
		return NTFSMAC_USERMAP_P_TOO_BIG;
	while (pos < len) {
		const char *l = buf + pos;
		const char *nl = memchr(l, '\n', len - pos);
		size_t n;
		bool comment;
		record rec;

		line++;
		if (nl == NULL) {
			r = NTFSMAC_USERMAP_P_NO_EOL;
			break;
		}
		n = (size_t)(nl - l);
		pos += n + 1u;
		comment = n > 0 && l[0] == '#';
		if (n == 0) {
			/* The engine stops reading at a blank line; that loses nothing only if nothing
			 * but blank lines and comments follows (checked at the next mapping line). */
			if (blank_line == 0)
				blank_line = line;
			continue;
		}
		for (i = 0; i < n && r == NTFSMAC_USERMAP_P_OK; i++) {
			unsigned char ch = (unsigned char)l[i];

			if (ch == '\t' && comment)
				continue;
			if (ch < 0x20u || ch == 0x7fu || (ch >= 0x80u && !comment))
				r = NTFSMAC_USERMAP_P_CHAR;
		}
		if (r != NTFSMAC_USERMAP_P_OK)
			break;
		if (comment)
			continue;
		if (n > NTFSMAC_USERMAP_LINE_MAX)
			r = NTFSMAC_USERMAP_P_LINE_LONG;
		else
			r = mapping_line(l, n, &rec);
		if (r != NTFSMAC_USERMAP_P_OK)
			break;
		if (blank_line != 0) {
			r = NTFSMAC_USERMAP_P_BLANK;
			line = blank_line;
			break;
		}
		if (implicit_line != 0) {
			/* ntfs_find_user() and friends stop at the implicit entry: this line is dead. */
			r = NTFSMAC_USERMAP_P_AFTER_IMPLICIT;
			break;
		}
		if (!rec.has_uid && !rec.has_gid) {
			implicit_line = line;
			users++;
			groups++;
		} else {
			if (rec.has_uid)
				users++;
			if (rec.has_gid)
				groups++;
		}
		records++;
		if (!keys_add_record(&ks, &rec, line)) {
			r = NTFSMAC_USERMAP_P_INVAL; /* out of memory: fail closed */
			break;
		}
	}
	if (r == NTFSMAC_USERMAP_P_OK) {
		dup = keys_first_duplicate(&ks);
		if (dup != 0) {
			r = NTFSMAC_USERMAP_P_DUPLICATE;
			line = dup;
		}
	}
	free(ks.k);
	if (r == NTFSMAC_USERMAP_P_OK) {
		/* ntfs_build_mapping() applies nothing unless there is a valid user and a valid group. */
		if (records == 0)
			return NTFSMAC_USERMAP_P_EMPTY;
		if (users == 0 || groups == 0)
			return NTFSMAC_USERMAP_P_INCOMPLETE;
		return NTFSMAC_USERMAP_P_OK;
	}
	if (bad_line != NULL)
		*bad_line = line;
	return r;
}

const char *ntfsmac_usermap_parse_str(ntfsmac_usermap_parse p)
{
	switch (p) {
	case NTFSMAC_USERMAP_P_OK: return "ok";
	case NTFSMAC_USERMAP_P_INVAL: return "bad argument";
	case NTFSMAC_USERMAP_P_TOO_BIG: return "file too big";
	case NTFSMAC_USERMAP_P_EMPTY: return "no mapping line";
	case NTFSMAC_USERMAP_P_NO_EOL: return "last line has no newline (the engine ignores it)";
	case NTFSMAC_USERMAP_P_BLANK: return "blank line (the engine stops reading there)";
	case NTFSMAC_USERMAP_P_CHAR: return "control or non-ASCII character";
	case NTFSMAC_USERMAP_P_LINE_LONG: return "line longer than 120 bytes";
	case NTFSMAC_USERMAP_P_SYNTAX: return "not uid:gid:SID";
	case NTFSMAC_USERMAP_P_UID: return "uid is not empty or a number from 1";
	case NTFSMAC_USERMAP_P_GID: return "gid is not empty or a number from 1";
	case NTFSMAC_USERMAP_P_SID: return "malformed SID";
	case NTFSMAC_USERMAP_P_SID_KIND: return "SID of the wrong kind for this line";
	case NTFSMAC_USERMAP_P_PATTERN: return "Bad implicit SID pattern (last number must be 1000 to 2147483647)";
	case NTFSMAC_USERMAP_P_INCOMPLETE: return "needs at least one user mapping and one group mapping (the engine applies nothing otherwise)";
	case NTFSMAC_USERMAP_P_AFTER_IMPLICIT: return "mapping line after the implicit ::SID line (the engine never reaches it)";
	case NTFSMAC_USERMAP_P_DUPLICATE: return "repeats a uid, a group-only gid or a SID of an earlier line";
	}
	return "unknown";
}

void ntfsmac_usermap_ident_from_stat(ntfsmac_usermap_ident *id, const struct stat *st)
{
	memset(id, 0, sizeof(*id));
	id->dev = st->st_dev;
	id->ino = st->st_ino;
	id->mode = st->st_mode;
	id->uid = st->st_uid;
	id->size = st->st_size;
#if defined(__APPLE__)
	id->mtime_s = (int64_t)st->st_mtimespec.tv_sec;
	id->mtime_ns = (int64_t)st->st_mtimespec.tv_nsec;
	id->ctime_s = (int64_t)st->st_ctimespec.tv_sec;
	id->ctime_ns = (int64_t)st->st_ctimespec.tv_nsec;
#else
	id->mtime_s = (int64_t)st->st_mtim.tv_sec;
	id->mtime_ns = (int64_t)st->st_mtim.tv_nsec;
	id->ctime_s = (int64_t)st->st_ctim.tv_sec;
	id->ctime_ns = (int64_t)st->st_ctim.tv_nsec;
#endif
}

ntfsmac_usermap_verdict ntfsmac_usermap_stat_verdict(const ntfsmac_usermap_ident *o, uid_t owner)
{
	if (o == NULL || !S_ISREG(o->mode))
		return NTFSMAC_USERMAP_V_NOT_REGULAR;
	if (o->uid != owner)
		return NTFSMAC_USERMAP_V_NOT_OWNER;
	if ((o->mode & 07777) != NTFSMAC_USERMAP_MODE)
		return NTFSMAC_USERMAP_V_BAD_MODE;
	if (o->size < 0 || (uint64_t)o->size > NTFSMAC_USERMAP_MAX_SIZE)
		return NTFSMAC_USERMAP_V_TOO_BIG;
	return NTFSMAC_USERMAP_V_ENABLE;
}

static bool same_file(const ntfsmac_usermap_ident *a, const ntfsmac_usermap_ident *b)
{
	return a->dev == b->dev && a->ino == b->ino && a->mode == b->mode && a->uid == b->uid
	       && a->size == b->size && a->mtime_s == b->mtime_s && a->mtime_ns == b->mtime_ns
	       && a->ctime_s == b->ctime_s && a->ctime_ns == b->ctime_ns;
}

ntfsmac_usermap_verdict ntfsmac_usermap_decide(const ntfsmac_usermap_ident *opened, uid_t owner,
					       ntfsmac_usermap_parse parse,
					       const ntfsmac_usermap_ident *fd_now,
					       const ntfsmac_usermap_ident *path_now)
{
	ntfsmac_usermap_verdict v = ntfsmac_usermap_stat_verdict(opened, owner);

	if (v != NTFSMAC_USERMAP_V_ENABLE)
		return v;
	if (parse != NTFSMAC_USERMAP_P_OK)
		return NTFSMAC_USERMAP_V_PARSE;
	if (fd_now == NULL || !same_file(opened, fd_now))
		return NTFSMAC_USERMAP_V_CHANGED;
	if (path_now == NULL || S_ISLNK(path_now->mode))
		return NTFSMAC_USERMAP_V_GONE;
	if (!same_file(opened, path_now))
		return NTFSMAC_USERMAP_V_CHANGED;
	return NTFSMAC_USERMAP_V_ENABLE;
}

const char *ntfsmac_usermap_verdict_str(ntfsmac_usermap_verdict v)
{
	switch (v) {
	case NTFSMAC_USERMAP_V_ENABLE: return "accepted";
	case NTFSMAC_USERMAP_V_NOT_REGULAR: return "not a regular file";
	case NTFSMAC_USERMAP_V_NOT_OWNER: return "not owned by root";
	case NTFSMAC_USERMAP_V_BAD_MODE: return "mode is not 0644";
	case NTFSMAC_USERMAP_V_TOO_BIG: return "larger than 1 MiB";
	case NTFSMAC_USERMAP_V_UNREADABLE: return "cannot be opened or read";
	case NTFSMAC_USERMAP_V_PARSE: return "content refused";
	case NTFSMAC_USERMAP_V_CHANGED: return "changed between the check and the use";
	case NTFSMAC_USERMAP_V_GONE: return "the path no longer names the checked file";
	}
	return "unknown";
}
