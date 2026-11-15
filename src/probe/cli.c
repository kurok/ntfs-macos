/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Command line of ntfsmac-probe (docs/ARCHITECTURE.md section 3.1):
 *
 *   ntfsmac-probe --fd N [--bsd-name diskNsM] [--debug]
 *   ntfsmac-probe --version
 *
 * Prints one JSON verdict on stdout. Exit 0 verdict printed, 2 usage error,
 * 3 I/O error on the descriptor, 4 internal error or timeout. A volume
 * condition is never an exit status. The probe opens no path: --bsd-name is
 * checked and otherwise unused (the daemon passes it for the log).
 *
 * The test build (probe_test) also takes --scan-budget-ms N and --no-alarm.
 */
#include "posix.h"
#include "devio.h"
#include "nfc.h"
#include "probe_core.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

/* The daemon kills the probe after 20 s; stop a little earlier on our own. */
#define HARD_LIMIT_SECONDS 19u

/*
 * Address space cap. Worst case of the name walk (names.c): 4 Mi directories of 24 bytes (96 MiB)
 * + 64 MiB of directory names + the set of record numbers (16 Mi slots of 8 bytes, 128 MiB, 64 MiB
 * more while it grows) + the doubling copies of the first two (about 100 MiB transient) is near
 * 400 MiB, plus libntfs-3g's caches, buffers and the C library, so the cap is 1 GiB. A volume that
 * needs more gets malloc() failures, which end the walk as `unknown`, not a crash. Measured on 1,000,000
 * entries: 32 MiB resident. Not set in a sanitizer build (the shadow memory needs terabytes of
 * address space). RLIMIT_AS is not enforced on every system (macOS may ignore it): best effort.
 */
#define ADDRESS_SPACE_CAP (UINT64_C(1) << 30)

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define NTFSMAC_UNDER_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define NTFSMAC_UNDER_SANITIZER 1
#endif
#endif

static void limit_memory(void)
{
#if !defined(NTFSMAC_UNDER_SANITIZER)
	struct rlimit rl;

	if (getrlimit(RLIMIT_AS, &rl) != 0)
		return;
	if (rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur <= ADDRESS_SPACE_CAP)
		return; /* already lower */
	rl.rlim_cur = (rlim_t)ADDRESS_SPACE_CAP;
	if (rl.rlim_max != RLIM_INFINITY && rl.rlim_max < rl.rlim_cur)
		rl.rlim_cur = rl.rlim_max;
	(void)setrlimit(RLIMIT_AS, &rl);
#endif
}

static void on_alarm(int sig)
{
	(void)sig;
	_exit(NTFSMAC_PROBE_EXIT_INTERNAL);
}

static int usage(void)
{
	fputs("usage: ntfsmac-probe --fd N [--bsd-name diskNsM] [--debug]\n", stderr);
	return NTFSMAC_PROBE_EXIT_USAGE;
}

/* A decimal number without sign or leading zero, at most max. -1 if not. */
static long parse_number(const char *s, long max)
{
	long n = 0;

	if (*s < '0' || *s > '9' || (s[0] == '0' && s[1] != '\0'))
		return -1;
	for (; *s; s++) {
		if (*s < '0' || *s > '9')
			return -1;
		n = n * 10 + (*s - '0');
		if (n > max)
			return -1;
	}
	return n;
}

/* ^disk[0-9]+(s[0-9]+)?$ */
static int bsd_name_ok(const char *s)
{
	size_t i = 4, digits;

	if (strncmp(s, "disk", 4) != 0)
		return 0;
	for (digits = 0; s[i] >= '0' && s[i] <= '9'; i++)
		digits++;
	if (digits == 0 || digits > 9)
		return 0;
	if (s[i] == '\0')
		return 1;
	if (s[i] != 's')
		return 0;
	for (i++, digits = 0; s[i] >= '0' && s[i] <= '9'; i++)
		digits++;
	return digits > 0 && digits <= 9 && s[i] == '\0';
}

int ntfsmac_probe_cli(int argc, char **argv, bool test_opts)
{
	ntfsmac_probe_opts opts = { -1, false, ntfsmac_probe_now_ms() };
	ntfsmac_probe_verdict *v;
	char *json;
	size_t n;
	long fd = -1;
	bool alarm_on = true;
	int i, rc;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--version") && argc == 2) {
			printf("ntfsmac-probe schema %d, NFC tables Unicode %s\n", NTFSMAC_PROBE_SCHEMA_VERSION,
			       ntfsmac_nfc_unicode_version());
			return NTFSMAC_PROBE_EXIT_OK;
		}
		if (!strcmp(argv[i], "--fd") && i + 1 < argc && fd < 0) {
			fd = parse_number(argv[++i], 1023);
			if (fd < 3) /* 0 to 2 are the standard streams */
				return usage();
		} else if (!strcmp(argv[i], "--bsd-name") && i + 1 < argc) {
			if (!bsd_name_ok(argv[++i]))
				return usage();
		} else if (!strcmp(argv[i], "--debug")) {
			opts.debug = true;
		} else if (test_opts && !strcmp(argv[i], "--scan-budget-ms") && i + 1 < argc) {
			long ms = parse_number(argv[++i], 86400000L);

			if (ms < 0)
				return usage();
			opts.scan_budget_ms = ms;
		} else if (test_opts && !strcmp(argv[i], "--no-alarm")) {
			alarm_on = false;
		} else {
			return usage();
		}
	}
	if (fd < 0)
		return usage();

	limit_memory();
	signal(SIGPIPE, SIG_IGN);
	if (alarm_on) {
		signal(SIGALRM, on_alarm);
		alarm(HARD_LIMIT_SECONDS);
	}

	v = malloc(sizeof(*v));
	json = malloc(NTFSMAC_PROBE_MAX_JSON_BYTES);
	if (!v || !json) {
		free(v);
		free(json);
		return NTFSMAC_PROBE_EXIT_INTERNAL;
	}
	if (opts.debug)
		fprintf(stderr, "ntfsmac-probe: NFC tables: Unicode %s\n", ntfsmac_nfc_unicode_version());
	rc = ntfsmac_probe_run((int)fd, &opts, v);
	if (opts.debug)
		fprintf(stderr, "ntfsmac-probe: device writes refused: %lu\n", devio_refused_writes());
	if (rc == NTFSMAC_PROBE_EXIT_OK) {
		n = ntfsmac_probe_json(v, json, NTFSMAC_PROBE_MAX_JSON_BYTES);
		if (n == 0 || fwrite(json, 1, n, stdout) != n || fflush(stdout) != 0)
			rc = NTFSMAC_PROBE_EXIT_INTERNAL;
	}
	free(v);
	free(json);
	return rc;
}
