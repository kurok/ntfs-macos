/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_DAEMON_INTERNAL_H
#define NTFSMAC_DAEMON_INTERNAL_H

/*
 * Internal interfaces of the daemon `ntfsmacd` (T-M4-00).
 *
 * Source of truth: docs/ARCHITECTURE.md section 3 (components), section 3.4
 * (control protocol), section 4.1 and 4.5 (data flows), section 5 (the volume
 * state machine), section 7 (session channel).
 *
 * Rules:
 *  - volume.c is pure C: no Disk Arbitration, no CoreFoundation, no I/O.
 *    It builds and is tested on the macOS host and on Linux.
 *  - macOS-only APIs stay behind the interfaces below. Only da.c (T-M4-02)
 *    and main.c (T-M4-08) may include DiskArbitration or CoreFoundation
 *    headers. Everything else here is plain POSIX.
 *  - Every function that is not implemented yet returns
 *    NTFSMAC_DMN_E_NOTIMPL. A later task replaces the stub in its own file
 *    and does not change a signature without a coordinator-scheduled edit of
 *    this header.
 *  - Disk-derived strings (label, probe text) never appear in these
 *    interfaces unless they have passed the sanitiser of ADR-016.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "ntfsmac/journal.h"
#include "ntfsmac/policy.h"
#include "ntfsmac/probe.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Common status                                                       */
/* ------------------------------------------------------------------ */

typedef enum ntfsmac_dmn_status {
	NTFSMAC_DMN_OK = 0,
	NTFSMAC_DMN_E_NOTIMPL,  /* stub: the owning task has not run yet */
	NTFSMAC_DMN_E_INVAL,    /* bad argument */
	NTFSMAC_DMN_E_STATE,    /* the volume is in a state that refuses the call */
	NTFSMAC_DMN_E_BUSY,     /* a retry later may work (unmount with open files) */
	NTFSMAC_DMN_E_IO,       /* system call failed; errno in the caller's out field */
	NTFSMAC_DMN_E_TIMEOUT,  /* a deadline of section 4.1 passed */
	NTFSMAC_DMN_E_DENIED    /* refused by a gate, a peer check or the policy */
} ntfsmac_dmn_status;

/* Fixed name of the status, for logs. Never NULL. */
const char *ntfsmac_dmn_status_name(ntfsmac_dmn_status s);

/* ------------------------------------------------------------------ */
/* Volume state machine (volume.c; section 5)                          */
/* ------------------------------------------------------------------ */

/*
 * States of one volume, keyed by BSD name, owned by that volume's serial
 * queue. `Mounted` has a mode (rw or ro) kept in the volume record, not as
 * two states.
 */
typedef enum ntfsmac_vol_state {
	NTFSMAC_VOL_APPEARED = 0, /* disk seen, nothing done; also: unmounted, disk still there */
	NTFSMAC_VOL_PROBING,      /* descriptor A open, probe running (section 4.1 step 4) */
	NTFSMAC_VOL_DECIDING,     /* verdict in hand; policy and binding (steps 5 and 6) */
	NTFSMAC_VOL_MOUNTING,     /* mount point, volmark, worker spawn, mount wait (steps 7, 8) */
	NTFSMAC_VOL_MOUNTED,      /* mounted rw or ro (step 9) */
	NTFSMAC_VOL_UNMOUNTING,   /* unmount, worker exit, session end, cleanup (section 4.5) */
	NTFSMAC_VOL_GONE,         /* the disk is gone and cleanup is done; terminal */
	NTFSMAC_VOL_FAILED,       /* refused or failed; `mount` may try again */
	NTFSMAC_VOL_STATE_COUNT_
} ntfsmac_vol_state;

/* Events that drive the machine. Each names the fact that happened. */
typedef enum ntfsmac_vol_event {
	NTFSMAC_VEV_START = 0,   /* disk approved for handling or a `mount` request */
	NTFSMAC_VEV_PROBE_DONE,  /* probe finished (a timeout or crash is verdict unknown) */
	NTFSMAC_VEV_PROBE_FAILED,/* probe could not run: open failed, spawn failed */
	NTFSMAC_VEV_DECIDED,     /* policy gave rw or ro and the binding holds */
	NTFSMAC_VEV_REFUSED,     /* policy gave refuse, or the binding or setup failed */
	NTFSMAC_VEV_MOUNTED,     /* mount seen in getfsstat, flags compared with the decision */
	NTFSMAC_VEV_MOUNT_FAILED,/* spawn, wait deadline, mode mismatch (ro decision, rw mount) */
	NTFSMAC_VEV_UNMOUNT,     /* eject, `unmount` request, worker exit, outside unmount */
	NTFSMAC_VEV_UNMOUNT_BUSY,/* plain unmount kept failing with EBUSY; volume stays mounted */
	NTFSMAC_VEV_UNMOUNTED,   /* cleanup finished (mount gone, worker gone, journal handled) */
	NTFSMAC_VEV_UNMOUNT_FAILED, /* the mount could not be removed even with the forced path */
	NTFSMAC_VEV_DISK_GONE,   /* Disk Arbitration reported the disk disappeared */
	NTFSMAC_VEV_COUNT_
} ntfsmac_vol_event;

