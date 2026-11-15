/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-VERDICT-01..05 (docs/TEST_STRATEGY.md section 4) on the strict probe-verdict parser,
 * plus the cross-field validator, the shared severity table and the name tables.
 * test_verdict_schema.py runs the same parser against docs/schemas/probe.schema.json.
 */
#include "ntfsmac/journal.h"
#include "ntfsmac/probe.h"
#include "sanitize.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "oracle.h"

/* The probe's own writer (src/probe/json.c): the producer for the round trip of U-VERDICT-04. */
size_t ntfsmac_probe_json(const ntfsmac_probe_verdict *v, char *buf, size_t cap);

/* sprintf without the deprecation: every buffer here is a document buffer (docbuf) or a small array. */
static int spf(char *dst, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static int spf(char *dst, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(dst, NTFSMAC_PROBE_MAX_JSON_BYTES + 4096, fmt, ap);
	va_end(ap);
	return n;
}

typedef struct kv {
	const char *key;
	const char *val;
} kv;

/* Example of ARCHITECTURE.md section 3.1, one member per row. */
static const kv base_doc[] = {
	{ "schema", "1" },
	{ "is_ntfs", "true" },
	{ "ntfs_version", "\"3.1\"" },
	{ "label", "\"DATA\"" },
	{ "serial", "\"0x1A2B3C4D5E6F7788\"" },
	{ "total_sectors", "131071" },
	{ "cluster_size", "4096" },
	{ "sector_size", "512" },
	{ "volume_flags", "[\"DIRTY\"]" },
	{ "logfile", "\"clean\"" },
	{ "logfile_lsn", "\"0x0000000001A2B3C4\"" },
	{ "hiberfile", "\"absent\"" },
	{ "bitlocker", "false" },
	{ "state", "\"dirty_flag\"" },
	{ "reason_codes", "[\"FLAG_DIRTY\"]" },
	{ "nonnfc_count", "0" },
	{ "nonnfc_paths", "[]" },
};
#define NKEYS (sizeof(base_doc) / sizeof(base_doc[0]))

/* Result buffers are heap allocated: a document can be 64 KiB. */
static char *docbuf(void)
{
	char *b = malloc(NTFSMAC_PROBE_MAX_JSON_BYTES + 4096);

	if (b == NULL)
		abort();
	return b;
}

/*
 * Build the example with `key` set to `val` (NULL: the key left out). A key that is not in the
 * example is appended as an extra member (also with the key text as given, so a duplicate can be
 * made by passing a name twice through `extra`).
 */
static size_t doc2(char *out, const char *key, const char *val, const char *extra)
{
	size_t i, n = 0;
	bool found = false;

	out[n++] = '{';
	for (i = 0; i < NKEYS; i++) {
		const char *v = base_doc[i].val;

		if (key != NULL && strcmp(key, base_doc[i].key) == 0) {
			found = true;
			if (val == NULL)
				continue;
			v = val;
		}
		n += (size_t)spf(out + n, "%s\"%s\":%s", n > 1 ? "," : "", base_doc[i].key, v);
	}
	if (key != NULL && !found && val != NULL)
		n += (size_t)spf(out + n, ",\"%s\":%s", key, val);
	if (extra != NULL)
		n += (size_t)spf(out + n, ",%s", extra);
	out[n++] = '}';
	out[n] = '\0';
	return n;
}

/* The example with several members replaced; `ov` ends with a NULL key. */
static size_t doc_ov(char *out, const kv *ov)
{
	size_t i, n = 0;

	out[n++] = '{';
	for (i = 0; i < NKEYS; i++) {
		const kv *o;
		const char *v = base_doc[i].val;

		for (o = ov; o->key != NULL; o++)
			if (strcmp(o->key, base_doc[i].key) == 0)
				v = o->val;
		n += (size_t)spf(out + n, "%s\"%s\":%s", i ? "," : "", base_doc[i].key, v);
	}
	out[n++] = '}';
	out[n] = '\0';
	return n;
}

static ntfsmac_probe_status parse_doc(const char *key, const char *val, ntfsmac_probe_verdict *v)
{
	char *d = docbuf();
	size_t n = doc2(d, key, val, NULL);
	ntfsmac_probe_status st = ntfsmac_probe_parse(d, n, v);

	free(d);
	return st;
}

static bool is_unknown(const ntfsmac_probe_verdict *v)
{
	ntfsmac_probe_verdict u;

	ntfsmac_probe_verdict_unknown(&u);
	return v->state == NTFSMAC_PROBE_STATE_UNKNOWN &&
	       v->reason_codes == NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_CHECK_FAILED) &&
	       !v->is_ntfs && !v->bitlocker && !v->has_ntfs_version && v->label_len == 0 &&
	       v->label[0] == '\0' && v->serial == 0 && v->total_sectors == 0 &&
	       v->cluster_size == 0 && v->sector_size == 0 && v->volume_flags == 0 &&
	       v->logfile == NTFSMAC_PROBE_LOGFILE_UNREADABLE && !v->has_logfile_lsn &&
	       v->hiberfile == NTFSMAC_PROBE_HIBERFILE_UNREADABLE && v->nonnfc_count == 0 &&
	       v->nonnfc_path_count == 0 && memcmp(&u, v, sizeof(u)) == 0;
}

/* The parse must fail with `want` and leave the fail-closed verdict. */
static void rejects_raw(const char *json, size_t len, ntfsmac_probe_status want, const char *what)
{
	ntfsmac_probe_verdict v;
	ntfsmac_probe_status st;

	memset(&v, 0x5A, sizeof(v));
	st = ntfsmac_probe_parse(json, len, &v);
	if (st != want) {
		fprintf(stderr, "FAIL: %s: status %d, want %d\n", what, (int)st, (int)want);
		g_failures++;
	}
	if (st != NTFSMAC_PROBE_OK && !is_unknown(&v)) {
		fprintf(stderr, "FAIL: %s: output is not the fail-closed verdict\n", what);
		g_failures++;
	}
}

static void rejects(const char *key, const char *val, ntfsmac_probe_status want)
{
	ntfsmac_probe_verdict v;
	ntfsmac_probe_status st;

	memset(&v, 0x5A, sizeof(v));
	st = parse_doc(key, val, &v);
	if (st != want) {
		fprintf(stderr, "FAIL: %s=%s: status %d, want %d\n", key, val != NULL ? val : "(absent)",
			(int)st, (int)want);
		g_failures++;
	}
	if (st != NTFSMAC_PROBE_OK && !is_unknown(&v)) {
		fprintf(stderr, "FAIL: %s=%s: output is not the fail-closed verdict\n", key,
			val != NULL ? val : "(absent)");
		g_failures++;
	}
}

static void accepts(const char *key, const char *val, ntfsmac_probe_verdict *v)
{
	ntfsmac_probe_status st = parse_doc(key, val, v);

	if (st != NTFSMAC_PROBE_OK) {
		fprintf(stderr, "FAIL: %s=%s: status %d, want ok\n", key, val != NULL ? val : "(absent)",
			(int)st);
		g_failures++;
	}
}

/* ------------------------------------------------------------------ U-VERDICT-01 */

