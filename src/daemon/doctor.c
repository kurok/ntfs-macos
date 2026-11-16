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

static int check_journal(const char *dir, ntfsmac_doctor_cb cb, void *ctx)
{
	struct stat st;
	ntfsmac_journal *j = NULL;
	unsigned long entries = 0, tombs = 0;
	unsigned skipped = 0;

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
	if (ntfsmac_journal_list(j, count_entry, &entries, &skipped) != NTFSMAC_JOURNAL_OK ||
	    ntfsmac_journal_list_tombstones(j, count_tomb, &tombs) != NTFSMAC_JOURNAL_OK) {
		ntfsmac_journal_close(j);
		return emit(cb, ctx, NTFSMAC_DOCTOR_JOURNAL_UNREADABLE, false, 0);
	}
	ntfsmac_journal_close(j);
	return emit(cb, ctx, NTFSMAC_DOCTOR_JOURNAL_ENTRIES, entries == 0, (long)entries) ||
	       emit(cb, ctx, NTFSMAC_DOCTOR_JOURNAL_CORRUPT, skipped == 0, (long)skipped) ||
	       emit(cb, ctx, NTFSMAC_DOCTOR_JOURNAL_CLEARED, true, (long)tombs);
}

ntfsmac_dmn_status ntfsmac_doctor_run_env(const ntfsmac_doctor_env *env, ntfsmac_doctor_cb cb,
					  void *ctx)
{
	static const ntfsmac_doctor_env defaults = {NULL, NULL, NULL, NULL};
	const char *fs_dir, *journal_dir;

	if (cb == NULL)
		return NTFSMAC_DMN_E_INVAL;
	if (env == NULL)
		env = &defaults;
	fs_dir = env->fs_dir != NULL ? env->fs_dir : NTFSMAC_FS_DIR_DEFAULT;
	journal_dir = env->journal_dir != NULL ? env->journal_dir : NTFSMAC_JOURNAL_DEFAULT_DIR;

	if (check_macfuse(env, fs_dir, cb, ctx) || check_conflicts(fs_dir, cb, ctx) ||
	    check_journal(journal_dir, cb, ctx))
		return NTFSMAC_DMN_OK; /* the callback stopped the run */
	return NTFSMAC_DMN_OK;
}

ntfsmac_dmn_status ntfsmac_doctor_run(ntfsmac_doctor_cb cb, void *ctx)
{
	return ntfsmac_doctor_run_env(NULL, cb, ctx);
}
