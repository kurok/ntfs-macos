/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_OPTIONS_H
#define NTFSMAC_OPTIONS_H

/*
 * Worker option builder and final argv validator contract (T-M3-00). Frozen:
 * changes only through a coordinator-scheduled task.
 *
 * Source of truth: docs/ARCHITECTURE.md section 3.3 (option sets, banned list,
 * `argv_check()` rules 1 to 7) as corrected by T-M2-04 (streams_interface
 * `openxattr`, no `volname`, `permissions` excludes uid/gid/fmask/dmask,
 * mandatory backend=fskit,local,norecover), ADR-005 (`recover` and
 * `remove_hiberfile` unreachable), ADR-016 (allow-list, independent
 * re-check), integration/mount-options.json (machine-readable allow-list).
 *
 * The builder (src/options/, T-M3-03) is the only producer of the worker
 * vector. argv_check() (src/sanitize/, T-M3-19) is written independently
 * against the same specification and re-parses the vector before every
 * posix_spawn. Declarations only.
 *
 * The vector is exactly (rule 1):
 *   [0] worker path (install manifest)
 *   [1] /dev/fd/<N>        N decimal, 3..255
 *   [2] /Volumes/<leaf>    absolute; <leaf> from the label sanitiser
 *   [3] -o
 *   [4] option string
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ntfsmac/policy.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NTFSMAC_WORKER_ARGC 5

/* Rule 1: descriptor number range. */
#define NTFSMAC_WORKER_FD_MIN 3
#define NTFSMAC_WORKER_FD_MAX 255

/* Rule 3: option string at most 1024 bytes (without the NUL). */
#define NTFSMAC_OPTSTR_MAX_BYTES 1024u

/*
 * Mount-point leaf: ADR-016 caps the sanitised label at 64 bytes and the
 * collision rule appends " 2" to " 99", so the leaf is at most 67 bytes
 * (ambiguity A9: rule 2 says "every label rule of ADR-016" but the suffix
 * makes the created name longer than the label cap; this header allows 67).
 */
#define NTFSMAC_LEAF_MAX_BYTES 67u
#define NTFSMAC_VOLUMES_PREFIX "/Volumes/"
#define NTFSMAC_MOUNTPOINT_SIZE (sizeof(NTFSMAC_VOLUMES_PREFIX) - 1u + NTFSMAC_LEAF_MAX_BYTES + 1u)

/* Fixed option values (section 3.3, ADR-012). */
#define NTFSMAC_OPT_FMASK "0133"
#define NTFSMAC_OPT_DMASK "0022"
#define NTFSMAC_USERMAPPING_PATH "/usr/local/etc/ntfs-macos/UserMapping"

/*
 * Option string layout produced by the builder (ambiguity A11: section 3.3
 * gives sets, not an order; argv_check() is order independent):
 *
 *   backend=fskit,local,norecover,noatime,windows_names,
 *   streams_interface=openxattr[,uid=<U>,gid=<G>,fmask=0133,dmask=0022]
 *   [,ro][,hide_hid_files][,permissions][,usermapping=<path>]
 *
 * (one line, no spaces). The bracketed uid..dmask group is omitted when
 * `permissions` is chosen. Nothing from the disk enters the string: no label,
 * no volname, no fsname, no `recover`, no `remove_hiberfile`.
 */
typedef struct ntfsmac_options_input {
	const char *worker_path;   /* install manifest path; absolute; not copied, must outlive the result */
	int device_fd;             /* N of /dev/fd/N, 3..255 */
	const char *leaf;          /* sanitised mount-point leaf, 1..67 bytes, no '/' */
	ntfsmac_policy_decision decision; /* RW or RO; REFUSE is an error */
	uint32_t console_uid;      /* decimal, no sign, no leading zero */
	uint32_t console_gid;
	/* Opt-ins from configuration (section 3.3). `permissions` and
	 * `usermapping` are independent flags (ambiguity A12: the section
	 * writes "or" between them but only forbids the uid..dmask mix). */
	bool hide_hid_files;
	bool permissions;
	bool usermapping;          /* the daemon checks the file before the spawn (section 3.3) */
} ntfsmac_options_input;

/*
 * Builder output. The strings live inside the structure and argv[] points
 * into it (argv[0] to the caller's worker_path), so do not copy the structure
 * after building; argv[NTFSMAC_WORKER_ARGC] is NULL for posix_spawn.
 */
typedef struct ntfsmac_worker_argv {
	const char *argv[NTFSMAC_WORKER_ARGC + 1];
	char devpath[16];                         /* "/dev/fd/255" */
	char mountpoint[NTFSMAC_MOUNTPOINT_SIZE]; /* "/Volumes/<leaf>" */
	char optstr[NTFSMAC_OPTSTR_MAX_BYTES + 1];
} ntfsmac_worker_argv;

