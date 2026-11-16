/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stub of the Disk Arbitration module. T-M4-02 replaces this file. */
#include "internal.h"

ntfsmac_dmn_status ntfsmac_da_start(const ntfsmac_da_callbacks *cb, void *ctx,
				    ntfsmac_da **out)
{
	(void)cb;
	(void)ctx;
	if (out != NULL)
		*out = NULL;
	return NTFSMAC_DMN_E_NOTIMPL;
}

void ntfsmac_da_stop(ntfsmac_da *da)
{
	(void)da;
}

ntfsmac_dmn_status ntfsmac_da_dissent(ntfsmac_da *da, const char *bsd_name)
{
	(void)da;
	(void)bsd_name;
	return NTFSMAC_DMN_E_NOTIMPL;
}

ntfsmac_dmn_status ntfsmac_da_withdraw(ntfsmac_da *da, const char *bsd_name)
{
	(void)da;
	(void)bsd_name;
	return NTFSMAC_DMN_E_NOTIMPL;
}

ntfsmac_dmn_status ntfsmac_da_describe(ntfsmac_da *da, const char *bsd_name,
				       ntfsmac_disk_info *out)
{
	(void)da;
	(void)bsd_name;
	(void)out;
	return NTFSMAC_DMN_E_NOTIMPL;
}

ntfsmac_dmn_status ntfsmac_da_unmount(ntfsmac_da *da, const char *bsd_name)
{
	(void)da;
	(void)bsd_name;
	return NTFSMAC_DMN_E_NOTIMPL;
}

ntfsmac_dmn_status ntfsmac_da_eject(ntfsmac_da *da, const char *bsd_name)
{
	(void)da;
	(void)bsd_name;
	return NTFSMAC_DMN_E_NOTIMPL;
}
