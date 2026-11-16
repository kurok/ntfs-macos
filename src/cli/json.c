/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdlib.h>
#include <string.h>

#include "json.h"

struct json_doc {
	json_node nodes[JSON_NODES_MAX];
	unsigned n;
	char *arena; /* decoded strings, NUL separated */
	size_t used, cap;
};

typedef struct parser {
	const unsigned char *p, *end;
	json_doc *d;
} parser;

static void skip_ws(parser *ps)
{
	while (ps->p < ps->end &&
	       (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\r' || *ps->p == '\n'))
		ps->p++;
}

static json_node *new_node(parser *ps, json_type t)
{
	json_node *n;

	if (ps->d->n >= JSON_NODES_MAX)
		return NULL;
	n = &ps->d->nodes[ps->d->n++];
	memset(n, 0, sizeof *n);
	n->type = t;
	return n;
}

static int hexv(unsigned char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static bool hex4(parser *ps, unsigned *out)
{
	unsigned v = 0;
	int i;

	if (ps->end - ps->p < 4)
		return false;
	for (i = 0; i < 4; i++) {
		int h = hexv(ps->p[i]);

		if (h < 0)
			return false;
		v = v * 16 + (unsigned)h;
	}
	ps->p += 4;
	*out = v;
	return true;
}

/* Decode a string at the opening quote into the arena. */
static const char *read_string(parser *ps)
{
	json_doc *d = ps->d;
	char *start = d->arena + d->used;
	size_t n = 0;

	if (ps->p >= ps->end || *ps->p != '"')
		return NULL;
	ps->p++;
	for (;;) {
		unsigned char c;

		if (ps->p >= ps->end)
			return NULL;
		c = *ps->p++;
		if (c == '"')
			break;
		if (c < 0x20)
			return NULL;
		/* the decoded text is never longer than the source, so the arena (sized
		 * from the input) always has room for it and its NUL */
		if (c != '\\') {
			start[n++] = (char)c;
			continue;
		}
		if (ps->p >= ps->end)
			return NULL;
		c = *ps->p++;
		switch (c) {
		case '"': case '\\': case '/': start[n++] = (char)c; break;
		case 'b': start[n++] = '\b'; break;
		case 'f': start[n++] = '\f'; break;
		case 'n': start[n++] = '\n'; break;
		case 'r': start[n++] = '\r'; break;
		case 't': start[n++] = '\t'; break;
		case 'u': {
			unsigned cp;

			if (!hex4(ps, &cp))
				return NULL;
			if (cp >= 0xD800 && cp <= 0xDBFF) {
				unsigned lo;

				if (ps->end - ps->p < 2 || ps->p[0] != '\\' || ps->p[1] != 'u')
					return NULL;
				ps->p += 2;
				if (!hex4(ps, &lo) || lo < 0xDC00 || lo > 0xDFFF)
					return NULL;
				cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
			} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
				return NULL;
			}
			if (cp == 0)
				return NULL;
			if (cp < 0x80) {
				start[n++] = (char)cp;
			} else if (cp < 0x800) {
				start[n++] = (char)(0xC0 | (cp >> 6));
				start[n++] = (char)(0x80 | (cp & 0x3F));
			} else if (cp < 0x10000) {
				start[n++] = (char)(0xE0 | (cp >> 12));
				start[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
				start[n++] = (char)(0x80 | (cp & 0x3F));
			} else {
				start[n++] = (char)(0xF0 | (cp >> 18));
				start[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
				start[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
				start[n++] = (char)(0x80 | (cp & 0x3F));
			}
			break;
		}
		default:
			return NULL;
		}
	}
	start[n++] = '\0';
	d->used += n;
	return start;
}

static json_node *parse_value(parser *ps, unsigned depth);

static bool word(parser *ps, const char *w)
{
	size_t n = strlen(w);

	if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, w, n) != 0)
		return false;
	ps->p += n;
	return true;
}

static json_node *parse_container(parser *ps, unsigned depth, bool is_obj)
{
	json_node *node = new_node(ps, is_obj ? JSON_OBJECT : JSON_ARRAY);
	json_node **link;
	unsigned char close = is_obj ? '}' : ']';

	if (node == NULL || depth >= JSON_DEPTH_MAX)
		return NULL;
	ps->p++; /* opening bracket */
	link = &node->first;
	skip_ws(ps);
	if (ps->p < ps->end && *ps->p == close) {
		ps->p++;
		return node;
	}
	for (;;) {
		const char *key = NULL;
		json_node *child;

		skip_ws(ps);
		if (is_obj) {
			key = read_string(ps);
			if (key == NULL)
				return NULL;
			skip_ws(ps);
			if (ps->p >= ps->end || *ps->p != ':')
				return NULL;
			ps->p++;
			skip_ws(ps);
		}
		child = parse_value(ps, depth + 1);
		if (child == NULL)
			return NULL;
		child->key = key;
		*link = child;
		link = &child->next;
		skip_ws(ps);
		if (ps->p >= ps->end)
			return NULL;
		if (*ps->p == ',') {
			ps->p++;
			continue;
		}
		if (*ps->p == close) {
			ps->p++;
			return node;
		}
		return NULL;
	}
}

static json_node *parse_value(parser *ps, unsigned depth)
{
	json_node *n;
	const unsigned char *start;

	if (ps->p >= ps->end)
		return NULL;
	switch (*ps->p) {
	case '{':
		return parse_container(ps, depth, true);
	case '[':
		return parse_container(ps, depth, false);
	case '"':
		n = new_node(ps, JSON_STRING);
		if (n == NULL)
			return NULL;
		n->text = read_string(ps);
		return n->text != NULL ? n : NULL;
	case 't':
	case 'f':
		n = new_node(ps, JSON_BOOL);
		if (n == NULL)
			return NULL;
		if (word(ps, "true"))
			n->boolean = true;
		else if (!word(ps, "false"))
			return NULL;
		return n;
	case 'n':
		n = new_node(ps, JSON_NULL);
		return n != NULL && word(ps, "null") ? n : NULL;
	default:
		break;
	}
	/* number: kept as text, checked only for the characters JSON allows */
	start = ps->p;
	while (ps->p < ps->end && strchr("+-.0123456789eE", *ps->p) != NULL && *ps->p != '\0')
		ps->p++;
	if (ps->p == start || (size_t)(ps->p - start) > 40)
		return NULL;
	n = new_node(ps, JSON_NUMBER);
	if (n == NULL)
		return NULL;
	memcpy(ps->d->arena + ps->d->used, start, (size_t)(ps->p - start));
	n->text = ps->d->arena + ps->d->used;
	ps->d->used += (size_t)(ps->p - start);
	ps->d->arena[ps->d->used++] = '\0';
	return n;
}

json_doc *json_parse(const char *text, size_t len)
{
	json_doc *d;
	parser ps;
	json_node *root;

	if (text == NULL)
		return NULL;
	d = malloc(sizeof *d);
	if (d == NULL)
		return NULL;
	d->n = 0;
	d->used = 0;
	/* every decoded string and number is no longer than its source, plus a NUL */
	d->cap = len + 2 * JSON_NODES_MAX;
	d->arena = malloc(d->cap);
	if (d->arena == NULL) {
		free(d);
		return NULL;
	}
	ps.p = (const unsigned char *)text;
	ps.end = ps.p + len;
	ps.d = d;
	skip_ws(&ps);
	root = parse_value(&ps, 0);
	skip_ws(&ps);
	if (root == NULL || ps.p != ps.end || root != &d->nodes[0]) {
		json_free(d);
		return NULL;
	}
	return d;
}

const json_node *json_root(const json_doc *d)
{
	return d != NULL && d->n > 0 ? &d->nodes[0] : NULL;
}

void json_free(json_doc *d)
{
	if (d == NULL)
		return;
	free(d->arena);
	free(d);
}

const json_node *json_member(const json_node *obj, const char *key)
{
	const json_node *c;

	if (obj == NULL || obj->type != JSON_OBJECT)
		return NULL;
	for (c = obj->first; c != NULL; c = c->next)
		if (c->key != NULL && strcmp(c->key, key) == 0)
			return c;
	return NULL;
}

const char *json_member_str(const json_node *obj, const char *key)
{
	const json_node *m = json_member(obj, key);

	return m != NULL && m->type == JSON_STRING ? m->text : NULL;
}
