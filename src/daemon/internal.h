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
	NTFSMAC_VREASON_STRAY_MOUNT,    /* the worker mounted another file system (T-M4-08, H1) */
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
	/*
	 * Worker only. How the start chain ended when ntfsmac_spawn_worker() returned E_IO because
	 * the worker exited before the mount appeared (it was reaped there). The chain is exec all the
	 * way (launchctl asuser, --exec-as, sandbox-exec), so the status is the worker's own.
	 * UNKNOWN for every other result, and when the process could not be reaped.
	 */
	int exit_kind; /* ntfsmac_child_exit_kind */
	int exit_code; /* the exit code, or the signal number */
} ntfsmac_child;

typedef enum ntfsmac_child_exit_kind {
	NTFSMAC_CHILD_EXIT_UNKNOWN = 0,
	NTFSMAC_CHILD_EXIT_CODE,
	NTFSMAC_CHILD_EXIT_SIGNAL
} ntfsmac_child_exit_kind;

/* ntfs-3g exits with this when fuse_mount() is refused (NTFS_VOLUME_FUSE_ERROR). */
#define NTFSMAC_WORKER_EXIT_FUSE_REFUSED 21

/* "exited with status 21", "was killed by signal 9" or "ended with an unknown status". */
void ntfsmac_child_exit_text(const ntfsmac_child *child, char *buf, size_t cap);

/*
 * Is this start failure the one macFUSE refusal that is worth one more try? Only when the chain
 * ended on its own before the mount (E_IO, never E_TIMEOUT or E_DENIED), was reaped (no process
 * left), and the worker's own exit status is NTFSMAC_WORKER_EXIT_FUSE_REFUSED. The caller adds
 * the facts only it knows (no S record, nothing mounted, the device free, no stray mount).
 */
bool ntfsmac_spawn_retry_wanted(ntfsmac_dmn_status st, const ntfsmac_child *child);

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

/*
 * `set` or `clear` of the on-disk marker; the result is volmark's exit code mapped.
 * `*exit_code` is volmark's exit code, or -1 (timeout before `w`: nothing was written), -2
 * (timeout after `w`: a write may have been cut, ARCHITECTURE.md section 3.6), -3 (not run) or -4
 * (status E_BUSY: SIGKILL was sent and the child could not be reaped, so it may still hold the
 * device: the caller must not go on with this descriptor).
 */
ntfsmac_dmn_status ntfsmac_spawn_volmark(const ntfsmac_volmark_run *run,
					 bool set, int *exit_code);

/*
 * The volmark commands: the three marker commands of section 3.6 (T-M4-14 adds `clear-repair`,
 * ADR-017) and the stale-mirror repair `sync-mirror` of section 3.7 (T-M4-16, ADR-021).
 */
typedef enum ntfsmac_volmark_op {
	NTFSMAC_VOLMARK_SET = 0,
	NTFSMAC_VOLMARK_CLEAR,
	NTFSMAC_VOLMARK_CLEAR_REPAIR,
	NTFSMAC_VOLMARK_SYNC_MIRROR
} ntfsmac_volmark_op;
ntfsmac_dmn_status ntfsmac_spawn_volmark_op(const ntfsmac_volmark_run *run,
					    ntfsmac_volmark_op op, int *exit_code);

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

/*
 * `ntfsmacd --exec-as UID GID PROGRAM [ARG...]`: the step of the worker's start that runs inside
 * the console user's bootstrap context (`launchctl asuser`): drop from root to UID and GID, then
 * exec PROGRAM. Returns only when it fails: 2 bad arguments, 126 not root or the drop failed, 127
 * the exec failed.
 */
#define NTFSMAC_EXEC_AS_FLAG "--exec-as"
int ntfsmac_spawn_exec_as(int argc, char **argv);
/* Passive disk I/O for this process and what it starts (docs/spikes/M5-perf.md). 0 or -1 and errno. */
int ntfsmac_spawn_io_passive(void);

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
 * The call scans the processes of that user (not of others) for the device and looks again, 25
 * times 20 ms at most. It sends SIGKILL only to a leftover of the worker: a process running the
 * worker binary or the macFUSE helper (same device and inode as cfg.worker_path or
 * NTFSMAC_PATH_MACFUSE_HELPER), or, while the worker is alive, the worker and its descendants.
 * Every other holder (lsof, diskutil, a backup agent, a process whose executable cannot be read,
 * a leftover of a worker binary that was replaced since) is not killed and is logged at warning
 * level with pid and executable path, but it still counts as holding the device. `*killed` (may
 * be NULL) counts the processes it killed. ntfsmac_child_poll() does this when it reports an
 * exit, and ntfsmac_spawn_worker() when it gives up on a worker (both ignore the result); the
 * daemon calls it once more before the volume is reported detached, with the handle of the
 * worker, and a non-OK result there makes the session unclean (state write_error; the entry and
 * the marker stay, so the next attach is read-only). The device is the one of the descriptor the
 * last ntfsmac_spawn_worker() call was given, so no new worker of the same volume may be running.
 *   OK      no process holds the device.
 *   BUSY    a process still did after the last round (killed leftover or not).
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
	char volume_path[128];    /* DAVolumePath as a file system path, "" if none or too long */
} ntfsmac_disk_info;