/* Why a volume is in `Failed`. Mapped to user text by the daemon and CLI. */
typedef enum ntfsmac_vol_reason {
	NTFSMAC_VREASON_NONE = 0,
	NTFSMAC_VREASON_PROBE_FAILED,   /* section 4.1 step 4 */
	NTFSMAC_VREASON_POLICY_REFUSED, /* section 3.2 `refuse` */
	NTFSMAC_VREASON_BINDING,        /* ADR-020 check failed, no ro fallback possible */
	NTFSMAC_VREASON_MOUNT_POINT,    /* all leaf names taken or mkdirat failed */
	NTFSMAC_VREASON_ARGV_REJECTED,  /* `argv_check()` refused the vector (section 3.3) */
	NTFSMAC_VREASON_SPAWN,          /* worker or volmark did not start */
	NTFSMAC_VREASON_MOUNT_TIMEOUT,  /* the 30 s wait of step 8 */
	NTFSMAC_VREASON_MODE_MISMATCH,  /* ro decision, writable mount (step 8) */
	NTFSMAC_VREASON_UNMOUNT,        /* unmount failed (section 4.5) */
	NTFSMAC_VREASON_COUNT_
} ntfsmac_vol_reason;

/* Mode of the current or planned mount. */
typedef enum ntfsmac_vol_mode {
	NTFSMAC_VMODE_NONE = 0,
	NTFSMAC_VMODE_RO,
	NTFSMAC_VMODE_RW
} ntfsmac_vol_mode;

/*
 * The machine's own record of one volume. It holds no pointer and no string:
 * the daemon keeps the rest (BSD name, descriptors, session) beside it.
 * Not thread-safe: one volume record belongs to one serial queue.
 */
typedef struct ntfsmac_volume {
	ntfsmac_vol_state state;
	ntfsmac_vol_mode planned_mode; /* set by DECIDED; NONE before it */
	ntfsmac_vol_mode mode;         /* set by MOUNTED; kept in Unmounting; NONE otherwise */
	ntfsmac_vol_reason reason;     /* meaningful in Failed, NONE otherwise */
	bool disk_present;             /* false after DISK_GONE */
} ntfsmac_volume;

/*
 * Arguments of an event. Only the fields named in the comments are read.
 * Pass NULL for an event that takes none.
 */
typedef struct ntfsmac_vol_event_arg {
	ntfsmac_vol_mode mode;     /* DECIDED: planned mode (RO or RW). MOUNTED: the
				    * mode the mount really has (from MNT_RDONLY). */
	ntfsmac_vol_reason reason; /* PROBE_FAILED, REFUSED, MOUNT_FAILED,
				    * UNMOUNT_FAILED: why. NONE is replaced by the
				    * default reason of the event. */
} ntfsmac_vol_event_arg;

typedef enum ntfsmac_vol_result {
	NTFSMAC_VRES_OK = 0,
	NTFSMAC_VRES_INVALID_TRANSITION, /* event not allowed in this state; record unchanged */
	NTFSMAC_VRES_BAD_ARG,            /* NULL record, event or state out of range, missing or
					  * wrong mode or reason; record unchanged */
	NTFSMAC_VRES_MODE_MISMATCH       /* MOUNTED with a writable mount for a ro decision;
					  * the caller reports MOUNT_FAILED instead; record unchanged */
} ntfsmac_vol_result;

/* Fresh record: Appeared, disk present, no mode, no reason. */
void ntfsmac_volume_init(ntfsmac_volume *v);

/*
 * The transition table. Returns the next state for `event` in `state` when
 * the disk is (`disk_present` true) or is not present, or
 * NTFSMAC_VOL_STATE_COUNT_ if the event is not allowed there. Pure.
 */
ntfsmac_vol_state ntfsmac_vol_next_state(ntfsmac_vol_state state,
					 ntfsmac_vol_event event,
					 bool disk_present);

/* Apply one event. On anything but NTFSMAC_VRES_OK the record is unchanged. */
ntfsmac_vol_result ntfsmac_volume_apply(ntfsmac_volume *v,
					ntfsmac_vol_event event,
					const ntfsmac_vol_event_arg *arg);

/* True if a `mount` request is accepted now (state Appeared or Failed; section 5). */
bool ntfsmac_volume_accepts_mount(const ntfsmac_volume *v);

const char *ntfsmac_vol_state_name(ntfsmac_vol_state s);
const char *ntfsmac_vol_event_name(ntfsmac_vol_event e);
const char *ntfsmac_vol_result_name(ntfsmac_vol_result r);

