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
	[NTFSMAC_CTL_REFUSE_WRONG_CHANNEL] =
		"This request is not served on this connection. Use the ntfsmac command for it.",
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

/*
 * Text for the reason of a refused journal_clear or repair: the gate of docs/ARCHITECTURE.md
 * section 3.2 that failed, or a step before it (ctl_journal.c, ntfsmac_priv_gate_name()).
 */
static const struct {
	const char *id;
	const char *text;
} gate_text[] = {
	{"no_volume", "The daemon does not know this volume. Check the name with `ntfsmac list`."},
	{NTFSMAC_CTL_REASON_JOURNAL_BUSY,
	 "The daemon is busy with the session journal (a repair or a mount is running). Try again in a "
	 "moment."},
	{"not_ntfs",
	 "The volume is not NTFS (the system's own driver handles it), so ntfsmac does not mount it."},
	{"probe_failed", "The volume could not be checked just now (is it mounted by another driver?)."},
	{"invalid_input", "The request was not valid."},
	{"no_entry", "There is no session journal entry for this volume, so there is nothing to clear."},
	{"probe_not_clean",
	 "The volume is not clean. A journal entry can be cleared only on a volume that a fresh check "
	 "finds clean. Run Windows chkdsk."},
	{"mounted_rw", "The volume is mounted read/write. Unmount it first."},
	{"live_session",
	 "A mounted volume with the same serial (a clone, for example) owns this entry, so it was "
	 "not touched."},
	{"probe_not_dirty_flag",
	 "The volume does not show only the dirty flag. Only that state can be repaired here; run "
	 "Windows chkdsk."},
	{"probe_not_mirror_split",
	 "The volume does not show only the interrupted $MFT and $MFTMirr update that this tool can "
	 "repair (another finding is present, or it was already repaired). Nothing was changed. Run "
	 "Windows chkdsk, or shut Windows down fully."},
	{"log_not_clean", "The log of the volume is not clean. Run Windows chkdsk."},
	{"hiberfile", "The volume holds an active Windows hibernation file. Shut Windows down fully."},
	{"flags",
	 "The volume has flags besides the dirty flag that this tool will not clear. Run Windows "
	 "chkdsk."},
	{"mounted", "The volume is mounted. Unmount it first."},
	{"peer", "The request did not pass the checks for who may send it."},
	{"confirmation", "The label or the serial you typed does not match the volume."},
	{"volmark_refused",
	 "The repair tool looked at the volume and refused (it does not show exactly the damage it can "
	 "repair, or it changed since the check). Nothing was changed."},
	{"volmark_failed",
	 "The tool that writes to the volume refused or failed. For repair --sync-mirror: it was "
	 "interrupted or failed after it began to write, so the mirror may be partly written; run "
	 "`ntfsmac status` for the volume's state now, Windows chkdsk is strongly preferred. For "
	 "repair --clear-dirty: the volume was probably not changed; if in doubt, run Windows chkdsk."},
	{"journal", "The journal entry could not be moved. Nothing else was changed."},
	{"entry_changed", "The journal entry changed while the request was handled (another session started), so it was not touched."},
	{"binding", "The device is not the volume that was checked (or another driver has it mounted)."},
};

const char *ntfsmac_cli_gate_text(const char *id)
{
	size_t i;

	if (id == NULL)
		return NULL;
	for (i = 0; i < sizeof gate_text / sizeof gate_text[0]; i++)
		if (strcmp(id, gate_text[i].id) == 0)
			return gate_text[i].text;
	return NULL;
}

/*
 * Text of one `doctor` finding. The codes are those of ntfsmac_doctor_code in
 * src/daemon/internal.h, which the daemon sends as numbers (no disk-derived string is ever in a
 * finding). I-DMN-17 wants `doctor` to name a conflicting bundle, so a code is not left for the
 * reader to look up. `buf` is used for the texts that carry a number.
 */