typedef struct ntfsmac_da_callbacks {
	void (*disk_appeared)(const ntfsmac_disk_info *disk, void *ctx);
	void (*disk_disappeared)(const char *bsd_name, void *ctx);
	/* Mount of an NTFS disk by anyone (Apple's driver included) was seen. */
	void (*disk_mounted)(const ntfsmac_disk_info *disk, void *ctx);
	/* The disk was unmounted (user eject, `umount`, or our own). */
	void (*disk_unmounted)(const char *bsd_name, void *ctx);
	/*
	 * macFUSE's own disk of a mount (kind "macfuse-local") disappeared: the mount at `mount_path`
	 * was taken down through the file system layer, by `umount`, Finder eject or our own
	 * unmount. It does not happen when the FSKit module process is killed (T-M5-05). May be NULL.
	 */
	void (*fuse_volume_gone)(const char *bsd_name, void *ctx);
	/* A macFUSE disk got a volume path (the mount): the BSD name and the path, to bind it. */
	void (*fuse_volume_mounted)(const char *bsd_name, const char *mount_path, void *ctx);
} ntfsmac_da_callbacks;

typedef struct ntfsmac_da ntfsmac_da; /* opaque */

ntfsmac_dmn_status ntfsmac_da_start(const ntfsmac_da_callbacks *cb, void *ctx,
				    ntfsmac_da **out);
void ntfsmac_da_stop(ntfsmac_da *da);
/* Mount-approval dissenter for one disk (section 4.1 step 3) and its withdrawal. */
ntfsmac_dmn_status ntfsmac_da_dissent(ntfsmac_da *da, const char *bsd_name);
ntfsmac_dmn_status ntfsmac_da_withdraw(ntfsmac_da *da, const char *bsd_name);
/* True if two file system paths name the same thing under Unicode normalisation (NFC/NFD). */
bool ntfsmac_da_same_path(const char *a, const char *b);
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

/*
 * Rule 3 of the clean-session proof (ARCHITECTURE section 7): the daemon saw the unmount before
 * the worker exited. A mount that is merely gone does not count (a killed macFUSE module ends the
 * mount the same way, T-M5-05): the daemon must also have requested the unmount itself, or
 * Disk Arbitration must have reported macFUSE's disk of the volume gone.
 */
bool ntfsmac_unmount_seen(bool mount_gone, bool daemon_requested, bool da_volume_gone);

/*
 * What one session knows about its unmount (unmount_ev.c, pure; the caller locks). `requested`:
 * the daemon's own unmount call returned OK and no mount was left (never on a timeout or busy).
 * `fuse_bsd`: macFUSE's disk of THIS session's mount, bound when a `macfuse-local` disk gets the
 * session's mount point as its volume path while the session is current. `da_seen`: that disk
 * disappeared while the session was still current.
 */
typedef struct ntfsmac_unmount_ev {
	uint64_t session_id; /* 0: no session */
	bool requested;
	bool da_seen;
	char fuse_bsd[32];
} ntfsmac_unmount_ev;

void ntfsmac_uev_begin(ntfsmac_unmount_ev *e, uint64_t session_id); /* attach */
void ntfsmac_uev_reset(ntfsmac_unmount_ev *e);                       /* teardown */
/* After a daemon unmount call: sets `requested` only for OK with no mount left, else clears it. */
void ntfsmac_uev_note_unmount_call(ntfsmac_unmount_ev *e, ntfsmac_dmn_status st, bool still_mounted);
void ntfsmac_uev_unmount_failed(ntfsmac_unmount_ev *e); /* the UNMOUNT_BUSY path */
/* False (nothing bound) unless `current_session` is the session of `e` and is not 0. */
bool ntfsmac_uev_bind(ntfsmac_unmount_ev *e, uint64_t current_session, const char *bsd);
/* True and `da_seen` set only for the bound BSD name while the session is current. */
bool ntfsmac_uev_da_gone(ntfsmac_unmount_ev *e, uint64_t current_session, const char *bsd);
bool ntfsmac_uev_have(const ntfsmac_unmount_ev *e);
/* The bounded wait after a worker exit: keep waiting while nothing is known. */
bool ntfsmac_uev_wait_more(const ntfsmac_unmount_ev *e, unsigned waited_ms, unsigned max_ms,
			   bool stopping);

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