/* ------------------------------------------------------------------ */
/* Process spawn and supervision (spawn.c, T-M4-01)                    */
/* ------------------------------------------------------------------ */

/* Handle of one spawned child (probe, volmark or worker). */
typedef struct ntfsmac_child {
	pid_t pid;
	int status_fd; /* -1 if unused */
	/*
	 * Worker only. The user it runs as and its device (the device and inode numbers of the
	 * descriptor it was given), kept after the worker is gone so that
	 * ntfsmac_child_device_check() can look for a process that still holds the device.
	 */
	uid_t owner;
	dev_t dev_dev;
	ino_t dev_ino;
	bool dev_known;
} ntfsmac_child;

typedef struct ntfsmac_probe_run {
	int device_fd;            /* descriptor A, O_RDONLY, O_CLOEXEC */
	unsigned timeout_s;       /* 20 in production (section 4.1 step 4) */
} ntfsmac_probe_run;

/*
 * Run `ntfsmac-probe` as `nobody` with descriptor A and wait. On success
 * `verdict` holds the parsed verdict; any failure, timeout or parse error
 * gives state `unknown` in `verdict` and a non-OK status (section 3.1).
 */
ntfsmac_dmn_status ntfsmac_spawn_probe(const ntfsmac_probe_run *run,
				       ntfsmac_probe_verdict *verdict);

typedef struct ntfsmac_volmark_run {
	int device_fd;            /* descriptor B, O_RDWR */
	uint64_t serial;          /* --expect-serial */
	uint64_t total_sectors;   /* --expect-sectors */
	unsigned timeout_s;       /* 20 before the first write (section 3) */
} ntfsmac_volmark_run;

/* `set` or `clear` of the on-disk marker; the result is volmark's exit code mapped. */
ntfsmac_dmn_status ntfsmac_spawn_volmark(const ntfsmac_volmark_run *run,
					 bool set, int *exit_code);

typedef struct ntfsmac_worker_run {
	int device_fd;            /* A for ro, B for rw */
	int errfd_write;          /* write end of the session channel (section 7) */
	const char *mount_point;  /* absolute, created by mountpoint.c */
	bool read_only;
	uid_t console_uid;
	gid_t console_gid;
	unsigned mount_wait_s;    /* 30 in production (section 4.1 step 8) */
} ntfsmac_worker_run;

/*
 * Build the vector, run `argv_check()`, spawn the worker in its own process
 * group with POSIX_SPAWN_CLOEXEC_DEFAULT, wait for the mount. On the
 * deadline the process group gets SIGKILL and the status is E_TIMEOUT.
 * `mounted_ro` is the MNT_RDONLY flag of the mount that appeared.
 */
ntfsmac_dmn_status ntfsmac_spawn_worker(const ntfsmac_worker_run *run,
					ntfsmac_child *child, bool *mounted_ro);

/* Non-blocking wait on a child. OK with *exited false while it runs. */
ntfsmac_dmn_status ntfsmac_child_poll(ntfsmac_child *child, bool *exited,
				      int *exit_status);
/*
 * SIGKILL to the child's process group and to every descendant of the child, also those that
 * left the group with setpgid() or setsid(). The descendants are found while the child is alive
 * (it and each one found are stopped first, so that none can start another unseen), and the
 * search repeats until a round finds no new one, at most 16 rounds.
 */
ntfsmac_dmn_status ntfsmac_child_kill_group(ntfsmac_child *child);

/*
 * To be called after the worker is gone and before the volume is reported detached: no process
 * of the worker's user may still hold the device. A descendant that left the group and lost its
 * parent cannot be found by walking the tree, but it still holds the descriptor it inherited.
 * The call scans the processes of that user (not of others) for the device, sends SIGKILL to
 * each one that holds it and looks again, 25 times 20 ms at most. `*killed` (may be NULL) counts
 * the processes it killed. ntfsmac_child_poll() does this when it reports an exit, and
 * ntfsmac_spawn_worker() when it gives up on a worker; the daemon calls it once more before the
 * volume is reported detached, with the handle of the worker. The device is the one
 * of the descriptor the last ntfsmac_spawn_worker() call was given, so no new worker of the same
 * volume may be running.
 *   OK      no process holds the device.
 *   BUSY    one still did after the last round.
 *   STATE   the handle has no device (no worker was started with it).
 *   IO      the process table could not be read.
 */
ntfsmac_dmn_status ntfsmac_child_device_check(const ntfsmac_child *child, unsigned *killed);

/* ------------------------------------------------------------------ */
/* Disk Arbitration (da.c, T-M4-02; the only module that may use it)    */
/* ------------------------------------------------------------------ */

