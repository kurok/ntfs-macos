/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_PROBE_H
#define NTFSMAC_PROBE_H

/*
 * Probe verdict contract (T-M3-00). Frozen: changes only through a
 * coordinator-scheduled task (docs/AGENT_TASKS.md).
 *
 * Source of truth: docs/ARCHITECTURE.md section 3.1 (verdict and field
 * grammar), ADR-016 (strict parser, reason codes only, serial and LSN as
 * parsed integers). The JSON form is described by
 * docs/schemas/probe.schema.json; tests/unit/contracts checks that the enum
 * lists below and the schema enums agree.
 *
 * Declarations only. The parser lives in src/sanitize/ (T-M3-19), the producer
 * in src/probe/ (T-M3-02).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Schema version emitted and accepted (section 3.1: "schema version 1"). */
#define NTFSMAC_PROBE_SCHEMA_VERSION 1

/* Strict parser input cap (section 3.1: "at most 64 KiB"). */
#define NTFSMAC_PROBE_MAX_JSON_BYTES (64u * 1024u)

/* Label: at most 512 bytes of UTF-8 (section 3.1 grammar), no NUL. */
#define NTFSMAC_PROBE_LABEL_MAX_BYTES 512u

/*
 * Section 3.1: at most 9 volume_flags, at most 17 reason_codes. Each cap
 * equals the length of its list, so a verdict can carry every member once.
 * The reason cap was 16 until MFTMIRR_STALE was added (ADR-021).
 */
#define NTFSMAC_PROBE_MAX_VOLUME_FLAGS 9
#define NTFSMAC_PROBE_MAX_REASON_CODES 17

/*
 * Enum lists as X-macros: X(IDENTIFIER, "wire string"). The wire strings are
 * the only spellings the parser accepts. docs/schemas/probe.schema.json must
 * list exactly these strings.
 */

/*
 * `state` (section 3.1), in the order the document lists them; `mirror_split`
 * (ADR-021) is appended so that the earlier values keep their numbers.
 */
#define NTFSMAC_PROBE_STATE_LIST(X)         \
	X(CLEAN, "clean")                   \
	X(DIRTY_FLAG, "dirty_flag")         \
	X(UNCLEAN_LOG, "unclean_log")       \
	X(HIBERNATED, "hibernated")         \
	X(FAST_STARTUP, "fast_startup")     \
	X(UNSUPPORTED, "unsupported")       \
	X(CORRUPT, "corrupt")               \
	X(UNKNOWN, "unknown")               \
	X(MIRROR_SPLIT, "mirror_split")

/* `logfile` (section 3.1). */
#define NTFSMAC_PROBE_LOGFILE_LIST(X)           \
	X(CLEAN, "clean")                       \
	X(UNCLEAN, "unclean")                   \
	X(V2_FAST_STARTUP, "v2_fast_startup")   \
	X(UNREADABLE, "unreadable")

/* `hiberfile` (section 3.1). */
#define NTFSMAC_PROBE_HIBERFILE_LIST(X)         \
	X(ABSENT, "absent")                     \
	X(INACTIVE, "inactive")                 \
	X(HIBERNATED, "hibernated")             \
	X(UNREADABLE, "unreadable")

/* `volume_flags` members (section 3.1; bits per check 3 of the table). */
#define NTFSMAC_PROBE_VOLFLAG_LIST(X)                   \
	X(DIRTY, "DIRTY")                               \
	X(RESIZE_LOG_FILE, "RESIZE_LOG_FILE")           \
	X(UPGRADE_ON_MOUNT, "UPGRADE_ON_MOUNT")         \
	X(MOUNTED_ON_NT4, "MOUNTED_ON_NT4")             \
	X(DELETE_USN_UNDERWAY, "DELETE_USN_UNDERWAY")   \
	X(REPAIR_OBJECT_ID, "REPAIR_OBJECT_ID")         \
	X(CHKDSK_UNDERWAY, "CHKDSK_UNDERWAY")           \
	X(MODIFIED_BY_CHKDSK, "MODIFIED_BY_CHKDSK")     \
	X(UNKNOWN_BITS, "UNKNOWN_BITS")

/*
 * `reason_codes` members: the normative list of section 3.1 (ambiguity A1 of
 * the T-M3-00 report resolved: the list is closed). MFTMIRR_STALE was added
 * with state `mirror_split` (ADR-021). Adding a code is a
 * coordinator-scheduled change to this header and to the schema together.
 * Text for each code lives in the daemon and the CLI, never in the probe.
 */