/*
 * One session is one mount of one volume, from the journal entry to the removal of the entry
 * (ARCHITECTURE.md sections 3.5, 3.6, 4.1 steps 6 and 9, 4.5). The module owns the device
 * descriptor of the session: begin() takes over descriptor A (the probe's, O_RDONLY) and hands
 * back the descriptor the worker gets, end() closes it. All system facts come through
 * ntfsmac_session_sys, so every decision is tested on the host with fakes; the real one
 * (ntfsmac_session_sys_real_init) opens device nodes by BSD name and runs the real volmark.
 *
 * How T-M4-08 calls it, in this order, on the volume's serial queue:
 *   1. step 4: open A (O_RDONLY), read the first 4096 bytes through A into `block0`, run the
 *      probe (ntfsmac_spawn_probe), note the DAMediaPath (ntfsmac_disk_info.media_path);
 *   2. step 5: ntfsmac_session_decide() (journal lookup, evidence rule, policy table);
 *   3. step 6 and the marker: ntfsmac_session_begin(); on NTFSMAC_DMN_E_DENIED the volume is
 *      `Failed` with the reason in `res->failed`; otherwise spawn the worker with
 *      ntfsmac_session_device_fd() and `res->mode` (never the decision: the session may have
 *      fallen back to ro);
 *   4. while it runs: when the errwatch parser reports an `E` record or a protocol error, call
 *      ntfsmac_session_note_write_error();
 *   5. step 3 of 4.5: ntfsmac_session_end() after the worker exit and ntfsmac_errwatch_drain();
 *      show the notification that matches `res->report`, then ntfsmac_session_free().
 * A daemon restart re-adopts a session without a ntfsmac_session object: it can never be
 * proven clean, so the entry and the marker stay (section 7).
 */

#define NTFSMAC_BLOCK0_BYTES 4096u /* the bytes of section 4.1 step 4 that bind the device */

/* Identity of an open device: st_rdev of a device node, or st_dev and st_ino of a file. */
typedef struct ntfsmac_dev_id {
	uint64_t a;
	uint64_t b;
} ntfsmac_dev_id;

typedef struct ntfsmac_session_sys {
	void *ctx;
	/* Open the buffered node /dev/<bsd> (never /dev/rdisk*) with O_CLOEXEC; fd or -1. */
	int (*open_device)(void *ctx, const char *bsd, bool writable);
	void (*close_fd)(void *ctx, int fd);
	/* Identity of the open descriptor; 0 on success. */
	int (*dev_identity)(void *ctx, int fd, ntfsmac_dev_id *out);
	/* Read the first NTFSMAC_BLOCK0_BYTES bytes through `fd` (offset 0); 0 on success. */
	int (*read_block0)(void *ctx, int fd, uint8_t *buf);
	/* F_SETFL reset of the status flags of descriptor A after the probe; 0 on success. */
	int (*reset_flags)(void *ctx, int fd);
	/* Fresh Disk Arbitration description of the disk (the "no other mount" part of the binding). */
	ntfsmac_dmn_status (*describe)(void *ctx, const char *bsd, ntfsmac_disk_info *out);
	/* ntfsmac_spawn_volmark_op() with the same meaning of `*exit_code`. */
	ntfsmac_dmn_status (*volmark)(void *ctx, const ntfsmac_volmark_run *run,
				      ntfsmac_volmark_op op, int *exit_code);
	/* Section 4.5 step 3: fsync, then the drive flush. OK iff fsync worked; *flush_ok = flush. */
	ntfsmac_dmn_status (*sync_device)(void *ctx, int fd, bool *flush_ok);
	int64_t (*now)(void *ctx);             /* seconds since the epoch */
	uint64_t (*new_session_id)(void *ctx); /* non-zero, unique per read/write session */
} ntfsmac_session_sys;

typedef struct ntfsmac_session_sys_real {
	ntfsmac_session_sys sys;
	ntfsmac_da *da; /* for describe(); NULL: describe fails, so every binding fails closed */
} ntfsmac_session_sys_real;
void ntfsmac_session_sys_real_init(ntfsmac_session_sys_real *r, ntfsmac_da *da);

/* Configuration of the daemon that the session code reads. */
typedef struct ntfsmac_session_config {
	bool automount_ro;          /* `automount_mode=ro`: attaches are ro, no journal entry, no volmark */
	bool evidence_rule_enabled; /* section 3.5 rule 2; false until U12 is resolved */
#if defined(NTFSMAC_TEST_SEAMS)
	bool marker_disabled;       /* test seam: marker=none, volmark is not run (absent in `product`) */
#endif
	unsigned volmark_timeout_s; /* 0 means 20 */
} ntfsmac_session_config;

/*
 * The policy input mode of an attach (section 3.2 request column). `automatic` is an attach
 * seen by Disk Arbitration; a `mount` request from the CLI is not. With automount_mode=ro an
 * automatic attach is `ro` (the user then unmounts and runs `ntfsmac mount` for rw, section
 * 3.6); a request is `ro` only when it asked for it. Pure.
 */
ntfsmac_policy_mode ntfsmac_session_policy_mode(const ntfsmac_session_config *cfg,
						bool automatic, bool request_ro);

typedef struct ntfsmac_session_decision {
	ntfsmac_policy_result policy;
	bool evidence_removed; /* path 2 fired now: the entry went to the tombstones; stays ro */
	bool entry_corrupt;    /* the entry could not be read: treated as an entry that blocks rw */
} ntfsmac_session_decision;

