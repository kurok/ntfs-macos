/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * `doctor` checks (docs/ARCHITECTURE.md section 3.4 `doctor` row, section 8.4
 * and the macFUSE row of the failure table in section 5): macFUSE version
 * and extension state, other NTFS drivers found, journal entries.
 *
 * Each check reports one or more findings with a fixed code. The CLI maps
 * codes to text. No finding carries a disk-derived string.
 *
 * Pure parts: version parse and compare, the plist string reader. File
 * system access is limited to the two directories in ntfsmac_doctor_env, so
 * tests use temporary directories. All checks only read.
 */
#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PLIST_MAX_BYTES (256u * 1024u)

/* ---- pure helpers ---- */

/*
 * The output of `pluginkit -m -v -i <id>` (macOS 27.2, tests/results/evidence/m4-gate/doctor_ext.txt):
 *   "+    <id>((null))\t<uuid>\t<date>\t<path of the .appex>\n (1 plug-in)\n"
 * Column 0 is the election: '+' use, '-' ignore, ' ' not elected. No match prints "  (no matches)".
 */
ntfsmac_doctor_ext_state ntfsmac_doctor_ext_parse(const char *buf, size_t len)
{
	static const char id[] = NTFSMAC_EXT_MODULE_ID;
	static const char trailer[] = " (1 plug-in)";
	static const char bundle[] = "\t/Library/Filesystems/macfuse.fs/";
	size_t i = 0, idlen = sizeof id - 1, tlen = sizeof trailer - 1;
	char mark = 0;
	unsigned lines_mod = 0, lines_trl = 0;

	if (buf == NULL || len == 0 || len > NTFSMAC_EXT_OUT_MAX)
		return NTFSMAC_EXT_UNKNOWN;
	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)buf[i];

		if (c != '\n' && c != '\t' && (c < 0x20 || c > 0x7e))
			return NTFSMAC_EXT_UNKNOWN;
	}
	if (buf[len - 1] != '\n')
		return NTFSMAC_EXT_UNKNOWN;
	for (i = 0; i < len;) {
		const char *l = buf + i;
		size_t n = 0, k;
		bool has_bundle = false;

		while (i + n < len && l[n] != '\n')
			n++;
		i += n + 1;
		if (n == tlen && memcmp(l, trailer, tlen) == 0) {
			lines_trl++;
			continue;
		}
		if (n > idlen + 5 && memcmp(l + 1, "    ", 4) == 0 && memcmp(l + 5, id, idlen) == 0 &&
		    l[5 + idlen] == '(') {
			for (k = 5 + idlen; k + sizeof bundle - 1 <= n; k++)
				if (memcmp(l + k, bundle, sizeof bundle - 1) == 0)
					has_bundle = true;
			if (!has_bundle)
				return NTFSMAC_EXT_UNKNOWN;
			mark = l[0];
			lines_mod++;
			continue;
		}
		return NTFSMAC_EXT_UNKNOWN;
	}
	if (lines_mod != 1 || lines_trl != 1)
		return NTFSMAC_EXT_UNKNOWN;
	if (mark == '+')
		return NTFSMAC_EXT_ENABLED;
	if (mark == '-' || mark == ' ')
		return NTFSMAC_EXT_NOT_APPROVED;
	return NTFSMAC_EXT_UNKNOWN;
}

bool ntfsmac_version_parse(const char *s, ntfsmac_version *out)
{
	unsigned parts[3] = {0, 0, 0};
	unsigned n = 0;

	if (s == NULL || out == NULL)
		return false;
	for (;;) {
		unsigned long v = 0;
		unsigned digits = 0;

		while (*s >= '0' && *s <= '9') {
			v = v * 10 + (unsigned long)(*s - '0');
			if (++digits > 6)
				return false; /* absurdly long component */
			s++;
		}
		if (digits == 0)
			return false;
		parts[n++] = (unsigned)v;
		if (*s != '.' || n == 3)
			break;
		s++;
	}
	out->major = parts[0];
	out->minor = parts[1];
	out->patch = parts[2];
	return true;
}

