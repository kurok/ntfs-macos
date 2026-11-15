/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Strict probe-verdict parser (docs/ARCHITECTURE.md section 3.1, ADR-016 decision 1,
 * ADR-023). The probe's stdout is attacker-influenced text: this parser accepts exactly
 * the documents docs/schemas/probe.schema.json accepts, plus the rules the schema cannot
 * express (byte lengths, no duplicate key, no NUL, strict UTF-8, no lone surrogate, the
 * 64 KiB cap, integer form), and nothing else. It is a hand-written recursive-free scanner
 * over the input: no allocation, no library JSON parser, no pointer into the input in the
 * result. A value is converted at the first token that decides it; the first error ends
 * the parse, and *out then holds the fail-closed verdict (state `unknown`).
 *
 * Decisions beyond the text of section 3.1:
 *  - Strings are decoded first and compared afterwards, so `"schema"` is the key
 *    `schema` (and a duplicate of it). That is what a JSON reader sees and what the schema
 *    checks.
 *  - A lone surrogate escape is invalid (ERR_UTF8): it has no UTF-8 form.
 *  - Escapes that produce C0 controls other than U+0000 are accepted in `label` and in
 *    `nonnfc_paths` (the probe's writer emits them): the label sanitiser and the path
 *    display sanitiser handle them. In every other string the allowed values are fixed
 *    sets, so a control character fails the enum or pattern.
 *  - A value of the wrong JSON type is ERR_TYPE; a number that breaks the number form
 *    (sign, fraction, exponent, leading zero, trailing letters) is ERR_SYNTAX; a
 *    well-formed integer outside its range is ERR_RANGE.
 */
#include "ntfsmac/probe.h"

#include <string.h>

#include "sanitize.h"

/* Nesting: the object is depth 1, an array depth 2; a container inside an array is depth 3. */
#define KEY_BUF 32u
#define FIXED_BUF 32u

#define KEY_LIST(X)                          \
	X(SCHEMA, "schema")                  \
	X(IS_NTFS, "is_ntfs")                \
	X(NTFS_VERSION, "ntfs_version")      \
	X(LABEL, "label")                    \
	X(SERIAL, "serial")                  \
	X(TOTAL_SECTORS, "total_sectors")    \
	X(CLUSTER_SIZE, "cluster_size")      \
	X(SECTOR_SIZE, "sector_size")        \
	X(VOLUME_FLAGS, "volume_flags")      \
	X(LOGFILE, "logfile")                \
	X(LOGFILE_LSN, "logfile_lsn")        \
	X(HIBERFILE, "hiberfile")            \
	X(BITLOCKER, "bitlocker")            \
	X(STATE, "state")                    \
	X(REASON_CODES, "reason_codes")      \
	X(NONNFC_COUNT, "nonnfc_count")      \
	X(NONNFC_PATHS, "nonnfc_paths")

enum key {
#define X(id, str) K_##id,
	KEY_LIST(X)
#undef X
	K_COUNT
};

static const char *const key_names[K_COUNT] = {
#define X(id, str) str,
	KEY_LIST(X)
#undef X
};

static const char *const state_names[] = {
#define X(id, str) str,
	NTFSMAC_PROBE_STATE_LIST(X)
#undef X
};
static const char *const logfile_names[] = {
#define X(id, str) str,
	NTFSMAC_PROBE_LOGFILE_LIST(X)
#undef X
};
static const char *const hiberfile_names[] = {
#define X(id, str) str,
	NTFSMAC_PROBE_HIBERFILE_LIST(X)
#undef X
};
static const char *const volflag_names[] = {
#define X(id, str) str,
	NTFSMAC_PROBE_VOLFLAG_LIST(X)
#undef X
};
static const char *const reason_names[] = {
#define X(id, str) str,
	NTFSMAC_PROBE_REASON_LIST(X)
#undef X
};

_Static_assert(K_COUNT == 17, "section 3.1 has 17 keys");
_Static_assert(K_COUNT <= 32, "the seen set fits uint32_t");
_Static_assert(sizeof(state_names) / sizeof(state_names[0]) == NTFSMAC_PROBE_STATE_COUNT_, "states");
_Static_assert(sizeof(logfile_names) / sizeof(logfile_names[0]) == NTFSMAC_PROBE_LOGFILE_COUNT_, "logfile");
_Static_assert(sizeof(hiberfile_names) / sizeof(hiberfile_names[0]) == NTFSMAC_PROBE_HIBERFILE_COUNT_, "hiberfile");
_Static_assert(sizeof(volflag_names) / sizeof(volflag_names[0]) == NTFSMAC_PROBE_VOLFLAG_COUNT_, "volume flags");
_Static_assert(sizeof(reason_names) / sizeof(reason_names[0]) == NTFSMAC_PROBE_REASON_COUNT_, "reasons");

/* ---------------------------------------------------------------------- names */

const char *ntfsmac_probe_state_name(ntfsmac_probe_state s)
{
	return (unsigned)s < (unsigned)NTFSMAC_PROBE_STATE_COUNT_ ? state_names[s] : NULL;
}

const char *ntfsmac_probe_logfile_name(ntfsmac_probe_logfile l)
{
	return (unsigned)l < (unsigned)NTFSMAC_PROBE_LOGFILE_COUNT_ ? logfile_names[l] : NULL;
}

const char *ntfsmac_probe_hiberfile_name(ntfsmac_probe_hiberfile h)
{
	return (unsigned)h < (unsigned)NTFSMAC_PROBE_HIBERFILE_COUNT_ ? hiberfile_names[h] : NULL;
}

const char *ntfsmac_probe_volflag_name(ntfsmac_probe_volflag f)
{
	return (unsigned)f < (unsigned)NTFSMAC_PROBE_VOLFLAG_COUNT_ ? volflag_names[f] : NULL;
}

const char *ntfsmac_probe_reason_name(ntfsmac_probe_reason r)
{
	return (unsigned)r < (unsigned)NTFSMAC_PROBE_REASON_COUNT_ ? reason_names[r] : NULL;
}

/* ------------------------------------------------------------ fail-closed verdict */

void ntfsmac_probe_verdict_unknown(ntfsmac_probe_verdict *out)
{
	memset(out, 0, sizeof(*out));
	out->logfile = NTFSMAC_PROBE_LOGFILE_UNREADABLE;
	out->hiberfile = NTFSMAC_PROBE_HIBERFILE_UNREADABLE;
	out->state = NTFSMAC_PROBE_STATE_UNKNOWN;
	out->reason_codes = NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_CHECK_FAILED);
}

