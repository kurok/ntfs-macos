/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * F-SANITIZE-02 (docs/TEST_STRATEGY.md section 4): libFuzzer on the label sanitiser followed
 * by the existing ntfsmac_argv_check_text() of the builder's vector. Input: 8 bytes serial,
 * 1 byte of choices (decision, opt-ins, descriptor, collision suffix), then the raw label.
 * Oracles:
 *   - no crash, no sanitizer report;
 *   - the sanitised label equals, byte for byte, the output of the reference sanitiser of oracle.h
 *     (ADR-016 written again from the text) and satisfies every ADR-016 property;
 *   - ntfsmac_argv_check_text() accepts the builder's vector for that leaf, for every option
 *     set and for the collision suffixes " 2" to " 99" (rule 2 holds for a sanitised leaf);
 *   - fed the raw, unsanitised label as leaf, argv_check_text() accepts exactly the leaves of
 *     the full ADR-016 rule plus suffix grammar (oracle_leaf_violation()), in both directions.
 */
#include "ntfsmac/options.h"
#include "sanitize.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../unit/sanitize/oracle.h"

#define WORKER "/usr/local/libexec/ntfs-macos/ntfsmac-worker"

#define FAIL(msg)                                                                 \
	do {                                                                      \
		fprintf(stderr, "F-SANITIZE-02 oracle failure: %s (line %d)\n", msg, __LINE__); \
		abort();                                                          \
	} while (0)

static void check_leaf(const char *leaf, uint8_t choice)
{
	ntfsmac_options_input oi;
	ntfsmac_worker_argv w;
	ntfsmac_argv_expect ex;

	memset(&oi, 0, sizeof(oi));
	oi.worker_path = WORKER;
	oi.device_fd = 3 + (int)(choice % 200);
	oi.leaf = leaf;
	oi.decision = (choice & 1) ? NTFSMAC_POLICY_RO : NTFSMAC_POLICY_RW;
	oi.console_uid = 501;
	oi.console_gid = 20;
	oi.hide_hid_files = (choice & 2) != 0;
	oi.permissions = (choice & 4) != 0;
	oi.usermapping = (choice & 8) != 0;
	if (ntfsmac_options_build(&oi, &w) != NTFSMAC_OPTIONS_OK)
		FAIL("builder refused a sanitised leaf");
	ex.worker_path = WORKER;
	ex.leaf = leaf;
	ex.decision = oi.decision;
	ex.console_uid = 501;
	ex.console_gid = 20;
	if (ntfsmac_argv_check_text(w.argv, NTFSMAC_WORKER_ARGC, &ex) != NTFSMAC_ARGV_OK)
		FAIL("argv_check_text rejects the builder's vector");
}

/* argv_check_text() with `raw` (cut at a NUL) as leaf accepts exactly the leaves of ADR-016. */
static void check_raw(const char *label, size_t len)
{
	char *raw = malloc(len + 1);
	char *mp = malloc(len + 16);
	ntfsmac_argv_expect ex;
	const char *argv[NTFSMAC_WORKER_ARGC];
	size_t rl;
	bool accepted, valid;

	if (raw == NULL || mp == NULL)
		abort();
	memcpy(raw, label, len);
	raw[len] = '\0';
	rl = strlen(raw);
	snprintf(mp, len + 16, "/Volumes/%s", raw);
	ex.worker_path = WORKER;
	ex.leaf = raw;
	ex.decision = NTFSMAC_POLICY_RO;
	ex.console_uid = 501;
	ex.console_gid = 20;
	argv[0] = WORKER;
	argv[1] = "/dev/fd/5";
	argv[2] = mp;
	argv[3] = "-o";
	argv[4] = "backend=fskit,local,norecover,noatime,windows_names,nocompression,"
		  "streams_interface=openxattr,uid=501,gid=20,fmask=0133,dmask=0022,ro";
	accepted = ntfsmac_argv_check_text(argv, NTFSMAC_WORKER_ARGC, &ex) == NTFSMAC_ARGV_OK;
	valid = oracle_leaf_violation(raw, rl) == NULL;
	if (accepted && !valid)
		FAIL("argv_check accepted a leaf that ADR-016 refuses");
	if (!accepted && valid)
		FAIL("argv_check refused a leaf that ADR-016 allows");
	free(raw);
	free(mp);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	char part[NTFSMAC_LABEL_PART_SIZE], ref[NTFSMAC_LABEL_PART_SIZE + 16], leaf[NTFSMAC_LEAF_SIZE];
	uint64_t serial = 0;
	uint8_t choice;
	const char *label;
	size_t len, n, i;

	if (size < 9)
		return 0;
	for (i = 0; i < 8; i++)
		serial = (serial << 8) | data[i];
	choice = data[8];
	label = (const char *)data + 9;
	len = size - 9;

	n = ntfsmac_label_sanitize(label, len, serial, part, sizeof(part));
	if (n == 0 || n != strlen(part))
		FAIL("sanitiser returned 0 or a wrong length");
	if (oracle_label_violation(part, n, NTFSMAC_LABEL_PART_MAX_BYTES) != NULL)
		FAIL("leaf violates ADR-016");
	if (oracle_label_sanitize(label, len, serial, ref) != n || strcmp(ref, part) != 0)
		FAIL("sanitiser differs from the reference sanitiser");

	/* without and with the collision suffix (up to 67 bytes) */
	check_leaf(part, choice);
	snprintf(leaf, sizeof(leaf), "%s %u", part, 2u + (unsigned)(choice % 98));
	check_leaf(leaf, choice);
	snprintf(leaf, sizeof(leaf), "%s 99", part);
	check_leaf(leaf, choice);

	/* the raw label, and the sanitised label with good and bad suffixes, as leaf */
	check_raw(label, len);
	{
		static const char *const suf[] = { "", " 2", " 9", " 10", " 99", " 01", " 1", " 0", " 100", "  2", " 2x" };
		size_t k;

		for (k = 0; k < sizeof(suf) / sizeof(suf[0]); k++) {
			snprintf(leaf, sizeof(leaf), "%s%s", part, suf[k]);
			check_raw(leaf, strlen(leaf));
		}
	}
	return 0;
}