typedef enum ntfsmac_options_status {
	NTFSMAC_OPTIONS_OK = 0,
	NTFSMAC_OPTIONS_ERR_INVALID,  /* NULL argument */
	NTFSMAC_OPTIONS_ERR_DECISION, /* decision is REFUSE or out of range */
	NTFSMAC_OPTIONS_ERR_WORKER,   /* worker_path empty or not absolute */
	NTFSMAC_OPTIONS_ERR_FD,       /* outside 3..255 */
	NTFSMAC_OPTIONS_ERR_LEAF      /* empty, too long, or contains '/' or NUL */
} ntfsmac_options_status;

/*
 * Build the vector for an rw or ro decision. On error *out is zeroed. The
 * builder has no code path that emits a banned key (section 3.3), and `ro` is
 * emitted if and only if the decision is RO.
 */
ntfsmac_options_status ntfsmac_options_build(const ntfsmac_options_input *in,
					     ntfsmac_worker_argv *out);

/* What argv_check() compares the vector with (all from the daemon, none from the disk). */
typedef struct ntfsmac_argv_expect {
	const char *worker_path;   /* install manifest */
	const char *leaf;          /* the name created in section 4.1 step 7 */
	ntfsmac_policy_decision decision; /* RW or RO */
	uint32_t console_uid;
	uint32_t console_gid;
} ntfsmac_argv_expect;

/*
 * Result of argv_check(). One value per failing rule of section 3.3 so that
 * the daemon logs a code and the tests assert the rule. A key on the banned
 * list is reported as BANNED, not UNKNOWN_KEY. OK is 0; any other value
 * means: do not spawn, volume state `Failed`, reason
 * "internal: worker arguments rejected".
 */
typedef enum ntfsmac_argv_status {
	NTFSMAC_ARGV_OK = 0,
	NTFSMAC_ARGV_ERR_INVALID,       /* NULL argument or bad expectation */
	NTFSMAC_ARGV_ERR_ARGC,          /* rule 1: not exactly five elements */
	NTFSMAC_ARGV_ERR_WORKER_PATH,   /* rule 1: [0] differs from the manifest path */
	NTFSMAC_ARGV_ERR_DEVICE,        /* rule 1: [1] is not /dev/fd/N, N 3..255 decimal */
	NTFSMAC_ARGV_ERR_MOUNTPOINT,    /* rule 1: [2] is not /Volumes/<leaf> */
	NTFSMAC_ARGV_ERR_DASH_O,        /* rule 1: [3] is not "-o" */
	NTFSMAC_ARGV_ERR_LEAF,          /* rule 2: label rules, or not the created name */
	NTFSMAC_ARGV_ERR_OPT_LENGTH,    /* rule 3: over 1024 bytes */
	NTFSMAC_ARGV_ERR_OPT_BYTE,      /* rule 3: byte outside 0x21..0x7E */
	NTFSMAC_ARGV_ERR_EMPTY_TOKEN,   /* rule 4 */
	NTFSMAC_ARGV_ERR_BANNED,        /* rules 4 and 6: banned key */
	NTFSMAC_ARGV_ERR_UNKNOWN_KEY,   /* rule 4: key not in the allow-list */
	NTFSMAC_ARGV_ERR_DUPLICATE_KEY, /* rule 4 */
	NTFSMAC_ARGV_ERR_VALUE,         /* rule 5: value missing, unexpected or off-grammar */
	NTFSMAC_ARGV_ERR_MISSING,       /* rule 6: a mandatory token is absent */
	NTFSMAC_ARGV_ERR_RO_MISMATCH,   /* rule 6: `ro` present iff decision is RO */
	NTFSMAC_ARGV_ERR_OWNERSHIP,     /* rule 6: uid/gid/fmask/dmask vs `permissions` */
	NTFSMAC_ARGV_ERR_FD_MODE        /* rule 7: descriptor access mode vs decision */
} ntfsmac_argv_status;

/*
 * Rules 1 to 6: pure text check of the vector, no I/O. Mandatory tokens
 * (ambiguity A10: rule 6 names only `norecover`, the text of section 3.3 and
 * mount-options.json also make `backend=fskit` and `local` mandatory, and the
 * builder-vs-checker mutation test requires every token of the Always row to
 * be present): `backend=fskit`, `local`, `norecover`, `noatime`,
 * `windows_names`, `streams_interface=openxattr|none`, and `uid`, `gid`,
 * `fmask`, `dmask` unless `permissions` is present. Order is not checked.
 * Safe to fuzz.
 */
ntfsmac_argv_status ntfsmac_argv_check_text(const char *const *argv, size_t argc,
					    const ntfsmac_argv_expect *expect);

/*
 * All seven rules: ntfsmac_argv_check_text(), then rule 7:
 * fcntl(N, F_GETFL) & O_ACCMODE must be O_RDONLY for an RO decision and
 * O_RDWR for RW (ambiguity A13: the only I/O in the check, kept out of
 * the _text variant so that the text rules stay pure and fuzzable).
 */
ntfsmac_argv_status ntfsmac_argv_check(const char *const *argv, size_t argc,
				       const ntfsmac_argv_expect *expect);

/* Short stable name of a status ("ok", "argc", ...); NULL if out of range. */
const char *ntfsmac_argv_status_name(ntfsmac_argv_status s);

#ifdef __cplusplus
}
#endif

#endif /* NTFSMAC_OPTIONS_H */