static void u_verdict_01(void)
{
	ntfsmac_probe_verdict v;
	char *d = docbuf();
	size_t n = doc2(d, NULL, NULL, NULL), i;

	CHECK(ntfsmac_probe_parse(d, n, &v) == NTFSMAC_PROBE_OK);
	/* with surrounding whitespace, not NUL terminated */
	{
		char *w = docbuf();
		size_t m = (size_t)spf(w, " \t\r\n%s\n\n", d);

		CHECK(ntfsmac_probe_parse(w, m, &v) == NTFSMAC_PROBE_OK);
		free(w);
	}
	/* over 64 KiB: padded with whitespace, so every other rule holds */
	{
		char *big = docbuf();

		memcpy(big, d, n);
		memset(big + n, ' ', NTFSMAC_PROBE_MAX_JSON_BYTES - n);
		CHECK(ntfsmac_probe_parse(big, NTFSMAC_PROBE_MAX_JSON_BYTES, &v) == NTFSMAC_PROBE_OK);
		rejects_raw(big, NTFSMAC_PROBE_MAX_JSON_BYTES + 1, NTFSMAC_PROBE_ERR_TOO_LARGE, "64 KiB + 1");
		rejects_raw(big, NTFSMAC_PROBE_MAX_JSON_BYTES + 4096, NTFSMAC_PROBE_ERR_TOO_LARGE, "64 KiB + 4096");
		free(big);
	}
	/* every proper prefix is truncated JSON */
	for (i = 0; i < n; i++) {
		ntfsmac_probe_status st = ntfsmac_probe_parse(d, i, &v);

		if (st == NTFSMAC_PROBE_OK || !is_unknown(&v)) {
			fprintf(stderr, "FAIL: prefix of %zu bytes accepted or not fail-closed\n", i);
			g_failures++;
		}
	}
	/* trailing garbage */
	{
		char *t = docbuf();
		static const char *const tail[] = { "x", "{}", "{", "}", ",", "0", "\"", "null", " x", "\n,", "\xEF\xBB\xBF" };
		size_t k;

		for (k = 0; k < sizeof(tail) / sizeof(tail[0]); k++) {
			size_t m = (size_t)spf(t, "%s%s", d, tail[k]);

			rejects_raw(t, m, NTFSMAC_PROBE_ERR_SYNTAX, tail[k]);
		}
		/* a second object, a leading BOM, and no object at all */
		{
			size_t m = (size_t)spf(t, "%s\n%s", d, d);

			rejects_raw(t, m, NTFSMAC_PROBE_ERR_SYNTAX, "two objects");
			m = (size_t)spf(t, "\xEF\xBB\xBF%s", d);
			rejects_raw(t, m, NTFSMAC_PROBE_ERR_SYNTAX, "BOM");
		}
		rejects_raw("", 0, NTFSMAC_PROBE_ERR_SYNTAX, "empty");
		rejects_raw("   \n", 4, NTFSMAC_PROBE_ERR_SYNTAX, "blank");
		rejects_raw("[]", 2, NTFSMAC_PROBE_ERR_SYNTAX, "array");
		rejects_raw("null", 4, NTFSMAC_PROBE_ERR_SYNTAX, "null");
		rejects_raw("\"x\"", 3, NTFSMAC_PROBE_ERR_SYNTAX, "string");
		rejects_raw(NULL, 0, NTFSMAC_PROBE_ERR_SYNTAX, "NULL input");
		rejects_raw("{}", 2, NTFSMAC_PROBE_ERR_MISSING_KEY, "empty object");
		rejects_raw("{ }", 3, NTFSMAC_PROBE_ERR_MISSING_KEY, "empty object with space");
		free(t);
	}
	CHECK(ntfsmac_probe_parse(d, n, NULL) == NTFSMAC_PROBE_ERR_SYNTAX);
	free(d);
}

/* ------------------------------------------------------------------ U-VERDICT-02 */

/* JSON type of a key of the grammar: boolean, string, string or null, integer, array. */
static char key_type(const char *key)
{
	if (strcmp(key, "is_ntfs") == 0 || strcmp(key, "bitlocker") == 0)
		return 'b';
	if (strcmp(key, "ntfs_version") == 0 || strcmp(key, "logfile_lsn") == 0)
		return 'n';
	if (strcmp(key, "label") == 0 || strcmp(key, "serial") == 0 || strcmp(key, "logfile") == 0 ||
	    strcmp(key, "hiberfile") == 0 || strcmp(key, "state") == 0)
		return 's';
	if (strcmp(key, "volume_flags") == 0 || strcmp(key, "reason_codes") == 0 ||
	    strcmp(key, "nonnfc_paths") == 0)
		return 'a';
	return 'i';
}

static char value_type(const char *v)
{
	if (v[0] == '"')
		return 's';
	if (v[0] == '[')
		return 'a';
	if (v[0] == '{')
		return 'o';
	if (strcmp(v, "true") == 0 || strcmp(v, "false") == 0)
		return 'b';
	if (strcmp(v, "null") == 0)
		return 'z';
	return 'i';
}