int ntfsmac_version_cmp(const ntfsmac_version *a, const ntfsmac_version *b)
{
	if (a->major != b->major)
		return a->major < b->major ? -1 : 1;
	if (a->minor != b->minor)
		return a->minor < b->minor ? -1 : 1;
	if (a->patch != b->patch)
		return a->patch < b->patch ? -1 : 1;
	return 0;
}

static const char *find_bytes(const char *buf, size_t len, const char *needle)
{
	size_t n = strlen(needle);

	if (n == 0 || len < n)
		return NULL;
	for (size_t i = 0; i + n <= len; i++)
		if (memcmp(buf + i, needle, n) == 0)
			return buf + i;
	return NULL;
}

static bool is_xml_space(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool ntfsmac_plist_string(const char *buf, size_t len, const char *key, char *out, size_t cap)
{
	char pat[96];
	const char *p, *end, *val, *close;
	size_t vlen;
	int n;

	if (buf == NULL || key == NULL || out == NULL || cap == 0)
		return false;
	out[0] = '\0';
	n = snprintf(pat, sizeof pat, "<key>%s</key>", key);
	if (n < 0 || (size_t)n >= sizeof pat)
		return false;
	p = find_bytes(buf, len, pat);
	if (p == NULL)
		return false;
	p += n;
	end = buf + len;
	while (p < end && is_xml_space(*p))
		p++;
	if ((size_t)(end - p) < 8 || memcmp(p, "<string>", 8) != 0)
		return false;
	val = p + 8;
	close = find_bytes(val, (size_t)(end - val), "</string>");
	if (close == NULL)
		return false;
	vlen = (size_t)(close - val);
	if (vlen >= cap)
		return false;
	memcpy(out, val, vlen);
	out[vlen] = '\0';
	return true;
}

/* ---- file system checks ---- */

static bool join3(char *out, size_t cap, const char *a, const char *b, const char *c)
{
	int n = snprintf(out, cap, "%s/%s%s%s", a, b, c[0] != '\0' ? "/" : "", c);

	return n >= 0 && (size_t)n < cap;
}

/* Read a regular file of at most PLIST_MAX_BYTES into buf. Returns length or -1. */
static long read_small_file(const char *path, char *buf, size_t cap)
{
	struct stat st;
	size_t got = 0;
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

	if (fd < 0)
		return -1;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || (size_t)st.st_size > cap) {
		close(fd);
		return -1;
	}
	while (got < cap) {
		ssize_t r = read(fd, buf + got, cap - got);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			return -1;
		}
		if (r == 0)
			break;
		got += (size_t)r;
	}
	close(fd);
	return (long)got;
}

static int emit(ntfsmac_doctor_cb cb, void *ctx, ntfsmac_doctor_code code, bool ok, long value)
{
	ntfsmac_doctor_finding f;

	f.code = (int)code;
	f.ok = ok;
	f.value = value;
	return cb != NULL ? cb(&f, ctx) : 0;
}

