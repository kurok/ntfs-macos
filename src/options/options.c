/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Worker option builder (docs/ARCHITECTURE.md section 3.3, ADR-005, ADR-016).
 *
 * The only producer of the worker vector. Every token is a string literal or
 * a number formatted here; nothing from the disk enters the option string,
 * and there is no code path that can emit `recover`, `remove_hiberfile`,
 * `fsname` or any other banned key.
 */
#include "ntfsmac/options.h"

#include <stdio.h>
#include <string.h>

/* strnlen is POSIX, not C17: bounded length without it. Returns at most max. */
static size_t bounded_len(const char *s, size_t max)
{
	const char *p = memchr(s, '\0', max);

	return p != NULL ? (size_t)(p - s) : max;
}

static ntfsmac_options_status fail(ntfsmac_worker_argv *out, ntfsmac_options_status s)
{
	if (out != NULL)
		memset(out, 0, sizeof(*out));
	return s;
}

ntfsmac_options_status ntfsmac_options_build(const ntfsmac_options_input *in,
					     ntfsmac_worker_argv *out)
{
	size_t leaf_len;
	size_t i;
	int n;
	size_t len;

	if (out != NULL)
		memset(out, 0, sizeof(*out));
	if (in == NULL || out == NULL || in->worker_path == NULL || in->leaf == NULL)
		return NTFSMAC_OPTIONS_ERR_INVALID;
	if (in->decision != NTFSMAC_POLICY_RW && in->decision != NTFSMAC_POLICY_RO)
		return fail(out, NTFSMAC_OPTIONS_ERR_DECISION);
	if (in->worker_path[0] != '/')
		return fail(out, NTFSMAC_OPTIONS_ERR_WORKER);
	if (in->device_fd < NTFSMAC_WORKER_FD_MIN || in->device_fd > NTFSMAC_WORKER_FD_MAX)
		return fail(out, NTFSMAC_OPTIONS_ERR_FD);

	leaf_len = bounded_len(in->leaf, NTFSMAC_LEAF_MAX_BYTES + 1u);
	if (leaf_len == 0 || leaf_len > NTFSMAC_LEAF_MAX_BYTES)
		return fail(out, NTFSMAC_OPTIONS_ERR_LEAF);
	for (i = 0; i < leaf_len; i++) {
		if (in->leaf[i] == '/')
			return fail(out, NTFSMAC_OPTIONS_ERR_LEAF);
	}

	n = snprintf(out->devpath, sizeof(out->devpath), "/dev/fd/%d", in->device_fd);
	if (n < 0 || (size_t)n >= sizeof(out->devpath))
		return fail(out, NTFSMAC_OPTIONS_ERR_FD);
	n = snprintf(out->mountpoint, sizeof(out->mountpoint), "%s%s",
		     NTFSMAC_VOLUMES_PREFIX, in->leaf);
	if (n < 0 || (size_t)n >= sizeof(out->mountpoint))
		return fail(out, NTFSMAC_OPTIONS_ERR_LEAF);

	/* Always row: fixed literals. */
	len = 0;
	n = snprintf(out->optstr, sizeof(out->optstr),
		     "backend=fskit,local,norecover,noatime,windows_names,"
		     "streams_interface=openxattr");
	if (n < 0 || (size_t)n >= sizeof(out->optstr))
		return fail(out, NTFSMAC_OPTIONS_ERR_INVALID);
	len = (size_t)n;

	/* `permissions` replaces uid, gid, fmask and dmask. */
	if (!in->permissions) {
		n = snprintf(out->optstr + len, sizeof(out->optstr) - len,
			     ",uid=%u,gid=%u,fmask=" NTFSMAC_OPT_FMASK ",dmask=" NTFSMAC_OPT_DMASK,
			     (unsigned)in->console_uid, (unsigned)in->console_gid);
		if (n < 0 || (size_t)n >= sizeof(out->optstr) - len)
			return fail(out, NTFSMAC_OPTIONS_ERR_INVALID);
		len += (size_t)n;
	}

	{
		/* Fixed tokens only; each gated by a boolean that is not from the disk. */
		const char *const extra[4] = {
			in->decision == NTFSMAC_POLICY_RO ? ",ro" : "",
			in->hide_hid_files ? ",hide_hid_files" : "",
			in->permissions ? ",permissions" : "",
			in->usermapping ? ",usermapping=" NTFSMAC_USERMAPPING_PATH : "",
		};
		size_t k;

		for (k = 0; k < 4; k++) {
			size_t xl = strlen(extra[k]);

			if (xl >= sizeof(out->optstr) - len)
				return fail(out, NTFSMAC_OPTIONS_ERR_INVALID);
			memcpy(out->optstr + len, extra[k], xl + 1);
			len += xl;
		}
	}

	out->argv[0] = in->worker_path;
	out->argv[1] = out->devpath;
	out->argv[2] = out->mountpoint;
	out->argv[3] = "-o";
	out->argv[4] = out->optstr;
	out->argv[5] = NULL;
	return NTFSMAC_OPTIONS_OK;
}
