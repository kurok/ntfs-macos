/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NTFSMAC_TEST_CHECK_H
#define NTFSMAC_TEST_CHECK_H

#include <stdio.h>

/* Count and report every failure; the test exits non-zero at the end. */
static int g_failures;

#define CHECK(cond)                                                          \
	do {                                                                 \
		if (!(cond)) {                                               \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
				#cond);                                      \
			g_failures++;                                        \
		}                                                            \
	} while (0)

#define CHECK_EQ_INT(a, b)                                                   \
	do {                                                                 \
		long long a_ = (long long)(a), b_ = (long long)(b);          \
		if (a_ != b_) {                                              \
			fprintf(stderr, "FAIL %s:%d: %s == %s (%lld vs %lld)\n", \
				__FILE__, __LINE__, #a, #b, a_, b_);         \
			g_failures++;                                        \
		}                                                            \
	} while (0)

#endif