/* Returns non-zero when the callback asked to stop. */
static int check_macfuse(const ntfsmac_doctor_env *env, const char *fs_dir, ntfsmac_doctor_cb cb,
			 void *ctx)
{
	char path[PATH_MAX];
	struct stat st;
	char *buf;
	long len;
	char text[64];
	ntfsmac_version have, min = {NTFSMAC_MACFUSE_MIN_MAJOR, NTFSMAC_MACFUSE_MIN_MINOR,
				     NTFSMAC_MACFUSE_MIN_PATCH};
	bool have_ver = false;
	ntfsmac_doctor_ext_state ext = NTFSMAC_EXT_UNKNOWN;

	if (!join3(path, sizeof path, fs_dir, "macfuse.fs", "") || stat(path, &st) != 0 ||
	    !S_ISDIR(st.st_mode))
		return emit(cb, ctx, NTFSMAC_DOCTOR_MACFUSE_INSTALLED, false, 0);
	if (emit(cb, ctx, NTFSMAC_DOCTOR_MACFUSE_INSTALLED, true, 0))
		return 1;

	buf = malloc(PLIST_MAX_BYTES);
	if (buf != NULL && join3(path, sizeof path, fs_dir, "macfuse.fs", "Contents/Info.plist") &&
	    (len = read_small_file(path, buf, PLIST_MAX_BYTES)) >= 0) {
		have_ver = (ntfsmac_plist_string(buf, (size_t)len, "CFBundleShortVersionString",
						 text, sizeof text) ||
			    ntfsmac_plist_string(buf, (size_t)len, "CFBundleVersion", text,
						 sizeof text)) &&
			   ntfsmac_version_parse(text, &have);
	}
	free(buf);
	if (emit(cb, ctx, NTFSMAC_DOCTOR_MACFUSE_VERSION,
		 have_ver && ntfsmac_version_cmp(&have, &min) >= 0,
		 have_ver ? (long)(have.major * 10000u + have.minor * 100u + have.patch) : 0))
		return 1;

	if (env->ext_fn != NULL)
		ext = env->ext_fn(env->ext_ctx);
	return emit(cb, ctx, NTFSMAC_DOCTOR_MACFUSE_EXTENSION, ext == NTFSMAC_EXT_ENABLED,
		    (long)ext);
}

static int check_conflicts(const char *fs_dir, ntfsmac_doctor_cb cb, void *ctx)
{
	ntfsmac_conflict_report r;
	static const struct {
		ntfsmac_conflict_kind kind;
		ntfsmac_doctor_code code;
	} map[] = {
		{NTFSMAC_CONFLICT_PARAGON, NTFSMAC_DOCTOR_CONFLICT_PARAGON},
		{NTFSMAC_CONFLICT_TUXERA, NTFSMAC_DOCTOR_CONFLICT_TUXERA},
		{NTFSMAC_CONFLICT_OTHER, NTFSMAC_DOCTOR_CONFLICT_OTHER},
	};

	if (ntfsmac_conflict_scan(fs_dir, &r) != NTFSMAC_DMN_OK)
		return emit(cb, ctx, NTFSMAC_DOCTOR_CONFLICT_UNREADABLE, false, 0);
	if (r.drivers_found == 0)
		return emit(cb, ctx, NTFSMAC_DOCTOR_CONFLICT_NONE, true, 0);
	for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
		if ((r.kind_mask & (1u << (unsigned)map[i].kind)) != 0 &&
		    emit(cb, ctx, map[i].code, false, 0))
			return 1;
	return 0;
}

static int count_entry(const ntfsmac_journal_entry *e, void *ctx)
{
	(void)e;
	(*(unsigned long *)ctx)++;
	return 0;
}

static int count_tomb(uint64_t serial, int64_t when, void *ctx)
{
	(void)serial;
	(void)when;
	(*(unsigned long *)ctx)++;
	return 0;
}

