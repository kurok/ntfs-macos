/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Strict parser of one control request line (T-M4-04, section 3.4).
 * No system call, no allocation, no recursion. The input comes from any
 * process that can connect to the socket, so every path ends in either a
 * filled request or a result code.
 */
#include <string.h>

#include "ctl_priv.h"

/* ------------------------------------------------------------------ */
/* Wire names                                                          */
/* ------------------------------------------------------------------ */

#define NAME_FN(fn, type, count, list)                      \
	const char *fn(type v)                              \
	{                                                   \
		static const char *const names[] = {list(NAME_X)}; \
		if ((unsigned)v >= (unsigned)(count))       \
			return "?";                         \
		return names[v];                            \
	}
#define NAME_X(id, str) str,
NAME_FN(ntfsmac_ctl_cmd_name, ntfsmac_ctl_cmd, NTFSMAC_CTL_CMD_COUNT_, NTFSMAC_CTL_CMD_LIST)
NAME_FN(ntfsmac_ctl_error_name, ntfsmac_ctl_error, NTFSMAC_CTL_ERR_COUNT_, NTFSMAC_CTL_ERROR_LIST)
NAME_FN(ntfsmac_ctl_refusal_name, ntfsmac_ctl_refusal, NTFSMAC_CTL_REFUSE_COUNT_,
	NTFSMAC_CTL_REFUSAL_LIST)
#undef NAME_X
#undef NAME_FN

/* ------------------------------------------------------------------ */
/* Field table                                                         */
/* ------------------------------------------------------------------ */

enum field {
	F_CMD,
	F_BSD,
	F_MODE,
	F_FORCE,
	F_ACTION,
	F_CONFIRM_LABEL,
	F_CONFIRM_SERIAL,
	F_AUTH,
	F_COUNT
};

static const struct {
	const char *key;
	bool is_bool;
	size_t max; /* longest decoded string, in bytes */
} fields[F_COUNT] = {
	[F_CMD] = {"cmd", false, 15},
	[F_BSD] = {"bsd", false, NTFSMAC_CTL_BSD_MAX},
	[F_MODE] = {"mode", false, 7},
	[F_FORCE] = {"force", true, 0},
	[F_ACTION] = {"action", false, 15},
	[F_CONFIRM_LABEL] = {"confirm_label", false, NTFSMAC_CTL_LABEL_MAX},
	[F_CONFIRM_SERIAL] = {"confirm_serial", false, NTFSMAC_CTL_SERIAL_DIGITS},
	[F_AUTH] = {"auth", false, 2 * NTFSMAC_CTL_AUTH_FORM_BYTES},
};

/* Which fields a command requires (bit mask) and which it may carry. */
#define BIT(f) (1u << (f))
static const struct {
	unsigned required;
	unsigned optional;
} cmd_fields[NTFSMAC_CTL_CMD_COUNT_] = {
	[NTFSMAC_CTL_CMD_LIST] = {0, 0},
	[NTFSMAC_CTL_CMD_STATUS] = {BIT(F_BSD), 0},
	[NTFSMAC_CTL_CMD_MOUNT] = {BIT(F_BSD), BIT(F_MODE)},
	[NTFSMAC_CTL_CMD_UNMOUNT] = {BIT(F_BSD), BIT(F_FORCE)},
	[NTFSMAC_CTL_CMD_DOCTOR] = {0, 0},
	/* `auth` is optional here on purpose: a missing form is a refusal of check 4
	 * (reason no_authorization), after checks 1 to 3, not a malformed request. */
	[NTFSMAC_CTL_CMD_JOURNAL_CLEAR] = {BIT(F_BSD) | BIT(F_CONFIRM_LABEL) | BIT(F_CONFIRM_SERIAL),
					   BIT(F_AUTH)},
	[NTFSMAC_CTL_CMD_REPAIR] = {BIT(F_BSD) | BIT(F_ACTION) | BIT(F_CONFIRM_LABEL) |
					    BIT(F_CONFIRM_SERIAL),
				    BIT(F_AUTH)},
};

/* ------------------------------------------------------------------ */
/* Lexer                                                               */
/* ------------------------------------------------------------------ */

