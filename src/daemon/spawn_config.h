/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_DAEMON_SPAWN_CONFIG_H
#define NTFSMAC_DAEMON_SPAWN_CONFIG_H

/*
 * Settings of the spawn module (T-M4-01). The calls themselves are declared
 * in internal.h; this header holds what the daemon sets once at start and
 * what tests replace.
 *
 * Status of the calls in internal.h, as spawn.c gives them:
 *
 *   ntfsmac_spawn_probe
 *     OK         verdict parsed; `verdict` holds it.
 *     INVAL      bad argument; `verdict` is still set to `unknown`.
 *     DENIED     this process may not start a child as the configured user.
 *     TIMEOUT    the probe ran past `timeout_s`; it was killed.
 *     IO         could not start, non-zero exit, killed by a signal, output
 *                over 64 KiB, or output that the strict parser refused.
 *     In every status other than OK `verdict` is the fail-closed `unknown`.
 *
 * Sandbox (T-M4-06): on macOS every child starts through /usr/bin/sandbox-exec
 * with the profile `NTFSMAC_PATH_SANDBOX_DIR/probe.sb`, `volmark.sb` or
 * `worker.sb`. The module opens the profile without following a link, checks
 * with fstat on that descriptor that it is a regular file owned by root that
 * no one else can write, reads it and gives sandbox-exec the text (`-p`), so
 * the file cannot change between the check and the use. If the check fails the
 * call returns DENIED and nothing starts. A profile that does not load makes
 * sandbox-exec exit before the program runs, so the program never runs
 * unsandboxed. The profiles are in integration/sandbox/; the parameters they
 * take are listed there. Elsewhere (Linux, for tests of the portable parts)
 * there is no sandbox.
 *
 * The test seams (the sandbox directory, the tool, the owner of the profiles,
 * the hook after `argv_check()`) exist only if NTFSMAC_TEST_SEAMS is defined.
 * CMake defines it unless the option of that name is OFF (preset `product`),
 * so a product build cannot start a child without the sandbox or with another
 * tool or profile directory.
 *
 *   ntfsmac_spawn_volmark
 *     OK         exit code 0.
 *     INVAL      bad argument.
 *     DENIED     as for the probe.
 *     IO         could not start (`*exit_code` is -3), or the exit code was
 *                not 0 (`*exit_code` holds it; 128 + n for signal n).
 *     TIMEOUT    killed. `*exit_code` is -1 if it never printed `w` (no
 *                write was started) and -2 if it did (a write may be cut).
 *     BUSY       killed but not reaped within the bound (`*exit_code` is -4): the
 *                child may still hold the device.
 *
 *   ntfsmac_spawn_worker
 *     OK         the mount appeared; `child` is live, `*mounted_ro` is set.
 *     INVAL      bad argument.
 *     DENIED     the option builder or `argv_check()` refused. Nothing was
 *                started. (A `usermapping` file the daemon does not accept is not
 *                a refusal: the worker is started without the option.)
 *     IO         could not start, or the worker exited before the mount
 *                appeared. The process group was killed and reaped and
 *                `child->pid` is -1.
 *     TIMEOUT    the mount did not appear within `mount_wait_s`. The process
 *                group got SIGKILL. `child->pid` is -1 once the leader is
 *                reaped; if it could not be reaped in 5 s it stays set and the
 *                caller keeps calling ntfsmac_child_poll().
 *
 * Teardown of a worker (audit M1): every kill of a worker (the failures above,
 * ntfsmac_child_kill_group()) also kills its descendants that left the process
 * group, found by walking the process tree; and when the worker is gone
 * (ntfsmac_child_poll() reports the exit, or a spawn fails) every process of the
 * worker's user that still holds the device is killed. The daemon asks once more
 * with ntfsmac_child_device_check() before it reports a volume detached.
 */

#include <stdbool.h>

