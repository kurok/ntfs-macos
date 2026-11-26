/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_DAEMON_QBOUNDED_H
#define NTFSMAC_DAEMON_QBOUNDED_H

/*
 * A synchronous call onto a serial dispatch queue with a time bound (T-M7-14). The control socket
 * has one server thread, so a request must not wait without limit for a queue that a long job
 * holds (the journal queue during a repair, for example). macOS only (libdispatch).
 */

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Run `fn` on `q` on a private copy of the `size` bytes at `arg`, and wait at most `ms` for it.
 * True: `fn` ran and its copy was written back to `arg`. False: it did not finish in time (or no
 * memory); `arg` is unchanged, and when the queue gets to the job later `fn` runs on the copy,
 * which is then freed. So `fn` must not use pointers into the caller's stack: everything it reads
 * or writes is in the `size` bytes.
 */
bool ntfsmac_queue_call_bounded(dispatch_queue_t q, void *arg, size_t size, void (*fn)(void *),
				unsigned ms);

#ifdef __cplusplus
}
#endif

#endif /* __APPLE__ */

#endif /* NTFSMAC_DAEMON_QBOUNDED_H */
