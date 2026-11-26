/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * T-M4-16, I-DMN-27 (module level): `repair --sync-mirror` end to end against the real probe and
 * volmark under the sandbox, in a macOS guest (Environment B), run as root by dmn_mirror_vm.sh.
 * It does what the daemon of T-M4-08 will do with the privileged `repair` request: the real
 * ntfsmac_priv_repair() (action `sync_mirror`), the real spawn module and sandbox, the real
 * journal, on NTFS image FILES (never a host disk). What it cannot have without the wired daemon
 * is Disk Arbitration (the disk description is a fake, as in journal_vm) and the XPC door; the
 * five checks of ADR-019 are the dispatcher's and are tested on the host (tests/unit/ctl).
 *
 *   dmn_mirror_vm WORKDIR UID GID SBXDIR PROBE VOLMARK WORKER STUBDIR [CASE]
 *
 * WORKDIR contains, all mode 600 and root's: split.img (`mirror-split`), splitdirty.img
 * (`mirror-split-dirty`), newer.img (`mirror-newer`), splitlog.img (`mirror-split` with an unclean
 * $LogFile) and clean.img (a clean volume). SBXDIR holds the root-owned sandbox profiles.
 * STUBDIR holds links to journal_stub named jstub-exit1 and jstub-sector0.
 *
 * Cases (each prints PASS, FAIL or NOTE lines; exit 0 only if none failed):
 *   refuse    the request is refused, the image hash unchanged, no progress: wrong label, wrong
 *             serial, mounted, a clean volume, mirror-newer, a split with an unclean log; and the
 *             policy table refuses the split at attach (decision refuse, note mirror_split)
 *   repair    mirror-split: accepted; the reply carries the fresh probe state (clean); only bytes
 *             inside the $MFTMirr record 0 changed and it equals the $MFT record; the journal entry
 *             of a session is neither read nor changed; nothing is mounted; the next probe says clean
 *             and the policy decides rw. mirror-split-dirty: accepted, the reply says dirty_flag and
 *             the policy decides ro. A second request is refused (nothing differs any more).
 *   volmark   volmark replaced by a program that exits 1, and by one that changes sector 0: the
 *             request fails or is refused with a fixed word, no mirror byte changed
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "spawn_config.h"

static int g_fails;
static const char *g_work, *g_sbx, *g_probe, *g_volmark, *g_worker, *g_stubdir;
static char g_image[320], g_jdir[320];
static int g_log_fd = -1;
static uid_t g_uid;