#define NTFSMAC_PROBE_REASON_LIST(X)                            \
	X(NOT_NTFS, "NOT_NTFS")                                 \
	X(BITLOCKER, "BITLOCKER")                               \
	X(MOUNT_FAILED, "MOUNT_FAILED")                         \
	X(FLAG_DIRTY, "FLAG_DIRTY")                             \
	X(FLAG_CHKDSK_UNDERWAY, "FLAG_CHKDSK_UNDERWAY")         \
	X(FLAG_PENDING_WORK, "FLAG_PENDING_WORK")               \
	X(FLAG_UNKNOWN_BITS, "FLAG_UNKNOWN_BITS")               \
	X(LOG_UNCLEAN, "LOG_UNCLEAN")                           \
	X(LOG_V2, "LOG_V2")                                     \
	X(LOG_UNREADABLE, "LOG_UNREADABLE")                     \
	X(HIBER_SIGNATURE, "HIBER_SIGNATURE")                   \
	X(HIBER_WAKE, "HIBER_WAKE")                             \
	X(HIBER_SHORT, "HIBER_SHORT")                           \
	X(HIBER_UNREADABLE, "HIBER_UNREADABLE")                 \
	X(VERSION_UNKNOWN, "VERSION_UNKNOWN")                   \
	X(CHECK_FAILED, "CHECK_FAILED")                         \
	X(MFTMIRR_STALE, "MFTMIRR_STALE")

typedef enum ntfsmac_probe_state {
#define X(id, str) NTFSMAC_PROBE_STATE_##id,
	NTFSMAC_PROBE_STATE_LIST(X)
#undef X
	NTFSMAC_PROBE_STATE_COUNT_
} ntfsmac_probe_state;

typedef enum ntfsmac_probe_logfile {
#define X(id, str) NTFSMAC_PROBE_LOGFILE_##id,
	NTFSMAC_PROBE_LOGFILE_LIST(X)
#undef X
	NTFSMAC_PROBE_LOGFILE_COUNT_
} ntfsmac_probe_logfile;

typedef enum ntfsmac_probe_hiberfile {
#define X(id, str) NTFSMAC_PROBE_HIBERFILE_##id,
	NTFSMAC_PROBE_HIBERFILE_LIST(X)
#undef X
	NTFSMAC_PROBE_HIBERFILE_COUNT_
} ntfsmac_probe_hiberfile;

/* Index of a `volume_flags` member; the verdict stores them as a bit set. */
typedef enum ntfsmac_probe_volflag {
#define X(id, str) NTFSMAC_PROBE_VOLFLAG_##id,
	NTFSMAC_PROBE_VOLFLAG_LIST(X)
#undef X
	NTFSMAC_PROBE_VOLFLAG_COUNT_
} ntfsmac_probe_volflag;

/* Index of a `reason_codes` member; the verdict stores them as a bit set. */
typedef enum ntfsmac_probe_reason {
#define X(id, str) NTFSMAC_PROBE_REASON_##id,
	NTFSMAC_PROBE_REASON_LIST(X)
#undef X
	NTFSMAC_PROBE_REASON_COUNT_
} ntfsmac_probe_reason;

#define NTFSMAC_PROBE_VOLFLAG_BIT(f) (UINT32_C(1) << (unsigned)(f))
#define NTFSMAC_PROBE_REASON_BIT(r) (UINT32_C(1) << (unsigned)(r))

/*
 * Parsed verdict. Every field is the result of the strict parser; nothing here
 * is a pointer into the input and no probe string other than `label` survives.
 *
 * Choice (ambiguity A2): `volume_flags` and `reason_codes` are bit sets, so
 * "distinct" holds by construction and the wire order is not preserved. The
 * daemon never needs the order.
 */
typedef struct ntfsmac_probe_verdict {
	bool is_ntfs;
	bool bitlocker;

	/* `ntfs_version`: "M.m" with M in 1..3, m in 0..9; has_ntfs_version
	 * is false for JSON null. Display only. */
	bool has_ntfs_version;
	uint8_t ntfs_version_major;
	uint8_t ntfs_version_minor;

	/* `label`: raw attacker-controlled UTF-8, no NUL inside, label_len <= 512,
	 * always NUL terminated in the buffer. Input to the label sanitiser only
	 * (ADR-016); never a path, option, log line or display string. */
	char label[NTFSMAC_PROBE_LABEL_MAX_BYTES + 1];
	size_t label_len;

	/* `serial` parsed from "0x" + 16 upper-case hex digits. The input string
	 * is discarded; all later text is formatted from this integer. */
	uint64_t serial;

	uint64_t total_sectors; /* 1 .. 2^63 - 1 */
	uint32_t cluster_size;  /* power of two, 512 .. 2097152 */
	uint32_t sector_size;   /* 512, 1024, 2048 or 4096 */

	uint32_t volume_flags;  /* NTFSMAC_PROBE_VOLFLAG_BIT() set */

	ntfsmac_probe_logfile logfile;

	/* `logfile_lsn`: has_logfile_lsn is false (JSON null) if and only if
	 * logfile == UNREADABLE. Evidence for the rule of section 3.5 only. */
	bool has_logfile_lsn;
	uint64_t logfile_lsn;

	ntfsmac_probe_hiberfile hiberfile;
	ntfsmac_probe_state state;

	uint32_t reason_codes;  /* NTFSMAC_PROBE_REASON_BIT() set */
} ntfsmac_probe_verdict;