static void u_verdict_02(void)
{
	static const char *const wrong[] = { "\"x\"", "7", "true", "false", "null", "[]", "{}", "[\"a\"]", "{\"a\":1}" };
	size_t k, w;
	char *d = docbuf();
	char extra[256];

	/* duplicate keys: every key repeated with the same and with another value */
	for (k = 0; k < NKEYS; k++) {
		size_t n;

		snprintf(extra, sizeof(extra), "\"%s\":%s", base_doc[k].key, base_doc[k].val);
		n = doc2(d, NULL, NULL, extra);
		rejects_raw(d, n, NTFSMAC_PROBE_ERR_DUP_KEY, base_doc[k].key);
	}
	/* a duplicate spelled with an escape is still a duplicate */
	{
		size_t n = doc2(d, NULL, NULL, "\"sc\\u0068ema\":1");

		rejects_raw(d, n, NTFSMAC_PROBE_ERR_DUP_KEY, "escaped duplicate");
	}
	/* unknown keys, including the old free-text `reasons`, and near misses */
	{
		static const char *const unk[] = {
			"\"reasons\":[\"text\"]", "\"Schema\":1", "\"schema \":1", "\"\":1", "\"x\":null",
			"\"nonnfc_path\":[]", "\"label2\":\"x\"", "\"__proto__\":1",
			"\"averyveryveryveryveryverylongkeyname_0123456789\":1", "\"\xC3\xA9\":1",
		};

		for (k = 0; k < sizeof(unk) / sizeof(unk[0]); k++) {
			size_t n = doc2(d, NULL, NULL, unk[k]);

			rejects_raw(d, n, NTFSMAC_PROBE_ERR_UNKNOWN_KEY, unk[k]);
		}
		/* an unknown key whose value is hostile is rejected at the key */
		rejects_raw("{\"x\":[[[[[[[[[[1]]]]]]]]]]}", 27, NTFSMAC_PROBE_ERR_UNKNOWN_KEY, "unknown key, deep value");
	}
	/* missing keys */
	for (k = 0; k < NKEYS; k++)
		rejects(base_doc[k].key, NULL, NTFSMAC_PROBE_ERR_MISSING_KEY);
	/* enum values: unknown, wrong case, with text, with padding */
	rejects("state", "\"CLEAN\"", NTFSMAC_PROBE_ERR_ENUM);
	rejects("state", "\"clean | dirty_flag\"", NTFSMAC_PROBE_ERR_ENUM);
	rejects("state", "\"dirty_flag \"", NTFSMAC_PROBE_ERR_ENUM);
	rejects("state", "\"\"", NTFSMAC_PROBE_ERR_ENUM);
	rejects("state", "\"mirror-split\"", NTFSMAC_PROBE_ERR_ENUM);
	rejects("logfile", "\"Clean\"", NTFSMAC_PROBE_ERR_ENUM);
	rejects("logfile", "\"corrupt\"", NTFSMAC_PROBE_ERR_ENUM);
	rejects("hiberfile", "\"hibernating\"", NTFSMAC_PROBE_ERR_ENUM);
	rejects("volume_flags", "[\"DIRTY\",\"SOMETHING\"]", NTFSMAC_PROBE_ERR_ENUM);
	rejects("volume_flags", "[\"dirty\"]", NTFSMAC_PROBE_ERR_ENUM);
	rejects("reason_codes", "[\"FLAG_DIRTY\",\"TOTALLY_NEW\"]", NTFSMAC_PROBE_ERR_ENUM);
	rejects("reason_codes", "[\"flag_dirty\"]", NTFSMAC_PROBE_ERR_ENUM);
	rejects("reason_codes", "[\"FLAG_DIRTY\\n\"]", NTFSMAC_PROBE_ERR_ENUM);
	rejects("state", "\"averyveryveryveryveryveryveryveryverylongenumvalue\"", NTFSMAC_PROBE_ERR_ENUM);
	/* the new members of the grammar are accepted */
	{
		ntfsmac_probe_verdict v;

		accepts("state", "\"mirror_split\"", &v);
		CHECK(v.state == NTFSMAC_PROBE_STATE_MIRROR_SPLIT);
		accepts("state", "\"names_not_nfc\"", &v);
		CHECK(v.state == NTFSMAC_PROBE_STATE_NAMES_NOT_NFC);
	}
	/* a value of the wrong JSON type for every key is ERR_TYPE; the right type is checked elsewhere */
	for (k = 0; k < NKEYS; k++) {
		char kt = key_type(base_doc[k].key);

		for (w = 0; w < sizeof(wrong) / sizeof(wrong[0]); w++) {
			char vt = value_type(wrong[w]);
			bool match = (kt == 'b' && vt == 'b') || (kt == 's' && vt == 's') ||
				     (kt == 'n' && (vt == 's' || vt == 'z')) || (kt == 'i' && vt == 'i') ||
				     (kt == 'a' && vt == 'a');

			if (!match)
				rejects(base_doc[k].key, wrong[w], NTFSMAC_PROBE_ERR_TYPE);
		}
	}
	/* the plain type errors, asserted exactly */
	rejects("schema", "\"1\"", NTFSMAC_PROBE_ERR_TYPE);
	rejects("schema", "true", NTFSMAC_PROBE_ERR_TYPE);
	rejects("schema", "2", NTFSMAC_PROBE_ERR_RANGE);
	rejects("schema", "0", NTFSMAC_PROBE_ERR_RANGE);
	rejects("is_ntfs", "\"true\"", NTFSMAC_PROBE_ERR_TYPE);
	rejects("is_ntfs", "1", NTFSMAC_PROBE_ERR_TYPE);
	rejects("bitlocker", "0", NTFSMAC_PROBE_ERR_TYPE);
	rejects("label", "5", NTFSMAC_PROBE_ERR_TYPE);
	rejects("label", "null", NTFSMAC_PROBE_ERR_TYPE);
	rejects("serial", "123", NTFSMAC_PROBE_ERR_TYPE);
	rejects("total_sectors", "\"131071\"", NTFSMAC_PROBE_ERR_TYPE);
	rejects("volume_flags", "\"DIRTY\"", NTFSMAC_PROBE_ERR_TYPE);
	rejects("volume_flags", "[1]", NTFSMAC_PROBE_ERR_TYPE);
	rejects("reason_codes", "[null]", NTFSMAC_PROBE_ERR_TYPE);
	rejects("reason_codes", "{\"a\":1}", NTFSMAC_PROBE_ERR_TYPE);
	rejects("nonnfc_paths", "[5]", NTFSMAC_PROBE_ERR_TYPE);
	rejects("nonnfc_paths", "\"/a\"", NTFSMAC_PROBE_ERR_TYPE);
	rejects("state", "5", NTFSMAC_PROBE_ERR_TYPE);
	/* nesting beyond depth 2 */
	rejects("reason_codes", "[[\"FLAG_DIRTY\"]]", NTFSMAC_PROBE_ERR_DEPTH);
	rejects("reason_codes", "[{}]", NTFSMAC_PROBE_ERR_DEPTH);
	rejects("nonnfc_paths", "[[]]", NTFSMAC_PROBE_ERR_DEPTH);
	rejects("volume_flags", "[[[[]]]]", NTFSMAC_PROBE_ERR_DEPTH);
	{
		/* 20000 nested brackets: rejected at the second one, no recursion */
		char *deep = docbuf();
		size_t i, m = 0;

		for (i = 0; i < 20000 && m < 60000; i++)
			deep[m++] = '[';
		deep[m] = '\0';
		{
			char *full = docbuf();
			size_t n = (size_t)spf(full, "{\"schema\":1,\"is_ntfs\":true,\"volume_flags\":%s", deep);

			rejects_raw(full, n, NTFSMAC_PROBE_ERR_DEPTH, "20000 nested arrays");
			free(full);
		}
		free(deep);
	}
	/* syntax: trailing commas, missing colons, single quotes, comments, bare words */
	{
		static const char *const syn[] = {
			"{\"schema\":1,}", "{\"schema\" 1}", "{'schema':1}", "{\"schema\":1 // c\n}",
			"{schema:1}", "{\"schema\":1,,\"is_ntfs\":true}", "{\"schema\":01}", "{,}",
			"{\"schema\":1 \"is_ntfs\":true}", "{\"schema\":tru}", "{\"schema\":nul}",
			"{\"schema\":True}", "{\"schema\":NaN}", "{\"schema\":Infinity}",
		};

		for (k = 0; k < sizeof(syn) / sizeof(syn[0]); k++) {
			ntfsmac_probe_verdict v;

			if (ntfsmac_probe_parse(syn[k], strlen(syn[k]), &v) == NTFSMAC_PROBE_OK || !is_unknown(&v)) {
				fprintf(stderr, "FAIL: syntax case %zu accepted\n", k);
				g_failures++;
			}
		}
		rejects("reason_codes", "[\"FLAG_DIRTY\",]", NTFSMAC_PROBE_ERR_SYNTAX);
		rejects("reason_codes", "[,]", NTFSMAC_PROBE_ERR_SYNTAX);
		rejects("reason_codes", "[\"FLAG_DIRTY\" \"LOG_V2\"]", NTFSMAC_PROBE_ERR_SYNTAX);
	}
	free(d);
}

/* ------------------------------------------------------------------ U-VERDICT-03 */