/*
 * Section 4.1 step 5: look up the entry for the verdict's serial (ntfsmac_journal_get), let
 * ntfsmac_policy_decide() decide, and apply the journal action `remove` (rule 2, evidence of an
 * external check) through ntfsmac_journal_remove_if(), unless `session_live` (may be NULL) says
 * the entry's session is live. A missing entry is no entry; an entry that
 * is corrupt or cannot be read stands in as an entry with marker `failed` and state
 * `write_error`, so that it blocks rw and rule 2 (section 3.5). A removal that fails leaves the
 * entry in place. The decision is never rw while an entry exists.
 */
ntfsmac_dmn_status ntfsmac_session_decide(const ntfsmac_session_config *cfg, ntfsmac_journal *j,
					  const ntfsmac_probe_verdict *v, bool automatic,
					  bool request_ro, int64_t now,
					  bool (*session_live)(void *, uint64_t), void *live_ctx,
					  ntfsmac_session_decision *out);

/*
 * Remove the entry for expected->serial only if it is still the one `expected` describes (same
 * session_id and created; a stand-in with both 0 matches a corrupt entry). ERR_PATH_CONDITION
 * when another entry is there now, NOT_FOUND when none. Serialised with the entry creation of
 * ntfsmac_session_begin(): an entry that appeared after the caller looked is never removed.
 * Every removal that follows a gate or a slow step uses this, not ntfsmac_journal_remove().
 */
ntfsmac_journal_status ntfsmac_journal_remove_if(ntfsmac_journal *j,
						 const ntfsmac_journal_entry *expected,
						 ntfsmac_journal_removal path, int64_t now);

/* Binding (ADR-020), pure parts. */
typedef enum ntfsmac_bind_result {
	NTFSMAC_BIND_OK = 0,
	NTFSMAC_BIND_APPLE_MOUNTED, /* only Apple's read-only driver has the disk: ro */
	NTFSMAC_BIND_OTHER_MOUNTED, /* another file system has it: Failed */
	NTFSMAC_BIND_DISK_CHANGED,  /* not the object the probe ran on (BSD name, DAMediaPath) */
	NTFSMAC_BIND_NO_ANSWER      /* the description could not be had: fail closed */
} ntfsmac_bind_result;
ntfsmac_bind_result ntfsmac_bind_check_disk(const char *probed_bsd, const char *probed_media_path,
					    const ntfsmac_disk_info *now,
					    ntfsmac_dmn_status describe_status);
bool ntfsmac_bind_same_device(const ntfsmac_dev_id *a, const ntfsmac_dev_id *b);
const char *ntfsmac_bind_result_name(ntfsmac_bind_result r);

/* Clear O_NONBLOCK, O_APPEND and O_ASYNC on `fd`; F_SETFL only when one is set (device nodes refuse it). */
int ntfsmac_fd_reset_flags(int fd);

typedef struct ntfsmac_session ntfsmac_session; /* opaque, one per mounted volume */

/* Everything the attach flow knows at step 6. */
typedef struct ntfsmac_session_begin_in {
	const char *bsd;                   /* "disk4s1"; validated again here */
	const ntfsmac_probe_verdict *verdict;
	ntfsmac_policy_decision decision;  /* RW or RO; REFUSE is E_INVAL */
	int probe_fd;                      /* descriptor A; the session owns it from the call on */
	uint8_t block0[NTFSMAC_BLOCK0_BYTES]; /* first 4096 bytes read through A before the probe ran */
	char media_path[128];              /* DAMediaPath of the disk at probe time */
} ntfsmac_session_begin_in;

/* Why the session is ro although rw was decided. Fixed texts only. */
typedef enum ntfsmac_session_reason {
	NTFSMAC_SREASON_NONE = 0,          /* rw as decided, or ro as decided */
	NTFSMAC_SREASON_OTHER_DRIVER,      /* Apple's driver has the disk mounted: ro */
	NTFSMAC_SREASON_JOURNAL,           /* the entry could not be created (or exists): ro */
	NTFSMAC_SREASON_BINDING,           /* the writable descriptor did not match the probe: ro, nothing was written */
	NTFSMAC_SREASON_MARKER_FAILED,     /* "could not mark the volume; mounted read-only" */
	NTFSMAC_SREASON_MARKER_INTERRUPTED, /* the same, and a write may have been cut (section 3.6) */
	NTFSMAC_SREASON_BINDING_AFTER_MARK  /* sector 0 differs after volmark: the flag may be on the disk
					     * (repair --clear-dirty applies), unlike BINDING */
} ntfsmac_session_reason;

typedef struct ntfsmac_session_start {
	ntfsmac_vol_mode mode;     /* the mode the worker gets: RO or RW; NONE on E_DENIED */
	ntfsmac_session_reason reason;
	ntfsmac_vol_reason failed; /* on E_DENIED: NTFSMAC_VREASON_BINDING, or _SPAWN when volmark
				    * could not be reaped after SIGKILL */
	uint64_t session_id;       /* 0 for a ro session without an entry */
} ntfsmac_session_start;

