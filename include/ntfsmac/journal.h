/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_JOURNAL_H
#define NTFSMAC_JOURNAL_H

/*
 * Session journal contract (T-M3-00). Frozen: changes only through a
 * coordinator-scheduled task.
 *
 * Source of truth: docs/ARCHITECTURE.md section 3.5 (entry fields, marker and
 * state transitions, removal paths, tombstones, file naming), section 3.6
 * (marker values), ADR-016 (names from the parsed serial only), ADR-017
 * (removal paths). Library used by the root daemon; implemented in
 * src/journal/ (T-M3-13). Declarations only.
 *
 * Every entry is keyed by the 64-bit serial (section 3.1 grammar). No byte of
 * the probe's serial string is ever used in a name: names are formatted here
 * from the integer. The directory is held as a descriptor opened with
 * O_DIRECTORY | O_NOFOLLOW; every open, rename and unlink is relative to it
 * (openat, renameat, O_NOFOLLOW | O_EXCL for the temporary file).
 *
 * The on-disk byte format of an entry is private to the implementation. The
 * contract is the field set below and the naming and durability rules.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Section 3.5: production directory (root, mode 0600 files). */
#define NTFSMAC_JOURNAL_DEFAULT_DIR "/var/db/ntfs-macos/journal"

/* Section 3.5: tombstones kept for `doctor`, "at most 32 kept". */
#define NTFSMAC_JOURNAL_MAX_TOMBSTONES 32

/* Sanitised label (ADR-016: at most 64 bytes of UTF-8) plus NUL. */
#define NTFSMAC_JOURNAL_LABEL_MAX_BYTES 64u

/* Entry name: 16 upper-case hex digits plus NUL. */
#define NTFSMAC_JOURNAL_NAME_SIZE 17u

/* ".<16 hex>.tmp" plus NUL. */
#define NTFSMAC_JOURNAL_TMPNAME_SIZE 22u

/* "<16 hex>-<YYYYMMDDTHHMMSSZ>" is 16 + 1 + 16 = 33 characters plus NUL. */
#define NTFSMAC_JOURNAL_TOMBNAME_SIZE 34u

/* `marker` (section 3.5, section 3.6). */
typedef enum ntfsmac_journal_marker {
	NTFSMAC_JOURNAL_MARKER_PENDING = 0, /* volmark set not yet done */
	NTFSMAC_JOURNAL_MARKER_SET,         /* VOLUME_IS_DIRTY set on disk */
	NTFSMAC_JOURNAL_MARKER_FAILED,      /* volmark set failed; session is ro */
	NTFSMAC_JOURNAL_MARKER_NONE         /* marker step disabled (ADR-006) */
} ntfsmac_journal_marker;

/* `state` (section 3.5). */
typedef enum ntfsmac_journal_state {
	NTFSMAC_JOURNAL_STATE_OPEN = 0,
	NTFSMAC_JOURNAL_STATE_WRITE_ERROR
} ntfsmac_journal_state;

/*
 * Entry fields; "nothing else is stored" (section 3.5).
 *
 * Choices where the section gives no type (ambiguity A4): `session_id` is a
 * non-zero 64-bit identifier chosen by the daemon, unique per read/write
 * session; timestamps are signed 64-bit seconds since the Unix epoch (UTC);
 * `logfile_lsn_at_start` is always present, because a verdict that allows rw
 * is `clean` and a `clean` verdict has a readable $LogFile (section 3.1
 * cross-field rules), so the LSN exists.
 */
typedef struct ntfsmac_journal_entry {
	uint64_t serial;
	char label[NTFSMAC_JOURNAL_LABEL_MAX_BYTES + 1]; /* sanitised, NUL terminated */
	uint64_t total_sectors;
	uint64_t session_id;
	int64_t created;
	int64_t updated;
	uint64_t logfile_lsn_at_start;
	ntfsmac_journal_marker marker;
	ntfsmac_journal_state state;
} ntfsmac_journal_entry;

typedef enum ntfsmac_journal_status {
	NTFSMAC_JOURNAL_OK = 0,
	NTFSMAC_JOURNAL_ERR_INVALID,        /* bad argument or field out of range */
	NTFSMAC_JOURNAL_ERR_NOT_FOUND,      /* no entry for this serial */
	NTFSMAC_JOURNAL_ERR_EXISTS,         /* create over an existing entry */
	NTFSMAC_JOURNAL_ERR_TRANSITION,     /* marker or state change not allowed */
	NTFSMAC_JOURNAL_ERR_IMMUTABLE,      /* update changed an immutable field */
	NTFSMAC_JOURNAL_ERR_PATH_CONDITION, /* removal path precondition not met */
	NTFSMAC_JOURNAL_ERR_CORRUPT,        /* stored entry fails its own grammar */
	NTFSMAC_JOURNAL_ERR_IO              /* open, write, fsync or rename failed */
} ntfsmac_journal_status;

/* The four ways an entry leaves the journal (section 3.5, "How an entry is removed"). */
typedef enum ntfsmac_journal_removal {
	NTFSMAC_JOURNAL_REMOVE_CLEAN_SESSION = 1, /* 1: clean end of our own session */
	NTFSMAC_JOURNAL_REMOVE_EXTERNAL_CHECK,    /* 2: external-check evidence */
	NTFSMAC_JOURNAL_REMOVE_JOURNAL_CLEAR,     /* 3: ntfsmac journal-clear */
	NTFSMAC_JOURNAL_REMOVE_REPAIR             /* 4: ntfsmac repair --clear-dirty */
} ntfsmac_journal_removal;