/* What the daemon needs to know about a disk. Plain C, no CF types. */
typedef struct ntfsmac_disk_info {
	char bsd_name[32];        /* "disk4s1" */
	uint64_t size_bytes;
	bool whole_disk;
	bool is_ntfs_candidate;   /* passed the filter of section 4.1 step 1 */
	bool has_volume_path;     /* DAVolumePath present: some driver mounted it (ADR-020) */
	char media_path[128];     /* DAMediaPath, for the binding check */
	char volume_kind[16];     /* DAVolumeKind ("ntfs" for Apple's driver), "" if none or odd */
} ntfsmac_disk_info;

typedef struct ntfsmac_da_callbacks {
	void (*disk_appeared)(const ntfsmac_disk_info *disk, void *ctx);
	void (*disk_disappeared)(const char *bsd_name, void *ctx);
	/* Mount of an NTFS disk by anyone (Apple's driver included) was seen. */
	void (*disk_mounted)(const ntfsmac_disk_info *disk, void *ctx);
	/* The disk was unmounted (user eject, `umount`, or our own). */
	void (*disk_unmounted)(const char *bsd_name, void *ctx);
} ntfsmac_da_callbacks;

typedef struct ntfsmac_da ntfsmac_da; /* opaque */

ntfsmac_dmn_status ntfsmac_da_start(const ntfsmac_da_callbacks *cb, void *ctx,
				    ntfsmac_da **out);
void ntfsmac_da_stop(ntfsmac_da *da);
/* Mount-approval dissenter for one disk (section 4.1 step 3) and its withdrawal. */
ntfsmac_dmn_status ntfsmac_da_dissent(ntfsmac_da *da, const char *bsd_name);
ntfsmac_dmn_status ntfsmac_da_withdraw(ntfsmac_da *da, const char *bsd_name);
/* Fresh description, for the binding check (step 6). */
ntfsmac_dmn_status ntfsmac_da_describe(ntfsmac_da *da, const char *bsd_name,
				       ntfsmac_disk_info *out);
ntfsmac_dmn_status ntfsmac_da_unmount(ntfsmac_da *da, const char *bsd_name);
ntfsmac_dmn_status ntfsmac_da_eject(ntfsmac_da *da, const char *bsd_name);

/* ------------------------------------------------------------------ */
/* Mount point and unmount (mountpoint.c, unmount.c, T-M4-03)          */
/* ------------------------------------------------------------------ */

typedef struct ntfsmac_mountpoint {
	int dirfd;                /* the created directory, opened O_DIRECTORY | O_NOFOLLOW */
	dev_t dev;                /* recorded by fstat, section 4.1 step 7 */
	ino_t ino;
	char path[128];           /* "/Volumes/<leaf>", built from the sanitised leaf */
} ntfsmac_mountpoint;

/* `label` is the raw label from the verdict; it goes through the sanitiser first. */
ntfsmac_dmn_status ntfsmac_mountpoint_create(const char *label, size_t label_len,
					     uint64_t serial, uid_t console_uid,
					     gid_t console_gid, ntfsmac_mountpoint *mp);
/* Removes the directory only if device and inode match and it is empty. */
ntfsmac_dmn_status ntfsmac_mountpoint_remove(ntfsmac_mountpoint *mp);

typedef struct ntfsmac_unmount_opts {
	bool force;               /* explicit forced eject or the worker is gone */
	unsigned retries;         /* plain unmount retries on EBUSY */
} ntfsmac_unmount_opts;

/* On E_BUSY, `busy_pid` is a process that holds the volume, or 0 if unknown. */
ntfsmac_dmn_status ntfsmac_unmount_path(const char *mount_point,
					const ntfsmac_unmount_opts *opts,
					pid_t *busy_pid);

/* ------------------------------------------------------------------ */
/* Session channel watcher (errwatch.c pure parser, errwatch_io.c pipe) */
/* ------------------------------------------------------------------ */

#define NTFSMAC_ERRWATCH_LINE_MAX 32
#define NTFSMAC_ERRWATCH_RECORDS_MAX 64
/* End-of-file must arrive this long after the worker exit (section 7). */
#define NTFSMAC_ERRWATCH_EOF_WAIT_MS 2000

typedef enum ntfsmac_errwatch_flag {
	NTFSMAC_EW_SAW_S_RW = 1u << 0,
	NTFSMAC_EW_SAW_S_RO = 1u << 1,
	NTFSMAC_EW_SAW_E = 1u << 2,
	NTFSMAC_EW_SAW_W = 1u << 3,
	NTFSMAC_EW_SAW_U0 = 1u << 4,
	NTFSMAC_EW_PROTOCOL_ERROR = 1u << 5,
	NTFSMAC_EW_EOF = 1u << 6,
	NTFSMAC_EW_SAW_U = 1u << 7 /* any U record, whatever its count */
} ntfsmac_errwatch_flag;

/* Parser state. No allocation; fixed size. */
typedef struct ntfsmac_errwatch {
	uint32_t flags;           /* ntfsmac_errwatch_flag bits */
	unsigned records;
	size_t line_len;
	char line[NTFSMAC_ERRWATCH_LINE_MAX + 1];
} ntfsmac_errwatch;