static void u_verdict_03(void)
{
	ntfsmac_probe_verdict v;
	char *d = docbuf();
	size_t n, k;

	/* \u0000 anywhere */
	rejects("label", "\"a\\u0000b\"", NTFSMAC_PROBE_ERR_NUL);
	rejects("label", "\"\\u0000\"", NTFSMAC_PROBE_ERR_NUL);
	rejects("label", "\"a\\u00000\"", NTFSMAC_PROBE_ERR_NUL); /* escape then a digit */
	rejects("nonnfc_paths", "[\"/a\\u0000\"]", NTFSMAC_PROBE_ERR_NUL);
	rejects("serial", "\"0x1A2B3C4D5E6F77\\u0000\\u0000\"", NTFSMAC_PROBE_ERR_NUL);
	rejects("state", "\"dirty_flag\\u0000\"", NTFSMAC_PROBE_ERR_NUL);
	/* a raw NUL byte, inside and outside a string */
	n = doc2(d, "label", "\"ab\"", NULL);
	{
		char *p = strstr(d, "\"ab\"");

		p[2] = '\0';
		rejects_raw(d, n, NTFSMAC_PROBE_ERR_NUL, "raw NUL in label");
	}
	n = doc2(d, NULL, NULL, NULL);
	d[n - 1] = '\0';
	rejects_raw(d, n, NTFSMAC_PROBE_ERR_NUL, "raw NUL at the end");
	d[n - 1] = '}';
	d[n] = '\0';
	rejects_raw(d, n + 1, NTFSMAC_PROBE_ERR_NUL, "raw NUL after the object");

	/* invalid UTF-8 in label, a path, a key and outside any string */
	{
		static const char *const bad[] = {
			"\xC0\xAF", "\x80", "\xBF", "\xED\xA0\x80", "\xF8\x88\x80\x80\x80", "\xE2\x82", "\xF4\x90\x80\x80",
			"\xFF", "\xC1\xBF", "\xE0\x80\x80", "\xF0\x8F\xBF\xBF",
		};

		for (k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
			char val[64];

			snprintf(val, sizeof(val), "\"a%sb\"", bad[k]);
			rejects("label", val, NTFSMAC_PROBE_ERR_UTF8);
			snprintf(val, sizeof(val), "[\"/a%sb\"]", bad[k]);
			rejects("nonnfc_paths", val, NTFSMAC_PROBE_ERR_UTF8);
			snprintf(val, sizeof(val), "\"clean%s\"", bad[k]);
			rejects("logfile", val, NTFSMAC_PROBE_ERR_UTF8);
			n = doc2(d, NULL, NULL, NULL);
			memmove(d + 1 + strlen(bad[k]), d + 1, n);
			memcpy(d + 1, bad[k], strlen(bad[k]));
			rejects_raw(d, n + strlen(bad[k]), NTFSMAC_PROBE_ERR_UTF8, "invalid UTF-8 before the first key");
		}
	}
	/* lone and swapped surrogate escapes have no UTF-8 form */
	rejects("label", "\"\\ud800\"", NTFSMAC_PROBE_ERR_UTF8);
	rejects("label", "\"\\udc00\"", NTFSMAC_PROBE_ERR_UTF8);
	rejects("label", "\"\\ud800x\"", NTFSMAC_PROBE_ERR_UTF8);
	rejects("label", "\"\\ud800\\u0041\"", NTFSMAC_PROBE_ERR_UTF8);
	rejects("label", "\"\\udc00\\ud800\"", NTFSMAC_PROBE_ERR_UTF8);
	rejects("label", "\"\\ud83d\\ud83d\"", NTFSMAC_PROBE_ERR_UTF8);
	rejects("label", "\"\\u00zz\"", NTFSMAC_PROBE_ERR_SYNTAX);
	rejects("label", "\"\\u00e\"", NTFSMAC_PROBE_ERR_SYNTAX);
	rejects("label", "\"\\x41\"", NTFSMAC_PROBE_ERR_SYNTAX);
	rejects("label", "\"\\\"", NTFSMAC_PROBE_ERR_SYNTAX);
	rejects("label", "\"a\nb\"", NTFSMAC_PROBE_ERR_SYNTAX); /* raw control character */
	rejects("label", "\"a\x01" "b\"", NTFSMAC_PROBE_ERR_SYNTAX);
	rejects("label", "\"abc", NTFSMAC_PROBE_ERR_SYNTAX);

	/* valid escapes decode to the UTF-8 text; control escapes other than \u0000 are allowed */
	accepts("label", "\"\\u00e9\\u00E9 \\ud83d\\ude00 \\/\\\\\\\"\\b\\f\\n\\r\\t\\u001b\\u007f\"", &v);
	CHECK(strcmp(v.label, "\xC3\xA9\xC3\xA9 \xF0\x9F\x98\x80 /\\\"\b\f\n\r\t\x1B\x7F") == 0);
	CHECK_EQ_INT(v.label_len, strlen(v.label));
	accepts("label", "\"\xF0\x9F\x98\x80\xEF\xBF\xBF\"", &v); /* raw, noncharacter included */
	CHECK(strcmp(v.label, "\xF0\x9F\x98\x80\xEF\xBF\xBF") == 0);
	/* the same value spelled by escape is the same key and the same enum */
	accepts("state", "\"dirty\\u005Fflag\"", &v);
	CHECK(v.state == NTFSMAC_PROBE_STATE_DIRTY_FLAG);
	{
		char *e = docbuf();
		size_t m = (size_t)spf(e, "{\"\\u0073chema\":1");
		size_t i;

		for (i = 1; i < NKEYS; i++)
			m += (size_t)spf(e + m, ",\"%s\":%s", base_doc[i].key, base_doc[i].val);
		e[m++] = '}';
		CHECK(ntfsmac_probe_parse(e, m, &v) == NTFSMAC_PROBE_OK);
		free(e);
	}
	/* label length is bytes: 512 fits, 513 does not */
	{
		char val[2048];
		size_t i, m;

		m = 0;
		val[m++] = '"';
		for (i = 0; i < 256; i++) {
			val[m++] = (char)0xC3;
			val[m++] = (char)0xA9;
		}
		val[m++] = '"';
		val[m] = '\0';
		accepts("label", val, &v);
		CHECK_EQ_INT(v.label_len, 512);
		m = 0;
		val[m++] = '"';
		for (i = 0; i < 171; i++) {
			val[m++] = (char)0xE4;
			val[m++] = (char)0xB8;
			val[m++] = (char)0xAD;
		}
		val[m++] = '"';
		val[m] = '\0';
		rejects("label", val, NTFSMAC_PROBE_ERR_RANGE); /* 513 bytes of 171 characters */
		m = 0;
		val[m++] = '"';
		for (i = 0; i < 513; i++)
			val[m++] = 'x';
		val[m++] = '"';
		val[m] = '\0';
		rejects("label", val, NTFSMAC_PROBE_ERR_RANGE);
		m = 0;
		val[m++] = '"';
		for (i = 0; i < 100; i++)
			m += (size_t)spf(val + m, "\\u00e9"); /* 200 bytes from 600 input bytes */
		val[m++] = '"';
		val[m] = '\0';
		accepts("label", val, &v);
		CHECK_EQ_INT(v.label_len, 200);
	}
	free(d);
}

/* ------------------------------------------------------------------ U-VERDICT-04 */

static void fill_verdict(ntfsmac_probe_verdict *v, ntfsmac_probe_state st, uint32_t reasons,
			 uint32_t nonnfc_count)
{
	uint32_t i;

	memset(v, 0, sizeof(*v));
	v->is_ntfs = true;
	v->has_ntfs_version = true;
	v->ntfs_version_major = 3;
	v->ntfs_version_minor = 1;
	memcpy(v->label, "Vol \"A\" \\ \xC3\xA9\x1B", 12);
	v->label_len = strlen(v->label);
	v->serial = UINT64_C(0xFEDCBA9876543210);
	v->total_sectors = UINT64_C(0x7FFFFFFFFFFFFFFF);
	v->cluster_size = 2097152;
	v->sector_size = 4096;
	v->volume_flags = NTFSMAC_PROBE_VOLFLAG_BIT(NTFSMAC_PROBE_VOLFLAG_DIRTY) |
			  NTFSMAC_PROBE_VOLFLAG_BIT(NTFSMAC_PROBE_VOLFLAG_UNKNOWN_BITS);
	v->logfile = NTFSMAC_PROBE_LOGFILE_UNCLEAN;
	v->has_logfile_lsn = true;
	v->logfile_lsn = UINT64_C(0xFFFFFFFFFFFFFFFF);
	v->hiberfile = NTFSMAC_PROBE_HIBERFILE_INACTIVE;
	v->state = st;
	v->reason_codes = reasons;
	v->nonnfc_count = nonnfc_count;
	v->nonnfc_path_count = nonnfc_count < 16 ? nonnfc_count : 16;
	for (i = 0; i < v->nonnfc_path_count; i++)
		snprintf(v->nonnfc_paths[i], sizeof(v->nonnfc_paths[i]), "/d%u/e\xCC\x81\x1B", (unsigned)i);
}