/* ------------------------------------------------------------ cross-field rules */

ntfsmac_probe_status ntfsmac_probe_verdict_validate(const ntfsmac_probe_verdict *v)
{
	uint32_t i, want_paths;

	if (v == NULL)
		return NTFSMAC_PROBE_ERR_CROSS_FIELD;

	/* A built verdict must hold values the grammar has: fail closed on anything else. */
	if ((unsigned)v->state >= (unsigned)NTFSMAC_PROBE_STATE_COUNT_ ||
	    (unsigned)v->logfile >= (unsigned)NTFSMAC_PROBE_LOGFILE_COUNT_ ||
	    (unsigned)v->hiberfile >= (unsigned)NTFSMAC_PROBE_HIBERFILE_COUNT_ ||
	    (v->volume_flags >> (unsigned)NTFSMAC_PROBE_VOLFLAG_COUNT_) != 0 ||
	    (v->reason_codes >> (unsigned)NTFSMAC_PROBE_REASON_COUNT_) != 0)
		return NTFSMAC_PROBE_ERR_CROSS_FIELD;
	if (v->label_len > NTFSMAC_PROBE_LABEL_MAX_BYTES || v->label[v->label_len] != '\0' ||
	    memchr(v->label, '\0', v->label_len) != NULL)
		return NTFSMAC_PROBE_ERR_CROSS_FIELD;

	/* state `clean` if and only if reason_codes is empty. */
	if ((v->state == NTFSMAC_PROBE_STATE_CLEAN) != (v->reason_codes == 0))
		return NTFSMAC_PROBE_ERR_CROSS_FIELD;

	/* logfile_lsn is null if and only if logfile is `unreadable`. */
	if (v->has_logfile_lsn == (v->logfile == NTFSMAC_PROBE_LOGFILE_UNREADABLE))
		return NTFSMAC_PROBE_ERR_CROSS_FIELD;

	/* ADR-023: nonnfc_count > 0 iff NAMES_NOT_NFC; paths: exactly min(count, 16). */
	if ((v->nonnfc_count > 0) !=
	    ((v->reason_codes & NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_NAMES_NOT_NFC)) != 0))
		return NTFSMAC_PROBE_ERR_CROSS_FIELD;
	want_paths = v->nonnfc_count < NTFSMAC_PROBE_MAX_NONNFC_PATHS ? v->nonnfc_count
								      : NTFSMAC_PROBE_MAX_NONNFC_PATHS;
	if (v->nonnfc_path_count != want_paths)
		return NTFSMAC_PROBE_ERR_CROSS_FIELD;
	for (i = 0; i < v->nonnfc_path_count; i++) {
		if (memchr(v->nonnfc_paths[i], '\0', NTFSMAC_PROBE_NONNFC_PATH_MAX_BYTES + 1u) == NULL)
			return NTFSMAC_PROBE_ERR_CROSS_FIELD;
	}
	return NTFSMAC_PROBE_OK;
}

