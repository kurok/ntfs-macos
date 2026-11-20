/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * JSON replies of `list`, `status` and `doctor`, and the fixed reason tokens (T-M4-08).
 * See report.h. Pure C, no allocation.
 */
#include "report.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "sanitize.h"

/* ------------------------------------------------------------------ */
/* Writer                                                              */
/* ------------------------------------------------------------------ */

typedef struct jw {
	char *buf;
	size_t cap;
	size_t len;
	bool ovf;
} jw;

static void jw_init(jw *w, char *buf, size_t cap)
{
	w->buf = buf;
	w->cap = cap;
	w->len = 0;
	w->ovf = cap == 0;
	if (cap > 0)
		buf[0] = '\0';
}

static void jw_raw(jw *w, const char *s)
{
	size_t n = strlen(s);

	if (w->ovf || n >= w->cap - w->len) {
		w->ovf = true;
		return;
	}
	memcpy(w->buf + w->len, s, n + 1);
	w->len += n;
}

static void jw_fmt(jw *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void jw_fmt(jw *w, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (w->ovf)
		return;
	va_start(ap, fmt);
	n = vsnprintf(w->buf + w->len, w->cap - w->len, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= w->cap - w->len) {
		w->ovf = true;
		return;
	}
	w->len += (size_t)n;
}

/* A JSON string: quote, backslash and control bytes escaped; other bytes as they are. */
static void jw_str(jw *w, const char *s)
{
	const unsigned char *p = (const unsigned char *)(s != NULL ? s : "");

	jw_raw(w, "\"");
	/* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound): the analyzer does not model the NUL of the literal "" */
	for (; *p != '\0' && !w->ovf; p++) {
		if (*p == '"')
			jw_raw(w, "\\\"");
		else if (*p == '\\')
			jw_raw(w, "\\\\");
		else if (*p < 0x20u || *p == 0x7Fu)
			jw_fmt(w, "\\u%04x", (unsigned)*p);
		else {
			char c[2] = {(char)*p, '\0'};

			jw_raw(w, c);
		}
	}
	jw_raw(w, "\"");
}

static void jw_kv(jw *w, const char *key, const char *val)
{
	jw_str(w, key);
	jw_raw(w, ":");
	jw_str(w, val);
}

static size_t jw_done(jw *w)
{
	if (w->ovf) {
		if (w->cap > 0)
			w->buf[0] = '\0';
		return 0;
	}
	return w->len;
}

/* ------------------------------------------------------------------ */
/* Volumes                                                             */
/* ------------------------------------------------------------------ */

static void put_vol(jw *w, const ntfsmac_report_vol *v)
{
	jw_raw(w, "{");
	jw_kv(w, "bsd", v->bsd);
	jw_raw(w, ",");
	jw_kv(w, "label", v->label);
	jw_raw(w, ",");
	jw_kv(w, "state", v->state);
	jw_raw(w, ",");
	jw_kv(w, "mount", v->mount != NULL && v->mount[0] != '\0' ? v->mount : "none");
	jw_raw(w, ",");
	jw_kv(w, "mountpoint", v->mountpoint);
	jw_raw(w, ",");
	jw_kv(w, "reason", v->reason);
}

size_t ntfsmac_report_list(const ntfsmac_report_vol *vols, size_t n, char *buf, size_t cap)
{
	jw w;
	size_t i;

	jw_init(&w, buf, cap);
	jw_raw(&w, "{\"ok\":true,\"volumes\":[");
	for (i = 0; i < n; i++) {
		if (i > 0)
			jw_raw(&w, ",");
		put_vol(&w, &vols[i]);
		jw_raw(&w, "}");
	}
	jw_raw(&w, "]}");
	return jw_done(&w);
}

/* ------------------------------------------------------------------ */
/* Status                                                              */
/* ------------------------------------------------------------------ */

static void put_bits(jw *w, const char *key, uint32_t bits, unsigned count,
		     const char *(*name)(unsigned))
{
	unsigned i;
	bool first = true;

	jw_str(w, key);
	jw_raw(w, ":[");
	for (i = 0; i < count; i++) {
		if ((bits & (UINT32_C(1) << i)) == 0u)
			continue;
		if (!first)
			jw_raw(w, ",");
		first = false;
		jw_str(w, name(i));
	}
	jw_raw(w, "]");
}

static const char *reason_name(unsigned i)
{
	const char *s = ntfsmac_probe_reason_name((ntfsmac_probe_reason)i);

	return s != NULL ? s : "?";
}

static const char *volflag_name(unsigned i)
{
	const char *s = ntfsmac_probe_volflag_name((ntfsmac_probe_volflag)i);

	return s != NULL ? s : "?";
}

static void put_probe(jw *w, const ntfsmac_probe_verdict *v, size_t room_for_tail, size_t cap_total)
{
	uint32_t i;
	bool first = true;
	char serial[24];

	(void)cap_total;
	jw_raw(w, "\"probe\":{");
	jw_kv(w, "state", ntfsmac_probe_state_name(v->state));
	snprintf(serial, sizeof serial, "0x%016" PRIX64, v->serial);
	jw_raw(w, ",");
	jw_kv(w, "serial", serial);
	jw_raw(w, ",");
	put_bits(w, "reason_codes", v->reason_codes, NTFSMAC_PROBE_REASON_COUNT_, reason_name);
	jw_raw(w, ",");
	jw_kv(w, "logfile", ntfsmac_probe_logfile_name(v->logfile));
	jw_raw(w, ",");
	jw_kv(w, "hiberfile", ntfsmac_probe_hiberfile_name(v->hiberfile));
	jw_raw(w, ",");
	put_bits(w, "volume_flags", v->volume_flags, NTFSMAC_PROBE_VOLFLAG_COUNT_, volflag_name);
	jw_fmt(w, ",\"nonnfc_count\":%" PRIu32 ",\"nonnfc_paths\":[", v->nonnfc_count);
	for (i = 0; i < v->nonnfc_path_count && i < NTFSMAC_PROBE_MAX_NONNFC_PATHS; i++) {
		char disp[NTFSMAC_PATH_DISPLAY_SIZE];
		jw trial = *w;
		size_t len = strnlen(v->nonnfc_paths[i], NTFSMAC_PROBE_NONNFC_PATH_MAX_BYTES);

		(void)ntfsmac_path_display(v->nonnfc_paths[i], len, disp, sizeof disp);
		if (!first)
			jw_raw(&trial, ",");
		jw_str(&trial, disp);
		/* Whole paths only, and leave room for what follows the list. */
		if (trial.ovf || trial.len + room_for_tail >= trial.cap)
			break;
		*w = trial;
		first = false;
	}
	jw_raw(w, "]}");
}

size_t ntfsmac_report_status(const ntfsmac_report_vol *vol, const ntfsmac_probe_verdict *last,
			     const ntfsmac_journal_entry *je, char *buf, size_t cap)
{
	jw w;
	char jm[256];

	if (vol == NULL)
		return 0;
	jw_init(&w, buf, cap);
	jw_raw(&w, "{\"ok\":true,\"volume\":");
	put_vol(&w, vol);
	jw_raw(&w, "}");
	if (last != NULL) {
		jw_raw(&w, ",");
		put_probe(&w, last, sizeof jm + 8u, cap);
	}
	if (je != NULL && ntfsmac_priv_journal_member(je, last, jm, sizeof jm) > 0) {
		jw_raw(&w, ",");
		jw_raw(&w, jm);
	}
	jw_raw(&w, "}");
	return jw_done(&w);
}

/* ------------------------------------------------------------------ */
/* Doctor                                                              */
/* ------------------------------------------------------------------ */

size_t ntfsmac_report_doctor(const ntfsmac_doctor_finding *f, size_t n, char *buf, size_t cap)
{
	jw w;
	size_t i;

	jw_init(&w, buf, cap);
	jw_raw(&w, "{\"ok\":true,\"findings\":[");
	for (i = 0; i < n; i++) {
		if (i > 0)
			jw_raw(&w, ",");
		jw_fmt(&w, "{\"code\":%d,\"ok\":%s,\"value\":%ld}", f[i].code,
		       f[i].ok ? "true" : "false", f[i].value);
	}
	jw_raw(&w, "]}");
	return jw_done(&w);
}

/* ------------------------------------------------------------------ */
/* Tokens                                                              */
/* ------------------------------------------------------------------ */

const char *ntfsmac_report_vreason_token(ntfsmac_vol_reason r)
{
	switch (r) {
	case NTFSMAC_VREASON_PROBE_FAILED: return "probe_failed";
	case NTFSMAC_VREASON_POLICY_REFUSED: return "policy_refused";
	case NTFSMAC_VREASON_BINDING: return "binding";
	case NTFSMAC_VREASON_MOUNT_POINT: return "mount_point";
	case NTFSMAC_VREASON_ARGV_REJECTED: return "argv_rejected";
	case NTFSMAC_VREASON_SPAWN: return "spawn";
	case NTFSMAC_VREASON_MOUNT_TIMEOUT: return "mount_timeout";
	case NTFSMAC_VREASON_MODE_MISMATCH: return "mode_mismatch";
	case NTFSMAC_VREASON_UNMOUNT: return "unmount";
	case NTFSMAC_VREASON_STRAY_MOUNT: return "stray_mount";
	default: return "";
	}
}

const char *ntfsmac_report_policy_note_token(ntfsmac_policy_note n)
{
	switch (n) {
	case NTFSMAC_POLICY_NOTE_MODE_RO_REQUESTED: return "ro_requested";
	case NTFSMAC_POLICY_NOTE_EXTERNAL_CHECK: return "external_check";
	case NTFSMAC_POLICY_NOTE_UNCLEAN_SESSION: return "unclean_session";
	case NTFSMAC_POLICY_NOTE_DIRTY_FLAG: return "dirty_flag";
	case NTFSMAC_POLICY_NOTE_DIRTY_FLAG_JOURNAL: return "dirty_flag_journal";
	case NTFSMAC_POLICY_NOTE_UNCLEAN_LOG: return "unclean_log";
	case NTFSMAC_POLICY_NOTE_HIBERNATED: return "hibernated";
	case NTFSMAC_POLICY_NOTE_UNKNOWN: return "unknown";
	case NTFSMAC_POLICY_NOTE_REFUSED: return "refused";
	case NTFSMAC_POLICY_NOTE_MIRROR_SPLIT: return "mirror_split";
	case NTFSMAC_POLICY_NOTE_NAMES_NOT_NFC: return "names_not_nfc";
	default: return "";
	}
}

const char *ntfsmac_report_session_reason_token(ntfsmac_session_reason r)
{
	switch (r) {
	case NTFSMAC_SREASON_OTHER_DRIVER: return "other_driver";
	case NTFSMAC_SREASON_JOURNAL: return "journal";
	case NTFSMAC_SREASON_BINDING: return "session_binding";
	case NTFSMAC_SREASON_MARKER_FAILED: return "marker_failed";
	case NTFSMAC_SREASON_MARKER_INTERRUPTED: return "marker_interrupted";
	case NTFSMAC_SREASON_BINDING_AFTER_MARK: return "binding_after_mark";
	default: return "";
	}
}

const char *ntfsmac_report_reason_text(const char *token)
{
	static const struct {
		const char *token;
		const char *text;
	} t[] = {
		{"probe_failed", "the volume could not be checked"},
		{"policy_refused", "the volume is damaged or not supported"},
		{"binding", "the disk changed while it was being checked"},
		{"session_binding", "the disk changed while it was being checked"},
		{"mount_point", "no mount point could be created"},
		{"argv_rejected", "a safety check refused the mount"},
		{"spawn", "the file system program did not start"},
		{"mount_timeout", "the mount did not appear in time"},
		{"mode_mismatch", "the mount did not have the mode that was decided"},
		{"unmount", "the volume could not be unmounted"},
		{"stray_mount", "the program that serves the volume mounted another file system"},
		{"ro_requested", "read-only was requested"},
		{"external_check", "checked on another system; re-attach or `ntfsmac mount` for read/write"},
		{"unclean_session", "the last session on this Mac did not end cleanly; run chkdsk on Windows, then attach it again"},
		{"dirty_flag", "the volume is marked dirty; run chkdsk on Windows"},
		{"dirty_flag_journal", "this Mac left the volume open and writes from that session may be incomplete; run chkdsk on Windows"},
		{"unclean_log", "the NTFS journal shows an unclean shutdown; run chkdsk on Windows"},
		{"hibernated", "Windows hibernation or fast startup is on; boot Windows, run powercfg /h off, shut down fully"},
		{"unknown", "the safety check could not finish"},
		{"names_not_nfc", "the volume holds names this Mac cannot open; rename them on Windows or Linux"},
		{"refused", "the volume is damaged or not supported"},
		{"other_driver", "another driver has the volume mounted"},
		{"journal", "the session journal could not be written"},
		{"marker_failed", "could not mark the volume; mounted read-only"},
		{"binding_after_mark", "the disk changed while it was being marked"},
		{"engine_ro", "the engine mounted read-only"},
	};
	size_t i;

	if (token == NULL)
		return "";
	for (i = 0; i < sizeof t / sizeof t[0]; i++)
		if (strcmp(t[i].token, token) == 0)
			return t[i].text;
	return "";
}