static bool same_verdict(const ntfsmac_probe_verdict *a, const ntfsmac_probe_verdict *b)
{
	uint32_t i;

	if (a->is_ntfs != b->is_ntfs || a->bitlocker != b->bitlocker ||
	    a->has_ntfs_version != b->has_ntfs_version ||
	    a->ntfs_version_major != b->ntfs_version_major ||
	    a->ntfs_version_minor != b->ntfs_version_minor || a->label_len != b->label_len ||
	    strcmp(a->label, b->label) != 0 || a->serial != b->serial ||
	    a->total_sectors != b->total_sectors || a->cluster_size != b->cluster_size ||
	    a->sector_size != b->sector_size || a->volume_flags != b->volume_flags ||
	    a->logfile != b->logfile || a->has_logfile_lsn != b->has_logfile_lsn ||
	    a->logfile_lsn != b->logfile_lsn || a->hiberfile != b->hiberfile || a->state != b->state ||
	    a->reason_codes != b->reason_codes || a->nonnfc_count != b->nonnfc_count ||
	    a->nonnfc_path_count != b->nonnfc_path_count)
		return false;
	for (i = 0; i < a->nonnfc_path_count; i++)
		if (strcmp(a->nonnfc_paths[i], b->nonnfc_paths[i]) != 0)
			return false;
	return true;
}

static void roundtrip(const ntfsmac_probe_verdict *in, const char *what)
{
	char *json = malloc(128 * 1024);
	ntfsmac_probe_verdict out;
	size_t n;
	ntfsmac_probe_status st;

	CHECK(json != NULL);
	CHECK(ntfsmac_probe_verdict_validate(in) == NTFSMAC_PROBE_OK);
	n = ntfsmac_probe_json(in, json, 128 * 1024);
	CHECK(n > 0);
	st = ntfsmac_probe_parse(json, n, &out);
	if (st != NTFSMAC_PROBE_OK || !same_verdict(in, &out)) {
		fprintf(stderr, "FAIL: round trip of %s: status %d\n", what, (int)st);
		g_failures++;
	}
	free(json);
}

static void u_verdict_04(void)
{
	ntfsmac_probe_verdict v;
	unsigned s, r;
	const uint32_t all_reasons = (UINT32_C(1) << NTFSMAC_PROBE_REASON_COUNT_) - 1u;

	/* the example of the document */
	accepts(NULL, NULL, &v);
	CHECK(v.is_ntfs && !v.bitlocker && v.has_ntfs_version && v.ntfs_version_major == 3 &&
	      v.ntfs_version_minor == 1);
	CHECK(strcmp(v.label, "DATA") == 0 && v.label_len == 4);
	CHECK(v.serial == UINT64_C(0x1A2B3C4D5E6F7788) && v.total_sectors == 131071);
	CHECK(v.cluster_size == 4096 && v.sector_size == 512);
	CHECK(v.volume_flags == NTFSMAC_PROBE_VOLFLAG_BIT(NTFSMAC_PROBE_VOLFLAG_DIRTY));
	CHECK(v.logfile == NTFSMAC_PROBE_LOGFILE_CLEAN && v.has_logfile_lsn &&
	      v.logfile_lsn == UINT64_C(0x1A2B3C4));
	CHECK(v.hiberfile == NTFSMAC_PROBE_HIBERFILE_ABSENT && v.state == NTFSMAC_PROBE_STATE_DIRTY_FLAG);
	CHECK(v.reason_codes == NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_FLAG_DIRTY));
	CHECK(v.nonnfc_count == 0 && v.nonnfc_path_count == 0);

	/* every state with its natural reasons, through the probe's own writer */
	fill_verdict(&v, NTFSMAC_PROBE_STATE_CLEAN, 0, 0);
	roundtrip(&v, "clean");
	for (s = 1; s < (unsigned)NTFSMAC_PROBE_STATE_COUNT_; s++) {
		/* one reason that stands for the state (the probe's mapping), and then all of them */
		for (r = 0; r < (unsigned)NTFSMAC_PROBE_REASON_COUNT_; r++) {
			uint32_t bit = NTFSMAC_PROBE_REASON_BIT(r);
			uint32_t cnt = r == NTFSMAC_PROBE_REASON_NAMES_NOT_NFC ? 3u : 0u;

			fill_verdict(&v, (ntfsmac_probe_state)s, bit, cnt);
			roundtrip(&v, ntfsmac_probe_state_name((ntfsmac_probe_state)s));
		}
		fill_verdict(&v, (ntfsmac_probe_state)s, all_reasons, 4294967295u);
		roundtrip(&v, "all reason codes, 16 paths");
	}
	/* unreadable log: null lsn; null version; names counts 1, 15, 16, 17 */
	fill_verdict(&v, NTFSMAC_PROBE_STATE_UNKNOWN, NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_LOG_UNREADABLE), 0);
	v.logfile = NTFSMAC_PROBE_LOGFILE_UNREADABLE;
	v.has_logfile_lsn = false;
	v.logfile_lsn = 0;
	v.has_ntfs_version = false;
	v.ntfs_version_major = v.ntfs_version_minor = 0;
	roundtrip(&v, "unreadable log");
	{
		static const uint32_t counts[] = { 1, 2, 15, 16, 17, 22, 1000000 };
		size_t i;

		for (i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
			fill_verdict(&v, NTFSMAC_PROBE_STATE_NAMES_NOT_NFC,
				     NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_NAMES_NOT_NFC), counts[i]);
			roundtrip(&v, "names_not_nfc");
		}
	}
	/* the largest verdict: 512 byte label of escapes, 16 paths of 512 bytes */
	{
		size_t i, k;

		fill_verdict(&v, NTFSMAC_PROBE_STATE_NAMES_NOT_NFC,
			     NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_NAMES_NOT_NFC), 20);
		memset(v.label, 0x01, 512);
		v.label[512] = '\0';
		v.label_len = 512;
		for (i = 0; i < 16; i++) {
			for (k = 0; k < 512; k++)
				v.nonnfc_paths[i][k] = (char)(k % 2 ? 0x1F : '\n');
			v.nonnfc_paths[i][512] = '\0';
		}
		roundtrip(&v, "largest verdict");
	}
}

/* ------------------------------------------------------------------ U-VERDICT-05 */

static bool all_upper_hex16(const char *s)
{
	size_t i;

	if (strlen(s) != 16)
		return false;
	for (i = 0; i < 16; i++)
		if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'A' && s[i] <= 'F')))
			return false;
	return true;
}

static void hostile_hex(const char *key)
{
	static const char *const bad[] = {
		"\"../../../Library/LaunchDaemons/x\"", "\"0x1a2b3c4d5e6f7788\"", "\"0x1A2B3C4D5E6F778\"",
		"\"0x1A2B3C4D5E6F77889\"", "\"1A2B3C4D5E6F7788\"", "\"0X1A2B3C4D5E6F7788\"",
		"\"0x1A2B3C4D5E6F778G\"", "\"0x1A2B3C4D5E6F7788\\n\"", "\"0x1A2B3C4D5E6F7788\\u001b\"",
		"\"0x1A2B3C4D5E6F77/8\"", "\"0x1A2B3C4D5E6F77\\u002f\\u002e\"", "\"0x1A2B3C4D5E6F7788 \"",
		"\" 0x1A2B3C4D5E6F7788\"", "\"\"", "\"0x\"", "\"0x0x1A2B3C4D5E6F78\"", "\"+x1A2B3C4D5E6F7788\"",
		"\"0x-1A2B3C4D5E6F778\"", "\"0x1A2B3C4D5E6F7788\\u0000\"", "\"0x1A2B3C4D5E6F77\\ud83d\\ude00\"",
		"\"0x1A2B\\u00e9C4D5E6F7788\"", "1", "null\"", "\"null\"", "\"0x1A2B3C4D5E6F7788/../../x\"",
	};
	size_t k;

	for (k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
		ntfsmac_probe_verdict v;
		ntfsmac_probe_status st = parse_doc(key, bad[k], &v);

		if (st == NTFSMAC_PROBE_OK || !is_unknown(&v)) {
			fprintf(stderr, "FAIL: %s=%s accepted\n", key, bad[k]);
			g_failures++;
		}
	}
}

