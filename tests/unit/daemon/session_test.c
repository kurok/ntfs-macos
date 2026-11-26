/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-DMN-SES-01..18: session lifecycle (src/daemon/session.c; ARCHITECTURE.md sections 3.5, 3.6,
 * 4.1 step 6, 4.5 step 3; ADR-017, ADR-020). Host test: the system comes through
 * ntfsmac_session_sys, here a fake that records every call in order and can fail at any point;
 * the journal is the real library on a temporary directory. The real calls are covered by
 * tests/integration/daemon/journal_vm.sh in a guest.
 *
 *   01 policy mode and automount_mode=ro        07 volmark failures, the w/d rule's results
 *   02 binding decision table                   08 sector 0 changed by volmark (I-DMN-25 c)
 *   03 decide: entry, corrupt entry, evidence   09 journal create refused or entry exists
 *   04 rw happy path and its order              10 marker disabled
 *   05 ro sessions                              11 write error record
 *   06 binding failures (I-DMN-25 a, b)        12-18 end of a session
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "internal.h"
#include "report.h"

#define SERIAL UINT64_C(0x1122334455667788)
#define NOW0 INT64_C(1700000000)

/* ---------------- the fake system ---------------- */

#define MAXFD 8
#define MAXEV 40

typedef struct fake {
	ntfsmac_journal *j;
	/* knobs */
	bool fail_open_rw, fail_open_ro, fail_flags;
	ntfsmac_dev_id id_ro, id_rw;       /* identity of a ro / rw descriptor */
	int ro_block_variant, rw_block_variant;  /* 0: the probe-time bytes, other: different */
	int after_volmark_variant;         /* block 0 through B after volmark */
	int ro2_block_variant;             /* block 0 through the second ro descriptor */
	ntfsmac_disk_info disk;            /* describe() answer */
	ntfsmac_dmn_status describe_status;
	ntfsmac_dmn_status vm_status[4];   /* volmark answers in call order */
	int vm_exit[4];
	bool vm_changes_block0;
	ntfsmac_dmn_status sync_status;
	bool flush_ok;
	uint64_t next_session_id;
	int64_t now;
	/* record */
	char ev[MAXEV][40];
	int nev;
	bool fd_open[MAXFD];
	bool fd_rw[MAXFD];
	bool fd_second_ro[MAXFD];
	int nopen_total;
	int vm_calls;
	ntfsmac_volmark_op vm_ops[4];
	ntfsmac_volmark_run vm_run[4];
	int vm_open_fds, vm_a_open;       /* what was open when volmark ran */
	ntfsmac_journal_marker marker_at_volmark;
	bool entry_at_open_rw, a_open_at_open_rw;
	int open_rw_calls, open_ro_calls, flags_calls;
	int rw_fd_at_vm;
	bool a_established;
} fake;

static void ev(fake *f, const char *s)
{
	if (f->nev < MAXEV)
		snprintf(f->ev[f->nev++], sizeof f->ev[0], "%s", s);
}

static bool saw(const fake *f, const char *s)
{
	int i;

	for (i = 0; i < f->nev; i++)
		if (strcmp(f->ev[i], s) == 0)
			return true;
	return false;
}

static int pos(const fake *f, const char *s)
{
	int i;

	for (i = 0; i < f->nev; i++)
		if (strcmp(f->ev[i], s) == 0)
			return i;
	return -1;
}

static int last_pos(const fake *f, const char *s)
{
	int i, r = -1;

	for (i = 0; i < f->nev; i++)
		if (strcmp(f->ev[i], s) == 0)
			r = i;
	return r;
}

static int count_open(const fake *f)
{
	int i, n = 0;

	for (i = 0; i < MAXFD; i++)
		n += f->fd_open[i] ? 1 : 0;
	return n;
}

static void fill_block(uint8_t *b, int variant)
{
	memset(b, 0xA5, NTFSMAC_BLOCK0_BYTES);
	b[0] = 0xEB;
	b[3] = 'N';
	b[4] = 'T';
	b[NTFSMAC_BLOCK0_BYTES - 1] = (uint8_t)variant; /* variant 0 is the probe-time block */
}

static int f_open(void *ctx, const char *bsd, bool writable)
{
	fake *f = ctx;
	int fd;

	(void)bsd;
	if (writable) {
		f->open_rw_calls++;
		{
			ntfsmac_journal_entry e;

			f->entry_at_open_rw = ntfsmac_journal_get(f->j, SERIAL, 131072, &e) == NTFSMAC_JOURNAL_OK;
			f->a_open_at_open_rw = count_open(f) > 0;
		}
		if (f->fail_open_rw)
			return -1;
	} else {
		f->open_ro_calls++;
		if (f->fail_open_ro)
			return -1;
	}
	for (fd = 3; fd < MAXFD; fd++)
		if (!f->fd_open[fd])
			break;
	if (fd == MAXFD)
		return -1;
	f->fd_open[fd] = true;
	f->fd_rw[fd] = writable;
	f->fd_second_ro[fd] = !writable && f->a_established;
	f->nopen_total++;
	ev(f, writable ? "open_rw" : "open_ro");
	return fd;
}

static void f_close(void *ctx, int fd)
{
	fake *f = ctx;

	CHECK(fd >= 3 && fd < MAXFD && f->fd_open[fd]);
	if (fd >= 3 && fd < MAXFD) {
		f->fd_open[fd] = false;
		ev(f, f->fd_rw[fd] ? "close_rw" : "close_ro");
	}
}

static int f_identity(void *ctx, int fd, ntfsmac_dev_id *out)
{
	fake *f = ctx;

	*out = f->fd_rw[fd] ? f->id_rw : f->id_ro;
	return 0;
}

/* The probe-time bytes are variant 0. `fd` 3 is the probe descriptor A in these tests. */
static int f_block0(void *ctx, int fd, uint8_t *buf)
{
	fake *f = ctx;
	int variant = 0;

	if (f->fd_rw[fd])
		variant = f->vm_calls > 0 ? f->after_volmark_variant : f->rw_block_variant;
	else if (f->fd_second_ro[fd])
		variant = f->ro2_block_variant;
	else
		variant = f->ro_block_variant;
	fill_block(buf, variant);
	return 0;
}

static int f_flags(void *ctx, int fd)
{
	fake *f = ctx;

	(void)fd;
	f->flags_calls++;
	ev(f, "reset_flags");
	return f->fail_flags ? -1 : 0;
}

static ntfsmac_dmn_status f_describe(void *ctx, const char *bsd, ntfsmac_disk_info *out)
{
	fake *f = ctx;

	(void)bsd;
	ev(f, "describe");
	*out = f->disk;
	return f->describe_status;
}

static ntfsmac_dmn_status f_volmark(void *ctx, const ntfsmac_volmark_run *run, ntfsmac_volmark_op op,
				    int *exit_code)
{
	fake *f = ctx;
	int k = f->vm_calls < 4 ? f->vm_calls : 3;
	ntfsmac_journal_entry e;

	f->vm_ops[k] = op;
	f->vm_run[k] = *run;
	f->vm_open_fds = count_open(f);
	f->vm_a_open = 0;
	f->rw_fd_at_vm = run->device_fd;
	f->marker_at_volmark = ntfsmac_journal_get(f->j, SERIAL, 131072, &e) == NTFSMAC_JOURNAL_OK
				       ? e.marker
				       : NTFSMAC_JOURNAL_MARKER_NONE;
	ev(f, op == NTFSMAC_VOLMARK_SET ? "volmark_set" : op == NTFSMAC_VOLMARK_CLEAR ? "volmark_clear"
										    : "volmark_repair");
	f->vm_calls++;
	*exit_code = f->vm_exit[k];
	return f->vm_status[k];
}

static ntfsmac_dmn_status f_sync(void *ctx, int fd, bool *flush_ok)
{
	fake *f = ctx;

	(void)fd;
	ev(f, "sync");
	*flush_ok = f->flush_ok;
	return f->sync_status;
}

static int64_t f_now(void *ctx)
{
	return ((fake *)ctx)->now;
}

static uint64_t f_sid(void *ctx)
{
	return ((fake *)ctx)->next_session_id++;
}

static ntfsmac_session_sys g_sys = {.open_device = f_open,
				    .close_fd = f_close,
				    .dev_identity = f_identity,
				    .read_block0 = f_block0,
				    .reset_flags = f_flags,
				    .describe = f_describe,
				    .volmark = f_volmark,
				    .sync_device = f_sync,
				    .now = f_now,
				    .new_session_id = f_sid};

/* ---------------- rig ---------------- */

static char g_dir[160];

static void make_dir(void)
{
	const char *t = getenv("TMPDIR");

	snprintf(g_dir, sizeof g_dir, "%s%sntfsmac-ses-XXXXXX", t != NULL ? t : "/tmp",
		 (t != NULL && t[strlen(t) - 1] == '/') ? "" : "/");
	CHECK(mkdtemp(g_dir) != NULL);
}

static void rm_tree(const char *dir)
{
	char cmd[400];

	snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir); /* our own temporary directory */
	if (system(cmd) != 0)
		fprintf(stderr, "cleanup of %s failed\n", dir);
}

static char g_jdir[220];
static unsigned g_case;

static void fake_init(fake *f)
{
	memset(f, 0, sizeof *f);
	snprintf(g_jdir, sizeof g_jdir, "%s/j%u", g_dir, ++g_case);
	CHECK_EQ_INT(ntfsmac_journal_open(g_jdir, &f->j), NTFSMAC_JOURNAL_OK);
	f->id_ro = f->id_rw = (ntfsmac_dev_id){0x1000, 1};
	snprintf(f->disk.bsd_name, sizeof f->disk.bsd_name, "disk4s1");
	snprintf(f->disk.media_path, sizeof f->disk.media_path, "IODeviceTree:/pci/usb@1");
	f->describe_status = NTFSMAC_DMN_OK;
	f->flush_ok = true;
	f->next_session_id = 1000;
	f->now = NOW0;
	g_sys.ctx = f;
}

static void fake_done(fake *f)
{
	ntfsmac_journal_close(f->j);
}

