/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * F-PROBE (docs/TEST_STRATEGY.md section 8): libFuzzer on ntfsmac_probe_run(), the engine of
 * ntfsmac-probe, over an in-memory image (image.h builds it from a base plus edits).
 *
 * The probe takes a descriptor, so the image goes into a memory file (memfd on Linux, an
 * unlinked temporary file elsewhere) that is written through one descriptor and handed to the
 * probe through a second one opened O_RDONLY, as the daemon does. The probe's own read-only
 * device layer (devio.c) and its own mount of libntfs-3g are what runs: nothing is replaced.
 *
 * Oracles, for every input:
 *   - no crash, no sanitizer report, no leak;
 *   - the exit status is OK (verdict filled) or IO (descriptor or boot sector unusable), never
 *     INTERNAL; an image of at least 512 bytes is always usable, so it is always OK;
 *   - the verdict passes the cross-field rules, its JSON form is within the size limit and is
 *     accepted by the daemon's strict parser (ntfsmac_probe_parse), which returns a verdict
 *     that serialises to the same text;
 *   - `clean` only for an image that has the NTFS boot signature and holds the whole volume;
 *   - the probe never asks its device layer to write (devio_refused_writes() stays 0).
 * The scan budget is 1 s so that a pathological tree costs the verdict (`unknown`), not the
 * run; the real budget is 15 s.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* memfd_create */
#endif
#include "posix.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/mman.h>
#endif

#include "devio.h"
#include "image.h"
#include "probe_core.h"

#define FAIL(msg)                                                                              \
	do {                                                                                   \
		fprintf(stderr, "F-PROBE oracle failure: %s (line %d)\n", msg, __LINE__);      \
		abort();                                                                       \
	} while (0)

#define SCAN_BUDGET_MS 1000

static int wfd = -1, rfd = -1;
static ntfsmac_probe_verdict *verdict;
static char *json, *json2;

static void open_image_file(void)
{
#if defined(__linux__)
	char path[64];

	wfd = memfd_create("ntfsmac-fuzz-image", 0);
	if (wfd < 0)
		FAIL("memfd_create");
	snprintf(path, sizeof(path), "/proc/self/fd/%d", wfd);
	rfd = open(path, O_RDONLY);
#else
	char path[] = "/tmp/ntfsmac-fuzz-XXXXXX";

	wfd = mkstemp(path);
	if (wfd < 0)
		FAIL("mkstemp");
	rfd = open(path, O_RDONLY);
	unlink(path);
#endif
	if (rfd < 0)
		FAIL("reopen read-only");
	verdict = malloc(sizeof(*verdict));
	json = malloc(NTFSMAC_PROBE_MAX_JSON_BYTES);
	json2 = malloc(NTFSMAC_PROBE_MAX_JSON_BYTES);
	if (verdict == NULL || json == NULL || json2 == NULL)
		abort();
}

static void put_image(const uint8_t *img, size_t size)
{
	size_t done = 0;

	if (ftruncate(wfd, 0) != 0)
		FAIL("ftruncate");
	while (done < size) {
		ssize_t w = pwrite(wfd, img + done, size - done, (off_t)done);

		if (w <= 0)
			FAIL("pwrite");
		done += (size_t)w;
	}
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	ntfsmac_probe_opts opts = { SCAN_BUDGET_MS, false, -1 };
	ntfsmac_probe_verdict parsed;
	const uint8_t *img;
	size_t isize, n, n2;
	unsigned long refused;
	int rc;

	if (wfd < 0)
		open_image_file();
	img = fz_image_build(data, size, &isize);
	put_image(img, isize);
	refused = devio_refused_writes();

	rc = ntfsmac_probe_run(rfd, &opts, verdict);
	if (rc != NTFSMAC_PROBE_EXIT_OK)
		FAIL("probe exit status is not OK for a readable image");
	if (devio_refused_writes() != refused)
		FAIL("the probe asked its device layer to write");
	if (ntfsmac_probe_verdict_validate(verdict) != NTFSMAC_PROBE_OK)
		FAIL("verdict fails the cross-field rules");
	n = ntfsmac_probe_json(verdict, json, NTFSMAC_PROBE_MAX_JSON_BYTES);
	if (n == 0 || n > NTFSMAC_PROBE_MAX_JSON_BYTES)
		FAIL("verdict does not serialise");
	if (ntfsmac_probe_parse(json, n, &parsed) != NTFSMAC_PROBE_OK)
		FAIL("the daemon's parser refuses the probe's own output");
	n2 = ntfsmac_probe_json(&parsed, json2, NTFSMAC_PROBE_MAX_JSON_BYTES);
	if (n2 != n || memcmp(json, json2, n) != 0)
		FAIL("verdict changes through JSON");
	if (verdict->state == NTFSMAC_PROBE_STATE_CLEAN) {
		if (!verdict->is_ntfs || verdict->sector_size == 0 ||
		    verdict->total_sectors > isize / verdict->sector_size)
			FAIL("clean verdict for an image that is not a whole NTFS volume");
	}
	return 0;
}
