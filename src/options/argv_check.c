/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Final worker argv validator (docs/ARCHITECTURE.md section 3.3 rules 1 to 7,
 * ADR-016). Written against the specification, not against the builder: it
 * parses the vector the way the engine does (split on ',', key at the first
 * '=') and accepts only the allow-list with per-key grammars.
 *
 * ntfsmac_argv_check_text() is pure and safe to fuzz. Fail closed: anything
 * not recognised is rejected.
 */
#include "ntfsmac/options.h"
#include "ntfsmac/label_cp.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>

/* Allow-list keys, one bit each for the duplicate and presence checks. */
enum {
	K_BACKEND,
	K_LOCAL,
	K_NORECOVER,
	K_NOATIME,
	K_WINDOWS_NAMES,
	K_NOCOMPRESSION,
	K_NO_DETACH,
	K_STREAMS,
	K_UID,
	K_GID,
	K_FMASK,
	K_DMASK,
	K_RO,
	K_HIDE_HID,
	K_PERMISSIONS,
	K_USERMAPPING,
	K_COUNT
};

static const char *const allowed_keys[K_COUNT] = {
	"backend", "local", "norecover", "noatime", "windows_names", "nocompression", "no_detach",
	"streams_interface", "uid", "gid", "fmask", "dmask", "ro", "hide_hid_files", "permissions", "usermapping",
};

/* Banned list of section 3.3 (banned and rejected-as-useless), exact match. */
static const char *const banned_keys[] = {
	"recover", "remove_hiberfile", "fsname", "rdonly", "no_def_opts", "dev", "suid",
	"allow_root", "efs_raw", "big_writes", "volname", "allow_other", "nonempty",
	"default_permissions", "blkdev", "blksize", "noowners", "noappledouble",
	"noapplexattr", "nobrowse", "daemon_timeout", "iosize", "sync", "async",
	"user_xattr", "umask", "rw",
};

#define BIT(k) (1u << (k))

/* strnlen is POSIX, not C17: bounded length without it. Returns at most max. */
static size_t bounded_len(const char *s, size_t max)
{
	const char *p = memchr(s, '\0', max);

	return p != NULL ? (size_t)(p - s) : max;
}

static int is_banned(const char *key, size_t klen)
{
	size_t i;

	for (i = 0; i < sizeof(banned_keys) / sizeof(banned_keys[0]); i++) {
		if (strlen(banned_keys[i]) == klen && memcmp(banned_keys[i], key, klen) == 0)
			return 1;
	}
	return 0;
}

static int key_index(const char *key, size_t klen)
{
	int i;

	for (i = 0; i < K_COUNT; i++) {
		if (strlen(allowed_keys[i]) == klen && memcmp(allowed_keys[i], key, klen) == 0)
			return i;
	}
	return -1;
}

static int streq_n(const char *s, size_t n, const char *lit)
{
	return strlen(lit) == n && memcmp(s, lit, n) == 0;
}

/*
 * Rule 2: the leaf is a label part of ADR-016 (1 to 64 bytes, strict UTF-8, every code point on
 * the shared allow-list ntfsmac_label_cp_allowed(), no leading or trailing space or dot, no
 * leading hyphen (audit M4 F-01, fuzz finding M5-01), no combining mark first), optionally
 * followed by a collision suffix: a space and 2 to 9, or 10 to 99 (no leading zero, no 100).
 * Both forms are tried: the whole leaf as a part, or a part and a suffix.
 */
static int suffix_ok(const char *s, size_t n)
{
	if (n == 2)
		return s[0] == ' ' && s[1] >= '2' && s[1] <= '9';
	if (n == 3)
		return s[0] == ' ' && s[1] >= '1' && s[1] <= '9' && s[2] >= '0' && s[2] <= '9';
	return 0;
}

static int leaf_ok(const char *leaf)
{
	size_t len = bounded_len(leaf, NTFSMAC_LEAF_MAX_BYTES + 1u);

	if (len == 0 || len > NTFSMAC_LEAF_MAX_BYTES)
		return 0;
	if (ntfsmac_label_part_ok(leaf, len))
		return 1;
	if (len > 3 && suffix_ok(leaf + len - 3, 3) && ntfsmac_label_part_ok(leaf, len - 3))
		return 1;
	if (len > 2 && suffix_ok(leaf + len - 2, 2) && ntfsmac_label_part_ok(leaf, len - 2))
		return 1;
	return 0;
}

/* Canonical decimal: no sign, no leading zero (except "0"), at most 10 digits. */
static int fmt_u32(char *buf, size_t size, uint32_t v)
{
	int n = snprintf(buf, size, "%u", (unsigned)v);

	return n > 0 && (size_t)n < size;
}

