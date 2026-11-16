/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Output of the CLI (T-M4-04). Everything printed goes through
 * ntfsmac_cli_print_safe(), so no byte of a reply can act on the terminal.
 */
#include <string.h>

#include "cli.h"
#include "json.h"

/* ------------------------------------------------------------------ */
/* Safe printing                                                       */
/* ------------------------------------------------------------------ */

/* Length of a valid UTF-8 sequence at s (NUL terminated string), or 0. */
static size_t utf8_len(const unsigned char *s)
{
	unsigned char b = s[0];

	if (b >= 0xC2 && b <= 0xDF)
		return (s[1] & 0xC0) == 0x80 ? 2 : 0;
	if (b >= 0xE0 && b <= 0xEF) {
		if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80)
			return 0;
		if ((b == 0xE0 && s[1] < 0xA0) || (b == 0xED && s[1] >= 0xA0))
			return 0;
		return 3;
	}
	if (b >= 0xF0 && b <= 0xF4) {
		if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80)
			return 0;
		if ((b == 0xF0 && s[1] < 0x90) || (b == 0xF4 && s[1] >= 0x90))
			return 0;
		return 4;
	}
	return 0;
}

/*
 * Code points that can change how a terminal shows text, so they are escaped:
 *   U+0080..U+009F        C1 controls
 *   U+061C                Arabic letter mark
 *   U+200B..U+200F        zero-width space and joiners, left and right marks
 *   U+2028, U+2029        line and paragraph separators
 *   U+202A..U+202E        bidirectional embeddings and overrides
 *   U+2066..U+2069        bidirectional isolates
 *   U+FEFF                zero-width no-break space (byte order mark)
 */
static bool risky_sequence(const unsigned char *s, size_t n)
{
	if (n == 2 && s[0] == 0xC2 && s[1] >= 0x80 && s[1] <= 0x9F)
		return true;
	if (n == 2 && s[0] == 0xD8 && s[1] == 0x9C)
		return true;
	if (n == 3 && s[0] == 0xE2) {
		/* U+200B..U+200F, U+2028, U+2029, U+202A..U+202E */
		if (s[1] == 0x80 && ((s[2] >= 0x8B && s[2] <= 0x8F) || (s[2] >= 0xA8 && s[2] <= 0xAE)))
			return true;
		/* U+2066..U+2069 */
		if (s[1] == 0x81 && s[2] >= 0xA6 && s[2] <= 0xA9)
			return true;
	}
	if (n == 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF)
		return true;
	return false;
}

static size_t emit(FILE *f, const char *str)
{
	const unsigned char *s = (const unsigned char *)str;
	size_t cols = 0;

	while (*s != '\0') {
		size_t n = *s < 0x80 ? 1 : utf8_len(s);
		size_t i;

		if (n == 1 && *s >= 0x20 && *s != 0x7F) {
			if (f != NULL)
				fputc(*s, f);
			cols++;
			s++;
		} else if (n >= 2 && !risky_sequence(s, n)) {
			for (i = 0; i < n && f != NULL; i++)
				fputc(s[i], f);
			cols++;
			s += n;
		} else {
			/* control byte, DEL, invalid UTF-8, C1, bidi or invisible control: show each byte */
			size_t k = n == 0 ? 1 : n;

			for (i = 0; i < k; i++) {
				if (f != NULL)
					fprintf(f, "\\x%02X", s[i]);
				cols += 4;
			}
			s += k;
		}
	}
	return cols;
}

size_t ntfsmac_cli_print_safe(FILE *f, const char *str)
{
	return emit(f, str);
}

/* Columns of `str` as ntfsmac_cli_print_safe() would write them. */
static size_t safe_width(const char *str)
{
	return emit(NULL, str);
}

static void pad(FILE *f, size_t from, size_t to)
{
	for (; from < to; from++)
		fputc(' ', f);
}

/* ------------------------------------------------------------------ */
/* Generic dump                                                        */
/* ------------------------------------------------------------------ */

static bool is_scalar(const json_node *n)
{
	return n->type != JSON_ARRAY && n->type != JSON_OBJECT;
}

static void print_scalar(FILE *f, const json_node *n)
{
	switch (n->type) {
	case JSON_BOOL:
		fputs(n->boolean ? "yes" : "no", f);
		break;
	case JSON_NULL:
		fputs("-", f);
		break;
	default:
		ntfsmac_cli_print_safe(f, n->text != NULL ? n->text : "");
		break;
	}
}

/* A value of a "reason" or "reason_codes" member: code and text if we know the code. */
static void print_reason(FILE *f, const json_node *n, const char *indent)
{
	const char *text = n->type == JSON_STRING ? ntfsmac_cli_reason_text(n->text) : NULL;

	fputs(indent, f);
	print_scalar(f, n);
	if (text != NULL)
		fprintf(f, ": %s", text);
	fputc('\n', f);
}