/*
 * Section 4.1 step 6 and section 3.6 "Ordering in the daemon", steps 1 and 2. For rw:
 *   journal entry (marker pending), synced
 *   -> open B (O_RDWR) while A is open; identity(A) == identity(B) and the first 4096 bytes
 *      through B equal block0; only then close A
 *   -> volmark set (--expect-serial/--expect-sectors from the verdict; 20 s until `w`, then up
 *      to 120 s, enforced by the spawn module); exit 0
 *   -> first 4096 bytes through B equal block0 again
 *   -> journal marker=set, synced
 * Any failure on that path closes B, records marker=failed where the entry exists, opens a fresh
 * O_RDONLY descriptor, binds it again (identity, bytes, Disk Arbitration) and returns mode RO
 * with the reason; no worker ever writes in that attach. Before any of this the Disk Arbitration
 * description must show the same disk (BSD name and DAMediaPath) with no mount path; Apple's
 * read-only mount gives ro, another mount gives E_DENIED. For ro the session uses A itself after
 * resetting its flags and checking the first 4096 bytes again; no entry, no volmark.
 * Returns OK with `*out` set (also for a ro fallback), E_DENIED (`res->failed`) or E_INVAL.
 * On any return the probe descriptor is no longer the caller's: it is closed or owned by `*out`.
 */
ntfsmac_dmn_status ntfsmac_session_begin(const ntfsmac_session_sys *sys,
					 const ntfsmac_session_config *cfg, ntfsmac_journal *j,
					 const ntfsmac_session_begin_in *in,
					 ntfsmac_session **out, ntfsmac_session_start *res);

int ntfsmac_session_device_fd(const ntfsmac_session *s); /* the descriptor for the worker */
ntfsmac_vol_mode ntfsmac_session_mode(const ntfsmac_session *s);
uint64_t ntfsmac_session_id(const ntfsmac_session *s);   /* 0 without an entry (ro) */
/* True while the session holds a journal entry (a rw session): for the live-session gate. */
bool ntfsmac_session_owns_entry(const ntfsmac_session *s);

/*
 * First `E` record, or a broken channel (section 7): journal state=write_error, synced.
 * Idempotent; a ro session has no entry and does nothing.
 */
ntfsmac_dmn_status ntfsmac_session_note_write_error(ntfsmac_session *s);

typedef enum ntfsmac_session_report {
	NTFSMAC_SREPORT_RO_CLOSED = 0,  /* ro session: nothing to settle */
	NTFSMAC_SREPORT_CLEAN,          /* "ejected, all data written" */
	NTFSMAC_SREPORT_CLEAN_NO_FLUSH, /* clean, but the drive did not confirm the flush (W or flush failed) */
	NTFSMAC_SREPORT_UNCLEAN,        /* no positive proof, or a failed device sync: entry and marker stay */
	NTFSMAC_SREPORT_CLEAR_FAILED    /* proof held, volmark clear failed: marker stays set, run chkdsk */
} ntfsmac_session_report;

typedef struct ntfsmac_session_end_result {
	ntfsmac_session_report report;
	bool entry_removed;
	bool clear_interrupted; /* volmark clear was cut after `w`: the residual text of section 3.6 */
} ntfsmac_session_end_result;

/*
 * Section 4.5 step 3. A rw session is clean only if all hold: the positive proof of section 7
 * (ntfsmac_errwatch_clean_proof, never re-adopted), the journal state is `open`, the daemon's
 * device sync (`fsync`) succeeded, and `volmark clear` exited 0 (skipped when marker=none).
 * Then the entry is removed (path 1). Anything less leaves the entry and the flag in place
 * ("silence is never success"). An `E` record or a protocol error also records state=write_error.
 * `force_unclean` (a stray mount, or a holder of the device that could not be cleared) is a hard
 * "not clean": the report is UNCLEAN and the entry and marker stay, whatever the proof and the
 * state of the journal entry say (also if recording state=write_error failed).
 * Closes the descriptor. The session stays allocated until ntfsmac_session_free().
 */
ntfsmac_dmn_status ntfsmac_session_end(ntfsmac_session *s, const ntfsmac_errwatch *w,
				       bool unmount_seen_first, int worker_exit_status, bool force_unclean,
				       ntfsmac_session_end_result *res);
void ntfsmac_session_free(ntfsmac_session *s); /* closes the descriptor if end() did not */

/*
 * A bound marker write outside a session (ADR-017 `repair --clear-dirty`). `in` is what was
 * captured before the gate probe ran on descriptor A (bsd, verdict, probe_fd, block0 read before
 * the probe, DAMediaPath), exactly as for ntfsmac_session_begin(); the call takes over probe_fd
 * and closes it. The fresh Disk Arbitration description must show the same disk and no mount
 * (Apple's included: the gate needs the volume unmounted), B (O_RDWR) must be the same device
 * with the same first 4096 bytes as before the probe, A is closed, volmark `op` runs on B with
 * the serial and sector count of the verdict, the first 4096 bytes are compared again and B is
 * closed. Volmark itself checks serial, sector count and flags (exits 15, 13) as well.
 * OK: volmark exited 0. E_DENIED: binding failed, nothing was written. E_IO or E_TIMEOUT:
 * volmark failed, `*exit_code` as in ntfsmac_spawn_volmark_op(). E_IO with exit code 0: sector 0
 * changed during the run.
 */
