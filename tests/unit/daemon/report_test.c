/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-DMN-RP-01..05: the replies of `list`, `status` and `doctor` and the fixed reason tokens
 * (src/daemon/report.c, T-M4-08). Every reply is parsed back with the CLI's own reader, so the
 * test shows that the CLI can read what the daemon writes.
 */
#include <string.h>

#include "check.h"
#include "json.h"
#include "ntfsmac/ctl_proto.h"
#include "report.h"

static const json_node *parse(const char *s, json_doc **doc)
{
	*doc = json_parse(s, strlen(s));
	return *doc != NULL ? json_root(*doc) : NULL;
}

/* U-DMN-RP-01: list is one object with a volumes array the CLI table can read. */
static void test_list(void)
{
	char buf[1024];
	ntfsmac_report_vol v[2];
	json_doc *d;
	const json_node *root, *vols;

	memset(v, 0, sizeof v);
	v[0] = (ntfsmac_report_vol){"disk4s1", "clean64m4k", "Mounted", "rw", "/Volumes/clean64m4k", ""};
	v[1] = (ntfsmac_report_vol){"disk5s1", "q\"uote\\back\x01", "Failed", NULL, NULL, "policy_refused"};
	CHECK(ntfsmac_report_list(v, 2, buf, sizeof buf) > 0);
	root = parse(buf, &d);
	CHECK(root != NULL);
	if (root != NULL) {
		vols = json_member(root, "volumes");
		CHECK(vols != NULL && vols->type == JSON_ARRAY && vols->first != NULL);
		CHECK(strcmp(json_member_str(vols->first, "bsd"), "disk4s1") == 0);
		CHECK(strcmp(json_member_str(vols->first, "mount"), "rw") == 0);
		CHECK(strcmp(json_member_str(vols->first->next, "label"), "q\"uote\\back\x01") == 0);
		CHECK(strcmp(json_member_str(vols->first->next, "mount"), "none") == 0);
		CHECK(strcmp(json_member_str(vols->first->next, "reason"), "policy_refused") == 0);
	}
	json_free(d);
	CHECK(strchr(buf, '\n') == NULL);

	CHECK(ntfsmac_report_list(NULL, 0, buf, sizeof buf) > 0 && strstr(buf, "\"volumes\":[]") != NULL);
	CHECK_EQ_INT(ntfsmac_report_list(v, 2, buf, 40), 0);
	CHECK(buf[0] == '\0');
}

/* U-DMN-RP-02: status carries the volume, the last probe and the `journal` member of the CLI. */
static void test_status(void)
{
	static char buf[NTFSMAC_CTL_REPLY_MAX];
	static ntfsmac_probe_verdict pv;
	ntfsmac_report_vol v = {"disk4s1", "Disk", "Mounted", "ro", "/Volumes/Disk", "dirty_flag"};
	ntfsmac_journal_entry e;
	json_doc *d;
	const json_node *root, *p, *j;

	memset(&pv, 0, sizeof pv);
	ntfsmac_probe_verdict_unknown(&pv);
	pv.serial = 0x123456789ABCDEF0ull;
	pv.state = NTFSMAC_PROBE_STATE_DIRTY_FLAG;
	pv.reason_codes = NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_FLAG_DIRTY) |
			  NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_NAMES_NOT_NFC);
	pv.volume_flags = NTFSMAC_PROBE_VOLFLAG_BIT(NTFSMAC_PROBE_VOLFLAG_DIRTY);
	pv.nonnfc_count = 1;
	pv.nonnfc_path_count = 1;
	snprintf(pv.nonnfc_paths[0], sizeof pv.nonnfc_paths[0], "/a\\b\n\"q\"");
	memset(&e, 0, sizeof e);
	e.serial = pv.serial;
	e.created = 1700000000;
	e.marker = NTFSMAC_JOURNAL_MARKER_SET;
	e.state = NTFSMAC_JOURNAL_STATE_OPEN;

	CHECK(ntfsmac_report_status(&v, &pv, &e, buf, sizeof buf) > 0);
	root = parse(buf, &d);
	CHECK(root != NULL);
	if (root != NULL) {
		p = json_member(root, "probe");
		j = json_member(root, "journal");
		CHECK(json_member(root, "volume") != NULL);
		CHECK(p != NULL && strcmp(json_member_str(p, "state"), "dirty_flag") == 0);
		CHECK(p != NULL && strcmp(json_member_str(p, "serial"), "0x123456789ABCDEF0") == 0);
		CHECK(p != NULL && json_member(p, "reason_codes") != NULL &&
		      json_member(p, "reason_codes")->first != NULL);
		CHECK(p != NULL && json_member(p, "nonnfc_paths")->first != NULL);
		/* the CLI reads marker, state, warning and left_open from this member */
		CHECK(j != NULL && strcmp(json_member_str(j, "marker"), "set") == 0);
		CHECK(j != NULL && strcmp(json_member_str(j, "state"), "open") == 0);
		CHECK(j != NULL && json_member_str(j, "warning") != NULL);
		CHECK(j != NULL && strcmp(json_member_str(j, "left_open"), "2023-11-14T22:13:20Z") == 0);
	}
	json_free(d);
	/* The path went through the display sanitiser: no raw control byte or bare backslash. */
	CHECK(strchr(buf, '\n') == NULL && strstr(buf, "u{005C}") != NULL);

	/* No probe, no entry: only the volume. */
	CHECK(ntfsmac_report_status(&v, NULL, NULL, buf, sizeof buf) > 0);
	CHECK(strstr(buf, "\"probe\"") == NULL && strstr(buf, "\"journal\"") == NULL);
	CHECK(strstr(buf, "restart_left_mounted") == NULL);
	CHECK_EQ_INT(ntfsmac_report_status(NULL, NULL, NULL, buf, sizeof buf), 0);

	/* #9: the restart left writable mounts mounted: a daemon-wide count, valid JSON. */
	CHECK(ntfsmac_report_status_ex(&v, NULL, NULL, 2, buf, sizeof buf) > 0);
	d = json_parse(buf, strlen(buf));
	CHECK(d != NULL && json_root(d) != NULL);
	{
		const json_node *n = json_member(json_root(d), "restart_left_mounted");

		CHECK(n != NULL && n->type == JSON_NUMBER && strcmp(n->text, "2") == 0);
	}
	json_free(d);
	CHECK(ntfsmac_report_status_ex(&v, NULL, NULL, 0, buf, sizeof buf) > 0);
	CHECK(strstr(buf, "restart_left_mounted") == NULL);
}