/* Rule 1: "/dev/fd/N", N decimal 3..255 without sign or leading zero. */
static int parse_fd(const char *s, int *fd_out)
{
	static const char prefix[] = "/dev/fd/";
	size_t plen = sizeof(prefix) - 1u;
	size_t i, nd;
	int v = 0;

	if (strncmp(s, prefix, plen) != 0)
		return 0;
	s += plen;
	nd = strlen(s);
	if (nd == 0 || nd > 3)
		return 0;
	if (s[0] == '0')
		return 0;
	for (i = 0; i < nd; i++) {
		if (s[i] < '0' || s[i] > '9')
			return 0;
		v = v * 10 + (s[i] - '0');
	}
	if (v < NTFSMAC_WORKER_FD_MIN || v > NTFSMAC_WORKER_FD_MAX)
		return 0;
	*fd_out = v;
	return 1;
}

static ntfsmac_argv_status check_value(int k, const char *val, size_t vlen, int has_val,
				       const ntfsmac_argv_expect *ex)
{
	char want[16];

	switch (k) {
	case K_BACKEND:
		return has_val && streq_n(val, vlen, "fskit") ? NTFSMAC_ARGV_OK
							      : NTFSMAC_ARGV_ERR_VALUE;
	case K_STREAMS:
		return has_val && (streq_n(val, vlen, "openxattr") || streq_n(val, vlen, "none"))
			       ? NTFSMAC_ARGV_OK
			       : NTFSMAC_ARGV_ERR_VALUE;
	case K_UID:
	case K_GID:
		if (!has_val || !fmt_u32(want, sizeof(want), k == K_UID ? ex->console_uid
									: ex->console_gid))
			return NTFSMAC_ARGV_ERR_VALUE;
		return streq_n(val, vlen, want) ? NTFSMAC_ARGV_OK : NTFSMAC_ARGV_ERR_VALUE;
	case K_FMASK:
		return has_val && streq_n(val, vlen, NTFSMAC_OPT_FMASK) ? NTFSMAC_ARGV_OK
									: NTFSMAC_ARGV_ERR_VALUE;
	case K_DMASK:
		return has_val && streq_n(val, vlen, NTFSMAC_OPT_DMASK) ? NTFSMAC_ARGV_OK
									: NTFSMAC_ARGV_ERR_VALUE;
	case K_USERMAPPING:
		return has_val && streq_n(val, vlen, NTFSMAC_USERMAPPING_PATH)
			       ? NTFSMAC_ARGV_OK
			       : NTFSMAC_ARGV_ERR_VALUE;
	default:
		/* local, norecover, noatime, windows_names, nocompression, no_detach, ro, hide_hid_files,
		 * permissions */
		return has_val ? NTFSMAC_ARGV_ERR_VALUE : NTFSMAC_ARGV_OK;
	}
}

ntfsmac_argv_status ntfsmac_argv_check_text(const char *const *argv, size_t argc,
					    const ntfsmac_argv_expect *expect)
{
	static const unsigned always = BIT(K_BACKEND) | BIT(K_LOCAL) | BIT(K_NORECOVER) |
				       BIT(K_NOATIME) | BIT(K_WINDOWS_NAMES) | BIT(K_NOCOMPRESSION) |
				       BIT(K_NO_DETACH) |
				       BIT(K_STREAMS);
	static const unsigned ownership = BIT(K_UID) | BIT(K_GID) | BIT(K_FMASK) | BIT(K_DMASK);
	static const char vol[] = NTFSMAC_VOLUMES_PREFIX;
	const char *opt;
	const char *leaf;
	size_t optlen, pos, i;
	unsigned seen = 0;
	int fd;

	if (argv == NULL || expect == NULL || expect->worker_path == NULL ||
	    expect->leaf == NULL)
		return NTFSMAC_ARGV_ERR_INVALID;
	if (expect->decision != NTFSMAC_POLICY_RW && expect->decision != NTFSMAC_POLICY_RO)
		return NTFSMAC_ARGV_ERR_INVALID;

	/* Rule 1 */
	if (argc != NTFSMAC_WORKER_ARGC)
		return NTFSMAC_ARGV_ERR_ARGC;
	for (i = 0; i < argc; i++) {
		if (argv[i] == NULL)
			return NTFSMAC_ARGV_ERR_ARGC;
	}
	if (strcmp(argv[0], expect->worker_path) != 0)
		return NTFSMAC_ARGV_ERR_WORKER_PATH;
	if (!parse_fd(argv[1], &fd))
		return NTFSMAC_ARGV_ERR_DEVICE;
	if (strncmp(argv[2], vol, sizeof(vol) - 1u) != 0)
		return NTFSMAC_ARGV_ERR_MOUNTPOINT;
	if (strcmp(argv[3], "-o") != 0)
		return NTFSMAC_ARGV_ERR_DASH_O;

	/* Rule 2 */
	leaf = argv[2] + (sizeof(vol) - 1u);
	if (!leaf_ok(leaf) || strcmp(leaf, expect->leaf) != 0)
		return NTFSMAC_ARGV_ERR_LEAF;

	/* Rule 3 */
	opt = argv[4];
	optlen = bounded_len(opt, NTFSMAC_OPTSTR_MAX_BYTES + 1u);
	if (optlen > NTFSMAC_OPTSTR_MAX_BYTES)
		return NTFSMAC_ARGV_ERR_OPT_LENGTH;
	for (i = 0; i < optlen; i++) {
		unsigned char c = (unsigned char)opt[i];

		if (c < 0x21 || c > 0x7E || c == '\\')
			return NTFSMAC_ARGV_ERR_OPT_BYTE;
	}

	/* Rules 4 and 5 */
	if (optlen == 0)
		return NTFSMAC_ARGV_ERR_EMPTY_TOKEN;
	pos = 0;
	for (;;) {
		size_t tend = pos, klen;
		const char *tok = opt + pos;
		const char *eq;
		int k, has_val;
		ntfsmac_argv_status st;

		while (opt[tend] != '\0' && opt[tend] != ',')
			tend++;
		if (tend == pos)
			return NTFSMAC_ARGV_ERR_EMPTY_TOKEN;
		eq = memchr(tok, '=', tend - pos);
		has_val = eq != NULL;
		klen = has_val ? (size_t)(eq - tok) : tend - pos;
		if (klen == 0)
			return NTFSMAC_ARGV_ERR_UNKNOWN_KEY;
		if (is_banned(tok, klen))
			return NTFSMAC_ARGV_ERR_BANNED;
		k = key_index(tok, klen);
		if (k < 0)
			return NTFSMAC_ARGV_ERR_UNKNOWN_KEY;
		if (seen & BIT(k))
			return NTFSMAC_ARGV_ERR_DUPLICATE_KEY;
		seen |= BIT(k);
		st = check_value(k, has_val ? eq + 1 : NULL,
				 has_val ? tend - pos - klen - 1u : 0, has_val, expect);
		if (st != NTFSMAC_ARGV_OK)
			return st;
		if (opt[tend] == '\0')
			break;
		pos = tend + 1u;
	}

	/* Rule 6 */
	if ((seen & always) != always)
		return NTFSMAC_ARGV_ERR_MISSING;
	if (((seen & BIT(K_RO)) != 0) != (expect->decision == NTFSMAC_POLICY_RO))
		return NTFSMAC_ARGV_ERR_RO_MISMATCH;
	if (seen & BIT(K_PERMISSIONS)) {
		if (seen & ownership)
			return NTFSMAC_ARGV_ERR_OWNERSHIP;
	} else if ((seen & ownership) != ownership) {
		return NTFSMAC_ARGV_ERR_OWNERSHIP;
	}
	return NTFSMAC_ARGV_OK;
}

