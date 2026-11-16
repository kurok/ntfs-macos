/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stub of the restart reconciliation module. T-M4-05 replaces this file. */
#include "internal.h"

ntfsmac_dmn_status ntfsmac_reconcile_run(ntfsmac_journal *j, ntfsmac_reconcile_cb cb,
					 void *ctx)
{
	(void)j;
	(void)cb;
	(void)ctx;
	return NTFSMAC_DMN_E_NOTIMPL;
}