typedef struct cursor {
	const unsigned char *p;
	const unsigned char *end;
} cursor;

typedef enum lex {
	LEX_OK = 0,
	LEX_SYNTAX, /* not JSON of our grammar */
	LEX_LONG    /* a string longer than the field allows */
} lex;

static void skip_ws(cursor *c)
{
	while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\r' || *c->p == '\n'))
		c->p++;
}

static int hex_val(unsigned char ch)
{
	if (ch >= '0' && ch <= '9')
		return ch - '0';
	if (ch >= 'A' && ch <= 'F')
		return ch - 'A' + 10;
	if (ch >= 'a' && ch <= 'f')
		return ch - 'a' + 10;
	return -1;
}

static bool read_hex4(cursor *c, unsigned *out)
{
	unsigned v = 0;
	int i;

	if ((size_t)(c->end - c->p) < 4)
		return false;
	for (i = 0; i < 4; i++) {
		int h = hex_val(c->p[i]);

		if (h < 0)
			return false;
		v = v * 16 + (unsigned)h;
	}
	c->p += 4;
	*out = v;
	return true;
}

/* Append the UTF-8 form of `cp` (a scalar value, not NUL). */
static lex put_utf8(unsigned cp, char *dst, size_t cap, size_t *n)
{
	unsigned char tmp[4];
	size_t k;

	if (cp < 0x80) {
		tmp[0] = (unsigned char)cp;
		k = 1;
	} else if (cp < 0x800) {
		tmp[0] = (unsigned char)(0xC0 | (cp >> 6));
		tmp[1] = (unsigned char)(0x80 | (cp & 0x3F));
		k = 2;
	} else if (cp < 0x10000) {
		tmp[0] = (unsigned char)(0xE0 | (cp >> 12));
		tmp[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
		tmp[2] = (unsigned char)(0x80 | (cp & 0x3F));
		k = 3;
	} else {
		tmp[0] = (unsigned char)(0xF0 | (cp >> 18));
		tmp[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
		tmp[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
		tmp[3] = (unsigned char)(0x80 | (cp & 0x3F));
		k = 4;
	}
	if (*n + k > cap)
		return LEX_LONG;
	memcpy(dst + *n, tmp, k);
	*n += k;
	return LEX_OK;
}

/* Length of a well-formed UTF-8 sequence at p (no overlong, no surrogate), or 0. */
static size_t utf8_seq_len(const unsigned char *p, size_t avail)
{
	unsigned char b = p[0];

	if (b < 0x80)
		return 1;
	if (b >= 0xC2 && b <= 0xDF)
		return avail >= 2 && (p[1] & 0xC0) == 0x80 ? 2 : 0;
	if (b >= 0xE0 && b <= 0xEF) {
		if (avail < 3 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80)
			return 0;
		if (b == 0xE0 && p[1] < 0xA0)
			return 0; /* overlong */
		if (b == 0xED && p[1] >= 0xA0)
			return 0; /* surrogate */
		return 3;
	}
	if (b >= 0xF0 && b <= 0xF4) {
		if (avail < 4 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 ||
		    (p[3] & 0xC0) != 0x80)
			return 0;
		if (b == 0xF0 && p[1] < 0x90)
			return 0; /* overlong */
		if (b == 0xF4 && p[1] >= 0x90)
			return 0; /* above U+10FFFF */
		return 4;
	}
	return 0;
}

/*
 * Read a JSON string at c->p (which must be the opening quote) into dst, at
 * most `max` decoded bytes, NUL terminated. dst has room for max + 1.
 */
static lex read_string(cursor *c, char *dst, size_t max, size_t *outlen)
{
	size_t n = 0;

	if (c->p >= c->end || *c->p != '"')
		return LEX_SYNTAX;
	c->p++;
	for (;;) {
		unsigned char ch;

		if (c->p >= c->end)
			return LEX_SYNTAX;
		ch = *c->p;
		if (ch == '"') {
			c->p++;
			break;
		}
		if (ch < 0x20)
			return LEX_SYNTAX; /* raw control byte, including NUL */
		if (ch == '\\') {
			unsigned cp = 0;
			lex r;

			c->p++;
			if (c->p >= c->end)
				return LEX_SYNTAX;
			switch (*c->p) {
			case '"': cp = '"'; c->p++; break;
			case '\\': cp = '\\'; c->p++; break;
			case '/': cp = '/'; c->p++; break;
			case 'b': cp = '\b'; c->p++; break;
			case 'f': cp = '\f'; c->p++; break;
			case 'n': cp = '\n'; c->p++; break;
			case 'r': cp = '\r'; c->p++; break;
			case 't': cp = '\t'; c->p++; break;
			case 'u':
				c->p++;
				if (!read_hex4(c, &cp))
					return LEX_SYNTAX;
				if (cp >= 0xD800 && cp <= 0xDBFF) {
					unsigned lo = 0;

					if ((size_t)(c->end - c->p) < 2 || c->p[0] != '\\' ||
					    c->p[1] != 'u')
						return LEX_SYNTAX;
					c->p += 2;
					if (!read_hex4(c, &lo) || lo < 0xDC00 || lo > 0xDFFF)
						return LEX_SYNTAX;
					cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
					return LEX_SYNTAX; /* lone low surrogate */
				}
				if (cp == 0)
					return LEX_SYNTAX; /* \u0000 */
				break;
			default:
				return LEX_SYNTAX;
			}
			r = put_utf8(cp, dst, max, &n);
			if (r != LEX_OK)
				return r;
			continue;
		}
		if (ch < 0x80) {
			if (n + 1 > max)
				return LEX_LONG;
			dst[n++] = (char)ch;
			c->p++;
			continue;
		}
		{
			size_t k = utf8_seq_len(c->p, (size_t)(c->end - c->p));

			if (k == 0)
				return LEX_SYNTAX; /* invalid UTF-8 */
			if (n + k > max)
				return LEX_LONG;
			memcpy(dst + n, c->p, k);
			n += k;
			c->p += k;
		}
	}
	dst[n] = '\0';
	*outlen = n;
	return LEX_OK;
}

/*
 * True if the decoded text holds a control code point: U+0000 to U+001F,
 * U+007F, or the C1 range U+0080 to U+009F (UTF-8 C2 80 to C2 9F). The text
 * is valid UTF-8 here. A label with such a code point cannot be the
 * sanitised label that `list` shows, and no other field may carry one.
 */
static bool has_control(const char *s, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		unsigned char b = (unsigned char)s[i];

		if (b < 0x20 || b == 0x7F)
			return true;
		if (b == 0xC2 && i + 1 < n && (unsigned char)s[i + 1] >= 0x80 &&
		    (unsigned char)s[i + 1] <= 0x9F)
			return true;
	}
	return false;
}

static bool match_word(cursor *c, const char *w)
{
	size_t n = strlen(w);

	if ((size_t)(c->end - c->p) < n || memcmp(c->p, w, n) != 0)
		return false;
	c->p += n;
	return true;
}

/* ------------------------------------------------------------------ */
/* Value checks                                                        */
/* ------------------------------------------------------------------ */

static bool all_digits(const char *s, size_t n)
{
	size_t i;

	if (n == 0 || n > 9)
		return false;
	for (i = 0; i < n; i++)
		if (s[i] < '0' || s[i] > '9')
			return false;
	return true;
}

bool ntfsmac_ctl_bsd_valid(const char *s)
{
	const char *p;
	size_t n;

	if (s == NULL || strncmp(s, "disk", 4) != 0)
		return false;
	p = s + 4;
	n = strspn(p, "0123456789");
	if (!all_digits(p, n))
		return false;
	p += n;
	if (*p == '\0')
		return true;
	if (*p != 's')
		return false;
	p++;
	n = strspn(p, "0123456789");
	return all_digits(p, n) && p[n] == '\0';
}

/* 64 hex digits (either case) to 32 bytes. */
static bool decode_form(const char *s, uint8_t out[NTFSMAC_CTL_AUTH_FORM_BYTES])
{
	size_t i;

	if (strlen(s) != 2 * NTFSMAC_CTL_AUTH_FORM_BYTES)
		return false;
	for (i = 0; i < NTFSMAC_CTL_AUTH_FORM_BYTES; i++) {
		int hi = hex_val((unsigned char)s[2 * i]);
		int lo = hex_val((unsigned char)s[2 * i + 1]);

		if (hi < 0 || lo < 0)
			return false;
		out[i] = (uint8_t)(hi * 16 + lo);
	}
	return true;
}

/* Exactly 8 hex digits; stored upper case. */
static bool normalise_serial(char *s)
{
	size_t i;

	if (strlen(s) != NTFSMAC_CTL_SERIAL_DIGITS)
		return false;
	for (i = 0; i < NTFSMAC_CTL_SERIAL_DIGITS; i++) {
		if (hex_val((unsigned char)s[i]) < 0)
			return false;
		if (s[i] >= 'a' && s[i] <= 'f')
			s[i] = (char)(s[i] - 'a' + 'A');
	}
	return true;
}

/* ------------------------------------------------------------------ */
/* Parser                                                              */
/* ------------------------------------------------------------------ */

static ntfsmac_ctl_parse_result lex_to_result(lex l)
{
	return l == LEX_LONG ? NTFSMAC_CTL_PARSE_BAD_FIELD : NTFSMAC_CTL_PARSE_BAD_JSON;
}

ntfsmac_ctl_parse_result ntfsmac_ctl_parse_request(const char *line, size_t len,
						   ntfsmac_ctl_request *out)
{
	/* Decoded values of every field; only the `seen` ones are meaningful. */
	char str[F_COUNT][NTFSMAC_CTL_LABEL_MAX + 1];
	bool boolean[F_COUNT] = {false};
	unsigned seen = 0;
	cursor c;
	unsigned cmd_id;

	if (out == NULL)
		return NTFSMAC_CTL_PARSE_BAD_JSON;
	memset(out, 0, sizeof *out);
	if (line == NULL || len > NTFSMAC_CTL_LINE_MAX)
		return line == NULL ? NTFSMAC_CTL_PARSE_BAD_JSON : NTFSMAC_CTL_PARSE_TOO_LONG;

	c.p = (const unsigned char *)line;
	c.end = c.p + len;
	skip_ws(&c);
	if (c.p >= c.end || *c.p != '{')
		return NTFSMAC_CTL_PARSE_BAD_JSON;
	c.p++;
	skip_ws(&c);
	if (c.p < c.end && *c.p == '}') {
		c.p++;
	} else {
		for (;;) {
			char key[16];
			size_t keylen = 0, vlen = 0;
			unsigned f;
			lex r;

			skip_ws(&c);
			r = read_string(&c, key, sizeof key - 1, &keylen);
			if (r == LEX_LONG)
				return NTFSMAC_CTL_PARSE_BAD_FIELD; /* no key is this long */
			if (r != LEX_OK)
				return NTFSMAC_CTL_PARSE_BAD_JSON;
			skip_ws(&c);
			if (c.p >= c.end || *c.p != ':')
				return NTFSMAC_CTL_PARSE_BAD_JSON;
			c.p++;
			skip_ws(&c);

			for (f = 0; f < F_COUNT; f++)
				if (strcmp(key, fields[f].key) == 0)
					break;
			if (f == F_COUNT)
				return NTFSMAC_CTL_PARSE_BAD_FIELD; /* unknown key */
			if (seen & BIT(f))
				return NTFSMAC_CTL_PARSE_BAD_FIELD; /* duplicate key */
			seen |= BIT(f);

			if (c.p >= c.end)
				return NTFSMAC_CTL_PARSE_BAD_JSON;
			if (fields[f].is_bool) {
				if (match_word(&c, "true"))
					boolean[f] = true;
				else if (!match_word(&c, "false"))
					/* a value of another type is a bad field, anything else bad JSON */
					return strchr("\"-0123456789{[n", *c.p) != NULL
						       ? NTFSMAC_CTL_PARSE_BAD_FIELD
						       : NTFSMAC_CTL_PARSE_BAD_JSON;
			} else {
				if (*c.p != '"')
					return NTFSMAC_CTL_PARSE_BAD_FIELD; /* wrong value type */
				r = read_string(&c, str[f], fields[f].max, &vlen);
				if (r == LEX_LONG && f == F_CMD)
					return NTFSMAC_CTL_PARSE_UNKNOWN_COMMAND;
				if (r != LEX_OK)
					return lex_to_result(r);
				if (f != F_CMD && has_control(str[f], vlen))
					return NTFSMAC_CTL_PARSE_BAD_FIELD;
			}

			skip_ws(&c);
			if (c.p >= c.end)
				return NTFSMAC_CTL_PARSE_BAD_JSON;
			if (*c.p == ',') {
				c.p++;
				continue;
			}
			if (*c.p == '}') {
				c.p++;
				break;
			}
			return NTFSMAC_CTL_PARSE_BAD_JSON;
		}
	}
	skip_ws(&c);
	if (c.p != c.end)
		return NTFSMAC_CTL_PARSE_BAD_JSON; /* trailing bytes */

	/* The command, then the fields it takes. */
	if (!(seen & BIT(F_CMD)))
		return NTFSMAC_CTL_PARSE_UNKNOWN_COMMAND;
	for (cmd_id = 0; cmd_id < NTFSMAC_CTL_CMD_COUNT_; cmd_id++)
		if (strcmp(str[F_CMD], ntfsmac_ctl_cmd_name((ntfsmac_ctl_cmd)cmd_id)) == 0)
			break;
	if (cmd_id == NTFSMAC_CTL_CMD_COUNT_)
		return NTFSMAC_CTL_PARSE_UNKNOWN_COMMAND;

	{
		unsigned allowed = cmd_fields[cmd_id].required | cmd_fields[cmd_id].optional | BIT(F_CMD);

		if ((seen & ~allowed) != 0)
			return NTFSMAC_CTL_PARSE_BAD_FIELD; /* a field this command does not take */
		if ((cmd_fields[cmd_id].required & ~seen) != 0)
			return NTFSMAC_CTL_PARSE_BAD_FIELD; /* a required field is missing */
	}

	out->cmd = (ntfsmac_ctl_cmd)cmd_id;
	if (seen & BIT(F_BSD)) {
		if (!ntfsmac_ctl_bsd_valid(str[F_BSD]))
			goto bad;
		memcpy(out->bsd, str[F_BSD], strlen(str[F_BSD]) + 1);
	}
	if (seen & BIT(F_MODE)) {
		if (strcmp(str[F_MODE], "ro") == 0)
			out->read_only = true;
		else if (strcmp(str[F_MODE], "auto") != 0)
			goto bad;
	}
	if (seen & BIT(F_FORCE))
		out->force = boolean[F_FORCE];
	if (seen & BIT(F_ACTION)) {
		if (strcmp(str[F_ACTION], "clear_dirty") == 0)
			out->action = NTFSMAC_CTL_REPAIR_CLEAR_DIRTY;
		else if (strcmp(str[F_ACTION], "sync_mirror") == 0)
			out->action = NTFSMAC_CTL_REPAIR_SYNC_MIRROR;
		else
			goto bad;
	}
	if (seen & BIT(F_CONFIRM_LABEL)) {
		if (str[F_CONFIRM_LABEL][0] == '\0')
			goto bad;
		memcpy(out->confirm_label, str[F_CONFIRM_LABEL], strlen(str[F_CONFIRM_LABEL]) + 1);
	}
	if (seen & BIT(F_CONFIRM_SERIAL)) {
		if (!normalise_serial(str[F_CONFIRM_SERIAL]))
			goto bad;
		memcpy(out->confirm_serial, str[F_CONFIRM_SERIAL], NTFSMAC_CTL_SERIAL_DIGITS + 1);
	}
	if (seen & BIT(F_AUTH)) {
		if (!decode_form(str[F_AUTH], out->auth))
			goto bad;
		out->has_auth = true;
	}
	return NTFSMAC_CTL_PARSE_OK;

bad:
	memset(out, 0, sizeof *out);
	return NTFSMAC_CTL_PARSE_BAD_FIELD;
}