ntfsmac_dmn_status ntfsmac_session_marker_write(const ntfsmac_session_sys *sys,
						const ntfsmac_session_begin_in *in,
						ntfsmac_volmark_op op, unsigned timeout_s,
						int *exit_code);

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
	NTFSMAC_DOCTOR_JOURNAL_UNREADABLE,    /* the journal directory could not be opened */
	NTFSMAC_DOCTOR_NOTIFY_DELIVERY        /* value = ntfsmac_banner_value(); ok when no banner yet */
} ntfsmac_doctor_code;

typedef enum ntfsmac_doctor_ext_state {
	NTFSMAC_EXT_UNKNOWN = 0,   /* could not be determined */
	NTFSMAC_EXT_ENABLED,
	NTFSMAC_EXT_NOT_APPROVED
} ntfsmac_doctor_ext_state;

/*
 * Reads the answer of `pluginkit -m -v -i io.macfuse.app.fsmodule.macfuse-local` (T-M4-07, section
 * 8.4). `buf` is the whole output; more than NTFSMAC_EXT_OUT_MAX bytes, a byte outside printable
 * ASCII, tab and newline, a line that is not the module line or the "(1 plug-in)" trailer, or a
 * module line that is not the macFUSE bundle: NTFSMAC_EXT_UNKNOWN. The election mark in column 0
 * decides: '+' enabled, '-' or blank (ignored, or never elected) not approved.
 */
#define NTFSMAC_EXT_OUT_MAX 4096u
#define NTFSMAC_EXT_MODULE_ID "io.macfuse.app.fsmodule.macfuse-local"
ntfsmac_doctor_ext_state ntfsmac_doctor_ext_parse(const char *buf, size_t len);

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
/* Privileged requests (ctl_journal.c, T-M4-14)                        */
/* ------------------------------------------------------------------ */

/*
 * `journal_clear` and `repair --clear-dirty` behind the five checks of ADR-019. Checks 1 to 4
 * are done by ctl.c before these functions run (and only on the XPC door); check 5, the
 * policy gates (section 3.2) and the typed label and serial, is done here. The fresh probe and
 * the writes are the daemon's, so they come in through ntfsmac_priv_sys. The functions fit
 * ntfsmac_ctl_ops.journal_clear and .repair with `ctx` pointing to a ntfsmac_priv; T-M4-08's
 * ops table calls them from small wrappers, on the volume's queue or under the daemon's lock
 * for the volume registry (they serialise among themselves with one mutex and nothing more).
 * A repair `sync_mirror` is handed to ntfsmac_priv_repair_mirror() (ctl_repair_mirror.c, T-M4-16).
 */
typedef struct ntfsmac_priv_volume {
	bool mounted;    /* Mounting, Mounted or Unmounting, in any mode */
	bool mounted_rw; /* mounted read/write now */
} ntfsmac_priv_volume;

/*
 * What a fresh probe leaves behind for the write that may follow (section 4.1 steps 4 and 6):
 * descriptor A, still open, the first 4096 bytes read through it before the probe ran, and the
 * DAMediaPath of the disk at that time.
 */
typedef struct ntfsmac_priv_bind {
	int fd; /* -1 when nothing is held */
	uint8_t block0[NTFSMAC_BLOCK0_BYTES];
	char media_path[128];
} ntfsmac_priv_bind;

typedef struct ntfsmac_priv_sys {
	void *ctx;
	/* The daemon's record of `bsd`. false: no such volume. */
	bool (*volume)(void *ctx, const char *bsd, ntfsmac_priv_volume *out);
	/*
	 * A probe run for this request on the volume: open A (O_RDONLY), read block 0, note the
	 * DAMediaPath, run ntfsmac_spawn_probe() on A. A stays open in `bind` (fd -1 on failure).
	 */
	ntfsmac_dmn_status (*probe_fresh)(void *ctx, const char *bsd, ntfsmac_probe_verdict *out,
					  ntfsmac_priv_bind *bind);
	/* Close the descriptor in `bind` and set it to -1 (every path that does not write). May be NULL. */
	void (*bind_release)(void *ctx, ntfsmac_priv_bind *bind);
	/* Is a volume, on any BSD name, Mounting or Mounted with this journal session id? */
	bool (*session_live)(void *ctx, uint64_t session_id);
	/*
	 * Bound `volmark clear-repair` (ntfsmac_session_marker_write) on the descriptor in `bind`,
	 * which it takes over and closes. *exit_code as there.
	 */
	ntfsmac_dmn_status (*clear_dirty)(void *ctx, const char *bsd, const ntfsmac_probe_verdict *v,
					  ntfsmac_priv_bind *bind, int *exit_code);
	/*
	 * Bound `volmark sync-mirror` (T-M4-16, same contract as clear_dirty). NULL: the request is
	 * answered NOTIMPL and nothing is probed.
	 */
	ntfsmac_dmn_status (*sync_mirror)(void *ctx, const char *bsd, const ntfsmac_probe_verdict *v,
					  ntfsmac_priv_bind *bind, int *exit_code);
	int64_t (*now)(void *ctx);
} ntfsmac_priv_sys;

