/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Path display sanitiser (ADR-023 decision 4): the one place where disk-derived
 * paths of `nonnfc_paths` become text for the `status` reply. Allow-list: a byte of
 * the input is copied only if it belongs to a code point that the label rules of
 * ADR-016 allow; anything else is replaced by an escape this code writes itself.
 */
#include "sanitize.h"

typedef struct sink {
	char *out;
	size_t cap;     /* bytes of out, including the NUL */
	size_t need;    /* length the whole text needs, without the NUL */
	size_t written; /* length in out so far */
	bool full;      /* an element did not fit: nothing more is written */
} sink;

static void emit(sink *s, const char *p, size_t n)
{
	size_t i;

	s->need += n;
	if (s->full || s->written + n + 1 > s->cap) {
		s->full = true;
		return;
	}
	for (i = 0; i < n; i++)
		s->out[s->written + i] = p[i];
	s->written += n;
	s->out[s->written] = '\0';
}

/* `\u{XXXX}` for a code point (at least 4 upper-case hex digits), or `\x{XX}` with digits == 2. */
static void emit_escape(sink *s, char kind, uint32_t v, unsigned digits)
{
	static const char hex[] = "0123456789ABCDEF";
	char buf[10];
	size_t n = 0;
	unsigned d = digits;

	while (d < 6 && (v >> (4 * d)) != 0)
		d++;
	buf[n++] = '\\';
	buf[n++] = kind;
	buf[n++] = '{';
	while (d > 0) {
		d--;
		buf[n++] = hex[(v >> (4 * d)) & 0xF];
	}
	buf[n++] = '}';
	emit(s, buf, n);
}

size_t ntfsmac_path_display(const char *path, size_t len, char *out, size_t cap)
{
	const unsigned char *in = (const unsigned char *)path;
	sink s = { out, cap, 0, 0, false };
	size_t i = 0;

	if (cap > 0 && out != NULL)
		out[0] = '\0';
	else
		s.full = true; /* count only */
	if (path == NULL && len != 0)
		return 0;
	while (i < len) {
		uint32_t cp = 0;
		size_t clen = ntfsmac_utf8_decode(in + i, len - i, &cp);

		if (clen == 0) {
			emit_escape(&s, 'x', in[i], 2);
			i++;
			continue;
		}
		if (cp == '/' || ntfsmac_label_cp_allowed(cp))
			emit(&s, path + i, clen);
		else
			emit_escape(&s, 'u', cp, 4);
		i += clen;
	}
	return s.need;
}