static ntfsmac_probe_verdict clean_verdict(void)
{
	ntfsmac_probe_verdict v;

	memset(&v, 0, sizeof v);
	v.is_ntfs = true;
	v.has_ntfs_version = true;
	v.ntfs_version_major = 3;
	v.ntfs_version_minor = 1;
	v.serial = SERIAL;
	v.total_sectors = 131072;
	v.cluster_size = 4096;
	v.sector_size = 512;
	v.logfile = NTFSMAC_PROBE_LOGFILE_CLEAN;
	v.has_logfile_lsn = true;
	v.logfile_lsn = 0x1234;
	v.hiberfile = NTFSMAC_PROBE_HIBERFILE_ABSENT;
	v.state = NTFSMAC_PROBE_STATE_CLEAN;
	snprintf(v.label, sizeof v.label, "Vol");
	v.label_len = 3;
	return v;
}

static ntfsmac_session_begin_in begin_in(const ntfsmac_probe_verdict *v, ntfsmac_policy_decision d)
{
	ntfsmac_session_begin_in in;

	memset(&in, 0, sizeof in);
	in.bsd = "disk4s1";
	in.verdict = v;
	in.decision = d;
	in.probe_fd = 3; /* the fake treats fd 3 as the already open descriptor A */
	fill_block(in.block0, 0);
	snprintf(in.media_path, sizeof in.media_path, "IODeviceTree:/pci/usb@1");
	return in;
}

/* Descriptor A is open before begin() is called. */
static void open_a(fake *f)
{
	int fd = f_open(f, "disk4s1", false);

	CHECK_EQ_INT(fd, 3);
	f->open_ro_calls = 0; /* A does not count as one of the session's opens */
	f->a_established = true;
}

static ntfsmac_session_config cfg0(void)
{
	ntfsmac_session_config c;

	memset(&c, 0, sizeof c);
	return c;
}

static bool entry_is(fake *f, ntfsmac_journal_marker m, ntfsmac_journal_state st)
{
	ntfsmac_journal_entry e;

	return ntfsmac_journal_get(f->j, SERIAL, 131072, &e) == NTFSMAC_JOURNAL_OK && e.marker == m &&
	       e.state == st;
}

static bool entry_exists(fake *f)
{
	ntfsmac_journal_entry e;

	return ntfsmac_journal_get(f->j, SERIAL, 131072, &e) == NTFSMAC_JOURNAL_OK;
}

typedef struct begun {
	ntfsmac_session *s;
	ntfsmac_session_start res;
	ntfsmac_dmn_status st;
} begun;

static begun do_begin(fake *f, const ntfsmac_session_config *cfg, ntfsmac_policy_decision d)
{
	ntfsmac_probe_verdict v = clean_verdict();
	ntfsmac_session_begin_in in = begin_in(&v, d);
	begun b;

	memset(&b, 0, sizeof b);
	open_a(f);
	b.st = ntfsmac_session_begin(&g_sys, cfg, f->j, &in, &b.s, &b.res);
	return b;
}

static ntfsmac_errwatch channel(const char *text, bool eof)
{
	ntfsmac_errwatch w;

	ntfsmac_errwatch_init(&w);
	ntfsmac_errwatch_feed(&w, text, strlen(text));
	if (eof)
		ntfsmac_errwatch_eof(&w);
	return w;
}

/* ---------------- 01 ---------------- */

static void test_policy_mode(void)
{
	ntfsmac_session_config c = cfg0();
	ntfsmac_session_config ro = cfg0();

	ro.automount_ro = true;
	CHECK_EQ_INT(ntfsmac_session_policy_mode(&c, true, false), NTFSMAC_POLICY_MODE_AUTO);
	CHECK_EQ_INT(ntfsmac_session_policy_mode(&c, true, true), NTFSMAC_POLICY_MODE_RO);
	CHECK_EQ_INT(ntfsmac_session_policy_mode(&c, false, true), NTFSMAC_POLICY_MODE_RO);
	/* automount_mode=ro: an automatic attach is ro; a `mount` request for rw is not */
	CHECK_EQ_INT(ntfsmac_session_policy_mode(&ro, true, false), NTFSMAC_POLICY_MODE_RO);
	CHECK_EQ_INT(ntfsmac_session_policy_mode(&ro, true, true), NTFSMAC_POLICY_MODE_RO);
	CHECK_EQ_INT(ntfsmac_session_policy_mode(&ro, false, false), NTFSMAC_POLICY_MODE_AUTO);
	CHECK_EQ_INT(ntfsmac_session_policy_mode(NULL, true, false), NTFSMAC_POLICY_MODE_AUTO);
}

/* ---------------- 02 ---------------- */

static void test_bind_table(void)
{
	ntfsmac_disk_info d;

	memset(&d, 0, sizeof d);
	snprintf(d.bsd_name, sizeof d.bsd_name, "disk4s1");
	snprintf(d.media_path, sizeof d.media_path, "P1");
	CHECK_EQ_INT(ntfsmac_bind_check_disk("disk4s1", "P1", &d, NTFSMAC_DMN_OK), NTFSMAC_BIND_OK);
	/* not the object the probe ran on: another name, another media path */
	CHECK_EQ_INT(ntfsmac_bind_check_disk("disk5s1", "P1", &d, NTFSMAC_DMN_OK),
		     NTFSMAC_BIND_DISK_CHANGED);
	CHECK_EQ_INT(ntfsmac_bind_check_disk("disk4s1", "P2", &d, NTFSMAC_DMN_OK),
		     NTFSMAC_BIND_DISK_CHANGED);
	/* a mount: Apple's driver is the one that leads to ro, any other is a failure */
	d.has_volume_path = true;
	snprintf(d.volume_kind, sizeof d.volume_kind, "ntfs");
	CHECK_EQ_INT(ntfsmac_bind_check_disk("disk4s1", "P1", &d, NTFSMAC_DMN_OK),
		     NTFSMAC_BIND_APPLE_MOUNTED);
	snprintf(d.volume_kind, sizeof d.volume_kind, "ufsd_NTFS");
	CHECK_EQ_INT(ntfsmac_bind_check_disk("disk4s1", "P1", &d, NTFSMAC_DMN_OK),
		     NTFSMAC_BIND_OTHER_MOUNTED);
	d.volume_kind[0] = '\0';
	CHECK_EQ_INT(ntfsmac_bind_check_disk("disk4s1", "P1", &d, NTFSMAC_DMN_OK),
		     NTFSMAC_BIND_OTHER_MOUNTED);
	/* no answer, no object, a NULL: fail closed */
	CHECK_EQ_INT(ntfsmac_bind_check_disk("disk4s1", "P1", &d, NTFSMAC_DMN_E_NOTIMPL),
		     NTFSMAC_BIND_NO_ANSWER);
	CHECK_EQ_INT(ntfsmac_bind_check_disk("disk4s1", "P1", NULL, NTFSMAC_DMN_OK),
		     NTFSMAC_BIND_NO_ANSWER);
	CHECK_EQ_INT(ntfsmac_bind_check_disk(NULL, "P1", &d, NTFSMAC_DMN_OK), NTFSMAC_BIND_NO_ANSWER);
	{
		ntfsmac_dev_id a = {1, 2}, b = {1, 2}, c = {1, 3};

		CHECK(ntfsmac_bind_same_device(&a, &b));
		CHECK(!ntfsmac_bind_same_device(&a, &c));
		CHECK(!ntfsmac_bind_same_device(NULL, &c));
	}
	CHECK(strcmp(ntfsmac_bind_result_name(NTFSMAC_BIND_OK), "ok") == 0);
}

static uint64_t g_live_id;
static unsigned g_live_calls;

static bool live_cb(void *ctx, uint64_t id)
{
	(void)ctx;
	g_live_calls++;
	return g_live_id != 0 && id == g_live_id;
}

/* ---------------- 03 ---------------- */