/* U-DMN-RP-03: a path list that does not fit is cut at a whole path; the reply stays valid. */
static void test_status_cut(void)
{
	static char buf[1400];
	static ntfsmac_probe_verdict pv;
	ntfsmac_report_vol v = {"disk4s1", "Disk", "Appeared", "none", "", ""};
	json_doc *d;
	const json_node *root, *paths;
	unsigned i;
	size_t n;

	ntfsmac_probe_verdict_unknown(&pv);
	pv.state = NTFSMAC_PROBE_STATE_NAMES_NOT_NFC;
	pv.nonnfc_count = 16;
	pv.nonnfc_path_count = 16;
	for (i = 0; i < 16; i++) {
		memset(pv.nonnfc_paths[i], 'x', 400);
		pv.nonnfc_paths[i][0] = '/';
		pv.nonnfc_paths[i][400] = '\0';
	}
	n = ntfsmac_report_status(&v, &pv, NULL, buf, sizeof buf);
	CHECK(n > 0 && n < sizeof buf);
	root = parse(buf, &d);
	CHECK(root != NULL);
	if (root != NULL) {
		paths = json_member(json_member(root, "probe"), "nonnfc_paths");
		CHECK(paths != NULL && paths->first != NULL);
		CHECK(strlen(paths->first->text) == 400);
	}
	json_free(d);
}

/* U-DMN-RP-04: doctor findings. */
static void test_doctor(void)
{
	char buf[512];
	ntfsmac_doctor_finding f[2] = {{NTFSMAC_DOCTOR_MACFUSE_INSTALLED, true, 0},
				       {NTFSMAC_DOCTOR_JOURNAL_ENTRIES, false, 3}};
	json_doc *d;
	const json_node *root, *list;

	CHECK(ntfsmac_report_doctor(f, 2, buf, sizeof buf) > 0);
	root = parse(buf, &d);
	CHECK(root != NULL);
	if (root != NULL) {
		list = json_member(root, "findings");
		CHECK(list != NULL && list->first != NULL && list->first->next != NULL);
		CHECK(list->first->next->first->text != NULL);
	}
	json_free(d);
	CHECK_EQ_INT(ntfsmac_report_doctor(f, 2, buf, 30), 0);
}

/* U-DMN-RP-05: every token of the three enums has a fixed text; NONE has no token. */
static void test_tokens(void)
{
	int i;

	for (i = 1; i < (int)NTFSMAC_VREASON_COUNT_; i++) {
		const char *t = ntfsmac_report_vreason_token((ntfsmac_vol_reason)i);

		CHECK(t[0] != '\0' && ntfsmac_report_reason_text(t)[0] != '\0');
	}
	for (i = 1; i <= (int)NTFSMAC_POLICY_NOTE_NAMES_NOT_NFC; i++) {
		const char *t = ntfsmac_report_policy_note_token((ntfsmac_policy_note)i);

		/* mirror_split has its own notification, so it has no reason text */
		if (i == (int)NTFSMAC_POLICY_NOTE_MIRROR_SPLIT)
			continue;
		CHECK(t[0] != '\0' && ntfsmac_report_reason_text(t)[0] != '\0');
	}
	for (i = 1; i <= (int)NTFSMAC_SREASON_BINDING_AFTER_MARK; i++) {
		const char *t = ntfsmac_report_session_reason_token((ntfsmac_session_reason)i);

		if (i == (int)NTFSMAC_SREASON_MARKER_INTERRUPTED)
			continue; /* has its own notification */
		CHECK(t[0] != '\0' && ntfsmac_report_reason_text(t)[0] != '\0');
	}
	CHECK(ntfsmac_report_vreason_token(NTFSMAC_VREASON_NONE)[0] == '\0');
	CHECK(ntfsmac_report_policy_note_token(NTFSMAC_POLICY_NOTE_NONE)[0] == '\0');
	CHECK(ntfsmac_report_session_reason_token(NTFSMAC_SREASON_NONE)[0] == '\0');
	CHECK(strstr(ntfsmac_report_reason_text("external_check"), "re-attach") != NULL);
	CHECK(ntfsmac_report_reason_text("nope")[0] == '\0' && ntfsmac_report_reason_text(NULL)[0] == '\0');
}

int main(void)
{
	test_list();
	test_status();
	test_status_cut();
	test_doctor();
	test_tokens();
	if (g_failures != 0) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	return 0;
}