static void hostile_int(const char *key)
{
	static const char *const bad[] = {
		"-1", "-0", "+1", "1.0", "1.5", "1e3", "1E3", "1e+3", "01", "00", "0x10", "1 2", "1_000",
		"18446744073709551616", "99999999999999999999999999", "\"1\"", "1a", "1.", ".5", "NaN",
		"Infinity", "0.0",
	};
	size_t k;

	for (k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
		ntfsmac_probe_verdict v;
		ntfsmac_probe_status st = parse_doc(key, bad[k], &v);

		if (st == NTFSMAC_PROBE_OK || !is_unknown(&v)) {
			fprintf(stderr, "FAIL: %s=%s accepted\n", key, bad[k]);
			g_failures++;
		}
	}
}

static void u_verdict_05(void)
{
	ntfsmac_probe_verdict v;
	char name[NTFSMAC_JOURNAL_NAME_SIZE];
	char *d = docbuf();
	char val[8192];
	size_t i, m;

	hostile_hex("serial");
	hostile_hex("logfile_lsn");
	hostile_int("schema");
	hostile_int("total_sectors");
	hostile_int("cluster_size");
	hostile_int("sector_size");
	hostile_int("nonnfc_count");

	/* exact ranges */
	rejects("total_sectors", "0", NTFSMAC_PROBE_ERR_RANGE);
	rejects("total_sectors", "9223372036854775808", NTFSMAC_PROBE_ERR_RANGE);
	accepts("total_sectors", "9223372036854775807", &v);
	CHECK(v.total_sectors == UINT64_C(0x7FFFFFFFFFFFFFFF));
	accepts("total_sectors", "1", &v);
	rejects("cluster_size", "256", NTFSMAC_PROBE_ERR_RANGE);
	rejects("cluster_size", "3000", NTFSMAC_PROBE_ERR_RANGE);
	rejects("cluster_size", "4194304", NTFSMAC_PROBE_ERR_RANGE);
	rejects("cluster_size", "0", NTFSMAC_PROBE_ERR_RANGE);
	for (i = 512; i <= 2097152; i *= 2) {
		spf(val, "%zu", i);
		accepts("cluster_size", val, &v);
		CHECK(v.cluster_size == i);
	}
	rejects("sector_size", "256", NTFSMAC_PROBE_ERR_RANGE);
	rejects("sector_size", "8192", NTFSMAC_PROBE_ERR_RANGE);
	rejects("sector_size", "1536", NTFSMAC_PROBE_ERR_RANGE);
	for (i = 512; i <= 4096; i *= 2) {
		spf(val, "%zu", i);
		accepts("sector_size", val, &v);
	}
	/* ntfs_version */
	rejects("ntfs_version", "\"4.0\"", NTFSMAC_PROBE_ERR_GRAMMAR);
	rejects("ntfs_version", "\"0.1\"", NTFSMAC_PROBE_ERR_GRAMMAR);
	rejects("ntfs_version", "\"3.10\"", NTFSMAC_PROBE_ERR_GRAMMAR);
	rejects("ntfs_version", "\"3,1\"", NTFSMAC_PROBE_ERR_GRAMMAR);
	rejects("ntfs_version", "\"3.\"", NTFSMAC_PROBE_ERR_GRAMMAR);
	rejects("ntfs_version", "\"\"", NTFSMAC_PROBE_ERR_GRAMMAR);
	rejects("ntfs_version", "\" 3.1\"", NTFSMAC_PROBE_ERR_GRAMMAR);
	rejects("ntfs_version", "\"3.1\\u0000\"", NTFSMAC_PROBE_ERR_NUL);
	accepts("ntfs_version", "\"1.0\"", &v);
	CHECK(v.has_ntfs_version && v.ntfs_version_major == 1 && v.ntfs_version_minor == 0);
	accepts("ntfs_version", "\"3.9\"", &v);
	CHECK(v.ntfs_version_major == 3 && v.ntfs_version_minor == 9);
	accepts("ntfs_version", "null", &v);
	CHECK(!v.has_ntfs_version);

	/* a `reasons` key with free text */
	m = doc2(d, "reasons", "[\"\\u001b[31mowned\"]", NULL);
	rejects_raw(d, m, NTFSMAC_PROBE_ERR_UNKNOWN_KEY, "reasons");

	/* reason_codes: unknown, duplicate, 19 entries, text with ESC [ */
	rejects("reason_codes", "[\"\\u001b[31mowned\"]", NTFSMAC_PROBE_ERR_ENUM);
	rejects("reason_codes", "[\"FLAG_DIRTY\",\"FLAG_DIRTY\"]", NTFSMAC_PROBE_ERR_GRAMMAR);
	rejects("volume_flags", "[\"DIRTY\",\"DIRTY\"]", NTFSMAC_PROBE_ERR_GRAMMAR);
	m = 0;
	val[m++] = '[';
	for (i = 0; i < (size_t)NTFSMAC_PROBE_REASON_COUNT_; i++)
		m += (size_t)spf(val + m, "%s\"%s\"", i ? "," : "",
				     ntfsmac_probe_reason_name((ntfsmac_probe_reason)i));
	val[m++] = ']';
	val[m] = '\0';
	{
		/* all 18 codes, count 3 and three paths: valid; without the paths: not */
		const kv ov[] = {
			{ "reason_codes", val }, { "state", "\"unsupported\"" }, { "nonnfc_count", "3" },
			{ "nonnfc_paths", "[\"/a\",\"/b\",\"/c\"]" }, { NULL, NULL },
		};
		const kv ov2[] = {
			{ "reason_codes", val }, { "state", "\"unsupported\"" }, { "nonnfc_count", "3" }, { NULL, NULL },
		};
		size_t n = doc_ov(d, ov);

		CHECK(ntfsmac_probe_parse(d, n, &v) == NTFSMAC_PROBE_OK);
		CHECK(v.reason_codes == (UINT32_C(1) << NTFSMAC_PROBE_REASON_COUNT_) - 1u);
		n = doc_ov(d, ov2);
		CHECK(ntfsmac_probe_parse(d, n, &v) == NTFSMAC_PROBE_ERR_CROSS_FIELD);
	}
	/* 19 entries: the 18 codes and one more */
	m = 0;
	val[m++] = '[';
	for (i = 0; i < (size_t)NTFSMAC_PROBE_REASON_COUNT_; i++)
		m += (size_t)spf(val + m, "%s\"%s\"", i ? "," : "",
				     ntfsmac_probe_reason_name((ntfsmac_probe_reason)i));
	m += (size_t)spf(val + m, ",\"FLAG_DIRTY\"]");
	rejects("reason_codes", val, NTFSMAC_PROBE_ERR_RANGE);
	m = 0;
	val[m++] = '[';
	for (i = 0; i < 19; i++)
		m += (size_t)spf(val + m, "%s\"X%zu\"", i ? "," : "", i);
	val[m++] = ']';
	val[m] = '\0';
	rejects("reason_codes", val, NTFSMAC_PROBE_ERR_ENUM);

	/* state clean with a reason code; a non-clean state without one */
	rejects("state", "\"clean\"", NTFSMAC_PROBE_ERR_CROSS_FIELD);
	rejects("reason_codes", "[]", NTFSMAC_PROBE_ERR_CROSS_FIELD);
	{
		const kv ov[] = { { "state", "\"clean\"" }, { "reason_codes", "[]" }, { NULL, NULL } };
		size_t n = doc_ov(d, ov);

		CHECK(ntfsmac_probe_parse(d, n, &v) == NTFSMAC_PROBE_OK);
		CHECK(v.state == NTFSMAC_PROBE_STATE_CLEAN && v.reason_codes == 0);
	}

	/* logfile and logfile_lsn */
	rejects("logfile_lsn", "null", NTFSMAC_PROBE_ERR_CROSS_FIELD);
	rejects("logfile", "\"unreadable\"", NTFSMAC_PROBE_ERR_CROSS_FIELD);
	{
		const kv ov[] = { { "logfile", "\"unreadable\"" }, { "logfile_lsn", "null" }, { NULL, NULL } };
		size_t n = doc_ov(d, ov);

		CHECK(ntfsmac_probe_parse(d, n, &v) == NTFSMAC_PROBE_OK);
		CHECK(!v.has_logfile_lsn && v.logfile == NTFSMAC_PROBE_LOGFILE_UNREADABLE);
	}

	/* nonnfc_count and nonnfc_paths against the code and each other */
	rejects("nonnfc_count", "1", NTFSMAC_PROBE_ERR_CROSS_FIELD);
	rejects("nonnfc_paths", "[\"/x\"]", NTFSMAC_PROBE_ERR_CROSS_FIELD);
	rejects("reason_codes", "[\"FLAG_DIRTY\",\"NAMES_NOT_NFC\"]", NTFSMAC_PROBE_ERR_CROSS_FIELD);
	{
		kv ov[] = {
			{ "reason_codes", "[\"NAMES_NOT_NFC\"]" }, { "state", "\"names_not_nfc\"" },
			{ "volume_flags", "[]" }, { "nonnfc_count", NULL }, { "nonnfc_paths", NULL }, { NULL, NULL },
		};
		char *paths = docbuf();
		size_t n;
		int pass;

		/* count 2 with no path; count 1 with two paths */
		ov[3].val = "2";
		ov[4].val = "[]";
		n = doc_ov(d, ov);
		rejects_raw(d, n, NTFSMAC_PROBE_ERR_CROSS_FIELD, "count 2, no paths");
		ov[3].val = "1";
		ov[4].val = "[\"/a\",\"/b\"]";
		n = doc_ov(d, ov);
		rejects_raw(d, n, NTFSMAC_PROBE_ERR_CROSS_FIELD, "count 1, two paths");

		/* 17 paths are one too many; 16 paths with counts 17 and 16 are right, with 15 not */
		m = 0;
		paths[m++] = '[';
		for (i = 0; i < 17; i++)
			m += (size_t)spf(paths + m, "%s\"/p%zu\"", i ? "," : "", i);
		paths[m++] = ']';
		paths[m] = '\0';
		ov[3].val = "17";
		ov[4].val = paths;
		n = doc_ov(d, ov);
		rejects_raw(d, n, NTFSMAC_PROBE_ERR_RANGE, "17 paths");
		m = 0;
		paths[m++] = '[';
		for (i = 0; i < 16; i++)
			m += (size_t)spf(paths + m, "%s\"/p%zu\"", i ? "," : "", i);
		paths[m++] = ']';
		paths[m] = '\0';
		ov[3].val = "17";
		n = doc_ov(d, ov);
		CHECK(ntfsmac_probe_parse(d, n, &v) == NTFSMAC_PROBE_OK);
		CHECK(v.nonnfc_count == 17 && v.nonnfc_path_count == 16);
		ov[3].val = "16";
		n = doc_ov(d, ov);
		CHECK(ntfsmac_probe_parse(d, n, &v) == NTFSMAC_PROBE_OK);
		ov[3].val = "15";
		n = doc_ov(d, ov);
		rejects_raw(d, n, NTFSMAC_PROBE_ERR_CROSS_FIELD, "count 15, 16 paths");
		ov[3].val = "4294967295";
		n = doc_ov(d, ov);
		CHECK(ntfsmac_probe_parse(d, n, &v) == NTFSMAC_PROBE_OK);
		ov[3].val = "4294967296";
		n = doc_ov(d, ov);
		rejects_raw(d, n, NTFSMAC_PROBE_ERR_RANGE, "count 2^32");

		/* a 513 byte path and a 512 byte path */
		for (pass = 0; pass < 2; pass++) {
			size_t plen = pass ? 512 : 513;

			m = 0;
			paths[m++] = '[';
			paths[m++] = '"';
			memset(paths + m, 'p', plen);
			m += plen;
			paths[m++] = '"';
			paths[m++] = ']';
			paths[m] = '\0';
			ov[3].val = "1";
			n = doc_ov(d, ov);
			if (pass == 0) {
				rejects_raw(d, n, NTFSMAC_PROBE_ERR_RANGE, "513 byte path");
			} else {
				CHECK(ntfsmac_probe_parse(d, n, &v) == NTFSMAC_PROBE_OK);
				CHECK_EQ_INT(strlen(v.nonnfc_paths[0]), 512);
			}
		}
		free(paths);
	}

	/*
	 * An accepted document with hostile text: the journal name the daemon builds comes from the
	 * integer only; the label is only ever used through the sanitiser; a path with ESC or a
	 * bidirectional control is shown escaped.
	 */
	{
		const kv ov[] = {
			{ "reason_codes", "[\"NAMES_NOT_NFC\"]" }, { "state", "\"names_not_nfc\"" },
			{ "volume_flags", "[]" }, { "nonnfc_count", "2" },
			{ "label", "\"../../x\\u001b[2J\\u202e,remove_hiberfile\"" },
			{ "nonnfc_paths", "[\"/a/\\u001b[31mred\\u001b[0m\",\"/b/\\u202egpj.exe\\u0007\\n\"]" },
			{ NULL, NULL },
		};
		size_t n = doc_ov(d, ov);
		char disp[NTFSMAC_PATH_DISPLAY_SIZE], leaf[NTFSMAC_LABEL_PART_SIZE];

		CHECK(ntfsmac_probe_parse(d, n, &v) == NTFSMAC_PROBE_OK);
		CHECK(ntfsmac_journal_format_name(name, sizeof(name), v.serial) == 0);
		CHECK(all_upper_hex16(name) && strcmp(name, "1A2B3C4D5E6F7788") == 0);
		CHECK_EQ_INT(v.nonnfc_path_count, 2);
		for (i = 0; i < v.nonnfc_path_count; i++) {
			size_t k, dn;

			dn = ntfsmac_path_display(v.nonnfc_paths[i], strlen(v.nonnfc_paths[i]), disp, sizeof(disp));
			CHECK_EQ_INT(dn, strlen(disp));
			for (k = 0; k < dn; k++)
				CHECK((unsigned char)disp[k] >= 0x20 && (unsigned char)disp[k] < 0x7F);
			if (i == 0)
				CHECK(strstr(disp, "\\u{001B}") != NULL);
			else
				CHECK(strstr(disp, "\\u{202E}") != NULL && strstr(disp, "\\u{0007}") != NULL &&
				      strstr(disp, "\\u{000A}") != NULL);
		}
		CHECK(ntfsmac_label_sanitize(v.label, v.label_len, v.serial, leaf, sizeof(leaf)) > 0);
		CHECK(oracle_label_violation(leaf, strlen(leaf), 64) == NULL);
		/* ../../x ESC [ 2 J RLO , remove_hiberfile: "/" ESC "[" RLO "," each become "_" */
		CHECK(strcmp(leaf, "_._.._x__2J__remove_hiberfile") == 0);
	}
	free(d);
}