/* ---------------------------------------------------------------------- scanner */

typedef ntfsmac_probe_status status;

typedef struct parser {
	const unsigned char *p;
	const unsigned char *end;
} parser;

#define OK NTFSMAC_PROBE_OK

static void skip_ws(parser *ps)
{
	while (ps->p < ps->end &&
	       (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
		ps->p++;
}

/* A token ends at whitespace, `,`, `}` or `]` (or at the end of the input). */
static bool at_delimiter(const parser *ps)
{
	unsigned char c;

	if (ps->p >= ps->end)
		return true;
	c = *ps->p;
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',' || c == '}' || c == ']';
}

static bool match_literal(parser *ps, const char *lit)
{
	size_t n = strlen(lit);

	if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, lit, n) != 0)
		return false;
	ps->p += n;
	return at_delimiter(ps);
}

/*
 * The value at ps->p is not of the type the key needs. A well-formed value of another
 * type is ERR_TYPE; anything that is no JSON value at all is ERR_SYNTAX. Nothing is
 * descended into.
 */
static status type_mismatch(parser *ps)
{
	unsigned char c;

	if (ps->p >= ps->end)
		return NTFSMAC_PROBE_ERR_SYNTAX;
	c = *ps->p;
	if (c == '"' || c == '[' || c == '{' || c == '-' || (c >= '0' && c <= '9'))
		return NTFSMAC_PROBE_ERR_TYPE;
	if (c == 't')
		return match_literal(ps, "true") ? NTFSMAC_PROBE_ERR_TYPE : NTFSMAC_PROBE_ERR_SYNTAX;
	if (c == 'f')
		return match_literal(ps, "false") ? NTFSMAC_PROBE_ERR_TYPE : NTFSMAC_PROBE_ERR_SYNTAX;
	if (c == 'n')
		return match_literal(ps, "null") ? NTFSMAC_PROBE_ERR_TYPE : NTFSMAC_PROBE_ERR_SYNTAX;
	return NTFSMAC_PROBE_ERR_SYNTAX;
}