void ntfsmac_errwatch_init(ntfsmac_errwatch *w);
/*
 * Feed bytes read from the pipe. OK while the stream is within the grammar;
 * E_STATE once a protocol error has happened (now or before): the bytes are
 * discarded and the session can no longer be clean; E_INVAL for bad arguments.
 * A U record with a count above 0 also sets NTFSMAC_EW_SAW_E.
 */
ntfsmac_dmn_status ntfsmac_errwatch_feed(ntfsmac_errwatch *w, const char *buf,
					 size_t len);
/* End of file reached. A line without its newline is a protocol error. */
void ntfsmac_errwatch_eof(ntfsmac_errwatch *w);
/*
 * Positive clean-session proof of section 7 for a rw session. The caller adds
 * the facts the parser cannot know (unmount seen before exit, not re-adopted,
 * exit status 0) through the arguments.
 */
bool ntfsmac_errwatch_clean_proof(const ntfsmac_errwatch *w, bool unmount_seen_first,
				  bool re_adopted, int worker_exit_status);

/*
 * The pipe and the reading (errwatch_io.c). `pipe_open` makes both ends
 * O_CLOEXEC and at least descriptor 3; the read end is non-blocking. The
 * write end goes to ntfsmac_worker_run.errfd_write; the daemon closes its
 * copy after the spawn and keeps the read end open while the worker runs.
 * Pass the worker's exit code as `worker_exit_status`, and a non-zero value
 * (for example -1) when it ended by a signal or was killed.
 */
ntfsmac_dmn_status ntfsmac_errwatch_pipe_open(int *rd, int *wr);
/*
 * Read what is there now (at most 64 KiB per call) into the parser, without
 * blocking. Sets NTFSMAC_EW_EOF at end-of-file. E_IO on a read error.
 */
ntfsmac_dmn_status ntfsmac_errwatch_pump(ntfsmac_errwatch *w, int rd);
/*
 * After the worker exit: pump until end-of-file or `timeout_ms`
 * (NTFSMAC_ERRWATCH_EOF_WAIT_MS). OK at end-of-file; E_TIMEOUT when it did
 * not come (the EOF flag stays unset, so the session is not clean).
 */
ntfsmac_dmn_status ntfsmac_errwatch_drain(ntfsmac_errwatch *w, int rd, unsigned timeout_ms);

/* ------------------------------------------------------------------ */
/* Session lifecycle (session.c, T-M4-14)                              */
/* ------------------------------------------------------------------ */

typedef struct ntfsmac_session ntfsmac_session; /* opaque, one per mounted volume */

/*
 * Start of a read/write session: journal entry, binding, volmark set,
 * ordering of section 3.6. Fills `mode` with the mode that results
 * (`ro` after a volmark failure).
 */
ntfsmac_dmn_status ntfsmac_session_begin(ntfsmac_journal *j, const ntfsmac_probe_verdict *v,
					 ntfsmac_policy_decision decision,
					 ntfsmac_session **out, ntfsmac_vol_mode *mode);
/* End: applies the clean-session proof and journal rules (sections 3.5 and 4.5). */
ntfsmac_dmn_status ntfsmac_session_end(ntfsmac_session *s, const ntfsmac_errwatch *w,
				       bool unmount_seen_first, int worker_exit_status);
void ntfsmac_session_free(ntfsmac_session *s);

/* ------------------------------------------------------------------ */
/* Restart reconciliation (reconcile.c, T-M4-05)                       */
/* ------------------------------------------------------------------ */

/*
 * What reconcile decided for one journal entry (section 7, `ntfsmacd` crash).
 * The decision is pure (reconcile.c): it takes the journal entries and the
 * list of live mounts and gives one item per entry or per mount to act on.
 */
typedef enum ntfsmac_reconcile_action {
	NTFSMAC_RECONCILE_KEEP_ENTRY = 0, /* no live mount: entry stays, next mount is ro */
	NTFSMAC_RECONCILE_READOPT,        /* live worker and mount: re-adopt (never clean) */
	NTFSMAC_RECONCILE_FORCE_UNMOUNT   /* re-adoption impractical */
} ntfsmac_reconcile_action;

/*
 * One live mount, as the platform lister sees it. `mount_point` is the path
 * under /Volumes; it is a name the daemon created, never read from a disk.
 * `worker_pid` is the pid of a live worker that serves this mount, or 0 if
 * none was found (the lister of this task never finds one, see reconcile.c).
 */
typedef struct ntfsmac_live_mount {
	char mount_point[128];   /* same size as ntfsmac_mountpoint.path */
	bool read_only;           /* MNT_RDONLY */
	pid_t worker_pid;
} ntfsmac_live_mount;

/*
 * The callback gets the entry, the action and, for READOPT and
 * FORCE_UNMOUNT, the live mount it applies to (NULL for KEEP_ENTRY). The
 * pointer is valid only during the call. A non-zero return stops the run.
 * (T-M4-05 added the `mount` parameter: without it the daemon cannot know
 * which path to unmount or which worker to adopt.)
 */