/* Result of parse and validate (section 3.1 grammar; ADR-016 decision 1). */
typedef enum ntfsmac_probe_status {
	NTFSMAC_PROBE_OK = 0,
	NTFSMAC_PROBE_ERR_TOO_LARGE,   /* input over 64 KiB */
	NTFSMAC_PROBE_ERR_UTF8,        /* invalid UTF-8 */
	NTFSMAC_PROBE_ERR_NUL,         /* \u0000 (or a raw NUL) anywhere */
	NTFSMAC_PROBE_ERR_SYNTAX,      /* not one JSON object; bad number form
	                                * (sign, fraction, exponent, leading zero) */
	NTFSMAC_PROBE_ERR_DEPTH,       /* nesting deeper than 2 */
	NTFSMAC_PROBE_ERR_DUP_KEY,     /* duplicate key */
	NTFSMAC_PROBE_ERR_UNKNOWN_KEY, /* key outside the grammar */
	NTFSMAC_PROBE_ERR_MISSING_KEY, /* a required key is absent */
	NTFSMAC_PROBE_ERR_TYPE,        /* wrong JSON type for a key */
	NTFSMAC_PROBE_ERR_RANGE,       /* integer or length out of range */
	NTFSMAC_PROBE_ERR_ENUM,        /* value not in the enum / reason list */
	NTFSMAC_PROBE_ERR_GRAMMAR,     /* string fails its pattern (serial, lsn,
	                                * ntfs_version) or duplicate array member */
	NTFSMAC_PROBE_ERR_CROSS_FIELD  /* cross-field rule violated (below) */
} ntfsmac_probe_status;

/*
 * Strict parser (section 3.1, ADR-016). `json` need not be NUL terminated.
 * Returns NTFSMAC_PROBE_OK and fills *out, or an error and fills *out with the
 * fail-closed verdict of ntfsmac_probe_verdict_unknown(). The caller never
 * sees a partly filled verdict, and any error means state `unknown`.
 */
ntfsmac_probe_status ntfsmac_probe_parse(const char *json, size_t len,
					 ntfsmac_probe_verdict *out);

/*
 * Cross-field rules of section 3.1 on an already built verdict:
 *  - state `clean` if and only if reason_codes is empty;
 *  - has_logfile_lsn is false if and only if logfile is `unreadable`.
 * Returns OK or NTFSMAC_PROBE_ERR_CROSS_FIELD. Used by the parser and by the
 * probe before it prints (ambiguity A3: the second rule is stated in the
 * `logfile_lsn` row of the grammar table, not in the cross-field paragraph).
 */
ntfsmac_probe_status ntfsmac_probe_verdict_validate(const ntfsmac_probe_verdict *v);

/*
 * The fail-closed verdict: state `unknown`, reason CHECK_FAILED, everything
 * else zero (is_ntfs false, no version, empty label, serial 0, logfile
 * `unreadable`, no LSN, hiberfile `unreadable`). Policy maps it to `ro`.
 * Not itself a valid schema-1 document (serial and sector counts are zero).
 */
void ntfsmac_probe_verdict_unknown(ntfsmac_probe_verdict *out);

/* Wire strings; NULL for an out-of-range value. Static storage. */
const char *ntfsmac_probe_state_name(ntfsmac_probe_state s);
const char *ntfsmac_probe_logfile_name(ntfsmac_probe_logfile l);
const char *ntfsmac_probe_hiberfile_name(ntfsmac_probe_hiberfile h);
const char *ntfsmac_probe_volflag_name(ntfsmac_probe_volflag f);
const char *ntfsmac_probe_reason_name(ntfsmac_probe_reason r);

/*
 * Severity rank for the "most severe finding" rule of section 3.1:
 * unsupported > corrupt > mirror_split > hibernated > fast_startup >
 * unclean_log > dirty_flag > unknown > clean. Higher is more severe; -1 for
 * an out-of-range value.
 */
int ntfsmac_probe_state_severity(ntfsmac_probe_state s);

#ifdef __cplusplus
}
#endif

#endif /* NTFSMAC_PROBE_H */
