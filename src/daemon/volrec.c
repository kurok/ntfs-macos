/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * What a volume record knows of the disk that is there now (T-M7-16; docs/ARCHITECTURE.md
 * section 5). Pure C, no locking: the caller holds the lock of the record.
 *
 * A BSD name is reused by the next disk that appears after the last one went. Everything here
 * belongs to the disk, not to the name: ntfsmac_volrec_reset() runs when a disk appears and when
 * it goes, so `status` never shows the serial, flags or file names of an earlier disk, never
 * looks up its journal entry, and `list` never hides a disk because of what an earlier disk was.
 * A record without a verdict says so (NULL); it is never a zeroed verdict, which could read as
 * clean.
 */
#include <stdlib.h>
#include <string.h>

#include "internal.h"

void ntfsmac_volrec_reset(ntfsmac_volrec *r)
{
	if (r == NULL)
		return;
	free(r->verdict);
	r->verdict = NULL;
}

ntfsmac_dmn_status ntfsmac_volrec_set_verdict(ntfsmac_volrec *r, const ntfsmac_probe_verdict *pv)
{
	if (r == NULL || pv == NULL)
		return NTFSMAC_DMN_E_INVAL;
	if (r->verdict == NULL)
		r->verdict = malloc(sizeof *r->verdict);
	if (r->verdict == NULL)
		return NTFSMAC_DMN_E_IO;
	memcpy(r->verdict, pv, sizeof *pv);
	return NTFSMAC_DMN_OK;
}

const ntfsmac_probe_verdict *ntfsmac_volrec_verdict(const ntfsmac_volrec *r)
{
	return r != NULL ? r->verdict : NULL;
}
