/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Text tables of the CLI (T-M4-04; ADR-016: reason codes only, text is ours).
 * Each table is indexed by the enum of its X-macro list, so a code added to
 * the list without a text shows up as a NULL entry, which the test
 * U-CTL-CLI-01 reports.
 */
#include <string.h>

#include "cli.h"
#include "ntfsmac/probe.h"

static const char *const reason_text[NTFSMAC_PROBE_REASON_COUNT_] = {
	[NTFSMAC_PROBE_REASON_NOT_NTFS] = "The volume is not NTFS.",
	[NTFSMAC_PROBE_REASON_BITLOCKER] = "The volume is encrypted with BitLocker.",
	[NTFSMAC_PROBE_REASON_MOUNT_FAILED] = "The safety check could not open the volume.",
	[NTFSMAC_PROBE_REASON_FLAG_DIRTY] =
		"The volume is marked dirty: it was not unmounted cleanly.",
	[NTFSMAC_PROBE_REASON_FLAG_CHKDSK_UNDERWAY] =
		"A disk check was still running when the volume was last used.",
	[NTFSMAC_PROBE_REASON_FLAG_PENDING_WORK] =
		"The volume has work pending (for example a resize or an upgrade).",
	[NTFSMAC_PROBE_REASON_FLAG_UNKNOWN_BITS] =
		"The volume has flag bits that this version does not know.",
	[NTFSMAC_PROBE_REASON_LOG_UNCLEAN] =
		"The NTFS journal shows that the volume was not shut down cleanly.",
	[NTFSMAC_PROBE_REASON_LOG_V2] =
		"The NTFS journal is in the Windows fast-startup format.",
	[NTFSMAC_PROBE_REASON_LOG_UNREADABLE] = "The NTFS journal could not be read.",
	[NTFSMAC_PROBE_REASON_HIBER_SIGNATURE] =
		"Windows hibernation data is present on the volume.",
	[NTFSMAC_PROBE_REASON_HIBER_WAKE] =
		"The Windows hibernation file carries a wake signature.",
	[NTFSMAC_PROBE_REASON_HIBER_SHORT] =
		"The Windows hibernation file is too short to check.",
	[NTFSMAC_PROBE_REASON_HIBER_UNREADABLE] =
		"The Windows hibernation file could not be read.",
	[NTFSMAC_PROBE_REASON_VERSION_UNKNOWN] = "The NTFS version is not supported.",
	[NTFSMAC_PROBE_REASON_CHECK_FAILED] = "A safety check could not finish.",
	[NTFSMAC_PROBE_REASON_MFTMIRR_STALE] =
		("The backup copy of the master file table does not match the original. "
		 "A write was probably interrupted."),
	[NTFSMAC_PROBE_REASON_NAMES_NOT_NFC] =
		"Some file names are not in Unicode NFC form.",
};

static const char *const refusal_text[NTFSMAC_CTL_REFUSE_COUNT_] = {
	[NTFSMAC_CTL_REFUSE_PEER_NOT_ALLOWED] =
		"Only the user at the console or an administrator may use ntfsmac.",
	[NTFSMAC_CTL_REFUSE_NOT_ADMIN] = "This action needs an administrator account.",
	[NTFSMAC_CTL_REFUSE_SPAWNED_PROCESS] =
		"The request came from a process that the NTFS daemon started, so it was refused.",
	[NTFSMAC_CTL_REFUSE_CLIENT_NOT_SIGNED] =
		"This ntfsmac is not the signed, installed copy, so the request was refused.",
	[NTFSMAC_CTL_REFUSE_NO_AUTHORIZATION] =
		"The request carried no administrator authorization.",
	[NTFSMAC_CTL_REFUSE_AUTHORIZATION_REPLAYED] = "That authorization was already used.",
	[NTFSMAC_CTL_REFUSE_AUTHORIZATION_DENIED] =
		"The administrator authorization was not accepted.",
	[NTFSMAC_CTL_REFUSE_AUTHORIZATION_RIGHT_INVALID] =
		("The administrator right of ntfsmac is missing or not set up as intended, "
		 "so the request was refused. Reinstall the package."),
	[NTFSMAC_CTL_REFUSE_CHECK_UNAVAILABLE] =
		"The daemon could not verify the request and refused it.",
};

static const char *const error_text[NTFSMAC_CTL_ERR_COUNT_] = {
	[NTFSMAC_CTL_ERR_BAD_JSON] = "The daemon could not read the request.",
	[NTFSMAC_CTL_ERR_LINE_TOO_LONG] = "The request was too long.",
	[NTFSMAC_CTL_ERR_UNKNOWN_COMMAND] = "The daemon does not know this command.",
	[NTFSMAC_CTL_ERR_BAD_FIELD] = "A field of the request is missing or not valid.",
	[NTFSMAC_CTL_ERR_DENIED] = "The request was refused.",
	[NTFSMAC_CTL_ERR_NOT_IMPLEMENTED] = "The daemon does not implement this command yet.",
	[NTFSMAC_CTL_ERR_FAILED] = "The request failed.",
	[NTFSMAC_CTL_ERR_INTERNAL] = "The daemon had an internal error.",
};

const char *ntfsmac_cli_reason_text(const char *code)
{
	static const char *const names[] = {
#define X(id, str) str,
		NTFSMAC_PROBE_REASON_LIST(X)
#undef X
	};
	size_t i;

	if (code == NULL)
		return NULL;
	for (i = 0; i < NTFSMAC_PROBE_REASON_COUNT_; i++)
		if (strcmp(code, names[i]) == 0)
			return reason_text[i];
	return NULL;
}

const char *ntfsmac_cli_refusal_text(const char *id)
{
	unsigned i;

	if (id == NULL)
		return NULL;
	for (i = 0; i < NTFSMAC_CTL_REFUSE_COUNT_; i++)
		if (strcmp(id, ntfsmac_ctl_refusal_name((ntfsmac_ctl_refusal)i)) == 0)
			return refusal_text[i];
	return NULL;
}

const char *ntfsmac_cli_error_text(const char *code)
{
	unsigned i;

	if (code == NULL)
		return NULL;
	for (i = 0; i < NTFSMAC_CTL_ERR_COUNT_; i++)
		if (strcmp(code, ntfsmac_ctl_error_name((ntfsmac_ctl_error)i)) == 0)
			return error_text[i];
	return NULL;
}