ntfsmac_argv_status ntfsmac_argv_check(const char *const *argv, size_t argc,
				       const ntfsmac_argv_expect *expect)
{
	ntfsmac_argv_status st = ntfsmac_argv_check_text(argv, argc, expect);
	int fd = 0, flags, acc;

	if (st != NTFSMAC_ARGV_OK)
		return st;
	if (!parse_fd(argv[1], &fd))
		return NTFSMAC_ARGV_ERR_DEVICE;
	flags = fcntl(fd, F_GETFL);
	if (flags < 0)
		return NTFSMAC_ARGV_ERR_FD_MODE;
	acc = flags & O_ACCMODE;
	if (expect->decision == NTFSMAC_POLICY_RO)
		return acc == O_RDONLY ? NTFSMAC_ARGV_OK : NTFSMAC_ARGV_ERR_FD_MODE;
	return acc == O_RDWR ? NTFSMAC_ARGV_OK : NTFSMAC_ARGV_ERR_FD_MODE;
}

const char *ntfsmac_argv_status_name(ntfsmac_argv_status s)
{
	switch (s) {
	case NTFSMAC_ARGV_OK: return "ok";
	case NTFSMAC_ARGV_ERR_INVALID: return "invalid";
	case NTFSMAC_ARGV_ERR_ARGC: return "argc";
	case NTFSMAC_ARGV_ERR_WORKER_PATH: return "worker_path";
	case NTFSMAC_ARGV_ERR_DEVICE: return "device";
	case NTFSMAC_ARGV_ERR_MOUNTPOINT: return "mountpoint";
	case NTFSMAC_ARGV_ERR_DASH_O: return "dash_o";
	case NTFSMAC_ARGV_ERR_LEAF: return "leaf";
	case NTFSMAC_ARGV_ERR_OPT_LENGTH: return "opt_length";
	case NTFSMAC_ARGV_ERR_OPT_BYTE: return "opt_byte";
	case NTFSMAC_ARGV_ERR_EMPTY_TOKEN: return "empty_token";
	case NTFSMAC_ARGV_ERR_BANNED: return "banned";
	case NTFSMAC_ARGV_ERR_UNKNOWN_KEY: return "unknown_key";
	case NTFSMAC_ARGV_ERR_DUPLICATE_KEY: return "duplicate_key";
	case NTFSMAC_ARGV_ERR_VALUE: return "value";
	case NTFSMAC_ARGV_ERR_MISSING: return "missing";
	case NTFSMAC_ARGV_ERR_RO_MISMATCH: return "ro_mismatch";
	case NTFSMAC_ARGV_ERR_OWNERSHIP: return "ownership";
	case NTFSMAC_ARGV_ERR_FD_MODE: return "fd_mode";
	}
	return NULL;
}