typedef struct ntfsmac_priv {
	const ntfsmac_priv_sys *sys;
	ntfsmac_journal *journal;
} ntfsmac_priv;

ntfsmac_dmn_status ntfsmac_priv_journal_clear(void *priv, const ntfsmac_ctl_peer *peer,
					      const char *bsd, const char *confirm_label,
					      const char *confirm_serial, char *reply, size_t cap);
ntfsmac_dmn_status ntfsmac_priv_repair(void *priv, const ntfsmac_ctl_peer *peer, const char *bsd,
				       const char *action, const char *confirm_label,
				       const char *confirm_serial, char *reply, size_t cap);

/*
 * The `journal` member that the `status` reply carries for a volume that has a journal entry
 * (T-M4-08 puts it into the reply), so that the CLI can show the specific warning of ADR-017
 * BEFORE it asks for the typed confirmation and the password:
 *   "journal":{"marker":"set","state":"write_error","warning":"write_error","left_open":"2026-11-17T08:00:00Z"}
 * `warning` is none, write_error or lost_flush (ntfsmac_policy_journal_clear_warning(), with the
 * last probe of the volume, which may be NULL); `left_open` is the creation time of the entry in
 * UTC. Fixed words and a formatted time only. Returns the number of bytes written (without the
 * NUL), or 0 if `e` is NULL or `cap` is too small.
 */
size_t ntfsmac_priv_journal_member(const ntfsmac_journal_entry *e, const ntfsmac_probe_verdict *last,
				   char *buf, size_t cap);

/* Fixed wire name of a policy gate result, the `reason` of a refusal ("probe_not_clean"). */
const char *ntfsmac_priv_gate_name(ntfsmac_policy_gate g);

/*
 * `repair` with action `sync_mirror` (ctl_repair_mirror.c, T-M4-16, ADR-021, section 3.7): fresh
 * probe, gate ntfsmac_policy_gate_sync_mirror(), the bound write by `ntfsmac-volmark sync-mirror`,
 * then a fresh probe whose state goes into the reply:
 *   {"ok":true,"result":"repaired","state":"clean","journal_entry":"untouched"}
 * The journal is neither read nor written. Called by ntfsmac_priv_repair() with the mutex of the
 * privileged requests held, after its argument checks (so `p`, `bsd`, the strings and `reply` are
 * not NULL); the reply conventions are those of ntfsmac_priv_repair().
 */
ntfsmac_dmn_status ntfsmac_priv_repair_mirror(ntfsmac_priv *p, const char *bsd,
					      const char *confirm_label, const char *confirm_serial,
					      char *reply, size_t cap);

/* Shared by ctl_journal.c and ctl_repair_mirror.c (fixed-word replies of the privileged requests). */
ntfsmac_dmn_status ntfsmac_priv_refuse(char *reply, size_t cap, const char *reason);
ntfsmac_dmn_status ntfsmac_priv_failed(char *reply, size_t cap, ntfsmac_dmn_status st,
				       const char *reason);
bool ntfsmac_priv_confirmation_ok(const ntfsmac_probe_verdict *v, const char *label,
				  const char *serial);
void ntfsmac_priv_release_bind(const ntfsmac_priv *p, ntfsmac_priv_bind *bind);

/*
 * The real system facts: the fresh probe is open A (O_RDONLY), ntfsmac_spawn_probe(), close A;
 * clear_dirty is ntfsmac_session_marker_write() with `clear-repair`. `volume` and
 * `session_live` are the daemon's registry (T-M4-08) and are given by the caller.
 */
typedef struct ntfsmac_priv_sys_real {
	ntfsmac_priv_sys sys;
	const ntfsmac_session_sys *ss;
	bool (*volume)(void *ctx, const char *bsd, ntfsmac_priv_volume *out);
	bool (*session_live)(void *ctx, uint64_t session_id);
	void *registry_ctx;
} ntfsmac_priv_sys_real;
void ntfsmac_priv_sys_real_init(ntfsmac_priv_sys_real *r, const ntfsmac_session_sys *ss,
				bool (*volume)(void *, const char *, ntfsmac_priv_volume *),
				bool (*session_live)(void *, uint64_t), void *registry_ctx);

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
	NTFSMAC_NOTIFY_MIRROR_SPLIT,
	NTFSMAC_NOTIFY_STRAY_MOUNT /* T-M4-08: the worker mounted another file system (H1) */
} ntfsmac_notify_kind;

/* `label` is the sanitised label only; `reason` is a fixed daemon text, not disk-derived. */
ntfsmac_dmn_status ntfsmac_notify_post(ntfsmac_notify_kind kind, const char *label,
				       const char *reason);

/*
 * The text of a notification, from a fixed template, `label` (sanitised) and `reason` (a fixed
 * text of the daemon, or a time for WRITE_ERROR; both may be NULL). Control bytes are replaced.
 * Returns the length (without the NUL), or 0 with an empty `buf` for a kind out of range or a
 * `buf` that is too small. Pure.
 */
