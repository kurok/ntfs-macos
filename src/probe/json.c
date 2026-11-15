/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The verdict as one JSON object (docs/ARCHITECTURE.md section 3.1, schema
 * version 1). Keys in the order of the example there. Strings are UTF-8 made
 * by text.c (no lone surrogate, no U+0000); this writer escapes `"`, `\` and
 * the C0 control characters, so the output is valid JSON and has no `\u0000`.
 */
#include "posix.h"
#include "probe_core.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct out {
	char *buf;
	size_t cap, len;
	bool overflow;
} out;

static void put(out *o, const char *s, size_t n)
{
	if (o->overflow || o->len + n + 1 > o->cap) {
		o->overflow = true;
		return;
	}
	memcpy(o->buf + o->len, s, n);
	o->len += n;
	o->buf[o->len] = '\0';
}

static void puts_(out *o, const char *s)
{
	put(o, s, strlen(s));
}

static void putf(out *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void putf(out *o, const char *fmt, ...)
{
	char tmp[64];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= sizeof(tmp)) {
		o->overflow = true;
		return;
	}
	put(o, tmp, (size_t)n);
}

static void put_string(out *o, const char *s)
{
	size_t i;

	put(o, "\"", 1);
	for (i = 0; s[i]; i++) {
		unsigned char c = (unsigned char)s[i];

		if (c == '"' || c == '\\') {
			char e[2] = { '\\', (char)c };

			put(o, e, 2);
		} else if (c < 0x20) {
			putf(o, "\\u%04X", (unsigned)c);
		} else {
			put(o, (const char *)&c, 1);
		}
	}
	put(o, "\"", 1);
}

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

static void put_bits(out *o, uint32_t bits, const char *const *names, unsigned count)
{
	unsigned i, n = 0;

	put(o, "[", 1);
	for (i = 0; i < count; i++) {
		if (!(bits & (UINT32_C(1) << i)))
			continue;
		if (n++)
			put(o, ",", 1);
		put_string(o, names[i]);
	}
	put(o, "]", 1);
}

size_t ntfsmac_probe_json(const ntfsmac_probe_verdict *v, char *buf, size_t cap)
{
	out o = { buf, cap, 0, false };
	uint32_t i;

	if (cap)
		buf[0] = '\0';
	if ((unsigned)v->state >= (unsigned)NTFSMAC_PROBE_STATE_COUNT_ ||
	    (unsigned)v->logfile >= (unsigned)NTFSMAC_PROBE_LOGFILE_COUNT_ ||
	    (unsigned)v->hiberfile >= (unsigned)NTFSMAC_PROBE_HIBERFILE_COUNT_)
		return 0;
	puts_(&o, "{\n");
	putf(&o, "  \"schema\": %d,\n", NTFSMAC_PROBE_SCHEMA_VERSION);
	putf(&o, "  \"is_ntfs\": %s,\n", v->is_ntfs ? "true" : "false");
	if (v->has_ntfs_version)
		putf(&o, "  \"ntfs_version\": \"%u.%u\",\n", (unsigned)v->ntfs_version_major,
		     (unsigned)v->ntfs_version_minor);
	else
		puts_(&o, "  \"ntfs_version\": null,\n");
	puts_(&o, "  \"label\": ");
	put_string(&o, v->label);
	puts_(&o, ",\n");
	putf(&o, "  \"serial\": \"0x%016" PRIX64 "\",\n", v->serial);
	putf(&o, "  \"total_sectors\": %" PRIu64 ",\n", v->total_sectors);
	putf(&o, "  \"cluster_size\": %u,\n", (unsigned)v->cluster_size);
	putf(&o, "  \"sector_size\": %u,\n", (unsigned)v->sector_size);
	puts_(&o, "  \"volume_flags\": ");
	put_bits(&o, v->volume_flags, volflag_names, (unsigned)NTFSMAC_PROBE_VOLFLAG_COUNT_);
	puts_(&o, ",\n");
	putf(&o, "  \"logfile\": \"%s\",\n", logfile_names[v->logfile]);
	if (v->has_logfile_lsn)
		putf(&o, "  \"logfile_lsn\": \"0x%016" PRIX64 "\",\n", v->logfile_lsn);
	else
		puts_(&o, "  \"logfile_lsn\": null,\n");
	putf(&o, "  \"hiberfile\": \"%s\",\n", hiberfile_names[v->hiberfile]);
	putf(&o, "  \"bitlocker\": %s,\n", v->bitlocker ? "true" : "false");
	putf(&o, "  \"state\": \"%s\",\n", state_names[v->state]);
	puts_(&o, "  \"reason_codes\": ");
	put_bits(&o, v->reason_codes, reason_names, (unsigned)NTFSMAC_PROBE_REASON_COUNT_);
	puts_(&o, ",\n");
	putf(&o, "  \"nonnfc_count\": %" PRIu32 ",\n", v->nonnfc_count);
	puts_(&o, "  \"nonnfc_paths\": [");
	for (i = 0; i < v->nonnfc_path_count && i < NTFSMAC_PROBE_MAX_NONNFC_PATHS; i++) {
		puts_(&o, i ? ",\n    " : "\n    ");
		put_string(&o, v->nonnfc_paths[i]);
	}
	puts_(&o, v->nonnfc_path_count ? "\n  ]\n" : "]\n");
	puts_(&o, "}\n");
	return o.overflow ? 0 : o.len;
}