static void dump(FILE *f, const json_node *n, unsigned depth)
{
	char indent[40], indent2[44];
	const json_node *c;
	unsigned k;

	for (k = 0; k < depth * 2 && k < sizeof indent - 1; k++)
		indent[k] = ' ';
	indent[k] = '\0';
	snprintf(indent2, sizeof indent2, "%s  ", indent);

	for (c = n->first; c != NULL; c = c->next) {
		bool reason_key = c->key != NULL && strcmp(c->key, "reason") == 0;
		bool reasons_key = c->key != NULL && strcmp(c->key, "reason_codes") == 0;

		if (n->type == JSON_OBJECT && c->key != NULL && strcmp(c->key, "ok") == 0 &&
		    depth == 0)
			continue; /* the exit status says it */
		if (reasons_key && c->type == JSON_ARRAY) {
			const json_node *r;

			if (c->first == NULL) {
				fprintf(f, "%sreason_codes: none\n", indent);
				continue;
			}
			fprintf(f, "%sreason_codes:\n", indent);
			for (r = c->first; r != NULL; r = r->next)
				print_reason(f, r, indent2);
			continue;
		}
		if (n->type == JSON_OBJECT) {
			fputs(indent, f);
			ntfsmac_cli_print_safe(f, c->key != NULL ? c->key : "");
			fputc(':', f);
		} else {
			fprintf(f, "%s-", indent);
		}
		if (is_scalar(c)) {
			fputc(' ', f);
			if (reason_key && c->type == JSON_STRING &&
			    ntfsmac_cli_reason_text(c->text) != NULL) {
				print_scalar(f, c);
				fprintf(f, ": %s", ntfsmac_cli_reason_text(c->text));
			} else {
				print_scalar(f, c);
			}
			fputc('\n', f);
		} else if (c->first == NULL) {
			fputs(c->type == JSON_ARRAY ? " none\n" : " {}\n", f);
		} else if (c->type == JSON_ARRAY && is_scalar(c->first)) {
			const json_node *e;

			fputc(' ', f);
			for (e = c->first; e != NULL; e = e->next) {
				if (e != c->first)
					fputs(", ", f);
				print_scalar(f, e);
			}
			fputc('\n', f);
		} else {
			fputc('\n', f);
			dump(f, c, depth + 1);
		}
	}
}

/* ------------------------------------------------------------------ */
/* list: a table                                                       */
/* ------------------------------------------------------------------ */

static const char *const list_cols[] = {"bsd", "label", "state", "mount", "mountpoint", "reason"};
#define LIST_NCOLS (sizeof list_cols / sizeof list_cols[0])
#define LIST_COL_MAX 40u

static const char *cell(const json_node *vol, const char *key)
{
	const json_node *m = json_member(vol, key);

	if (m == NULL || m->type != JSON_STRING)
		return "-";
	return m->text[0] != '\0' ? m->text : "-";
}

static void render_list(FILE *f, const json_node *root)
{
	const json_node *vols = json_member(root, "volumes");
	const json_node *v;
	size_t width[LIST_NCOLS];
	size_t i;

	if (vols == NULL || vols->type != JSON_ARRAY || vols->first == NULL) {
		fputs("No NTFS volumes.\n", f);
		return;
	}
	for (i = 0; i < LIST_NCOLS; i++)
		width[i] = strlen(list_cols[i]);
	for (v = vols->first; v != NULL; v = v->next) {
		if (v->type != JSON_OBJECT)
			continue;
		for (i = 0; i < LIST_NCOLS; i++) {
			size_t w = safe_width(cell(v, list_cols[i]));

			if (w > width[i])
				width[i] = w > LIST_COL_MAX ? LIST_COL_MAX : w;
		}
	}
	for (i = 0; i < LIST_NCOLS; i++) {
		const char *h = list_cols[i];
		size_t k;

		for (k = 0; h[k] != '\0'; k++)
			fputc(h[k] >= 'a' && h[k] <= 'z' ? h[k] - 'a' + 'A' : h[k], f);
		if (i + 1 < LIST_NCOLS)
			pad(f, k, width[i] + 2);
	}
	fputc('\n', f);
	for (v = vols->first; v != NULL; v = v->next) {
		if (v->type != JSON_OBJECT)
			continue;
		for (i = 0; i < LIST_NCOLS; i++) {
			size_t w = ntfsmac_cli_print_safe(f, cell(v, list_cols[i]));

			if (i + 1 < LIST_NCOLS)
				pad(f, w, width[i] + 2);
		}
		fputc('\n', f);
	}
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int ntfsmac_cli_render(const ntfsmac_cli_ctx *ctx, ntfsmac_ctl_cmd cmd, const char *reply)
{
	json_doc *doc = json_parse(reply, strlen(reply));
	const json_node *root = json_root(doc);
	const json_node *ok = json_member(root, "ok");
	int rc;

	if (root == NULL || root->type != JSON_OBJECT || ok == NULL || ok->type != JSON_BOOL) {
		fputs("ntfsmac: the daemon sent a reply that is not valid.\n", ctx->err);
		json_free(doc);
		return NTFSMAC_CLI_EXIT_UNAVAILABLE;
	}

	if (!ok->boolean) {
		const char *err = json_member_str(root, "error");
		const char *reason = json_member_str(root, "reason");
		const char *text = ntfsmac_cli_error_text(err);
		const char *rtext;

		if (err != NULL && strcmp(err, "denied") == 0 && reason != NULL &&
		    ntfsmac_cli_refusal_text(reason) != NULL)
			text = ntfsmac_cli_refusal_text(reason);
		fputs("ntfsmac: ", ctx->err);
		if (text != NULL) {
			fputs(text, ctx->err);
		} else {
			fputs("the request failed (", ctx->err);
			ntfsmac_cli_print_safe(ctx->err, err != NULL ? err : "no error code");
			fputc(')', ctx->err);
		}
		fputc('\n', ctx->err);
		/* a reason from an operation (a probe reason code) gets its text too */
		rtext = reason != NULL ? ntfsmac_cli_reason_text(reason) : NULL;
		if (rtext != NULL)
			fprintf(ctx->err, "  %s: %s\n", reason, rtext);
		rc = NTFSMAC_CLI_EXIT_FAILED;
		if (err != NULL && strcmp(err, "not_implemented") == 0)
			rc = NTFSMAC_CLI_EXIT_UNAVAILABLE;
	} else {
		if (cmd == NTFSMAC_CTL_CMD_LIST)
			render_list(ctx->out, root);
		else
			dump(ctx->out, root, 0);
		rc = NTFSMAC_CLI_EXIT_OK;
	}
	json_free(doc);
	return rc;
}