#include "internal.h"
#include "ntfsmac/options.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default paths of the install manifest (ARCHITECTURE.md section 10). */
#define NTFSMAC_PATH_PROBE "/usr/local/libexec/ntfs-macos/ntfsmac-probe"
#define NTFSMAC_PATH_VOLMARK "/usr/local/libexec/ntfs-macos/ntfsmac-volmark"
#define NTFSMAC_PATH_WORKER "/usr/local/libexec/ntfs-macos/ntfsmac-worker"
/*
 * The one other program the worker's sandbox lets it exec (process-exec in
 * integration/sandbox/worker.sb; a test keeps the two equal). A leftover of it is a leftover of
 * the worker for the device sweep.
 */
#define NTFSMAC_PATH_MACFUSE_HELPER \
	"/Library/Filesystems/macfuse.fs/Contents/Resources/macfuse.app/Contents/MacOS/macfuse"

/* Sandbox profiles and the tool that applies them (ARCHITECTURE.md section 6). */
#define NTFSMAC_PATH_SANDBOX_DIR "/Library/Application Support/ntfs-macos/sandbox"
#define NTFSMAC_PATH_SANDBOX_EXEC "/usr/bin/sandbox-exec"

/* Further wait for volmark after it printed `w` (ARCHITECTURE.md section 3.6). */
#define NTFSMAC_VOLMARK_WRITE_WAIT_S 120u

/*
 * Is `mount_point` in the mount table? Sets `*found` and, if found,
 * `*read_only`. The default asks getfsstat(MNT_NOWAIT) on macOS, which does
 * not call into the file system. Tests replace it.
 */
typedef ntfsmac_dmn_status (*ntfsmac_mount_lookup_fn)(const char *mount_point,
						       bool *found, bool *read_only);

/* Test seam (NTFSMAC_TEST_SEAMS): called after `argv_check()` accepted the vector, may change it. */
typedef void (*ntfsmac_vector_hook_fn)(ntfsmac_worker_argv *av);

/*
 * The strings are not copied and must outlive the calls. The daemon sets this
 * once before its first spawn; a call to ntfsmac_spawn_config_set() while a
 * spawn runs on another thread is not supported.
 */
typedef struct ntfsmac_spawn_config {
	const char *probe_path;
	const char *volmark_path;
	const char *worker_path;
#if defined(NTFSMAC_TEST_SEAMS)
	const char *sandbox_dir;       /* NULL: no sandbox */
	const char *sandbox_exec_path; /* NULL: NTFSMAC_PATH_SANDBOX_EXEC */
	uid_t sandbox_owner;           /* owner the profiles must have: 0 (root) */
	/*
	 * The file the daemon reads and checks for `usermapping=`, and the owner it must have; NULL is
	 * NTFSMAC_USERMAPPING_PATH and 0 (root) is the owner when the path is NULL. Only the file that
	 * is read changes: the option in the vector and the sandbox parameters keep the product path
	 * (`argv_check()` rule 3 requires it).
	 */
	const char *usermapping_path;
	uid_t usermapping_owner;       /* used only if `usermapping_path` is not NULL */
#endif
	uid_t unpriv_uid;          /* probe and volmark run as this user ("nobody") */
	gid_t unpriv_gid;
	int child_stderr_fd;       /* stderr of every child; -1 is /dev/null */
	bool hide_hid_files;       /* opt-ins of section 3.3, from the configuration */
	bool permissions;
	bool usermapping;
	unsigned mount_poll_ms;    /* 0 means 50 */
	unsigned volmark_write_wait_s; /* 0 means NTFSMAC_VOLMARK_WRITE_WAIT_S */
	ntfsmac_mount_lookup_fn mount_lookup;      /* NULL: the system table */
#if defined(NTFSMAC_TEST_SEAMS)
	ntfsmac_vector_hook_fn after_check_hook;   /* NULL: none */
#endif
} ntfsmac_spawn_config;

/*
 * Manifest paths, the user `nobody`, no opt-ins, no seams. On macOS the sandbox
 * is on (with seams, `sandbox_dir` is the install directory); elsewhere there is none.
 */
void ntfsmac_spawn_config_default(ntfsmac_spawn_config *cfg);
void ntfsmac_spawn_config_set(const ntfsmac_spawn_config *cfg);

#ifdef __cplusplus
}
#endif

#endif /* NTFSMAC_DAEMON_SPAWN_CONFIG_H */
