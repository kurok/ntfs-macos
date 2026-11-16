/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stub of the unmount module. T-M4-03 replaces this file. */
#include "internal.h"

ntfsmac_dmn_status ntfsmac_unmount_path(const char *mount_point,
					const ntfsmac_unmount_opts *opts,
					pid_t *busy_pid)
{
	(void)mount_point;
	(void)opts;
	if (busy_pid != NULL)
		*busy_pid = 0;
	return NTFSMAC_DMN_E_NOTIMPL;
}