typedef int (*ntfsmac_reconcile_cb)(const ntfsmac_journal_entry *e,
				    ntfsmac_reconcile_action action,
				    const ntfsmac_live_mount *mount, void *ctx);

/* One decision of the pure planner. `mount_index` is -1 for KEEP_ENTRY. */
typedef struct ntfsmac_reconcile_item {
	size_t entry_index;
	ntfsmac_reconcile_action action;
	long mount_index;
} ntfsmac_reconcile_item;

/*
 * Pure planner. Every entry gets at least one item. Returns the number of
 * items written; if `cap` is too small it returns 0 and sets *needed (when
 * not NULL) to the capacity that suffices. `items` may be NULL with cap 0
 * to ask for the size: the bound is `n_entries + n_mounts`.
 */
size_t ntfsmac_reconcile_plan(const ntfsmac_journal_entry *entries, size_t n_entries,
			      const ntfsmac_live_mount *mounts, size_t n_mounts,
			      ntfsmac_reconcile_item *items, size_t cap);

/* True if `mount_point` is /Volumes/<leaf> where <leaf> is the entry's label, or
 * the label followed by " 2" to " 99" (section 4.1 step 7). Pure. */
bool ntfsmac_reconcile_mount_matches(const ntfsmac_journal_entry *e, const char *mount_point);

/* Same as ntfsmac_reconcile_run with the live mounts given by the caller (tests). */
ntfsmac_dmn_status ntfsmac_reconcile_run_with(ntfsmac_journal *j,
					      const ntfsmac_live_mount *mounts,
					      size_t n_mounts, ntfsmac_reconcile_cb cb,
					      void *ctx);

/*
 * Platform lister (macOS: getfsstat(MNT_NOWAIT), no call into the served file
 * system). Fills a new array; the caller frees it with free(). Not
 * implemented off macOS (E_NOTIMPL).
 */
ntfsmac_dmn_status ntfsmac_reconcile_list_mounts(ntfsmac_live_mount **out, size_t *n);

/* Read the journal and the live mounts, decide, call `cb` once per item. */
ntfsmac_dmn_status ntfsmac_reconcile_run(ntfsmac_journal *j, ntfsmac_reconcile_cb cb,
					 void *ctx);

/* ------------------------------------------------------------------ */
/* Conflict detection and doctor (conflict.c, doctor.c, T-M4-07)       */
/* ------------------------------------------------------------------ */

/* Fixed bundle directory of third-party file systems on macOS. */
#define NTFSMAC_FS_DIR_DEFAULT "/Library/Filesystems"

/* Kinds of other NTFS drivers. The names shown to users are fixed strings. */
typedef enum ntfsmac_conflict_kind {
	NTFSMAC_CONFLICT_NONE = 0,
	NTFSMAC_CONFLICT_PARAGON,  /* ufsd_NTFS.fs */
	NTFSMAC_CONFLICT_TUXERA,   /* tuxera_ntfs.fs */
	NTFSMAC_CONFLICT_OTHER,    /* any other non-Apple "*ntfs*.fs" bundle */
	NTFSMAC_CONFLICT_KIND_COUNT_
} ntfsmac_conflict_kind;

typedef struct ntfsmac_conflict_report {
	bool passive;             /* another NTFS driver is installed (section 8.4) */
	unsigned drivers_found;   /* count; names are fixed strings, never disk-derived */
	unsigned kind_mask;       /* bit (1u << kind) for each kind found at least once */
} ntfsmac_conflict_report;

/*
 * Pure rule: classify one directory entry name of /Library/Filesystems.
 * Case is ignored. macFUSE, fuse-t, our own names (`ntfsmac*`) and Apple's
 * `ntfs.fs` are not conflicts. NTFSMAC_CONFLICT_NONE for anything else.
 */
ntfsmac_conflict_kind ntfsmac_conflict_classify(const char *name);
const char *ntfsmac_conflict_kind_name(ntfsmac_conflict_kind k);

/*
 * Scan `fs_dir` (a directory of bundles). Counts each entry that is a
 * directory (symbolic links followed) and classifies as a conflict. A
 * missing `fs_dir` means no driver. Any other read error gives E_IO and
 * `passive` true: when the daemon cannot tell, it stays out of the way.
 * `out` is always filled.
 */
ntfsmac_dmn_status ntfsmac_conflict_scan(const char *fs_dir, ntfsmac_conflict_report *out);

/* Scan NTFSMAC_FS_DIR_DEFAULT. */
ntfsmac_dmn_status ntfsmac_conflict_detect(ntfsmac_conflict_report *out);