static void test_decide(void)
{
	fake f;
	ntfsmac_session_config c = cfg0();
	ntfsmac_probe_verdict v = clean_verdict();
	ntfsmac_session_decision d;
	ntfsmac_journal_entry e;

	fake_init(&f);
	/* no entry: rw */
	CHECK_EQ_INT(ntfsmac_session_decide(&c, f.j, &v, true, false, NOW0, NULL, NULL, &d), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(d.policy.decision, NTFSMAC_POLICY_RW);
	/* automount_mode=ro: the same volume, ro, nothing changed in the journal */
	c.automount_ro = true;
	CHECK_EQ_INT(ntfsmac_session_decide(&c, f.j, &v, true, false, NOW0, NULL, NULL, &d), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(d.policy.decision, NTFSMAC_POLICY_RO);
	CHECK_EQ_INT(d.policy.note, NTFSMAC_POLICY_NOTE_MODE_RO_REQUESTED);
	CHECK(!entry_exists(&f));
	/* ... and a `mount` request for rw is rw again */
	CHECK_EQ_INT(ntfsmac_session_decide(&c, f.j, &v, false, false, NOW0, NULL, NULL, &d), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(d.policy.decision, NTFSMAC_POLICY_RW);
	c.automount_ro = false;

	/* an entry whose marker is set, LSN unchanged: ro, kept, whether the rule is on or off */
	memset(&e, 0, sizeof e);
	e.serial = SERIAL;
	snprintf(e.label, sizeof e.label, "Vol");
	e.total_sectors = v.total_sectors;
	e.session_id = 77;
	e.created = e.updated = NOW0;
	e.logfile_lsn_at_start = v.logfile_lsn;
	e.marker = NTFSMAC_JOURNAL_MARKER_PENDING;
	CHECK_EQ_INT(ntfsmac_journal_create(f.j, &e), NTFSMAC_JOURNAL_OK);
	e.marker = NTFSMAC_JOURNAL_MARKER_SET;
	CHECK_EQ_INT(ntfsmac_journal_update(f.j, &e), NTFSMAC_JOURNAL_OK);
	c.evidence_rule_enabled = true;
	CHECK_EQ_INT(ntfsmac_session_decide(&c, f.j, &v, true, false, NOW0, NULL, NULL, &d), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(d.policy.decision, NTFSMAC_POLICY_RO);
	CHECK(!d.evidence_removed && entry_exists(&f));

	/* I-DMN-20: the LSN moved and the flag is clear: rule on: ro this time, entry gone */
	v.logfile_lsn = 0x9999;
	c.evidence_rule_enabled = false;
	CHECK_EQ_INT(ntfsmac_session_decide(&c, f.j, &v, true, false, NOW0, NULL, NULL, &d), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(d.policy.decision, NTFSMAC_POLICY_RO); /* rule off: kept */
	CHECK(!d.evidence_removed && entry_exists(&f));
	c.evidence_rule_enabled = true;
	CHECK_EQ_INT(ntfsmac_session_decide(&c, f.j, &v, true, false, NOW0, NULL, NULL, &d), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(d.policy.decision, NTFSMAC_POLICY_RO); /* never rw on the attach where it fires */
	CHECK_EQ_INT(d.policy.note, NTFSMAC_POLICY_NOTE_EXTERNAL_CHECK);
	CHECK(d.evidence_removed && !entry_exists(&f));
	/* the next decision has no entry: rw */
	CHECK_EQ_INT(ntfsmac_session_decide(&c, f.j, &v, false, false, NOW0 + 1, NULL, NULL, &d), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(d.policy.decision, NTFSMAC_POLICY_RW);
	fake_done(&f);

	/* a corrupt entry is never repaired silently: ro, and the evidence rule does not fire */
	fake_init(&f);
	{
		char path[300];
		FILE *fp;

		snprintf(path, sizeof path, "%s/%016llX", g_jdir, (unsigned long long)SERIAL);
		fp = fopen(path, "w");
		CHECK(fp != NULL);
		if (fp != NULL) {
			fputs("garbage that is not an entry", fp);
			fclose(fp);
			chmod(path, 0600);
		}
	}
	c.evidence_rule_enabled = true;
	v.logfile_lsn = 0x9999;
	CHECK_EQ_INT(ntfsmac_session_decide(&c, f.j, &v, true, false, NOW0, NULL, NULL, &d), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(d.policy.decision, NTFSMAC_POLICY_RO);
	CHECK(d.entry_corrupt && !d.evidence_removed);
	fake_done(&f);

	/* rule 2 never removes the entry of a live session (a clone of a volume mounted rw) */
	fake_init(&f);
	memset(&e, 0, sizeof e);
	e.serial = SERIAL;
	snprintf(e.label, sizeof e.label, "Vol");
	e.total_sectors = v.total_sectors;
	e.session_id = 4242;
	e.created = e.updated = NOW0;
	e.logfile_lsn_at_start = 0x1234;
	e.marker = NTFSMAC_JOURNAL_MARKER_PENDING;
	CHECK_EQ_INT(ntfsmac_journal_create(f.j, &e), NTFSMAC_JOURNAL_OK);
	e.marker = NTFSMAC_JOURNAL_MARKER_SET;
	CHECK_EQ_INT(ntfsmac_journal_update(f.j, &e), NTFSMAC_JOURNAL_OK);
	c.evidence_rule_enabled = true;
	v.logfile_lsn = 0x9999; /* the evidence holds */
	g_live_id = 4242;
	g_live_calls = 0;
	CHECK_EQ_INT(ntfsmac_session_decide(&c, f.j, &v, true, false, NOW0, live_cb, NULL, &d), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(g_live_calls, 1); /* session_live is asked once per decision */
	CHECK_EQ_INT(d.policy.decision, NTFSMAC_POLICY_RO);
	CHECK(!d.evidence_removed && entry_exists(&f));
	CHECK(d.serial_live); /* same serial as a disk that is mounted rw: reported as such, not as a dirty volume */
	g_live_id = 0; /* the session is over: the rule fires */
	CHECK_EQ_INT(ntfsmac_session_decide(&c, f.j, &v, true, false, NOW0, live_cb, NULL, &d), NTFSMAC_DMN_OK);
	CHECK(d.evidence_removed && !entry_exists(&f));
	CHECK(!d.serial_live);
	fake_done(&f);

	/* a leftover entry of an ended session (no live session) is not a serial clash */
	fake_init(&f);
	memset(&e, 0, sizeof e);
	e.serial = SERIAL;
	snprintf(e.label, sizeof e.label, "Vol");
	e.total_sectors = v.total_sectors;
	e.session_id = 4243;
	e.created = e.updated = NOW0;
	e.logfile_lsn_at_start = 0x1234;
	e.marker = NTFSMAC_JOURNAL_MARKER_PENDING;
	CHECK_EQ_INT(ntfsmac_journal_create(f.j, &e), NTFSMAC_JOURNAL_OK);
	c.evidence_rule_enabled = false;
	g_live_id = 4242;
	CHECK_EQ_INT(ntfsmac_session_decide(&c, f.j, &v, true, false, NOW0, live_cb, NULL, &d), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(d.policy.decision, NTFSMAC_POLICY_RO);
	CHECK(!d.serial_live);
	g_live_id = 4243;
	CHECK_EQ_INT(ntfsmac_session_decide(&c, f.j, &v, true, false, NOW0, live_cb, NULL, &d), NTFSMAC_DMN_OK);
	CHECK(d.serial_live);
	/* same_serial replaces only the notes that the entry produced; every other note keeps its reason */
	{
		static const ntfsmac_policy_note own[] = { NTFSMAC_POLICY_NOTE_EXTERNAL_CHECK,
							   NTFSMAC_POLICY_NOTE_UNCLEAN_SESSION,
							   NTFSMAC_POLICY_NOTE_DIRTY_FLAG_JOURNAL };
		static const ntfsmac_policy_note keep[] = {
			NTFSMAC_POLICY_NOTE_NONE,        NTFSMAC_POLICY_NOTE_MODE_RO_REQUESTED,
			NTFSMAC_POLICY_NOTE_DIRTY_FLAG,  NTFSMAC_POLICY_NOTE_UNCLEAN_LOG,
			NTFSMAC_POLICY_NOTE_HIBERNATED,  NTFSMAC_POLICY_NOTE_UNKNOWN,
			NTFSMAC_POLICY_NOTE_REFUSED,     NTFSMAC_POLICY_NOTE_MIRROR_SPLIT,
			NTFSMAC_POLICY_NOTE_NAMES_NOT_NFC
		};
		ntfsmac_session_decision sd;
		size_t i;

		memset(&sd, 0, sizeof sd);
		sd.serial_live = true;
		for (i = 0; i < sizeof own / sizeof own[0]; i++) {
			sd.policy.note = own[i];
			CHECK(ntfsmac_session_serial_token(&sd) != NULL &&
			      strcmp(ntfsmac_session_serial_token(&sd), "same_serial") == 0);
		}
		for (i = 0; i < sizeof keep / sizeof keep[0]; i++) {
			sd.policy.note = keep[i];
			CHECK(ntfsmac_session_serial_token(&sd) == NULL);
		}
		sd.serial_live = false;
		sd.policy.note = NTFSMAC_POLICY_NOTE_UNCLEAN_SESSION;
		CHECK(ntfsmac_session_serial_token(&sd) == NULL);
		CHECK(ntfsmac_session_serial_token(NULL) == NULL);
	}
	/* the text names the serial, and neither the journal nor hibernation */
	{
		const char *t = ntfsmac_report_reason_text("same_serial");

		CHECK(strstr(t, "serial") != NULL && strstr(t, "journal") == NULL && strstr(t, "hibernat") == NULL &&
		      strstr(t, "chkdsk") == NULL);
	}
	g_live_id = 0;
	fake_done(&f);

	/* arguments */
	fake_init(&f);
	CHECK_EQ_INT(ntfsmac_session_decide(NULL, f.j, &v, true, false, NOW0, NULL, NULL, &d), NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_session_decide(&c, NULL, &v, true, false, NOW0, NULL, NULL, &d), NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_session_decide(&c, f.j, NULL, true, false, NOW0, NULL, NULL, &d), NTFSMAC_DMN_E_INVAL);
	CHECK_EQ_INT(ntfsmac_session_decide(&c, f.j, &v, true, false, NOW0, NULL, NULL, NULL), NTFSMAC_DMN_E_INVAL);
	fake_done(&f);
}

/* ---------------- 04 ---------------- */

/* I-DMN-22 at module level, ordering of ARCHITECTURE.md section 3.6 "Ordering in the daemon". */
static void test_rw_happy(void)
{
	fake f;
	ntfsmac_session_config c = cfg0();
	begun b;
	ntfsmac_journal_entry e;

	fake_init(&f);
	b = do_begin(&f, &c, NTFSMAC_POLICY_RW);
	CHECK_EQ_INT(b.st, NTFSMAC_DMN_OK);
	CHECK(b.s != NULL);
	CHECK_EQ_INT(b.res.mode, NTFSMAC_VMODE_RW);
	CHECK_EQ_INT(b.res.reason, NTFSMAC_SREASON_NONE);
	CHECK_EQ_INT(ntfsmac_session_mode(b.s), NTFSMAC_VMODE_RW);
	CHECK(ntfsmac_session_device_fd(b.s) >= 3);
	CHECK(b.res.session_id == 1000 && ntfsmac_session_id(b.s) == 1000);
	CHECK(ntfsmac_session_owns_entry(b.s));

	/* the entry existed, pending, before the writable descriptor was opened, and A was open */
	CHECK(f.entry_at_open_rw);
	CHECK(f.a_open_at_open_rw);
	/* volmark ran with A closed and only B open, on the pending entry, after the bind */
	CHECK_EQ_INT(f.vm_calls, 1);
	CHECK_EQ_INT(f.vm_ops[0], NTFSMAC_VOLMARK_SET);
	CHECK_EQ_INT(f.marker_at_volmark, NTFSMAC_JOURNAL_MARKER_PENDING);
	CHECK_EQ_INT(f.vm_open_fds, 1);
	CHECK_EQ_INT(f.vm_run[0].device_fd, ntfsmac_session_device_fd(b.s));
	CHECK(f.vm_run[0].serial == SERIAL && f.vm_run[0].total_sectors == 131072);
	CHECK_EQ_INT(f.vm_run[0].timeout_s, 20);
	CHECK(pos(&f, "describe") < pos(&f, "open_rw"));
	CHECK(pos(&f, "open_rw") < pos(&f, "close_ro"));
	CHECK(pos(&f, "close_ro") < pos(&f, "volmark_set"));
	/* after it the journal says `set`; the worker is started only now (the caller's step) */
	CHECK(entry_is(&f, NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_OPEN));
	CHECK_EQ_INT(ntfsmac_journal_get(f.j, SERIAL, 131072, &e), NTFSMAC_JOURNAL_OK);
	CHECK(e.session_id == 1000 && e.logfile_lsn_at_start == 0x1234 && e.total_sectors == 131072);
	CHECK(strcmp(e.label, "Vol") == 0);
	CHECK_EQ_INT(count_open(&f), 1);
	ntfsmac_session_free(b.s);
	CHECK_EQ_INT(count_open(&f), 0);
	/* a freed session without end(): the entry and the marker stay (silence is not success) */
	CHECK(entry_is(&f, NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_OPEN));
	fake_done(&f);

	/* a timeout of the daemon's choosing reaches volmark */
	fake_init(&f);
	c.volmark_timeout_s = 7;
	b = do_begin(&f, &c, NTFSMAC_POLICY_RW);
	CHECK_EQ_INT(f.vm_run[0].timeout_s, 7);
	ntfsmac_session_free(b.s);
	fake_done(&f);
}

/* ---------------- 05 ---------------- */

static void test_ro(void)
{
	fake f;
	ntfsmac_session_config c = cfg0();
	begun b;

	/* a ro decision: A itself, flags reset, bytes read again; no entry, no volmark, no rw open */
	fake_init(&f);
	b = do_begin(&f, &c, NTFSMAC_POLICY_RO);
	CHECK_EQ_INT(b.st, NTFSMAC_DMN_OK);
	CHECK_EQ_INT(b.res.mode, NTFSMAC_VMODE_RO);
	CHECK_EQ_INT(b.res.reason, NTFSMAC_SREASON_NONE);
	CHECK_EQ_INT(ntfsmac_session_device_fd(b.s), 3);
	CHECK_EQ_INT(f.flags_calls, 1);
	CHECK(!entry_exists(&f));
	CHECK_EQ_INT(f.vm_calls, 0);
	CHECK_EQ_INT(f.open_rw_calls, 0);
	CHECK_EQ_INT(ntfsmac_session_id(b.s), 0);
	CHECK(!ntfsmac_session_owns_entry(b.s));
	{
		ntfsmac_session_end_result r;
		ntfsmac_errwatch w = channel("", true);

		CHECK_EQ_INT(ntfsmac_session_end(b.s, &w, true, 0, false, &r), NTFSMAC_DMN_OK);
		CHECK_EQ_INT(r.report, NTFSMAC_SREPORT_RO_CLOSED);
		CHECK_EQ_INT(count_open(&f), 0);
		/* a second end is refused */
		CHECK_EQ_INT(ntfsmac_session_end(b.s, &w, true, 0, false, &r), NTFSMAC_DMN_E_STATE);
	}
	ntfsmac_session_free(b.s);
	fake_done(&f);

	/* the first 4096 bytes through A differ from the probe-time bytes: Failed */
	fake_init(&f);
	f.ro_block_variant = 1;
	b = do_begin(&f, &c, NTFSMAC_POLICY_RO);
	CHECK_EQ_INT(b.st, NTFSMAC_DMN_E_DENIED);
	CHECK(b.s == NULL);
	CHECK_EQ_INT(b.res.failed, NTFSMAC_VREASON_BINDING);
	CHECK_EQ_INT(b.res.mode, NTFSMAC_VMODE_NONE);
	CHECK_EQ_INT(count_open(&f), 0);
	fake_done(&f);

	/* the flags could not be reset */
	fake_init(&f);
	f.fail_flags = true;
	b = do_begin(&f, &c, NTFSMAC_POLICY_RO);
	CHECK_EQ_INT(b.st, NTFSMAC_DMN_E_DENIED);
	CHECK_EQ_INT(count_open(&f), 0);
	fake_done(&f);
}

/* ---------------- 06 ---------------- */

static void test_binding(void)
{
	fake f;
	ntfsmac_session_config c = cfg0();
	begun b;

	/* I-DMN-25 (b): another driver has the disk mounted: Failed, no rw open, no entry */
	fake_init(&f);
	f.disk.has_volume_path = true;
	snprintf(f.disk.volume_kind, sizeof f.disk.volume_kind, "ufsd_NTFS");
	b = do_begin(&f, &c, NTFSMAC_POLICY_RW);
	CHECK_EQ_INT(b.st, NTFSMAC_DMN_E_DENIED);
	CHECK_EQ_INT(b.res.failed, NTFSMAC_VREASON_BINDING);
	CHECK_EQ_INT(f.open_rw_calls, 0);
	CHECK(!entry_exists(&f));
	CHECK_EQ_INT(count_open(&f), 0);
	fake_done(&f);

	/* I-DMN-25 (b): only Apple's read-only driver has it: ro on A, no rw open, no entry */
	fake_init(&f);
	f.disk.has_volume_path = true;
	snprintf(f.disk.volume_kind, sizeof f.disk.volume_kind, "ntfs");
	b = do_begin(&f, &c, NTFSMAC_POLICY_RW);
	CHECK_EQ_INT(b.st, NTFSMAC_DMN_OK);
	CHECK_EQ_INT(b.res.mode, NTFSMAC_VMODE_RO);
	CHECK_EQ_INT(b.res.reason, NTFSMAC_SREASON_OTHER_DRIVER);
	CHECK_EQ_INT(f.open_rw_calls, 0);
	CHECK(!entry_exists(&f));
	CHECK_EQ_INT(f.vm_calls, 0);
	ntfsmac_session_free(b.s);
	fake_done(&f);

	/* I-DMN-25 (a): the disk is not the object that was probed (BSD name reused, media path) */
	fake_init(&f);
	snprintf(f.disk.media_path, sizeof f.disk.media_path, "IODeviceTree:/pci/usb@2");
	b = do_begin(&f, &c, NTFSMAC_POLICY_RW);
	CHECK_EQ_INT(b.st, NTFSMAC_DMN_E_DENIED);
	CHECK_EQ_INT(f.open_rw_calls, 0);
	CHECK(!entry_exists(&f));
	fake_done(&f);

	/* the description cannot be had: fail closed, ro decisions too */
	fake_init(&f);
	f.describe_status = NTFSMAC_DMN_E_NOTIMPL;
	b = do_begin(&f, &c, NTFSMAC_POLICY_RO);
	CHECK_EQ_INT(b.st, NTFSMAC_DMN_E_DENIED);
	CHECK_EQ_INT(count_open(&f), 0);
	fake_done(&f);

	/* I-DMN-25 (a): B is another device than A (swapped between probe and open) */
	fake_init(&f);
	f.id_rw = (ntfsmac_dev_id){0x2000, 1};
	b = do_begin(&f, &c, NTFSMAC_POLICY_RW);
	CHECK_EQ_INT(b.st, NTFSMAC_DMN_OK);
	CHECK_EQ_INT(b.res.mode, NTFSMAC_VMODE_RO);
	CHECK_EQ_INT(b.res.reason, NTFSMAC_SREASON_BINDING);
	CHECK_EQ_INT(f.vm_calls, 0);                 /* volmark never ran */
	CHECK(entry_is(&f, NTFSMAC_JOURNAL_MARKER_FAILED, NTFSMAC_JOURNAL_STATE_OPEN));
	CHECK_EQ_INT(count_open(&f), 1);             /* only the fresh read-only descriptor */
	CHECK(!f.fd_rw[ntfsmac_session_device_fd(b.s)]);
	CHECK_EQ_INT(f.open_rw_calls, 1);            /* the one that was refused, not kept */
	ntfsmac_session_free(b.s);
	fake_done(&f);

	/* I-DMN-25 (a): same device but the first 4096 bytes through B differ (disk swapped) */
	fake_init(&f);
	f.rw_block_variant = 5;
	b = do_begin(&f, &c, NTFSMAC_POLICY_RW);
	CHECK_EQ_INT(b.res.mode, NTFSMAC_VMODE_RO);
	CHECK_EQ_INT(b.res.reason, NTFSMAC_SREASON_BINDING);
	CHECK_EQ_INT(f.vm_calls, 0);
	CHECK(entry_is(&f, NTFSMAC_JOURNAL_MARKER_FAILED, NTFSMAC_JOURNAL_STATE_OPEN));
	ntfsmac_session_free(b.s);
	fake_done(&f);

	/* the writable open fails: ro, marker failed */
	fake_init(&f);
	f.fail_open_rw = true;
	b = do_begin(&f, &c, NTFSMAC_POLICY_RW);
	CHECK_EQ_INT(b.res.mode, NTFSMAC_VMODE_RO);
	CHECK(entry_is(&f, NTFSMAC_JOURNAL_MARKER_FAILED, NTFSMAC_JOURNAL_STATE_OPEN));
	ntfsmac_session_free(b.s);
	fake_done(&f);

	/* the fallback descriptor is bound again: if it is not the same volume, Failed */
	fake_init(&f);
	f.id_rw = (ntfsmac_dev_id){0x2000, 1};
	f.ro2_block_variant = 9;
	/* the first read-only descriptor is A (variant 0); the second is the fallback */
	b = do_begin(&f, &c, NTFSMAC_POLICY_RW);
	CHECK_EQ_INT(b.st, NTFSMAC_DMN_E_DENIED);
	CHECK(b.s == NULL);
	CHECK_EQ_INT(count_open(&f), 0);
	CHECK(entry_is(&f, NTFSMAC_JOURNAL_MARKER_FAILED, NTFSMAC_JOURNAL_STATE_OPEN));
	fake_done(&f);

	fake_init(&f);
	f.id_rw = (ntfsmac_dev_id){0x2000, 1};
	f.fail_open_ro = false;
	{
		/* the fallback open itself fails */
		ntfsmac_probe_verdict v = clean_verdict();
		ntfsmac_session_begin_in in = begin_in(&v, NTFSMAC_POLICY_RW);
		ntfsmac_session *s = NULL;
		ntfsmac_session_start res;
		ntfsmac_dmn_status st;

		open_a(&f);
		f.fail_open_ro = true;
		st = ntfsmac_session_begin(&g_sys, &c, f.j, &in, &s, &res);
		CHECK_EQ_INT(st, NTFSMAC_DMN_E_DENIED);
		CHECK(s == NULL);
		CHECK_EQ_INT(count_open(&f), 0);
	}
	fake_done(&f);

	/* bad arguments close the probe descriptor and report E_INVAL */
	fake_init(&f);
	{
		ntfsmac_probe_verdict v = clean_verdict();
		ntfsmac_session_begin_in in = begin_in(&v, NTFSMAC_POLICY_REFUSE);
		ntfsmac_session *s = (ntfsmac_session *)1;
		ntfsmac_session_start res;

		open_a(&f);
		CHECK_EQ_INT(ntfsmac_session_begin(&g_sys, &c, f.j, &in, &s, &res), NTFSMAC_DMN_E_INVAL);
		CHECK(s == NULL);
		CHECK_EQ_INT(count_open(&f), 0);
		open_a(&f);
		in.decision = NTFSMAC_POLICY_RW;
		in.bsd = "/dev/rdisk4";
		CHECK_EQ_INT(ntfsmac_session_begin(&g_sys, &c, f.j, &in, &s, &res), NTFSMAC_DMN_E_INVAL);
		CHECK_EQ_INT(count_open(&f), 0);
		/* a broken system table: the descriptor is still closed */
		{
			ntfsmac_session_sys bad = g_sys;
			int fd = open("/dev/null", O_RDONLY);

			bad.volmark = NULL;
			in.bsd = "disk4s1";
			in.probe_fd = fd;
			CHECK_EQ_INT(ntfsmac_session_begin(&bad, &c, f.j, &in, &s, &res), NTFSMAC_DMN_E_INVAL);
			CHECK(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
			in.probe_fd = 3;
		}
		in.bsd = "disk4s1;rm";
		open_a(&f);
		CHECK_EQ_INT(ntfsmac_session_begin(&g_sys, &c, f.j, &in, &s, &res), NTFSMAC_DMN_E_INVAL);
		CHECK_EQ_INT(count_open(&f), 0);
		CHECK_EQ_INT(f.open_rw_calls, 0);
	}
	fake_done(&f);
}

/* ---------------- 06b ---------------- */

/* repair --clear-dirty: the bound marker write against what was captured before the probe */
static void test_marker_write(void)
{
	fake f;
	ntfsmac_probe_verdict v = clean_verdict();
	ntfsmac_session_begin_in in;
	int ec = 99;

	/* the good case: A closed after B is bound, volmark clear-repair runs on B, bytes checked again */
	fake_init(&f);
	in = begin_in(&v, NTFSMAC_POLICY_RW);
	open_a(&f);
	CHECK_EQ_INT(ntfsmac_session_marker_write(&g_sys, &in, NTFSMAC_VOLMARK_CLEAR_REPAIR, 20, &ec), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(f.vm_calls, 1);
	CHECK_EQ_INT(f.vm_ops[0], NTFSMAC_VOLMARK_CLEAR_REPAIR);
	CHECK_EQ_INT(f.vm_open_fds, 1); /* A was closed before volmark ran */
	CHECK(pos(&f, "close_ro") < pos(&f, "volmark_repair"));
	CHECK_EQ_INT(count_open(&f), 0);
	fake_done(&f);

	/* the DAMediaPath at probe time is not the one now (the disk was swapped): nothing is opened rw */
	fake_init(&f);
	in = begin_in(&v, NTFSMAC_POLICY_RW);
	snprintf(in.media_path, sizeof in.media_path, "IODeviceTree:/pci/usb@9");
	open_a(&f);
	CHECK_EQ_INT(ntfsmac_session_marker_write(&g_sys, &in, NTFSMAC_VOLMARK_CLEAR_REPAIR, 20, &ec), NTFSMAC_DMN_E_DENIED);
	CHECK_EQ_INT(f.open_rw_calls + f.vm_calls, 0);
	CHECK_EQ_INT(count_open(&f), 0);
	fake_done(&f);

	/* a mount (any driver), another device, other bytes: refused, descriptors closed */
	fake_init(&f);
	in = begin_in(&v, NTFSMAC_POLICY_RW);
	f.disk.has_volume_path = true;
	snprintf(f.disk.volume_kind, sizeof f.disk.volume_kind, "ntfs");
	open_a(&f);
	CHECK_EQ_INT(ntfsmac_session_marker_write(&g_sys, &in, NTFSMAC_VOLMARK_CLEAR_REPAIR, 20, &ec), NTFSMAC_DMN_E_DENIED);
	CHECK_EQ_INT(f.open_rw_calls + f.vm_calls, 0);
	CHECK_EQ_INT(count_open(&f), 0);
	fake_done(&f);
	fake_init(&f);
	in = begin_in(&v, NTFSMAC_POLICY_RW);
	f.id_rw = (ntfsmac_dev_id){0x2000, 1};
	open_a(&f);
	CHECK_EQ_INT(ntfsmac_session_marker_write(&g_sys, &in, NTFSMAC_VOLMARK_CLEAR_REPAIR, 20, &ec), NTFSMAC_DMN_E_DENIED);
	CHECK_EQ_INT(f.vm_calls, 0);
	CHECK_EQ_INT(count_open(&f), 0);
	fake_done(&f);
	fake_init(&f);
	in = begin_in(&v, NTFSMAC_POLICY_RW);
	f.rw_block_variant = 2;
	open_a(&f);
	CHECK_EQ_INT(ntfsmac_session_marker_write(&g_sys, &in, NTFSMAC_VOLMARK_CLEAR_REPAIR, 20, &ec), NTFSMAC_DMN_E_DENIED);
	CHECK_EQ_INT(f.vm_calls, 0);
	CHECK_EQ_INT(count_open(&f), 0);
	fake_done(&f);

	/* sector 0 changed by volmark: reported as a failure */
	fake_init(&f);
	in = begin_in(&v, NTFSMAC_POLICY_RW);
	f.after_volmark_variant = 3;
	open_a(&f);
	CHECK_EQ_INT(ntfsmac_session_marker_write(&g_sys, &in, NTFSMAC_VOLMARK_CLEAR_REPAIR, 20, &ec), NTFSMAC_DMN_E_IO);
	CHECK_EQ_INT(count_open(&f), 0);
	fake_done(&f);
}

/* ---------------- 07 ---------------- */

static void test_volmark_failures(void)
{
	static const struct {
		ntfsmac_dmn_status st;
		int code;
		ntfsmac_session_reason reason;
		const char *what;
	} cases[] = {
		{NTFSMAC_DMN_E_IO, 10, NTFSMAC_SREASON_MARKER_FAILED, "exit 10: engine refused"},
		{NTFSMAC_DMN_E_IO, 11, NTFSMAC_SREASON_MARKER_FAILED, "exit 11: flag already set"},
		{NTFSMAC_DMN_E_IO, 12, NTFSMAC_SREASON_MARKER_FAILED, "exit 12: write failed"},
		{NTFSMAC_DMN_E_IO, 15, NTFSMAC_SREASON_MARKER_FAILED, "exit 15: boot sector disagrees"},
		{NTFSMAC_DMN_E_IO, 1, NTFSMAC_SREASON_MARKER_FAILED, "exit 1: a test binary"},
		{NTFSMAC_DMN_E_TIMEOUT, -1, NTFSMAC_SREASON_MARKER_FAILED, "timeout before `w`"},
		{NTFSMAC_DMN_E_TIMEOUT, -2, NTFSMAC_SREASON_MARKER_INTERRUPTED, "timeout after `w`"},
		{NTFSMAC_DMN_E_DENIED, -3, NTFSMAC_SREASON_MARKER_FAILED, "not started"},
	};
	size_t i;

	for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		fake f;
		ntfsmac_session_config c = cfg0();
		begun b;

		fake_init(&f);
		f.vm_status[0] = cases[i].st;
		f.vm_exit[0] = cases[i].code;
		b = do_begin(&f, &c, NTFSMAC_POLICY_RW);
		if (b.st != NTFSMAC_DMN_OK || b.s == NULL) {
			fprintf(stderr, "case %s\n", cases[i].what);
			CHECK(false);
			fake_done(&f);
			continue;
		}
		/* never rw: a fresh read-only descriptor, the entry kept with marker=failed */
		CHECK_EQ_INT(b.res.mode, NTFSMAC_VMODE_RO);
		CHECK_EQ_INT(b.res.reason, cases[i].reason);
		CHECK_EQ_INT(count_open(&f), 1);
		CHECK(!f.fd_rw[ntfsmac_session_device_fd(b.s)]);
		CHECK(entry_is(&f, NTFSMAC_JOURNAL_MARKER_FAILED, NTFSMAC_JOURNAL_STATE_OPEN));
		CHECK_EQ_INT(f.vm_calls, 1);
		CHECK(saw(&f, "close_rw") && last_pos(&f, "open_ro") > pos(&f, "close_rw"));
		ntfsmac_session_free(b.s);
		fake_done(&f);
	}
}

/* ---------------- 08 ---------------- */

static void test_sector0_after_volmark(void)
{
	fake f;
	ntfsmac_session_config c = cfg0();
	begun b;

	/* I-DMN-25 (c): volmark exits 0 but sector 0 changed: ro, marker failed, nothing kept open */
	fake_init(&f);
	f.after_volmark_variant = 3;
	b = do_begin(&f, &c, NTFSMAC_POLICY_RW);
	CHECK_EQ_INT(b.st, NTFSMAC_DMN_OK);
	CHECK_EQ_INT(b.res.mode, NTFSMAC_VMODE_RO);
	CHECK_EQ_INT(b.res.reason, NTFSMAC_SREASON_BINDING_AFTER_MARK); /* the flag may be on the disk */
	CHECK(entry_is(&f, NTFSMAC_JOURNAL_MARKER_FAILED, NTFSMAC_JOURNAL_STATE_OPEN));
	CHECK_EQ_INT(count_open(&f), 1);
	CHECK(!f.fd_rw[ntfsmac_session_device_fd(b.s)]);
	ntfsmac_session_free(b.s);
	fake_done(&f);

	/* volmark could not be reaped after SIGKILL: Failed, never ro on a device it may hold */
	fake_init(&f);
	f.vm_status[0] = NTFSMAC_DMN_E_BUSY;
	f.vm_exit[0] = -4;
	b = do_begin(&f, &c, NTFSMAC_POLICY_RW);
	CHECK_EQ_INT(b.st, NTFSMAC_DMN_E_DENIED);
	CHECK(b.s == NULL);
	CHECK_EQ_INT(b.res.failed, NTFSMAC_VREASON_SPAWN);
	CHECK_EQ_INT(count_open(&f), 0);
	CHECK(entry_is(&f, NTFSMAC_JOURNAL_MARKER_FAILED, NTFSMAC_JOURNAL_STATE_OPEN));
	fake_done(&f);
}

/* ---------------- 09 ---------------- */

static void test_journal_refusals(void)
{
	fake f;
	ntfsmac_session_config c = cfg0();
	begun b;
	ntfsmac_journal_entry e;

	/* an entry exists already (a race with another decision): ro, theirs untouched, no B */
	fake_init(&f);
	memset(&e, 0, sizeof e);
	e.serial = SERIAL;
	snprintf(e.label, sizeof e.label, "Old");
	e.total_sectors = 131072;
	e.session_id = 5;
	e.created = e.updated = NOW0 - 100;
	e.logfile_lsn_at_start = 1;
	e.marker = NTFSMAC_JOURNAL_MARKER_PENDING;
	CHECK_EQ_INT(ntfsmac_journal_create(f.j, &e), NTFSMAC_JOURNAL_OK);
	b = do_begin(&f, &c, NTFSMAC_POLICY_RW);
	CHECK_EQ_INT(b.st, NTFSMAC_DMN_OK);
	CHECK_EQ_INT(b.res.mode, NTFSMAC_VMODE_RO);
	CHECK_EQ_INT(b.res.reason, NTFSMAC_SREASON_JOURNAL);
	CHECK_EQ_INT(f.open_rw_calls, 0);
	CHECK_EQ_INT(f.vm_calls, 0);
	CHECK_EQ_INT(ntfsmac_journal_get(f.j, SERIAL, 131072, &e), NTFSMAC_JOURNAL_OK);
	CHECK(e.session_id == 5 && e.marker == NTFSMAC_JOURNAL_MARKER_PENDING);
	CHECK(!ntfsmac_session_owns_entry(b.s)); /* not ours */
	ntfsmac_session_free(b.s);
	fake_done(&f);

	/* a verdict without a log sequence number cannot make an entry: ro */
	fake_init(&f);
	{
		ntfsmac_probe_verdict v = clean_verdict();
		ntfsmac_session_begin_in in;
		ntfsmac_session *s = NULL;
		ntfsmac_session_start res;

		v.has_logfile_lsn = false;
		in = begin_in(&v, NTFSMAC_POLICY_RW);
		open_a(&f);
		CHECK_EQ_INT(ntfsmac_session_begin(&g_sys, &c, f.j, &in, &s, &res), NTFSMAC_DMN_OK);
		CHECK_EQ_INT(res.mode, NTFSMAC_VMODE_RO);
		CHECK_EQ_INT(res.reason, NTFSMAC_SREASON_JOURNAL);
		CHECK(!entry_exists(&f));
		ntfsmac_session_free(s);
	}
	fake_done(&f);

	/* a session id of 0 from the system is refused */
	fake_init(&f);
	f.next_session_id = 0;
	b = do_begin(&f, &c, NTFSMAC_POLICY_RW);
	CHECK_EQ_INT(b.res.mode, NTFSMAC_VMODE_RO);
	CHECK(!entry_exists(&f));
	ntfsmac_session_free(b.s);
	fake_done(&f);
}

/* ---------------- 10 ---------------- */

#if defined(NTFSMAC_TEST_SEAMS)
static void test_marker_disabled(void)
{
	fake f;
	ntfsmac_session_config c = cfg0();
	begun b;
	ntfsmac_session_end_result r;
	ntfsmac_errwatch w = channel("S rw\nU 0\n", true);

	c.marker_disabled = true; /* ADR-006: marker=none */
	fake_init(&f);
	b = do_begin(&f, &c, NTFSMAC_POLICY_RW);
	CHECK_EQ_INT(b.res.mode, NTFSMAC_VMODE_RW);
	CHECK_EQ_INT(f.vm_calls, 0);
	CHECK(entry_is(&f, NTFSMAC_JOURNAL_MARKER_NONE, NTFSMAC_JOURNAL_STATE_OPEN));
	CHECK_EQ_INT(ntfsmac_session_end(b.s, &w, true, 0, false, &r), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(r.report, NTFSMAC_SREPORT_CLEAN);
	CHECK_EQ_INT(f.vm_calls, 0); /* no clear either */
	CHECK(r.entry_removed && !entry_exists(&f));
	ntfsmac_session_free(b.s);
	fake_done(&f);
}
#endif

/* ---------------- 11 ---------------- */

static void test_write_error(void)
{
	fake f;
	ntfsmac_session_config c = cfg0();
	begun b;

	fake_init(&f);
	b = do_begin(&f, &c, NTFSMAC_POLICY_RW);
	CHECK_EQ_INT(ntfsmac_session_note_write_error(b.s), NTFSMAC_DMN_OK);
	CHECK(entry_is(&f, NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_WRITE_ERROR));
	CHECK_EQ_INT(ntfsmac_session_note_write_error(b.s), NTFSMAC_DMN_OK); /* idempotent */
	CHECK(entry_is(&f, NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_WRITE_ERROR));
	CHECK_EQ_INT(ntfsmac_session_note_write_error(NULL), NTFSMAC_DMN_E_INVAL);
	ntfsmac_session_free(b.s);
	fake_done(&f);

	/* a ro session has no entry: nothing to record */
	fake_init(&f);
	b = do_begin(&f, &c, NTFSMAC_POLICY_RO);
	CHECK_EQ_INT(ntfsmac_session_note_write_error(b.s), NTFSMAC_DMN_OK);
	CHECK(!entry_exists(&f));
	ntfsmac_session_free(b.s);
	fake_done(&f);
}

/* ---------------- 12-18: the end of a session ---------------- */

static begun rw_session(fake *f, const ntfsmac_session_config *c)
{
	begun b = do_begin(f, c, NTFSMAC_POLICY_RW);

	CHECK_EQ_INT(b.res.mode, NTFSMAC_VMODE_RW);
	return b;
}

static int count_tomb(uint64_t serial, int64_t when, void *ctx)
{
	(void)serial;
	(void)when;
	(*(unsigned *)ctx)++;
	return 0;
}

static void test_end_clean(void)
{
	fake f;
	ntfsmac_session_config c = cfg0();
	begun b;
	ntfsmac_session_end_result r;
	ntfsmac_errwatch w;
	unsigned n_tomb = 0;

	/* I-DMN-22: the full clean path, in the order of section 4.5 step 3 */
	fake_init(&f);
	b = rw_session(&f, &c);
	w = channel("S rw\nU 0\n", true);
	CHECK_EQ_INT(ntfsmac_session_end(b.s, &w, true, 0, false, &r), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(r.report, NTFSMAC_SREPORT_CLEAN);
	CHECK(r.entry_removed && !r.clear_interrupted);
	CHECK(!entry_exists(&f));
	CHECK_EQ_INT(f.vm_calls, 2);
	CHECK_EQ_INT(f.vm_ops[1], NTFSMAC_VOLMARK_CLEAR);
	CHECK_EQ_INT(f.vm_run[1].device_fd, f.vm_run[0].device_fd);
	CHECK(pos(&f, "sync") < pos(&f, "volmark_clear"));
	CHECK_EQ_INT(count_open(&f), 0);
	/* the entry went to the tombstones, not away */
	CHECK_EQ_INT(ntfsmac_journal_list_tombstones(f.j, count_tomb, &n_tomb), NTFSMAC_JOURNAL_OK);
	CHECK_EQ_INT(n_tomb, 1);
	ntfsmac_session_free(b.s);
	fake_done(&f);

	/* a `W` record: clean, but the report says the drive did not confirm */
	fake_init(&f);
	b = rw_session(&f, &c);
	w = channel("S rw\nW flush 5\nU 0\n", true);
	CHECK_EQ_INT(ntfsmac_session_end(b.s, &w, true, 0, false, &r), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(r.report, NTFSMAC_SREPORT_CLEAN_NO_FLUSH);
	CHECK(r.entry_removed);
	ntfsmac_session_free(b.s);
	fake_done(&f);

	/* the daemon's own drive flush failed: a warning only */
	fake_init(&f);
	f.flush_ok = false;
	b = rw_session(&f, &c);
	w = channel("S rw\nU 0\n", true);
	CHECK_EQ_INT(ntfsmac_session_end(b.s, &w, true, 0, false, &r), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(r.report, NTFSMAC_SREPORT_CLEAN_NO_FLUSH);
	CHECK(r.entry_removed);
	ntfsmac_session_free(b.s);
	fake_done(&f);

	/* a tombstone with the same name exists (same second): the entry stays, "unclean" */
	fake_init(&f);
	b = rw_session(&f, &c);
	w = channel("S rw\nU 0\n", true);
	CHECK_EQ_INT(ntfsmac_session_end(b.s, &w, true, 0, false, &r), NTFSMAC_DMN_OK);
	CHECK(r.entry_removed);
	ntfsmac_session_free(b.s);
	f.next_session_id = 2000;
	b = rw_session(&f, &c);
	CHECK_EQ_INT(ntfsmac_session_end(b.s, &w, true, 0, false, &r), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(r.report, NTFSMAC_SREPORT_UNCLEAN);
	CHECK(!r.entry_removed && entry_exists(&f));
	ntfsmac_session_free(b.s);
	fake_done(&f);
}

static void test_end_unclean(void)
{
	static const struct {
		const char *text;
		bool eof, unmount_first;
		int exit_status;
		const char *what;
	} cases[] = {
		{"", true, true, 0, "no records at all (silence)"},
		{"S rw\nU 0\n", false, true, 0, "no end of file"},
		{"S rw\n", true, true, 0, "no U record"},
		{"U 0\n", true, true, 0, "no S record"},
		{"S ro\nU 0\n", true, true, 0, "S ro in a rw session"},
		{"S rw\nE write 5\nU 1\n", true, true, 0, "an E record"},
		{"S rw\nU 0\n", true, false, 0, "unmount not seen before the exit"},
		{"S rw\nU 0\n", true, true, 1, "worker exit status 1"},
		{"S rw\nU 0\n", true, true, -1, "worker killed"},
		{"S rw\nX junk\nU 0\n", true, true, 0, "a protocol error"},
		{"S rw\nU 0 trailing\n", true, true, 0, "a protocol error in the U record"},
	};
	size_t i;

	for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		fake f;
		ntfsmac_session_config c = cfg0();
		begun b;
		ntfsmac_session_end_result r;
		ntfsmac_errwatch w;

		fake_init(&f);
		b = rw_session(&f, &c);
		w = channel(cases[i].text, cases[i].eof);
		CHECK_EQ_INT(ntfsmac_session_end(b.s, &w, cases[i].unmount_first, cases[i].exit_status, false, &r),
			     NTFSMAC_DMN_OK);
		if (r.report != NTFSMAC_SREPORT_UNCLEAN)
			fprintf(stderr, "case: %s\n", cases[i].what);
		CHECK_EQ_INT(r.report, NTFSMAC_SREPORT_UNCLEAN);
		CHECK(!r.entry_removed);
		CHECK_EQ_INT(f.vm_calls, 1); /* volmark clear never ran: the flag stays on the disk */
		CHECK(!saw(&f, "sync"));
		/* the entry and the marker stay; an E record or a broken channel adds write_error */
		CHECK(entry_exists(&f));
		{
			bool werr = (w.flags & (NTFSMAC_EW_SAW_E | NTFSMAC_EW_PROTOCOL_ERROR)) != 0;

			CHECK(entry_is(&f, NTFSMAC_JOURNAL_MARKER_SET,
				       werr ? NTFSMAC_JOURNAL_STATE_WRITE_ERROR
					    : NTFSMAC_JOURNAL_STATE_OPEN));
		}
		CHECK_EQ_INT(count_open(&f), 0);
		ntfsmac_session_free(b.s);
		fake_done(&f);
	}
}

/*
 * Rule 3 of the clean-session proof through the daemon's rule-3 function (T-M5-05 finding): a
 * mount that vanished with no unmount request of the daemon and no Disk Arbitration report (the
 * macFUSE module was killed) must not be clean, however perfect the worker's records are.
 */
static void test_end_unmount_evidence(void)
{
	static const struct {
		bool mount_gone, requested, da_seen, clean;
		const char *what;
	} cases[] = {
		{true, false, false, false, "module death: mount gone, no request, no DA report"},
		{false, false, false, false, "mount still there"},
		{false, true, false, false, "requested but the mount is still there"},
		{false, false, true, false, "DA report but the mount is still there"},
		{true, true, false, true, "the daemon's own unmount (ntfsmac unmount, eject, shutdown)"},
		{true, false, true, true, "DA report: umount, Finder eject"},
		{true, true, true, true, "request and DA report"},
	};
	size_t i;

	for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		fake f;
		ntfsmac_session_config c = cfg0();
		begun b;
		ntfsmac_session_end_result r;
		ntfsmac_errwatch w;
		bool seen = ntfsmac_unmount_seen(cases[i].mount_gone, cases[i].requested, cases[i].da_seen);

		fake_init(&f);
		b = rw_session(&f, &c);
		w = channel("S rw\nU 0\n", true);
		CHECK_EQ_INT(ntfsmac_session_end(b.s, &w, seen, 0, false, &r), NTFSMAC_DMN_OK);
		if ((r.report == NTFSMAC_SREPORT_CLEAN) != cases[i].clean || r.entry_removed != cases[i].clean)
			fprintf(stderr, "case: %s\n", cases[i].what);
		CHECK_EQ_INT(r.report == NTFSMAC_SREPORT_CLEAN, cases[i].clean);
		CHECK_EQ_INT(r.entry_removed, cases[i].clean);
		CHECK_EQ_INT(entry_exists(&f), !cases[i].clean);
		if (!cases[i].clean) {
			/* not clean: no volmark clear (the marker stays set), the journal entry stays open */
			CHECK_EQ_INT(r.report, NTFSMAC_SREPORT_UNCLEAN);
			CHECK_EQ_INT(f.vm_calls, 1);
			CHECK(entry_is(&f, NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_OPEN));
		}
		ntfsmac_session_free(b.s);
		fake_done(&f);
	}
}

static void test_end_failures(void)
{
	fake f;
	ntfsmac_session_config c = cfg0();
	begun b;
	ntfsmac_session_end_result r;
	ntfsmac_errwatch w = channel("S rw\nU 0\n", true);

	/* force_unclean (a holder of the device that could not be cleared), journal update works:
	 * UNCLEAN, no sync, no clear, the entry stays with state write_error and the marker set */
	fake_init(&f);
	b = rw_session(&f, &c);
	CHECK_EQ_INT(ntfsmac_session_end(b.s, &w, true, 0, true, &r), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(r.report, NTFSMAC_SREPORT_UNCLEAN);
	CHECK(!r.entry_removed);
	CHECK_EQ_INT(f.vm_calls, 1);
	CHECK(entry_is(&f, NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_WRITE_ERROR));
	ntfsmac_session_free(b.s);
	fake_done(&f);

	/* force_unclean and the journal update itself fails: the state stays `open`, which alone
	 * would let the proof pass, but the report is still UNCLEAN and the entry and the marker
	 * stay. The failure comes from a seam that also works as root (the Linux container): a
	 * non-empty directory sits on the temporary name the journal writes to, so the open and
	 * the retry after unlinkat both fail. No permission is involved (M5-F09). */
	{
		char tmpdir[320], inner[340];

		fake_init(&f);
		b = rw_session(&f, &c);
		/* the temporary name of the entry's key: serial and total_sectors (T-M7-13) */
		snprintf(tmpdir, sizeof tmpdir, "%s/.%016llX-%016llX.tmp", g_jdir, (unsigned long long)SERIAL,
			 131072ull);
		snprintf(inner, sizeof inner, "%s/x", tmpdir);
		CHECK_EQ_INT(mkdir(tmpdir, 0700), 0);
		CHECK_EQ_INT(mkdir(inner, 0700), 0);
		CHECK_EQ_INT(ntfsmac_session_note_write_error(b.s), NTFSMAC_DMN_E_IO); /* the premise */
		CHECK_EQ_INT(ntfsmac_session_end(b.s, &w, true, 0, true, &r), NTFSMAC_DMN_OK);
		CHECK_EQ_INT(r.report, NTFSMAC_SREPORT_UNCLEAN);
		CHECK(!r.entry_removed);
		CHECK_EQ_INT(f.vm_calls, 1);
		CHECK_EQ_INT(rmdir(inner), 0);
		CHECK_EQ_INT(rmdir(tmpdir), 0);
		CHECK(entry_is(&f, NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_OPEN));
		ntfsmac_session_free(b.s);
		fake_done(&f);
	}

	/* the daemon's fsync failed: not clean, no clear */
	fake_init(&f);
	f.sync_status = NTFSMAC_DMN_E_IO;
	b = rw_session(&f, &c);
	CHECK_EQ_INT(ntfsmac_session_end(b.s, &w, true, 0, false, &r), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(r.report, NTFSMAC_SREPORT_UNCLEAN);
	CHECK_EQ_INT(f.vm_calls, 1);
	CHECK(entry_exists(&f));
	ntfsmac_session_free(b.s);
	fake_done(&f);

	/* volmark clear fails (exit 13: someone changed the volume): the flag and the entry stay */
	fake_init(&f);
	b = rw_session(&f, &c);
	f.vm_status[1] = NTFSMAC_DMN_E_IO;
	f.vm_exit[1] = 13;
	CHECK_EQ_INT(ntfsmac_session_end(b.s, &w, true, 0, false, &r), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(r.report, NTFSMAC_SREPORT_CLEAR_FAILED);
	CHECK(!r.clear_interrupted && !r.entry_removed);
	CHECK(entry_is(&f, NTFSMAC_JOURNAL_MARKER_SET, NTFSMAC_JOURNAL_STATE_OPEN));
	ntfsmac_session_free(b.s);
	fake_done(&f);

	/* volmark clear cut after `w` (w/d rule): the residual text of section 3.6 */
	fake_init(&f);
	b = rw_session(&f, &c);
	f.vm_status[1] = NTFSMAC_DMN_E_TIMEOUT;
	f.vm_exit[1] = -2;
	CHECK_EQ_INT(ntfsmac_session_end(b.s, &w, true, 0, false, &r), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(r.report, NTFSMAC_SREPORT_CLEAR_FAILED);
	CHECK(r.clear_interrupted);
	CHECK(entry_exists(&f));
	ntfsmac_session_free(b.s);
	fake_done(&f);

	/* before `w` (-1): a failure, but nothing was cut */
	fake_init(&f);
	b = rw_session(&f, &c);
	f.vm_status[1] = NTFSMAC_DMN_E_TIMEOUT;
	f.vm_exit[1] = -1;
	CHECK_EQ_INT(ntfsmac_session_end(b.s, &w, true, 0, false, &r), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(r.report, NTFSMAC_SREPORT_CLEAR_FAILED);
	CHECK(!r.clear_interrupted);
	ntfsmac_session_free(b.s);
	fake_done(&f);

	/* the entry was marked write_error while the session ran: a clean-looking channel is not enough */
	fake_init(&f);
	b = rw_session(&f, &c);
	CHECK_EQ_INT(ntfsmac_session_note_write_error(b.s), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(ntfsmac_session_end(b.s, &w, true, 0, false, &r), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(r.report, NTFSMAC_SREPORT_UNCLEAN);
	CHECK_EQ_INT(f.vm_calls, 1);
	CHECK(entry_exists(&f));
	ntfsmac_session_free(b.s);
	fake_done(&f);

	/* a ro session that fell back after a failed marker never clears or removes anything */
	fake_init(&f);
	f.vm_status[0] = NTFSMAC_DMN_E_IO;
	f.vm_exit[0] = 11;
	b = do_begin(&f, &c, NTFSMAC_POLICY_RW);
	CHECK_EQ_INT(b.res.mode, NTFSMAC_VMODE_RO);
	CHECK_EQ_INT(ntfsmac_session_end(b.s, &w, true, 0, false, &r), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(r.report, NTFSMAC_SREPORT_RO_CLOSED);
	CHECK_EQ_INT(f.vm_calls, 1);
	CHECK(entry_is(&f, NTFSMAC_JOURNAL_MARKER_FAILED, NTFSMAC_JOURNAL_STATE_OPEN));
	ntfsmac_session_free(b.s);
	fake_done(&f);

	/* arguments */
	CHECK_EQ_INT(ntfsmac_session_end(NULL, &w, true, 0, false, &r), NTFSMAC_DMN_E_INVAL);
	fake_init(&f);
	b = rw_session(&f, &c);
	CHECK_EQ_INT(ntfsmac_session_end(b.s, &w, true, 0, false, NULL), NTFSMAC_DMN_E_INVAL);
	/* no channel at all: unclean */
	CHECK_EQ_INT(ntfsmac_session_end(b.s, NULL, true, 0, false, &r), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(r.report, NTFSMAC_SREPORT_UNCLEAN);
	ntfsmac_session_free(b.s);
	ntfsmac_session_free(NULL);
	fake_done(&f);
}

/* the sys facts of the real implementation that need no device: identity and block 0 of a file */
static void test_real_sys(void)
{
	ntfsmac_session_sys_real r;
	char path[300];
	int fd1, fd2, fd3;
	ntfsmac_dev_id a, b, c;
	uint8_t buf[NTFSMAC_BLOCK0_BYTES], buf2[NTFSMAC_BLOCK0_BYTES];
	char other[300];
	bool flush_ok = false;
	FILE *fp;

	ntfsmac_session_sys_real_init(&r, NULL);
	snprintf(path, sizeof path, "%s/image", g_dir);
	snprintf(other, sizeof other, "%s/image2", g_dir);
	fp = fopen(path, "w");
	CHECK(fp != NULL);
	if (fp == NULL)
		return;
	{
		size_t i;

		for (i = 0; i < 2 * NTFSMAC_BLOCK0_BYTES; i++)
			fputc((int)(i & 0xFF), fp);
	}
	fclose(fp);
	fp = fopen(other, "w");
	if (fp != NULL) {
		fputs("x", fp);
		fclose(fp);
	}
	fd1 = open(path, O_RDONLY);
	fd2 = open(path, O_RDWR);
	fd3 = open(other, O_RDONLY);
	CHECK(fd1 >= 0 && fd2 >= 0 && fd3 >= 0);
	CHECK_EQ_INT(r.sys.dev_identity(r.sys.ctx, fd1, &a), 0);
	CHECK_EQ_INT(r.sys.dev_identity(r.sys.ctx, fd2, &b), 0);
	CHECK_EQ_INT(r.sys.dev_identity(r.sys.ctx, fd3, &c), 0);
	CHECK(ntfsmac_bind_same_device(&a, &b));  /* the same file */
	CHECK(!ntfsmac_bind_same_device(&a, &c)); /* another file */
	CHECK_EQ_INT(r.sys.read_block0(r.sys.ctx, fd1, buf), 0);
	CHECK_EQ_INT(r.sys.read_block0(r.sys.ctx, fd2, buf2), 0);
	CHECK(memcmp(buf, buf2, sizeof buf) == 0 && buf[5] == 5);
	CHECK(r.sys.read_block0(r.sys.ctx, fd3, buf) != 0); /* a short file has no block 0 */
	CHECK_EQ_INT(r.sys.reset_flags(r.sys.ctx, fd1), 0);
	CHECK_EQ_INT(r.sys.sync_device(r.sys.ctx, fd2, &flush_ok), NTFSMAC_DMN_OK);
	/* no Disk Arbitration object: the description fails, so every binding fails closed */
	{
		ntfsmac_disk_info di;

		CHECK(r.sys.describe(r.sys.ctx, "disk4s1", &di) != NTFSMAC_DMN_OK);
	}
	/* device names are checked: a path or a raw node never opens */
	CHECK(r.sys.open_device(r.sys.ctx, "../etc/passwd", false) < 0);
	CHECK(r.sys.open_device(r.sys.ctx, "rdisk4", false) < 0);
	CHECK(r.sys.open_device(r.sys.ctx, "/dev/disk4", false) < 0);
	CHECK(r.sys.new_session_id(r.sys.ctx) != 0);
	CHECK(r.sys.now(r.sys.ctx) > NOW0);
	r.sys.close_fd(r.sys.ctx, fd1);
	r.sys.close_fd(r.sys.ctx, fd2);
	r.sys.close_fd(r.sys.ctx, fd3);
}

/* A journal entry of (SERIAL, total_sectors) with marker set: an unclean session of that volume. */
static void unclean_entry(fake *f, uint64_t total_sectors, uint64_t session_id)
{
	ntfsmac_journal_entry e;

	memset(&e, 0, sizeof e);
	e.serial = SERIAL;
	snprintf(e.label, sizeof e.label, "Vol");
	e.total_sectors = total_sectors;
	e.session_id = session_id;
	e.created = e.updated = NOW0;
	e.logfile_lsn_at_start = 0x1234;
	e.marker = NTFSMAC_JOURNAL_MARKER_PENDING;
	CHECK_EQ_INT(ntfsmac_journal_create(f->j, &e), NTFSMAC_JOURNAL_OK);
	e.marker = NTFSMAC_JOURNAL_MARKER_SET;
	CHECK_EQ_INT(ntfsmac_journal_update(f->j, &e), NTFSMAC_JOURNAL_OK);
}

/*
 * U-DMN-SESS-ID (T-M7-13): the decision looks up the entry of the probed volume, serial and
 * total_sectors. Another volume's entry with the same serial (a forged serial, say) gives the
 * decision of "no entry" and stays; the same identity, or an entry of format 1, is read-only as
 * before.
 */
static void test_decide_identity(void)
{
	fake f;
	ntfsmac_session_config c = cfg0();
	ntfsmac_probe_verdict v = clean_verdict();
	ntfsmac_session_decision d;
	ntfsmac_journal_entry e;
	const uint64_t other = 131072 + 2048;

	/* entry of (S, A), probed (S, B): rw, as with no entry; the entry of A is still there */
	fake_init(&f);
	unclean_entry(&f, other, 77);
	c.evidence_rule_enabled = true;
	CHECK_EQ_INT(ntfsmac_session_decide(&c, f.j, &v, true, false, NOW0, NULL, NULL, &d), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(d.policy.decision, NTFSMAC_POLICY_RW);
	CHECK_EQ_INT(d.policy.note, NTFSMAC_POLICY_NOTE_NONE);
	CHECK(!d.evidence_removed && !d.entry_corrupt && !d.serial_live);
	CHECK(ntfsmac_journal_get(f.j, SERIAL, other, &e) == NTFSMAC_JOURNAL_OK && e.session_id == 77 &&
	      e.marker == NTFSMAC_JOURNAL_MARKER_SET);
	CHECK(!entry_exists(&f));
	/* B gets an entry of its own beside it; each one names its own session */
	unclean_entry(&f, 131072, 78);
	CHECK(ntfsmac_journal_get(f.j, SERIAL, other, &e) == NTFSMAC_JOURNAL_OK && e.session_id == 77);
	CHECK(ntfsmac_journal_get(f.j, SERIAL, 131072, &e) == NTFSMAC_JOURNAL_OK && e.session_id == 78);
	/* ... and now B has one: same identity, read-only, unclean session */
	CHECK_EQ_INT(ntfsmac_session_decide(&c, f.j, &v, true, false, NOW0, NULL, NULL, &d), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(d.policy.decision, NTFSMAC_POLICY_RO);
	CHECK_EQ_INT(d.policy.note, NTFSMAC_POLICY_NOTE_UNCLEAN_SESSION);
	fake_done(&f);

	/* a format-1 entry (by serial alone, identity unknown) of another size: read-only, as before */
	fake_init(&f);
	{
		char path[300];
		FILE *fp;

		snprintf(path, sizeof path, "%s/%016llX", g_jdir, (unsigned long long)SERIAL);
		fp = fopen(path, "w");
		CHECK(fp != NULL);
		if (fp != NULL) {
			fprintf(fp,
				"ntfsmac-journal 1\nserial %016llX\nlabel 566f6c\ntotal_sectors %llu\n"
				"session_id 77\ncreated %lld\nupdated %lld\nlogfile_lsn_at_start 4660\n"
				"marker set\nstate open\nend\n",
				(unsigned long long)SERIAL, (unsigned long long)other, (long long)NOW0,
				(long long)NOW0);
			fclose(fp);
			chmod(path, 0600);
		}
	}
	CHECK_EQ_INT(ntfsmac_session_decide(&c, f.j, &v, true, false, NOW0, NULL, NULL, &d), NTFSMAC_DMN_OK);
	CHECK_EQ_INT(d.policy.decision, NTFSMAC_POLICY_RO);
	CHECK_EQ_INT(d.policy.note, NTFSMAC_POLICY_NOTE_UNCLEAN_SESSION);
	CHECK(!d.entry_corrupt);
	CHECK(ntfsmac_journal_get(f.j, SERIAL, 131072, &e) == NTFSMAC_JOURNAL_OK && e.legacy);
	fake_done(&f);
}

int main(void)
{
	make_dir();
	test_decide_identity();
	test_policy_mode();
	test_bind_table();
	test_decide();
	test_rw_happy();
	test_ro();
	test_binding();
	test_marker_write();
	test_volmark_failures();
	test_sector0_after_volmark();
	test_journal_refusals();
#if defined(NTFSMAC_TEST_SEAMS)
	test_marker_disabled();
#endif
	test_write_error();
	test_end_clean();
	test_end_unclean();
	test_end_unmount_evidence();
	test_end_failures();
	test_real_sys();
	rm_tree(g_dir);
	if (g_failures != 0) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	return 0;
}