/* ------------------------------------------------------------------ validator, tables */

static void validator_and_tables(void)
{
	ntfsmac_probe_verdict v, u;
	unsigned s;
	static const ntfsmac_probe_state order[] = {
		NTFSMAC_PROBE_STATE_UNSUPPORTED, NTFSMAC_PROBE_STATE_CORRUPT, NTFSMAC_PROBE_STATE_MIRROR_SPLIT,
		NTFSMAC_PROBE_STATE_HIBERNATED, NTFSMAC_PROBE_STATE_FAST_STARTUP, NTFSMAC_PROBE_STATE_UNCLEAN_LOG,
		NTFSMAC_PROBE_STATE_DIRTY_FLAG, NTFSMAC_PROBE_STATE_NAMES_NOT_NFC, NTFSMAC_PROBE_STATE_UNKNOWN,
		NTFSMAC_PROBE_STATE_CLEAN,
	};

	/* the fail-closed verdict is itself consistent */
	ntfsmac_probe_verdict_unknown(&u);
	CHECK(ntfsmac_probe_verdict_validate(&u) == NTFSMAC_PROBE_OK);
	CHECK(u.state == NTFSMAC_PROBE_STATE_UNKNOWN);

	/* cross-field rules on built verdicts */
	fill_verdict(&v, NTFSMAC_PROBE_STATE_DIRTY_FLAG, NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_FLAG_DIRTY), 0);
	CHECK(ntfsmac_probe_verdict_validate(&v) == NTFSMAC_PROBE_OK);
	v.state = NTFSMAC_PROBE_STATE_CLEAN;
	CHECK(ntfsmac_probe_verdict_validate(&v) == NTFSMAC_PROBE_ERR_CROSS_FIELD);
	fill_verdict(&v, NTFSMAC_PROBE_STATE_CLEAN, 0, 0);
	CHECK(ntfsmac_probe_verdict_validate(&v) == NTFSMAC_PROBE_OK);
	v.state = NTFSMAC_PROBE_STATE_DIRTY_FLAG;
	CHECK(ntfsmac_probe_verdict_validate(&v) == NTFSMAC_PROBE_ERR_CROSS_FIELD);
	fill_verdict(&v, NTFSMAC_PROBE_STATE_CLEAN, 0, 0);
	v.has_logfile_lsn = false;
	CHECK(ntfsmac_probe_verdict_validate(&v) == NTFSMAC_PROBE_ERR_CROSS_FIELD);
	v.has_logfile_lsn = true;
	v.logfile = NTFSMAC_PROBE_LOGFILE_UNREADABLE;
	CHECK(ntfsmac_probe_verdict_validate(&v) == NTFSMAC_PROBE_ERR_CROSS_FIELD);
	fill_verdict(&v, NTFSMAC_PROBE_STATE_NAMES_NOT_NFC, NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_NAMES_NOT_NFC), 5);
	CHECK(ntfsmac_probe_verdict_validate(&v) == NTFSMAC_PROBE_OK);
	v.nonnfc_path_count = 4;
	CHECK(ntfsmac_probe_verdict_validate(&v) == NTFSMAC_PROBE_ERR_CROSS_FIELD);
	v.nonnfc_path_count = 5;
	v.nonnfc_count = 0;
	CHECK(ntfsmac_probe_verdict_validate(&v) == NTFSMAC_PROBE_ERR_CROSS_FIELD);
	fill_verdict(&v, NTFSMAC_PROBE_STATE_NAMES_NOT_NFC, NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_NAMES_NOT_NFC), 99);
	CHECK(v.nonnfc_path_count == 16 && ntfsmac_probe_verdict_validate(&v) == NTFSMAC_PROBE_OK);
	v.reason_codes = NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_CHECK_FAILED);
	v.state = NTFSMAC_PROBE_STATE_UNKNOWN;
	CHECK(ntfsmac_probe_verdict_validate(&v) == NTFSMAC_PROBE_ERR_CROSS_FIELD); /* count without the code */
	/* values outside the grammar fail closed */
	fill_verdict(&v, NTFSMAC_PROBE_STATE_DIRTY_FLAG, NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_FLAG_DIRTY), 0);
	v.state = NTFSMAC_PROBE_STATE_COUNT_;
	CHECK(ntfsmac_probe_verdict_validate(&v) == NTFSMAC_PROBE_ERR_CROSS_FIELD);
	v.state = NTFSMAC_PROBE_STATE_DIRTY_FLAG;
	v.reason_codes |= UINT32_C(1) << NTFSMAC_PROBE_REASON_COUNT_;
	CHECK(ntfsmac_probe_verdict_validate(&v) == NTFSMAC_PROBE_ERR_CROSS_FIELD);
	v.reason_codes = NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_FLAG_DIRTY);
	v.volume_flags |= UINT32_C(1) << NTFSMAC_PROBE_VOLFLAG_COUNT_;
	CHECK(ntfsmac_probe_verdict_validate(&v) == NTFSMAC_PROBE_ERR_CROSS_FIELD);
	v.volume_flags = 0;
	v.label_len = 513;
	CHECK(ntfsmac_probe_verdict_validate(&v) == NTFSMAC_PROBE_ERR_CROSS_FIELD);
	CHECK(ntfsmac_probe_verdict_validate(NULL) == NTFSMAC_PROBE_ERR_CROSS_FIELD);

	/* the shared severity table: strictly decreasing along the documented order */
	for (s = 0; s + 1 < sizeof(order) / sizeof(order[0]); s++)
		CHECK(ntfsmac_probe_state_severity(order[s]) == ntfsmac_probe_state_severity(order[s + 1]) + 1);
	CHECK(ntfsmac_probe_state_severity(NTFSMAC_PROBE_STATE_CLEAN) == 0);
	CHECK(ntfsmac_probe_state_severity(NTFSMAC_PROBE_STATE_COUNT_) == -1);
	CHECK(ntfsmac_probe_state_severity((ntfsmac_probe_state)-1) == -1);

	/* the name tables */
	CHECK(strcmp(ntfsmac_probe_state_name(NTFSMAC_PROBE_STATE_NAMES_NOT_NFC), "names_not_nfc") == 0);
	CHECK(strcmp(ntfsmac_probe_state_name(NTFSMAC_PROBE_STATE_MIRROR_SPLIT), "mirror_split") == 0);
	CHECK(ntfsmac_probe_state_name(NTFSMAC_PROBE_STATE_COUNT_) == NULL);
	CHECK(strcmp(ntfsmac_probe_logfile_name(NTFSMAC_PROBE_LOGFILE_V2_FAST_STARTUP), "v2_fast_startup") == 0);
	CHECK(ntfsmac_probe_logfile_name(NTFSMAC_PROBE_LOGFILE_COUNT_) == NULL);
	CHECK(strcmp(ntfsmac_probe_hiberfile_name(NTFSMAC_PROBE_HIBERFILE_HIBERNATED), "hibernated") == 0);
	CHECK(ntfsmac_probe_hiberfile_name(NTFSMAC_PROBE_HIBERFILE_COUNT_) == NULL);
	CHECK(strcmp(ntfsmac_probe_volflag_name(NTFSMAC_PROBE_VOLFLAG_UNKNOWN_BITS), "UNKNOWN_BITS") == 0);
	CHECK(ntfsmac_probe_volflag_name(NTFSMAC_PROBE_VOLFLAG_COUNT_) == NULL);
	CHECK(strcmp(ntfsmac_probe_reason_name(NTFSMAC_PROBE_REASON_NAMES_NOT_NFC), "NAMES_NOT_NFC") == 0);
	CHECK(ntfsmac_probe_reason_name(NTFSMAC_PROBE_REASON_COUNT_) == NULL);
	CHECK(ntfsmac_probe_reason_name((ntfsmac_probe_reason)-1) == NULL);
}

int main(void)
{
	u_verdict_01();
	u_verdict_02();
	u_verdict_03();
	u_verdict_04();
	u_verdict_05();
	validator_and_tables();
	if (g_failures != 0) {
		fprintf(stderr, "test_verdict: %d failure(s)\n", g_failures);
		return 1;
	}
	printf("test_verdict: ok\n");
	return 0;
}