typedef struct ntfsmac_journal ntfsmac_journal; /* opaque: directory descriptor */

/*
 * Pure naming helpers (U-JRN-NAME-01). They format from the integer only and
 * write at most `size` bytes including the NUL; they return 0 on success and
 * -1 if `size` is too small or `when_utc` cannot be broken down.
 *   entry      <16 upper-case hex digits>
 *   temporary  .<16 hex>.tmp
 *   tombstone  <16 hex>-<YYYYMMDDTHHMMSSZ>   (inside cleared/)
 */
int ntfsmac_journal_format_name(char *out, size_t size, uint64_t serial);
int ntfsmac_journal_format_tmpname(char *out, size_t size, uint64_t serial);
int ntfsmac_journal_format_tombstone(char *out, size_t size, uint64_t serial,
				     int64_t when_utc);

/*
 * Transition rules (section 3.5 table). Identity is always allowed.
 *   marker: pending -> set | failed; set, failed, none: no change.
 *   state:  open -> write_error; write_error: no change (never back to open).
 * `set` stays `set` when `volmark clear` fails (section 3.6 ordering step 3),
 * and `failed` is final for that session (ambiguity A5: the section lists the
 * causes of each value but no explicit transition graph).
 */
bool ntfsmac_journal_marker_transition_ok(ntfsmac_journal_marker from,
					  ntfsmac_journal_marker to);
bool ntfsmac_journal_state_transition_ok(ntfsmac_journal_state from,
					 ntfsmac_journal_state to);

/*
 * Open the journal directory with O_DIRECTORY | O_NOFOLLOW (the last path
 * component must not be a symbolic link) and make sure `cleared/` exists
 * below it (mode 0700). The directory itself is created (mode 0700) if
 * missing; its parent must exist. Stray ".<hex>.tmp" files from an
 * interrupted write are removed. On success *out owns one descriptor.
 */
ntfsmac_journal_status ntfsmac_journal_open(const char *dir_path,
					    ntfsmac_journal **out);
void ntfsmac_journal_close(ntfsmac_journal *j);

/*
 * Create the entry for e->serial (entries are keyed by serial). Fails with
 * ERR_EXISTS rather than replacing. Write protocol: temporary file created
 * with O_NOFOLLOW | O_EXCL, mode 0600, written, F_FULLFSYNC, renameat to the
 * entry name, directory fsync. Returns OK only after the directory sync; a
 * kill at any earlier step leaves no entry, or the previous state, and never
 * a partial entry under the final name.
 */
ntfsmac_journal_status ntfsmac_journal_create(ntfsmac_journal *j,
					      const ntfsmac_journal_entry *e);

/* Read the entry for `serial`. NOT_FOUND if absent, CORRUPT if unreadable. */
ntfsmac_journal_status ntfsmac_journal_get(ntfsmac_journal *j, uint64_t serial,
					   ntfsmac_journal_entry *out);

/*
 * Replace the entry for e->serial with the same write protocol as create.
 * Checked against the stored entry: marker and state must pass the transition
 * helpers (ERR_TRANSITION) and serial, label, total_sectors, session_id,
 * created and logfile_lsn_at_start must be unchanged (ERR_IMMUTABLE); only
 * `updated`, `marker` and `state` may change. NOT_FOUND if absent.
 */
ntfsmac_journal_status ntfsmac_journal_update(ntfsmac_journal *j,
					      const ntfsmac_journal_entry *e);

/*
 * Remove the entry for `serial` by renaming it to cleared/<serial>-<UTC time>
 * (a tombstone; nothing is unlinked except by the retention rule), then keep
 * at most NTFSMAC_JOURNAL_MAX_TOMBSTONES of them (oldest by name time go
 * first), fsync both directories, and log which path removed it. Time and
 * count never remove an entry by themselves.
 *
 * Preconditions the library can see (ERR_PATH_CONDITION otherwise):
 *   CLEAN_SESSION   state == open and marker is set or none
 *   EXTERNAL_CHECK  marker == set
 *   JOURNAL_CLEAR   none (any marker, any state)
 *   REPAIR          none
 * The conditions on probe, peer and sessions (sections 3.2 to 3.5) are the
 * caller's: policy gates and the daemon. If a tombstone with the same name
 * exists (same second) the removal fails with ERR_EXISTS and the entry stays.
 */
ntfsmac_journal_status ntfsmac_journal_remove(ntfsmac_journal *j, uint64_t serial,
					      ntfsmac_journal_removal path,
					      int64_t now_utc);

/* Callback for the list functions; return non-zero to stop. */
typedef int (*ntfsmac_journal_entry_cb)(const ntfsmac_journal_entry *e, void *ctx);
typedef int (*ntfsmac_journal_tombstone_cb)(uint64_t serial, int64_t when_utc, void *ctx);

/* Entries (for `doctor` and daemon start reconciliation); unreadable files are skipped and counted in *skipped if non-NULL. */
ntfsmac_journal_status ntfsmac_journal_list(ntfsmac_journal *j,
					    ntfsmac_journal_entry_cb cb, void *ctx,
					    unsigned *skipped);

/* Tombstones, newest first (for `doctor`). Names that do not match the tombstone grammar are skipped. */
ntfsmac_journal_status ntfsmac_journal_list_tombstones(ntfsmac_journal *j,
						       ntfsmac_journal_tombstone_cb cb,
						       void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* NTFSMAC_JOURNAL_H */