/* Passive-mode gates (section 8.4); `r` NULL is treated as passive. */
bool ntfsmac_passive_allows_dissent(const ntfsmac_conflict_report *r);
bool ntfsmac_passive_allows_automount(const ntfsmac_conflict_report *r);
/*
 * `ntfsmac mount` in passive mode: OK only when no file system has the
 * volume mounted now (`volume_mounted_by_other`, from DAVolumePath);
 * E_DENIED otherwise. In active mode always OK here.
 */
ntfsmac_dmn_status ntfsmac_passive_check_manual_mount(const ntfsmac_conflict_report *r,
						      bool volume_mounted_by_other);

/* ---- doctor (doctor.c) ---- */

/* Oldest macFUSE the product accepts (ADR-013). */
#define NTFSMAC_MACFUSE_MIN_MAJOR 5
#define NTFSMAC_MACFUSE_MIN_MINOR 5
#define NTFSMAC_MACFUSE_MIN_PATCH 0

typedef struct ntfsmac_version {
	unsigned major, minor, patch;
} ntfsmac_version;

/* Pure: parse "5.5.0", "5.5" or "5.5.1-beta" (text after the digits is ignored). */
bool ntfsmac_version_parse(const char *s, ntfsmac_version *out);
/* Pure: -1, 0 or 1. */
int ntfsmac_version_cmp(const ntfsmac_version *a, const ntfsmac_version *b);
/*
 * Pure: find `<key>KEY</key><string>VALUE</string>` in an XML property list
 * and copy VALUE (at most cap - 1 bytes). False if absent, too long or not
 * XML (a binary plist is not parsed).
 */
bool ntfsmac_plist_string(const char *buf, size_t len, const char *key, char *out,
			  size_t cap);

/* Codes of `doctor` findings. The CLI maps them to text. */
typedef enum ntfsmac_doctor_code {
	NTFSMAC_DOCTOR_MACFUSE_INSTALLED = 1, /* macfuse.fs bundle present */
	NTFSMAC_DOCTOR_MACFUSE_VERSION,       /* value = major * 10000 + minor * 100 + patch */
	NTFSMAC_DOCTOR_MACFUSE_EXTENSION,     /* value = ntfsmac_doctor_ext_state */
	NTFSMAC_DOCTOR_CONFLICT_NONE,         /* no other NTFS driver (ok) */
	NTFSMAC_DOCTOR_CONFLICT_PARAGON,      /* found, passive mode (not ok) */
	NTFSMAC_DOCTOR_CONFLICT_TUXERA,
	NTFSMAC_DOCTOR_CONFLICT_OTHER,
	NTFSMAC_DOCTOR_CONFLICT_UNREADABLE,   /* the bundle directory could not be read */
	NTFSMAC_DOCTOR_JOURNAL_ENTRIES,       /* value = entries; not ok if any (next mount ro) */
	NTFSMAC_DOCTOR_JOURNAL_CORRUPT,       /* value = unreadable entries; not ok if any */
	NTFSMAC_DOCTOR_JOURNAL_CLEARED,       /* value = tombstones kept; always ok */
	NTFSMAC_DOCTOR_JOURNAL_UNREADABLE     /* the journal directory could not be opened */
} ntfsmac_doctor_code;

typedef enum ntfsmac_doctor_ext_state {
	NTFSMAC_EXT_UNKNOWN = 0,   /* could not be determined */
	NTFSMAC_EXT_ENABLED,
	NTFSMAC_EXT_NOT_APPROVED
} ntfsmac_doctor_ext_state;

/* One `doctor` finding: a fixed code, mapped to text by the CLI. */
typedef struct ntfsmac_doctor_finding {
	int code;                 /* ntfsmac_doctor_code */
	bool ok;
	long value;               /* meaning per code above; 0 if none */
} ntfsmac_doctor_finding;

typedef int (*ntfsmac_doctor_cb)(const ntfsmac_doctor_finding *f, void *ctx);

/* Source of the extension state. It needs a system query that is not plain
 * POSIX, so the daemon supplies it (T-M4-08); NULL means unknown. */
typedef ntfsmac_doctor_ext_state (*ntfsmac_doctor_ext_fn)(void *ctx);

typedef struct ntfsmac_doctor_env {
	const char *fs_dir;       /* NULL: NTFSMAC_FS_DIR_DEFAULT */
	const char *journal_dir;  /* NULL: NTFSMAC_JOURNAL_DEFAULT_DIR */
	ntfsmac_doctor_ext_fn ext_fn;
	void *ext_ctx;
} ntfsmac_doctor_env;

/*
 * Run every check and call `cb` once per finding; a non-zero return of `cb`
 * stops the run (still status OK). `env` NULL means all defaults.
 * Checks read only; a missing journal directory is not created.
 */
ntfsmac_dmn_status ntfsmac_doctor_run_env(const ntfsmac_doctor_env *env,
					  ntfsmac_doctor_cb cb, void *ctx);
ntfsmac_dmn_status ntfsmac_doctor_run(ntfsmac_doctor_cb cb, void *ctx);

