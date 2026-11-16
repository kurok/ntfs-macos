/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_TSAN_CHECK_H
#define NTFSMAC_TSAN_CHECK_H

/*
 * Thread-safe CHECK for the concurrent drivers (U-TSAN-*): the failure count
 * is behind a mutex, so a failing check in one thread is not itself a race.
 */
#include <pthread.h>
#include <stdio.h>

static pthread_mutex_t g_check_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_failures;

static inline void check_fail(const char *file, int line, const char *what)
{
	pthread_mutex_lock(&g_check_lock);
	fprintf(stderr, "FAIL %s:%d: %s\n", file, line, what);
	g_failures++;
	pthread_mutex_unlock(&g_check_lock);
}

static inline int check_failures(void)
{
	int n;

	pthread_mutex_lock(&g_check_lock);
	n = g_failures;
	pthread_mutex_unlock(&g_check_lock);
	return n;
}

#define CHECK(cond)                                            \
	do {                                                   \
		if (!(cond))                                   \
			check_fail(__FILE__, __LINE__, #cond); \
	} while (0)

#endif