const char *ntfsmac_cli_doctor_text(int code, bool ok, long value, char *buf, size_t cap)
{
	if (value < 0)
		value = 0; /* a count or a version cannot be negative: shown as nothing found */
	switch (code) {
	case NTFSMAC_CLI_DOCTOR_MACFUSE_INSTALLED:
		snprintf(buf, cap, ok ? "macFUSE is installed."
				      : "macFUSE is not installed. Install macFUSE %d.%d or newer.",
			 NTFSMAC_CLI_MACFUSE_MIN_MAJOR, NTFSMAC_CLI_MACFUSE_MIN_MINOR);
		return buf;
	case NTFSMAC_CLI_DOCTOR_MACFUSE_VERSION:
		if (!ok && value == 0)
			return "The macFUSE version could not be read.";
		snprintf(buf, cap,
			 ok ? "macFUSE version %ld.%ld.%ld is supported."
			    : "macFUSE version %ld.%ld.%ld is not supported; install %d.%d or newer.",
			 value / 10000, (value / 100) % 100, value % 100, NTFSMAC_CLI_MACFUSE_MIN_MAJOR,
			 NTFSMAC_CLI_MACFUSE_MIN_MINOR);
		return buf;
	case NTFSMAC_CLI_DOCTOR_MACFUSE_EXTENSION:
		if (ok)
			return "The macFUSE file system extension is enabled.";
		return value == NTFSMAC_CLI_DOCTOR_EXT_NOT_APPROVED
			       ? "The macFUSE file system extension is not approved: approve it in System "
				 "Settings, then restart."
			       : "The state of the macFUSE file system extension could not be read.";
	case NTFSMAC_CLI_DOCTOR_CONFLICT_NONE:
		return "No other NTFS driver is installed.";
	case NTFSMAC_CLI_DOCTOR_CONFLICT_PARAGON:
		return "Paragon NTFS (ufsd_NTFS.fs) is installed. ntfsmac is passive: it mounts nothing and "
		       "does not stop the other driver from mounting.";
	case NTFSMAC_CLI_DOCTOR_CONFLICT_TUXERA:
		return "Tuxera NTFS (tuxera_ntfs.fs) is installed. ntfsmac is passive: it mounts nothing and "
		       "does not stop the other driver from mounting.";
	case NTFSMAC_CLI_DOCTOR_CONFLICT_OTHER:
		return "Another NTFS driver (a bundle with ntfs in its name in /Library/Filesystems) is "
		       "installed. ntfsmac is passive: it mounts nothing.";
	case NTFSMAC_CLI_DOCTOR_CONFLICT_UNREADABLE:
		return "The folder of file system drivers could not be read, so ntfsmac stays passive.";
	case NTFSMAC_CLI_DOCTOR_JOURNAL_ENTRIES:
		if (ok)
			return "No session journal entries: no volume was left open by this Mac.";
		snprintf(buf, cap,
			 "%ld volume(s) were left open by this Mac: the next mount of each is read-only until "
			 "Windows chkdsk has run (or `ntfsmac journal-clear`).",
			 value);
		return buf;
	case NTFSMAC_CLI_DOCTOR_JOURNAL_CORRUPT:
		if (ok)
			return "No unreadable journal entries.";
		snprintf(buf, cap,
			 "%ld journal entries could not be read: the volumes they belong to are mounted read-only.",
			 value);
		return buf;
	case NTFSMAC_CLI_DOCTOR_JOURNAL_CLEARED:
		snprintf(buf, cap, "%ld cleared journal entries are kept as tombstones.", value);
		return buf;
	case NTFSMAC_CLI_DOCTOR_JOURNAL_UNREADABLE:
		return "The session journal could not be opened.";
	case NTFSMAC_CLI_DOCTOR_JOURNAL_LARGE:
		snprintf(buf, cap,
			 "The session journal holds %ld entries, more than expected. Entries are never removed "
			 "on their own; for each volume that Windows chkdsk has checked, run `sudo ntfsmac "
			 "journal-clear`.",
			 value);
		return buf;
	case NTFSMAC_CLI_DOCTOR_NOTIFY_DELIVERY:
		/* value = state * 1000 + exit status (ntfsmac_banner_value() in src/daemon/internal.h):
		 * 0 none yet, 1 delivered, 2 not started, 3 sudo refused, 4 timeout, 5 non-zero exit. */
		switch (value / 1000) {
		case 0:
			return "No notification banner has been sent since the daemon started, so banner "
			       "delivery is unknown.";
		case 1:
			return "The last notification banner was delivered.";
		case 2:
			return "The last notification banner was not shown: its helper could not be started.";
		case 3:
			return "The last notification banner was not shown: sudo refused it. Check that sudoers "
			       "lets root run commands as the console user (sudo -n -u).";
		case 4:
			snprintf(buf, cap,
				 "The last notification banner was not shown: its helper timed out after %d "
				 "seconds.",
				 NTFSMAC_CLI_BANNER_WAIT_S);
			return buf;
		case 5:
			snprintf(buf, cap,
				 "The last notification banner was not shown: the helper exited with status "
				 "%ld.",
				 value % 1000);
			return buf;
		default:
			return "The state of the last notification banner is not known to this program.";
		}
	case NTFSMAC_CLI_DOCTOR_RECONCILE_LEFT_MOUNTED:
		/* #9, owner decision 2026-11-26 */
		if (ok)
			return "No read/write volume was left mounted since the last daemon start.";
		snprintf(buf, cap,
			 "%ld read/write volume(s) were left mounted since the last daemon start: each has the "
			 "name of a volume left open by this Mac, but may not be it. Eject it before you "
			 "unplug its disk.",
			 value);
		return buf;
	default:
		return NULL;
	}
}