size_t ntfsmac_notify_text(ntfsmac_notify_kind kind, const char *label, const char *reason,
			   char *buf, size_t cap);

/*
 * Where the text goes. The default shows a banner to the console user (macOS) and logs it; a
 * sink replaces the banner (tests, and the VM driver that records the notifications). Pass NULL
 * to restore the default. ntfsmac_notify_post() returns OK when a sink took the text.
 */
typedef void (*ntfsmac_notify_sink_fn)(ntfsmac_notify_kind kind, const char *text, void *ctx);
void ntfsmac_notify_set_sink(ntfsmac_notify_sink_fn fn, void *ctx);

/*
 * Outcome of the last banner (T-M7-06, audit M4 F-05). The banner needs root's sudoers to allow
 * `sudo -n -u #UID`; when it does not, the helper fails and nothing was shown. The daemon keeps
 * the last outcome so that `doctor` can say why a banner is missing. It only ever feeds the
 * report and one warning in the log: no decision reads it.
 *
 * What can be told apart (the helper is `launchctl asuser UID sudo -n -u #UID osascript ...`, all
 * output discarded):
 *  - TIMEOUT (our own SIGKILL after NTFSMAC_BANNER_WAIT_MS ended it) and START_FAILED are exact.
 *  - A non-zero exit is NONZERO_EXIT with its status. Status 1 has two sources: `sudo -n` when it
 *    would need a password, and osascript or launchctl. So on status 1 the same thread runs
 *    `sudo -n -u #UID /usr/bin/true` once (bounded by NTFSMAC_BANNER_PROBE_WAIT_MS, never in
 *    doctor). Probe status 1: SUDO_REFUSED (exact). Probe status 0 or anything else (not started,
 *    timed out): stays NONZERO_EXIT 1, which is all that is known.
 */
#define NTFSMAC_BANNER_WAIT_MS 10000u      /* the helper is killed after this long */
#define NTFSMAC_BANNER_PROBE_WAIT_MS 3000u /* the sudo precondition probe */

typedef enum ntfsmac_banner_state {
	NTFSMAC_BANNER_NEVER = 0, /* no banner was handed to the helper yet: unknown */
	NTFSMAC_BANNER_OK,        /* the helper exited 0 */
	NTFSMAC_BANNER_START_FAILED,
	NTFSMAC_BANNER_SUDO_REFUSED, /* helper status 1 and the probe says sudo refuses */
	NTFSMAC_BANNER_TIMEOUT,
	NTFSMAC_BANNER_NONZERO_EXIT
} ntfsmac_banner_state;

typedef struct ntfsmac_banner_outcome {
	ntfsmac_banner_state state;
	int exit_code; /* 1..255 for SUDO_REFUSED and NONZERO_EXIT, else 0 */
} ntfsmac_banner_outcome;

/*
 * Pure: the outcome of one helper run. `started` is false when posix_spawn failed; `timed_out`
 * when our SIGKILL ended it; `status` as ntfsmac_banner_status() (-1: unknown, reported as
 * NONZERO_EXIT 255). Status 1 is NONZERO_EXIT 1 here; see ntfsmac_banner_refine().
 */
ntfsmac_banner_outcome ntfsmac_banner_classify(bool started, bool timed_out, int status);
/* Pure: from a wait status: the exit status, 128 + signal, or -1 when !waited or unreadable. */
int ntfsmac_banner_status(bool waited, int wst);
/* Pure: true when the wait status says our SIGKILL ended the child (the timeout). */
bool ntfsmac_banner_killed(bool waited, int wst);
/* Pure: whether the outcome (NONZERO_EXIT 1) needs the sudo probe to be told apart. */
bool ntfsmac_banner_needs_probe(const ntfsmac_banner_outcome *o);
/* Pure: apply the probe's status (-1: probe not run or not conclusive) to an outcome. */
ntfsmac_banner_outcome ntfsmac_banner_refine(ntfsmac_banner_outcome o, int probe_status);
/* Pure: a fixed name for logs. */
const char *ntfsmac_banner_state_name(ntfsmac_banner_state s);

/* Pure: the doctor finding of an outcome. NEVER is ok (unknown), OK is ok, the rest are not ok. */
ntfsmac_doctor_finding ntfsmac_banner_finding(const ntfsmac_banner_outcome *o);
/* Pure: the `value` of that finding: state * 1000 + exit_code. */
long ntfsmac_banner_value(const ntfsmac_banner_outcome *o);

/* Thread-safe store of the last outcome. Recording NEVER forgets the last one. */
void ntfsmac_banner_record(const ntfsmac_banner_outcome *o);
/*
 * A banner takes a sequence number when it starts; its result is stored only if no newer banner
 * has stored one, so a slow older helper never overwrites a newer outcome. Returns whether stored.
 */
uint64_t ntfsmac_banner_begin(void);
bool ntfsmac_banner_record_seq(uint64_t seq, const ntfsmac_banner_outcome *o);
ntfsmac_banner_outcome ntfsmac_banner_last(void);

#ifdef __cplusplus
}
#endif

#endif /* NTFSMAC_DAEMON_INTERNAL_H */