static int check_journal(const ntfsmac_doctor_env *env, const char *dir, ntfsmac_doctor_cb cb,
			 void *ctx)
{
	struct stat st;
	ntfsmac_journal *j = NULL;
	unsigned long entries = 0, tombs = 0;
	unsigned skipped = 0;
	ntfsmac_journal_list_info info;
	size_t soft = env->journal_soft_max != 0 ? env->journal_soft_max : NTFSMAC_JOURNAL_SOFT_MAX;

	/* ntfsmac_journal_open() creates a missing directory; doctor must not. */
	if (lstat(dir, &st) != 0) {
		if (errno != ENOENT)
			return emit(cb, ctx, NTFSMAC_DOCTOR_JOURNAL_UNREADABLE, false, 0);
		return emit(cb, ctx, NTFSMAC_DOCTOR_JOURNAL_ENTRIES, true, 0) ||
		       emit(cb, ctx, NTFSMAC_DOCTOR_JOURNAL_CORRUPT, true, 0) ||
		       emit(cb, ctx, NTFSMAC_DOCTOR_JOURNAL_CLEARED, true, 0);
	}
	if (ntfsmac_journal_open(dir, &j) != NTFSMAC_JOURNAL_OK)
		return emit(cb, ctx, NTFSMAC_DOCTOR_JOURNAL_UNREADABLE, false, 0);
	/* A journal with more entries than one listing holds is not "unreadable" (T-M7-15). */
	if (ntfsmac_journal_list_ex(j, env->journal_list_max, count_entry, &entries, &info) !=
		    NTFSMAC_JOURNAL_OK ||
	    ntfsmac_journal_list_tombstones(j, count_tomb, &tombs) != NTFSMAC_JOURNAL_OK) {
		ntfsmac_journal_close(j);
		return emit(cb, ctx, NTFSMAC_DOCTOR_JOURNAL_UNREADABLE, false, 0);
	}
	skipped = info.skipped;
	ntfsmac_journal_close(j);
	if (emit(cb, ctx, NTFSMAC_DOCTOR_JOURNAL_ENTRIES, entries == 0, (long)entries) ||
	    emit(cb, ctx, NTFSMAC_DOCTOR_JOURNAL_CORRUPT, skipped == 0, (long)skipped) ||
	    emit(cb, ctx, NTFSMAC_DOCTOR_JOURNAL_CLEARED, true, (long)tombs))
		return 1;
	if (info.unlisted != 0 || info.found >= soft)
		return emit(cb, ctx, NTFSMAC_DOCTOR_JOURNAL_LARGE, false,
			    info.found > (size_t)LONG_MAX ? LONG_MAX : (long)info.found);
	return 0;
}

/* The last banner (T-M7-06): never sent is "unknown" (ok), not a failure. */
static int check_notify(ntfsmac_doctor_cb cb, void *ctx)
{
	ntfsmac_banner_outcome o = ntfsmac_banner_last();
	ntfsmac_doctor_finding f = ntfsmac_banner_finding(&o);

	return emit(cb, ctx, NTFSMAC_DOCTOR_NOTIFY_DELIVERY, f.ok, f.value);
}

/*
 * #9, owner decision 2026-11-26: writable mounts that the last restart left mounted because they
 * have a journal entry's name and are no entry's recorded mount. A count only: no path.
 */
static int check_left_mounted(ntfsmac_doctor_cb cb, void *ctx)
{
	size_t n = ntfsmac_reconcile_left_mounted();

	return emit(cb, ctx, NTFSMAC_DOCTOR_RECONCILE_LEFT_MOUNTED, n == 0, (long)n);
}

ntfsmac_dmn_status ntfsmac_doctor_run_env(const ntfsmac_doctor_env *env, ntfsmac_doctor_cb cb,
					  void *ctx)
{
	static const ntfsmac_doctor_env defaults = {NULL, NULL, NULL, NULL, 0, 0};
	const char *fs_dir, *journal_dir;

	if (cb == NULL)
		return NTFSMAC_DMN_E_INVAL;
	if (env == NULL)
		env = &defaults;
	fs_dir = env->fs_dir != NULL ? env->fs_dir : NTFSMAC_FS_DIR_DEFAULT;
	journal_dir = env->journal_dir != NULL ? env->journal_dir : NTFSMAC_JOURNAL_DEFAULT_DIR;

	if (check_macfuse(env, fs_dir, cb, ctx) || check_conflicts(fs_dir, cb, ctx) ||
	    check_journal(env, journal_dir, cb, ctx) || check_notify(cb, ctx) ||
	    check_left_mounted(cb, ctx))
		return NTFSMAC_DMN_OK; /* the callback stopped the run */
	return NTFSMAC_DMN_OK;
}

ntfsmac_dmn_status ntfsmac_doctor_run(ntfsmac_doctor_cb cb, void *ctx)
{
	return ntfsmac_doctor_run_env(NULL, cb, ctx);
}
