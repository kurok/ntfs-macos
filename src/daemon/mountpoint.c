/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stub of the mount point module. T-M4-03 replaces this file. */
#include "internal.h"

ntfsmac_dmn_status ntfsmac_mountpoint_create(const char *label, size_t label_len,
					     uint64_t serial, uid_t console_uid,
					     gid_t console_gid, ntfsmac_mountpoint *mp)
{
	(void)label;
	(void)label_len;
	(void)serial;
	(void)console_uid;
	(void)console_gid;
	(void)mp;
	return NTFSMAC_DMN_E_NOTIMPL;
}

ntfsmac_dmn_status ntfsmac_mountpoint_remove(ntfsmac_mountpoint *mp)
{
	(void)mp;
	return NTFSMAC_DMN_E_NOTIMPL;
}