/* ------------------------------------------------------------------ */
/* Control socket (ctl.c, T-M4-04)                                     */
/* ------------------------------------------------------------------ */

/* Peer identity from LOCAL_PEERCRED and LOCAL_PEERPID, filled by ctl.c. */
typedef struct ntfsmac_ctl_peer {
	uid_t uid;
	pid_t pid;
	bool console_user;
	bool admin;
	/* LOCAL_PEERTOKEN (audit_token_t, 8 words). The signature check (ADR-019
	 * check 3) uses it, not the pid, so a reused pid cannot pass. */
	bool has_audit_token;
	uint32_t audit_token[8];
} ntfsmac_ctl_peer;

/*
 * The daemon core implements these; ctl.c authenticates the peer, parses one
 * request line and calls the matching function. Each writes one JSON reply
 * line into `reply` (at most `cap` bytes, NUL terminated) and returns the
 * status. `journal_clear` and `repair` are reached only after the checks of
 * section 3.4 (ADR-019).
 */
typedef struct ntfsmac_ctl_ops {
	ntfsmac_dmn_status (*list)(void *ctx, char *reply, size_t cap);
	ntfsmac_dmn_status (*status)(void *ctx, const char *bsd, char *reply, size_t cap);
	ntfsmac_dmn_status (*mount)(void *ctx, const char *bsd, bool read_only, char *reply,
				    size_t cap);
	ntfsmac_dmn_status (*unmount)(void *ctx, const char *bsd, bool force, char *reply,
				      size_t cap);
	ntfsmac_dmn_status (*doctor)(void *ctx, char *reply, size_t cap);
	/*
	 * Called only after checks 1 to 4 of ADR-019 passed. Check 5 (volume gate,
	 * both confirmation fields match) belongs to the implementation, so it
	 * receives the typed fields: `confirm_label` is UTF-8, `confirm_serial` is
	 * exactly 8 upper-case hex digits (both already validated for shape).
	 */
	ntfsmac_dmn_status (*journal_clear)(void *ctx, const ntfsmac_ctl_peer *peer,
					    const char *bsd, const char *confirm_label,
					    const char *confirm_serial, char *reply, size_t cap);
	/* `action` is exactly "clear_dirty" or "sync_mirror". Same rules as above. */
	ntfsmac_dmn_status (*repair)(void *ctx, const ntfsmac_ctl_peer *peer, const char *bsd,
				     const char *action, const char *confirm_label,
				     const char *confirm_serial, char *reply, size_t cap);
	/*
	 * Check 2 of ADR-019: true if `pid` is a live probe, volmark or worker of
	 * this daemon. Every spawn runs in its own process group, so the pid is
	 * also the process group id of that child. NULL means "cannot tell": the
	 * privileged requests are then refused (fail closed).
	 */
	bool (*is_spawned)(void *ctx, pid_t pid);
} ntfsmac_ctl_ops;

typedef struct ntfsmac_ctl ntfsmac_ctl; /* opaque */

ntfsmac_dmn_status ntfsmac_ctl_start(const char *socket_path, const ntfsmac_ctl_ops *ops,
				     void *ctx, ntfsmac_ctl **out);
void ntfsmac_ctl_stop(ntfsmac_ctl *c);
/*
 * Handle one request line (no trailing newline). Split from the socket code
 * so that tests and the fuzz loop can call it without a socket. Never
 * crashes on malformed input; a bad request gives an error reply.
 */
ntfsmac_dmn_status ntfsmac_ctl_handle_line(const ntfsmac_ctl_ops *ops, void *ctx,
					   const ntfsmac_ctl_peer *peer, const char *line,
					   size_t len, char *reply, size_t cap);

/* ------------------------------------------------------------------ */
/* Notifications (notify.c, T-M4-08)                                   */
/* ------------------------------------------------------------------ */

typedef enum ntfsmac_notify_kind {
	NTFSMAC_NOTIFY_MOUNTED_RW = 0,
	NTFSMAC_NOTIFY_MOUNTED_RO,
	NTFSMAC_NOTIFY_EJECTED_CLEAN,
	NTFSMAC_NOTIFY_EJECTED_NO_FLUSH_CONFIRM,
	NTFSMAC_NOTIFY_SESSION_UNCLEAN,
	NTFSMAC_NOTIFY_WRITE_ERROR,
	NTFSMAC_NOTIFY_REFUSED,
	NTFSMAC_NOTIFY_VOLMARK_INTERRUPTED,
	NTFSMAC_NOTIFY_MIRROR_SPLIT
} ntfsmac_notify_kind;

/* `label` is the sanitised label only; `reason` is a fixed daemon text, not disk-derived. */
ntfsmac_dmn_status ntfsmac_notify_post(ntfsmac_notify_kind kind, const char *label,
				       const char *reason);

#ifdef __cplusplus
}
#endif

#endif /* NTFSMAC_DAEMON_INTERNAL_H */