static int hex_digit(unsigned char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Four hex digits of a \u escape at ps->p. */
static bool read_hex4(parser *ps, uint32_t *v)
{
	uint32_t r = 0;
	int i;

	if ((size_t)(ps->end - ps->p) < 4)
		return false;
	for (i = 0; i < 4; i++) {
		int d = hex_digit(ps->p[i]);

		if (d < 0)
			return false;
		r = (r << 4) | (uint32_t)d;
	}
	ps->p += 4;
	*v = r;
	return true;
}

static status put_byte(char *buf, size_t cap, size_t *n, unsigned char b)
{
	if (*n + 1 >= cap) /* keep room for the NUL */
		return NTFSMAC_PROBE_ERR_RANGE;
	buf[(*n)++] = (char)b;
	return OK;
}

static status put_cp(char *buf, size_t cap, size_t *n, uint32_t cp)
{
	unsigned char u[4];
	size_t len, i;

	if (cp < 0x80) {
		u[0] = (unsigned char)cp;
		len = 1;
	} else if (cp < 0x800) {
		u[0] = (unsigned char)(0xC0 | (cp >> 6));
		u[1] = (unsigned char)(0x80 | (cp & 0x3F));
		len = 2;
	} else if (cp < 0x10000) {
		u[0] = (unsigned char)(0xE0 | (cp >> 12));
		u[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
		u[2] = (unsigned char)(0x80 | (cp & 0x3F));
		len = 3;
	} else {
		u[0] = (unsigned char)(0xF0 | (cp >> 18));
		u[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
		u[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
		u[3] = (unsigned char)(0x80 | (cp & 0x3F));
		len = 4;
	}
	for (i = 0; i < len; i++) {
		status st = put_byte(buf, cap, n, u[i]);

		if (st != OK)
			return st;
	}
	return OK;
}

/*
 * Decode the JSON string at ps->p (which is a `"`) into buf[0..cap), NUL terminated, and
 * store its decoded length. ERR_RANGE if the decoded text does not fit (the caller maps it
 * for fields whose limit is a grammar rather than a length).
 */
static status decode_string(parser *ps, char *buf, size_t cap, size_t *outlen)
{
	size_t n = 0;

	ps->p++; /* opening quote */
	for (;;) {
		unsigned char c;
		status st;

		if (ps->p >= ps->end)
			return NTFSMAC_PROBE_ERR_SYNTAX; /* unterminated */
		c = *ps->p++;
		if (c == '"')
			break;
		if (c == 0)
			return NTFSMAC_PROBE_ERR_NUL;
		if (c < 0x20)
			return NTFSMAC_PROBE_ERR_SYNTAX; /* raw control character */
		if (c != '\\') {
			st = put_byte(buf, cap, &n, c); /* UTF-8 was checked for the whole input */
			if (st != OK)
				return st;
			continue;
		}
		if (ps->p >= ps->end)
			return NTFSMAC_PROBE_ERR_SYNTAX;
		c = *ps->p++;
		switch (c) {
		case '"':
		case '\\':
		case '/':
			st = put_byte(buf, cap, &n, c);
			break;
		case 'b':
			st = put_byte(buf, cap, &n, 0x08);
			break;
		case 'f':
			st = put_byte(buf, cap, &n, 0x0C);
			break;
		case 'n':
			st = put_byte(buf, cap, &n, 0x0A);
			break;
		case 'r':
			st = put_byte(buf, cap, &n, 0x0D);
			break;
		case 't':
			st = put_byte(buf, cap, &n, 0x09);
			break;
		case 'u': {
			uint32_t cp, lo;

			if (!read_hex4(ps, &cp))
				return NTFSMAC_PROBE_ERR_SYNTAX;
			if (cp == 0)
				return NTFSMAC_PROBE_ERR_NUL;
			if (cp >= 0xD800 && cp <= 0xDBFF) {
				/* A high surrogate needs its low half right behind it. */
				if ((size_t)(ps->end - ps->p) < 2 || ps->p[0] != '\\' || ps->p[1] != 'u')
					return NTFSMAC_PROBE_ERR_UTF8;
				ps->p += 2;
				if (!read_hex4(ps, &lo))
					return NTFSMAC_PROBE_ERR_SYNTAX;
				if (lo < 0xDC00 || lo > 0xDFFF)
					return NTFSMAC_PROBE_ERR_UTF8;
				cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
			} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
				return NTFSMAC_PROBE_ERR_UTF8;
			}
			st = put_cp(buf, cap, &n, cp);
			break;
		}
		default:
			return NTFSMAC_PROBE_ERR_SYNTAX; /* unknown escape */
		}
		if (st != OK)
			return st;
	}
	buf[n] = '\0';
	*outlen = n;
	return OK;
}

/* A string value of a field with a fixed set of spellings; too long is simply not in the set. */
static status fixed_string(parser *ps, char *buf, size_t cap, size_t *len, status on_range)
{
	status st;

	if (ps->p >= ps->end)
		return NTFSMAC_PROBE_ERR_SYNTAX;
	if (*ps->p != '"')
		return type_mismatch(ps);
	st = decode_string(ps, buf, cap, len);
	return st == NTFSMAC_PROBE_ERR_RANGE ? on_range : st;
}

static status parse_enum(parser *ps, const char *const *names, unsigned count, unsigned *idx)
{
	char buf[FIXED_BUF];
	size_t len;
	unsigned i;
	status st = fixed_string(ps, buf, sizeof(buf), &len, NTFSMAC_PROBE_ERR_ENUM);

	if (st != OK)
		return st;
	for (i = 0; i < count; i++) {
		if (strlen(names[i]) == len && memcmp(names[i], buf, len) == 0) {
			*idx = i;
			return OK;
		}
	}
	return NTFSMAC_PROBE_ERR_ENUM;
}

/* "0x" and 16 upper-case hex digits. */
static status parse_hex64_string(parser *ps, uint64_t *v)
{
	char buf[FIXED_BUF];
	size_t len, i;
	uint64_t r = 0;
	status st = fixed_string(ps, buf, sizeof(buf), &len, NTFSMAC_PROBE_ERR_GRAMMAR);

	if (st != OK)
		return st;
	if (len != 18 || buf[0] != '0' || buf[1] != 'x')
		return NTFSMAC_PROBE_ERR_GRAMMAR;
	for (i = 2; i < 18; i++) {
		char c = buf[i];
		unsigned d;

		if (c >= '0' && c <= '9')
			d = (unsigned)(c - '0');
		else if (c >= 'A' && c <= 'F')
			d = (unsigned)(c - 'A' + 10);
		else
			return NTFSMAC_PROBE_ERR_GRAMMAR;
		r = (r << 4) | d;
	}
	*v = r;
	return OK;
}

static status parse_bool(parser *ps, bool *out)
{
	if (ps->p >= ps->end)
		return NTFSMAC_PROBE_ERR_SYNTAX;
	if (*ps->p == 't') {
		if (!match_literal(ps, "true"))
			return NTFSMAC_PROBE_ERR_SYNTAX;
		*out = true;
		return OK;
	}
	if (*ps->p == 'f') {
		if (!match_literal(ps, "false"))
			return NTFSMAC_PROBE_ERR_SYNTAX;
		*out = false;
		return OK;
	}
	return type_mismatch(ps);
}

/* `null` if the value is the literal null (and is consumed); sets *is_null. */
static status parse_null(parser *ps, bool *is_null)
{
	*is_null = false;
	if (ps->p < ps->end && *ps->p == 'n') {
		if (!match_literal(ps, "null"))
			return NTFSMAC_PROBE_ERR_SYNTAX;
		*is_null = true;
	}
	return OK;
}

/*
 * A JSON integer: `0`, or a non-zero digit and digits. No sign, fraction, exponent,
 * leading zero or trailing letters (ERR_SYNTAX); outside min..max is ERR_RANGE.
 */
static status parse_uint(parser *ps, uint64_t min, uint64_t max, uint64_t *out)
{
	uint64_t v = 0;
	bool overflow = false;
	unsigned char c;

	if (ps->p >= ps->end)
		return NTFSMAC_PROBE_ERR_SYNTAX;
	c = *ps->p;
	if (c == '-')
		return NTFSMAC_PROBE_ERR_SYNTAX;
	if (c < '0' || c > '9')
		return type_mismatch(ps);
	if (c == '0') {
		ps->p++;
		if (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9')
			return NTFSMAC_PROBE_ERR_SYNTAX; /* leading zero */
	} else {
		while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') {
			uint64_t d = (uint64_t)(*ps->p - '0');

			if (v > (UINT64_MAX - d) / 10u)
				overflow = true;
			else
				v = v * 10u + d;
			ps->p++;
		}
	}
	if (ps->p < ps->end && (*ps->p == '.' || *ps->p == 'e' || *ps->p == 'E'))
		return NTFSMAC_PROBE_ERR_SYNTAX; /* fraction or exponent */
	if (!at_delimiter(ps))
		return NTFSMAC_PROBE_ERR_SYNTAX;
	if (overflow || v < min || v > max)
		return NTFSMAC_PROBE_ERR_RANGE;
	*out = v;
	return OK;
}

/* `volume_flags`, `reason_codes` and `nonnfc_paths`: arrays of strings, depth 2. */
static status parse_array(parser *ps, enum key key, ntfsmac_probe_verdict *v)
{
	unsigned count = 0;
	unsigned cap = key == K_NONNFC_PATHS     ? NTFSMAC_PROBE_MAX_NONNFC_PATHS
		       : key == K_VOLUME_FLAGS ? (unsigned)NTFSMAC_PROBE_MAX_VOLUME_FLAGS
					       : (unsigned)NTFSMAC_PROBE_MAX_REASON_CODES;

	if (ps->p >= ps->end)
		return NTFSMAC_PROBE_ERR_SYNTAX;
	if (*ps->p != '[')
		return type_mismatch(ps);
	ps->p++;
	skip_ws(ps);
	if (ps->p < ps->end && *ps->p == ']') {
		ps->p++;
		return OK;
	}
	for (;;) {
		status st;

		skip_ws(ps);
		if (ps->p >= ps->end)
			return NTFSMAC_PROBE_ERR_SYNTAX;
		if (*ps->p == '[' || *ps->p == '{')
			return NTFSMAC_PROBE_ERR_DEPTH;
		if (*ps->p != '"')
			return type_mismatch(ps);
		if (count == cap)
			return NTFSMAC_PROBE_ERR_RANGE;
		if (key == K_NONNFC_PATHS) {
			size_t len;

			st = decode_string(ps, v->nonnfc_paths[count], sizeof(v->nonnfc_paths[count]),
					   &len);
			if (st != OK)
				return st;
		} else {
			unsigned idx;
			uint32_t *set = key == K_VOLUME_FLAGS ? &v->volume_flags : &v->reason_codes;

			if (key == K_VOLUME_FLAGS)
				st = parse_enum(ps, volflag_names, NTFSMAC_PROBE_VOLFLAG_COUNT_, &idx);
			else
				st = parse_enum(ps, reason_names, NTFSMAC_PROBE_REASON_COUNT_, &idx);
			if (st != OK)
				return st;
			if (*set & (UINT32_C(1) << idx))
				return NTFSMAC_PROBE_ERR_GRAMMAR; /* duplicate member */
			*set |= UINT32_C(1) << idx;
		}
		count++;
		skip_ws(ps);
		if (ps->p >= ps->end)
			return NTFSMAC_PROBE_ERR_SYNTAX;
		if (*ps->p == ',') {
			ps->p++;
			continue;
		}
		if (*ps->p == ']') {
			ps->p++;
			break;
		}
		return NTFSMAC_PROBE_ERR_SYNTAX;
	}
	if (key == K_NONNFC_PATHS)
		v->nonnfc_path_count = count;
	return OK;
}

static status parse_value(parser *ps, enum key key, ntfsmac_probe_verdict *v)
{
	status st;
	uint64_t u = 0;
	unsigned idx = 0;
	bool is_null;

	switch (key) {
	case K_SCHEMA:
		return parse_uint(ps, NTFSMAC_PROBE_SCHEMA_VERSION, NTFSMAC_PROBE_SCHEMA_VERSION, &u);
	case K_IS_NTFS:
		return parse_bool(ps, &v->is_ntfs);
	case K_BITLOCKER:
		return parse_bool(ps, &v->bitlocker);
	case K_NTFS_VERSION: {
		char buf[FIXED_BUF];
		size_t len;

		st = parse_null(ps, &is_null);
		if (st != OK || is_null)
			return st;
		st = fixed_string(ps, buf, sizeof(buf), &len, NTFSMAC_PROBE_ERR_GRAMMAR);
		if (st != OK)
			return st;
		if (len != 3 || buf[0] < '1' || buf[0] > '3' || buf[1] != '.' || buf[2] < '0' ||
		    buf[2] > '9')
			return NTFSMAC_PROBE_ERR_GRAMMAR;
		v->has_ntfs_version = true;
		v->ntfs_version_major = (uint8_t)(buf[0] - '0');
		v->ntfs_version_minor = (uint8_t)(buf[2] - '0');
		return OK;
	}
	case K_LABEL:
		if (ps->p >= ps->end)
			return NTFSMAC_PROBE_ERR_SYNTAX;
		if (*ps->p != '"')
			return type_mismatch(ps);
		return decode_string(ps, v->label, sizeof(v->label), &v->label_len);
	case K_SERIAL:
		return parse_hex64_string(ps, &v->serial);
	case K_TOTAL_SECTORS:
		return parse_uint(ps, 1, UINT64_C(0x7FFFFFFFFFFFFFFF), &v->total_sectors);
	case K_CLUSTER_SIZE:
		st = parse_uint(ps, 512, 2097152, &u);
		if (st != OK)
			return st;
		if ((u & (u - 1)) != 0)
			return NTFSMAC_PROBE_ERR_RANGE; /* not a power of two */
		v->cluster_size = (uint32_t)u;
		return OK;
	case K_SECTOR_SIZE:
		st = parse_uint(ps, 512, 4096, &u);
		if (st != OK)
			return st;
		if ((u & (u - 1)) != 0)
			return NTFSMAC_PROBE_ERR_RANGE; /* 512, 1024, 2048 or 4096 only */
		v->sector_size = (uint32_t)u;
		return OK;
	case K_VOLUME_FLAGS:
	case K_REASON_CODES:
	case K_NONNFC_PATHS:
		return parse_array(ps, key, v);
	case K_LOGFILE:
		st = parse_enum(ps, logfile_names, NTFSMAC_PROBE_LOGFILE_COUNT_, &idx);
		if (st == OK)
			v->logfile = (ntfsmac_probe_logfile)idx;
		return st;
	case K_LOGFILE_LSN:
		st = parse_null(ps, &is_null);
		if (st != OK || is_null)
			return st;
		st = parse_hex64_string(ps, &v->logfile_lsn);
		if (st == OK)
			v->has_logfile_lsn = true;
		return st;
	case K_HIBERFILE:
		st = parse_enum(ps, hiberfile_names, NTFSMAC_PROBE_HIBERFILE_COUNT_, &idx);
		if (st == OK)
			v->hiberfile = (ntfsmac_probe_hiberfile)idx;
		return st;
	case K_STATE:
		st = parse_enum(ps, state_names, NTFSMAC_PROBE_STATE_COUNT_, &idx);
		if (st == OK)
			v->state = (ntfsmac_probe_state)idx;
		return st;
	case K_NONNFC_COUNT:
		st = parse_uint(ps, 0, UINT32_MAX, &u);
		if (st == OK)
			v->nonnfc_count = (uint32_t)u;
		return st;
	case K_COUNT:
	default:
		return NTFSMAC_PROBE_ERR_UNKNOWN_KEY;
	}
}

static status parse_object(parser *ps, ntfsmac_probe_verdict *v)
{
	uint32_t seen = 0;
	const uint32_t all = (UINT32_C(1) << K_COUNT) - 1u;

	skip_ws(ps);
	if (ps->p >= ps->end || *ps->p != '{')
		return NTFSMAC_PROBE_ERR_SYNTAX; /* not one JSON object */
	ps->p++;
	skip_ws(ps);
	if (ps->p < ps->end && *ps->p == '}') {
		ps->p++;
		return NTFSMAC_PROBE_ERR_MISSING_KEY;
	}
	for (;;) {
		char kbuf[KEY_BUF];
		size_t klen;
		unsigned k;
		status st;

		skip_ws(ps);
		if (ps->p >= ps->end || *ps->p != '"')
			return NTFSMAC_PROBE_ERR_SYNTAX;
		st = decode_string(ps, kbuf, sizeof(kbuf), &klen);
		if (st == NTFSMAC_PROBE_ERR_RANGE)
			return NTFSMAC_PROBE_ERR_UNKNOWN_KEY; /* longer than any key */
		if (st != OK)
			return st;
		for (k = 0; k < (unsigned)K_COUNT; k++) {
			if (strlen(key_names[k]) == klen && memcmp(key_names[k], kbuf, klen) == 0)
				break;
		}
		if (k == (unsigned)K_COUNT)
			return NTFSMAC_PROBE_ERR_UNKNOWN_KEY;
		if (seen & (UINT32_C(1) << k))
			return NTFSMAC_PROBE_ERR_DUP_KEY;
		seen |= UINT32_C(1) << k;
		skip_ws(ps);
		if (ps->p >= ps->end || *ps->p != ':')
			return NTFSMAC_PROBE_ERR_SYNTAX;
		ps->p++;
		skip_ws(ps);
		st = parse_value(ps, (enum key)k, v);
		if (st != OK)
			return st;
		skip_ws(ps);
		if (ps->p >= ps->end)
			return NTFSMAC_PROBE_ERR_SYNTAX;
		if (*ps->p == ',') {
			ps->p++;
			continue;
		}
		if (*ps->p == '}') {
			ps->p++;
			break;
		}
		return NTFSMAC_PROBE_ERR_SYNTAX;
	}
	skip_ws(ps);
	if (ps->p != ps->end)
		return NTFSMAC_PROBE_ERR_SYNTAX; /* trailing data */
	return seen == all ? OK : NTFSMAC_PROBE_ERR_MISSING_KEY;
}

/* The whole input is one UTF-8 text without NUL, before any token is read. */
static status check_text(const unsigned char *s, size_t len)
{
	size_t i = 0;

	while (i < len) {
		uint32_t cp;
		size_t n;

		if (s[i] == 0)
			return NTFSMAC_PROBE_ERR_NUL;
		if (s[i] < 0x80) {
			i++;
			continue;
		}
		n = ntfsmac_utf8_decode(s + i, len - i, &cp);
		if (n == 0)
			return NTFSMAC_PROBE_ERR_UTF8;
		i += n;
	}
	return OK;
}

ntfsmac_probe_status ntfsmac_probe_parse(const char *json, size_t len, ntfsmac_probe_verdict *out)
{
	ntfsmac_probe_verdict v;
	parser ps;
	status st;

	if (out == NULL)
		return NTFSMAC_PROBE_ERR_SYNTAX;
	memset(&v, 0, sizeof(v));
	if (len > NTFSMAC_PROBE_MAX_JSON_BYTES) {
		st = NTFSMAC_PROBE_ERR_TOO_LARGE;
	} else if (json == NULL) {
		st = NTFSMAC_PROBE_ERR_SYNTAX;
	} else {
		st = check_text((const unsigned char *)json, len);
		if (st == OK) {
			ps.p = (const unsigned char *)json;
			ps.end = ps.p + len;
			st = parse_object(&ps, &v);
			if (st == OK)
				st = ntfsmac_probe_verdict_validate(&v);
		}
	}
	if (st != OK) {
		ntfsmac_probe_verdict_unknown(out);
		return st;
	}
	*out = v;
	return OK;
}