#define PASS(...) \
	do { printf("PASS: " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#define FAIL(...) \
	do { printf("FAIL: " __VA_ARGS__); printf("\n"); fflush(stdout); g_fails++; } while (0)
#define CHECK(cond, ...) \
	do { if (cond) PASS(__VA_ARGS__); else FAIL(__VA_ARGS__); } while (0)

static int run(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int run(const char *fmt, ...)
{
	char cmd[1024];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	fflush(stdout);
	return system(cmd);
}

/* ---------------- the real session facts, with the device node replaced by an image ---------------- */

static ntfsmac_session_sys_real g_real;
static ntfsmac_session_sys g_sys;

static int vm_open(void *ctx, const char *bsd, bool writable)
{
	(void)ctx;
	(void)bsd;
	return open(g_image, (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);
}

static ntfsmac_dmn_status vm_describe(void *ctx, const char *bsd, ntfsmac_disk_info *out)
{
	(void)ctx;
	memset(out, 0, sizeof(*out));
	snprintf(out->bsd_name, sizeof(out->bsd_name), "%s", bsd);
	snprintf(out->media_path, sizeof(out->media_path), "dmnmirror-image");
	return NTFSMAC_DMN_OK;
}

static void spawn_cfg(const char *volmark)
{
	ntfsmac_spawn_config c;

	ntfsmac_spawn_config_default(&c);
	c.probe_path = g_probe;
	c.volmark_path = volmark != NULL ? volmark : g_volmark;
	c.worker_path = g_worker;
	c.child_stderr_fd = g_log_fd;
	c.sandbox_dir = g_sbx;
	c.sandbox_owner = 0;
	c.volmark_write_wait_s = 10;
	ntfsmac_spawn_config_set(&c);
}

static void use_image(const char *name)
{
	snprintf(g_image, sizeof(g_image), "%s/ntfs.img", g_work);
	run("cp '%s/%s.img' '%s' && chmod 600 '%s' && cp '%s' '%s.pre'", g_work, name, g_image, g_image, g_image,
	    g_image);
}

static bool sha_of(const char *path, char *out, size_t cap)
{
	char cmd[400];
	FILE *f;
	bool ok;

	snprintf(cmd, sizeof(cmd), "/usr/bin/shasum -a 256 '%s'", path);
	f = popen(cmd, "r");
	if (f == NULL)
		return false;
	ok = fgets(out, (int)cap, f) != NULL;
	pclose(f);
	if (ok)
		out[strcspn(out, " ")] = '\0';
	return ok;
}

static bool same_sha(const char *want)
{
	char now[128];

	return sha_of(g_image, now, sizeof(now)) && strcmp(now, want) == 0;
}

static unsigned char *slurp(const char *path, off_t *len)
{
	struct stat st;
	unsigned char *b;
	int fd = open(path, O_RDONLY);

	if (fd < 0 || fstat(fd, &st) != 0) {
		if (fd >= 0)
			close(fd);
		return NULL;
	}
	b = malloc((size_t)st.st_size);
	if (b == NULL || pread(fd, b, (size_t)st.st_size, 0) != st.st_size) {
		free(b);
		close(fd);
		return NULL;
	}
	close(fd);
	*len = st.st_size;
	return b;
}

/*
 * How many bytes of the image differ from its copy `.pre`, and how many of them are outside the
 * $MFTMirr records 0 to 3 (the only place the repair may write). Also: does mirror record `k` now
 * equal the $MFT record `k` byte for byte (`*equal0`, record 0)?
 */
static bool diff_report(long *changed, long *outside, bool *equal0)
{
	char pre[340];
	off_t la = 0, lb = 0, i;
	unsigned char *a, *b;
	unsigned bps, spc, rec;
	uint64_t mft = 0, mir = 0, mir_off, mft_off;
	int8_t cpr;

	snprintf(pre, sizeof(pre), "%s.pre", g_image);
	a = slurp(pre, &la);
	b = slurp(g_image, &lb);
	if (a == NULL || b == NULL || la != lb) {
		free(a);
		free(b);
		return false;
	}
	bps = (unsigned)a[0x0B] | ((unsigned)a[0x0C] << 8);
	spc = a[0x0D];
	cpr = (int8_t)a[0x40];
	for (i = 0; i < 8; i++) {
		mft |= (uint64_t)a[0x30 + i] << (8 * i);
		mir |= (uint64_t)a[0x38 + i] << (8 * i);
	}
	if (bps == 0 || spc == 0 || cpr >= 0) {
		free(a);
		free(b);
		return false;
	}
	rec = 1u << (unsigned)(-cpr);
	mft_off = mft * spc * bps;
	mir_off = mir * spc * bps;
	*changed = *outside = 0;
	for (i = 0; i < la; i++) {
		uint64_t o = (uint64_t)i;

		if (a[i] == b[i])
			continue;
		(*changed)++;
		if (!(o >= mir_off && o < mir_off + 4u * rec))
			(*outside)++;
	}
	*equal0 = memcmp(b + mft_off, b + mir_off, rec) == 0;
	free(a);
	free(b);
	return true;
}

/* ---------------- probe and the privileged request ---------------- */

static bool probe_now(ntfsmac_probe_verdict *v)
{
	ntfsmac_probe_run pr;
	ntfsmac_dmn_status st;
	int fd = vm_open(NULL, "disk99s1", false);

	if (fd < 0)
		return false;
	memset(&pr, 0, sizeof(pr));
	pr.device_fd = fd;
	pr.timeout_s = 20;
	st = ntfsmac_spawn_probe(&pr, v);
	close(fd);
	return st == NTFSMAC_DMN_OK;
}

static const char *state_name(const ntfsmac_probe_verdict *v)
{
	static const char *const n[] = {
#define X(id, str) str,
		NTFSMAC_PROBE_STATE_LIST(X)
#undef X
	};

	return (unsigned)v->state < NTFSMAC_PROBE_STATE_COUNT_ ? n[v->state] : "?";
}

static const char *probe_state_now(void)
{
	static ntfsmac_probe_verdict v;

	return probe_now(&v) ? state_name(&v) : "probe-failed";
}

static bool g_vol_mounted;

static bool reg_volume(void *ctx, const char *bsd, ntfsmac_priv_volume *out)
{
	(void)ctx;
	(void)bsd;
	out->mounted = g_vol_mounted;
	out->mounted_rw = false;
	return true;
}

static bool reg_live(void *ctx, uint64_t id)
{
	(void)ctx;
	(void)id;
	return false;
}

static ntfsmac_priv_sys_real g_psys;
static ntfsmac_priv g_priv;

static ntfsmac_dmn_status priv_mirror(const char *label, const char *serial, char *reply, size_t cap)
{
	ntfsmac_ctl_peer p;

	memset(&p, 0, sizeof(p));
	p.uid = g_uid;
	p.admin = true;
	return ntfsmac_priv_repair(&g_priv, &p, "disk99s1", "sync_mirror", label, serial, reply, cap);
}

static void serial8(const ntfsmac_probe_verdict *v, char out[16])
{
	snprintf(out, 16, "%08X", (unsigned)(v->serial & 0xFFFFFFFFu));
}

/* ---------------- cases ---------------- */

static void new_journal(ntfsmac_journal **j)
{
	run("rm -rf '%s'", g_jdir);
	CHECK(ntfsmac_journal_open(g_jdir, j) == NTFSMAC_JOURNAL_OK, "the journal opens");
	ntfsmac_priv_sys_real_init(&g_psys, &g_sys, reg_volume, reg_live, NULL);
	g_priv.sys = &g_psys.sys;
	g_priv.journal = *j;
}

static void case_refuse(void)
{
	ntfsmac_journal *j;
	ntfsmac_probe_verdict v;
	ntfsmac_policy_input pin;
	ntfsmac_policy_result pr;
	char reply[512], ser[16], label[80], sha[128];
	static const struct {
		const char *image, *reason;
	} cases[] = {
		{ "clean", "probe_not_mirror_split" },
		{ "newer", "probe_not_mirror_split" },
		{ "splitlog", NULL }, /* probe_not_mirror_split or log_not_clean: either is a refusal here */
	};
	size_t i;

	printf("== refuse: repair --sync-mirror refusals on real images (I-DMN-27)\n");
	use_image("split");
	spawn_cfg(NULL);
	new_journal(&j);
	CHECK(probe_now(&v), "probe");
	CHECK(v.state == NTFSMAC_PROBE_STATE_MIRROR_SPLIT &&
		      (v.reason_codes & NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_MFTMIRR_STALE)) != 0,
	      "the split probes mirror_split with MFTMIRR_STALE (state %s)", state_name(&v));
	/* attach: the policy refuses it, the note names the repair */
	memset(&pin, 0, sizeof(pin));
	pin.verdict = &v;
	pin.mode = NTFSMAC_POLICY_MODE_AUTO;
	pin.evidence_rule_enabled = true;
	pr = ntfsmac_policy_decide(&pin);
	CHECK(pr.decision == NTFSMAC_POLICY_REFUSE && pr.note == NTFSMAC_POLICY_NOTE_MIRROR_SPLIT,
	      "attach: the policy refuses a mirror_split volume (note mirror_split)");
	serial8(&v, ser);
	snprintf(label, sizeof(label), "%s", v.label);
	sha_of(g_image, sha, sizeof(sha));

	CHECK(priv_mirror("nope", ser, reply, sizeof(reply)) == NTFSMAC_DMN_E_DENIED && strstr(reply, "confirmation") != NULL,
	      "refused with a wrong label (%s)", reply);
	CHECK(priv_mirror(label, "00000000", reply, sizeof(reply)) == NTFSMAC_DMN_E_DENIED && strstr(reply, "confirmation") != NULL,
	      "refused with a wrong serial (%s)", reply);
	g_vol_mounted = true;
	CHECK(priv_mirror(label, ser, reply, sizeof(reply)) == NTFSMAC_DMN_E_DENIED && strstr(reply, "mounted") != NULL,
	      "refused while mounted (%s)", reply);
	g_vol_mounted = false;
	CHECK(same_sha(sha), "the image hash is unchanged after the refusals");

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		ntfsmac_probe_verdict w;
		ntfsmac_dmn_status st;

		use_image(cases[i].image);
		CHECK(probe_now(&w), "probe of %s", cases[i].image);
		serial8(&w, ser);
		snprintf(label, sizeof(label), "%s", w.label);
		sha_of(g_image, sha, sizeof(sha));
		st = priv_mirror(label, ser, reply, sizeof(reply));
		CHECK(st == NTFSMAC_DMN_E_DENIED && (cases[i].reason == NULL ? strstr(reply, "\"reason\"") != NULL
									      : strstr(reply, cases[i].reason) != NULL),
		      "refused on %s with probe state %s (%s)", cases[i].image, state_name(&w), reply);
		CHECK(same_sha(sha), "the image hash is unchanged after the refusal on %s", cases[i].image);
	}
	ntfsmac_journal_close(j);
}

static void case_repair(void)
{
	ntfsmac_journal *j;
	ntfsmac_probe_verdict v, w;
	ntfsmac_policy_input pin;
	ntfsmac_policy_result pr;
	ntfsmac_journal_entry e, e2;
	char reply[512], ser[16], label[80], sha[128];
	long changed = 0, outside = 0;
	bool eq0 = false;

	printf("== repair: repair --sync-mirror on mirror-split and mirror-split-dirty (I-DMN-27)\n");
	use_image("split");
	spawn_cfg(NULL);
	new_journal(&j);
	CHECK(probe_now(&v), "probe");
	serial8(&v, ser);
	snprintf(label, sizeof(label), "%s", v.label);
	/* a journal entry of an earlier session of this volume: neither read nor changed */
	memset(&e, 0, sizeof(e));
	e.serial = v.serial;
	snprintf(e.label, sizeof(e.label), "%s", "dmnmirror");
	e.total_sectors = v.total_sectors;
	e.session_id = 4242;
	e.created = e.updated = 1700000000;
	e.logfile_lsn_at_start = 0x1234;
	e.marker = NTFSMAC_JOURNAL_MARKER_PENDING;
	CHECK(ntfsmac_journal_create(j, &e) == NTFSMAC_JOURNAL_OK, "a journal entry exists");
	e.marker = NTFSMAC_JOURNAL_MARKER_SET;
	CHECK(ntfsmac_journal_update(j, &e) == NTFSMAC_JOURNAL_OK, "the entry is marked set, as after an unclean session");
	CHECK(ntfsmac_journal_get(j, v.serial, v.total_sectors, &e) == NTFSMAC_JOURNAL_OK, "the entry reads back");

	CHECK(priv_mirror(label, ser, reply, sizeof(reply)) == NTFSMAC_DMN_OK && strstr(reply, "\"result\":\"repaired\"") != NULL &&
		      strstr(reply, "\"state\":\"clean\"") != NULL && strstr(reply, "\"journal_entry\":\"untouched\"") != NULL,
	      "accepted on mirror-split, the reply carries the fresh state clean (%s)", reply);
	CHECK(diff_report(&changed, &outside, &eq0) && changed > 0 && outside == 0,
	      "only bytes inside the $MFTMirr records changed (%ld changed, %ld outside)", changed, outside);
	CHECK(eq0, "the $MFTMirr record 0 equals the $MFT record 0 byte for byte");
	CHECK(ntfsmac_journal_get(j, v.serial, v.total_sectors, &e2) == NTFSMAC_JOURNAL_OK && memcmp(&e, &e2, sizeof(e)) == 0,
	      "the journal entry is unchanged");
	CHECK(strcmp(probe_state_now(), "clean") == 0, "the next probe says clean");
	CHECK(probe_now(&w), "probe again");
	memset(&pin, 0, sizeof(pin));
	pin.verdict = &w;
	pin.journal = &e2;
	pin.mode = NTFSMAC_POLICY_MODE_AUTO;
	pin.evidence_rule_enabled = true;
	pr = ntfsmac_policy_decide(&pin);
	CHECK(pr.decision == NTFSMAC_POLICY_RO, "the next attach follows the policy table (an entry of an unclean session: ro)");
	/* nothing was mounted by the request */
	CHECK(run("/sbin/mount | /usr/bin/grep -q '%s'", g_work) != 0, "nothing of this image is mounted");

	/* a second request finds nothing to do: refused, image unchanged */
	sha_of(g_image, sha, sizeof(sha));
	CHECK(priv_mirror(label, ser, reply, sizeof(reply)) == NTFSMAC_DMN_E_DENIED && strstr(reply, "probe_not_mirror_split") != NULL,
	      "a second request is refused (%s)", reply);
	CHECK(same_sha(sha), "the image is unchanged by the second request");
	ntfsmac_journal_close(j);

	/* the split with VOLUME_IS_DIRTY set: repaired, the volume comes back dirty_flag */
	use_image("splitdirty");
	new_journal(&j);
	CHECK(probe_now(&v), "probe of mirror-split-dirty");
	CHECK(v.state == NTFSMAC_PROBE_STATE_MIRROR_SPLIT &&
		      (v.reason_codes & NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_FLAG_DIRTY)) != 0,
	      "it probes mirror_split with FLAG_DIRTY");
	serial8(&v, ser);
	snprintf(label, sizeof(label), "%s", v.label);
	CHECK(priv_mirror(label, ser, reply, sizeof(reply)) == NTFSMAC_DMN_OK && strstr(reply, "\"state\":\"dirty_flag\"") != NULL,
	      "accepted, the reply carries dirty_flag (%s)", reply);
	CHECK(diff_report(&changed, &outside, &eq0) && changed > 0 && outside == 0 && eq0,
	      "only the $MFTMirr records changed (%ld changed, %ld outside)", changed, outside);
	CHECK(probe_now(&w), "probe again");
	memset(&pin, 0, sizeof(pin));
	pin.verdict = &w;
	pin.mode = NTFSMAC_POLICY_MODE_AUTO;
	pin.evidence_rule_enabled = true;
	pr = ntfsmac_policy_decide(&pin);
	CHECK(pr.decision == NTFSMAC_POLICY_RO, "the next attach is read-only (dirty_flag)");
	ntfsmac_journal_close(j);
}

static void case_volmark(void)
{
	ntfsmac_journal *j;
	ntfsmac_probe_verdict v;
	char reply[512], ser[16], label[80], sha[128], stub[400];
	ntfsmac_dmn_status st;
	long changed = 0, outside = 0;
	bool eq0 = true;
	static const char *const stubs[] = { "jstub-exit1", "jstub-sector0" };
	size_t i;

	printf("== volmark: volmark replaced by test binaries (I-DMN-27)\n");
	for (i = 0; i < 2; i++) {
		use_image("split");
		snprintf(stub, sizeof(stub), "%s/%s", g_stubdir, stubs[i]);
		spawn_cfg(stub);
		new_journal(&j);
		CHECK(probe_now(&v), "probe");
		serial8(&v, ser);
		snprintf(label, sizeof(label), "%s", v.label);
		sha_of(g_image, sha, sizeof(sha));
		st = priv_mirror(label, ser, reply, sizeof(reply));
		CHECK(st != NTFSMAC_DMN_OK && strstr(reply, "\"ok\":true") == NULL,
		      "%s: the request does not succeed (status %d, %s)", stubs[i], (int)st, reply);
		CHECK(strstr(reply, "\"state\":\"") != NULL, "%s: the reply carries the state of a fresh probe", stubs[i]);
		if (i == 0) {
			CHECK(strstr(reply, "volmark_failed") != NULL && same_sha(sha), "%s: a fixed word, the image is unchanged",
			      stubs[i]);
		} else {
			/* sector 0 changed under the binding: denied or failed, and the mirror was not written */
			CHECK(diff_report(&changed, &outside, &eq0) && changed == 1 && !eq0,
			      "%s: the mirror record was not written (changed %ld, only the byte the stub changed)",
			      stubs[i], changed);
		}
		ntfsmac_journal_close(j);
	}
	spawn_cfg(NULL);
}

int main(int argc, char **argv)
{
	char path[340];
	const char *only;

	if (argc < 8) {
		fprintf(stderr, "usage: dmn_mirror_vm WORKDIR UID GID SBXDIR PROBE VOLMARK WORKER STUBDIR [CASE]\n");
		return 2;
	}
	g_work = argv[1];
	g_uid = (uid_t)strtoul(argv[2], NULL, 10);
	g_sbx = argv[4];
	g_probe = argv[5];
	g_volmark = argv[6];
	g_worker = argv[7];
	g_stubdir = argc > 8 ? argv[8] : NULL;
	only = argc > 9 ? argv[9] : NULL;
	if (geteuid() != 0 || g_uid == 0) {
		fprintf(stderr, "dmn_mirror_vm: run as root, with a console user that is not root\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGPIPE, SIG_IGN);
	snprintf(g_jdir, sizeof(g_jdir), "%s/journal", g_work);
	snprintf(path, sizeof(path), "%s/worker.log", g_work);
	g_log_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
	ntfsmac_session_sys_real_init(&g_real, NULL);
	g_sys = g_real.sys;
	g_sys.open_device = vm_open;
	g_sys.describe = vm_describe;
	g_sys.ctx = &g_real;
	spawn_cfg(NULL);

	if (only == NULL || !strcmp(only, "refuse"))
		case_refuse();
	if (only == NULL || !strcmp(only, "repair"))
		case_repair();
	if ((only == NULL || !strcmp(only, "volmark")) && g_stubdir != NULL)
		case_volmark();
	printf("== result: %d failure(s)\n", g_fails);
	return g_fails == 0 ? 0 : 1;
}
