/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_PROBE_POSIX_H
#define NTFSMAC_PROBE_POSIX_H

/*
 * First include of every probe source. The project builds with -std=c17 and no
 * compiler extensions, so POSIX (pread, off_t, ssize_t, clock_gettime) is asked
 * for here, and what libntfs-3g's installed headers expect from the system (they
 * are written for a build with the library's config.h, which we do not have) is
 * declared before they are read.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h> /* defines st_mtime: ntfstime.h then does not declare its own timespec */
#include <sys/types.h>
#include <time.h> /* ntfstime.h calls time() without including it */

#endif /* NTFSMAC_PROBE_POSIX_H */
