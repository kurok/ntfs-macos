/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Another driver's mount at attach time (T-M7-11; docs/ARCHITECTURE.md section 4.1 steps 3 and
 * 5, ADR-003). Pure C: the Disk Arbitration calls go through ntfsmac_claim_ops.
 *
 * A candidate of the filter of step 1 is not necessarily NTFS: MBR type 07 is also exFAT, the
 * GPT basic-data GUID is also FAT32 and exFAT. The probe cannot look before a mount is removed
 * (EBUSY on the open, U18), so the volume kind of Disk Arbitration decides alone whether a mount
 * may be removed: only Apple's NTFS driver ("ntfs") or a kind that is unknown. Whatever the
 * daemon took, it gives back: the dissent is released at every end of the attach flow that is not
 * our own mount, and a mount the daemon removed is asked for again.
 */
#include <string.h>

#include "da_filter.h"
#include "internal.h"

static bool has_ntfs(const char *s)
{
	size_t i, n = strlen(s);

	for (i = 0; i + 4 <= n; i++) {
		char a = s[i], b = s[i + 1], c = s[i + 2], d = s[i + 3];

		if ((a == 'n' || a == 'N') && (b == 't' || b == 'T') && (c == 'f' || c == 'F') &&
		    (d == 's' || d == 'S'))
			return true;
	}
	return false;
}

ntfsmac_kind_class ntfsmac_kind_classify(const char *kind)
{
	if (kind == NULL || kind[0] == '\0')
		return NTFSMAC_KIND_UNKNOWN; /* only "no kind at all" */
	if (strcmp(kind, "ntfs") == 0 || strcmp(kind, "NTFS") == 0)
		return NTFSMAC_KIND_NTFS;
	if (has_ntfs(kind))
		return NTFSMAC_KIND_OTHER_NTFS;
	if (strcmp(kind, NTFSMAC_DA_KIND_UNREADABLE) == 0)
		return NTFSMAC_KIND_OTHER;
	return NTFSMAC_KIND_NOT_NTFS;
}

ntfsmac_claim_result ntfsmac_claim_take(const ntfsmac_claim_ops *ops, const char *bsd, bool mounted,
					const char *kind, bool *we_unmounted)
{
	ntfsmac_kind_class k = ntfsmac_kind_classify(kind);
	ntfsmac_dmn_status st;

	if (we_unmounted != NULL)
		*we_unmounted = false;
	/* Known to be another file system, mounted or not: never unmounted, never probed. */
	if (k == NTFSMAC_KIND_NOT_NTFS)
		return NTFSMAC_CLAIM_NOT_NTFS;
	if (!mounted)
		return NTFSMAC_CLAIM_FREE;
	if (k == NTFSMAC_KIND_OTHER_NTFS || k == NTFSMAC_KIND_OTHER)
		return NTFSMAC_CLAIM_OTHER_DRIVER; /* not Apple's: left alone (section 8.4) */
	if (ops == NULL || ops->unmount == NULL)
		return NTFSMAC_CLAIM_UNMOUNT_FAILED;
	st = ops->unmount(ops->ctx, bsd);
	if (st == NTFSMAC_DMN_OK) {
		if (we_unmounted != NULL)
			*we_unmounted = true;
		return NTFSMAC_CLAIM_FREE;
	}
	if (st == NTFSMAC_DMN_E_STATE)
		return NTFSMAC_CLAIM_FREE; /* it was gone already: nothing of ours to give back */
	return NTFSMAC_CLAIM_UNMOUNT_FAILED;
}

bool ntfsmac_claim_finish(const ntfsmac_claim_ops *ops, const char *bsd, ntfsmac_vol_state end,
			  bool we_unmounted, bool device_free)
{
	if (end == NTFSMAC_VOL_MOUNTING || end == NTFSMAC_VOL_MOUNTED || end == NTFSMAC_VOL_UNMOUNTING)
		return false; /* our own volume keeps its dissent until the disk is gone */
	if (!device_free)
		return false; /* a worker may still hold the device: no other driver may mount it */
	if (ops == NULL || ops->release == NULL)
		return false;
	(void)ops->release(ops->ctx, bsd, we_unmounted);
	return true;
}

bool ntfsmac_verdict_not_ntfs(const ntfsmac_probe_verdict *pv)
{
	return pv != NULL && !pv->is_ntfs && !pv->bitlocker &&
	       (pv->reason_codes & NTFSMAC_PROBE_REASON_BIT(NTFSMAC_PROBE_REASON_NOT_NTFS)) != 0;
}

void ntfsmac_devbusy_note(bool *busy, ntfsmac_dmn_status check, bool mount_left)
{
	if (busy == NULL)
		return;
	if (mount_left || (check != NTFSMAC_DMN_OK && check != NTFSMAC_DMN_E_STATE))
		*busy = true;
	else if (check == NTFSMAC_DMN_OK)
		*busy = false;
}

void ntfsmac_devbusy_reset(bool *busy)
{
	if (busy != NULL)
		*busy = false;
}

bool ntfsmac_devbusy_free(bool busy, bool this_attach_free)
{
	return this_attach_free && !busy;
}
