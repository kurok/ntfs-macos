/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stub of the control socket. T-M4-04 replaces this file. */
#include "internal.h"

ntfsmac_dmn_status ntfsmac_ctl_start(const char *socket_path, const ntfsmac_ctl_ops *ops,
				     void *ctx, ntfsmac_ctl **out)
{
	(void)socket_path;
	(void)ops;
	(void)ctx;
	if (out != NULL)
		*out = NULL;
	return NTFSMAC_DMN_E_NOTIMPL;
}

void ntfsmac_ctl_stop(ntfsmac_ctl *c)
{
	(void)c;
}

ntfsmac_dmn_status ntfsmac_ctl_handle_line(const ntfsmac_ctl_ops *ops, void *ctx,
					   const ntfsmac_ctl_peer *peer, const char *line,
					   size_t len, char *reply, size_t cap)
{
	(void)ops;
	(void)ctx;
	(void)peer;
	(void)line;
	(void)len;
	if (reply != NULL && cap > 0)
		reply[0] = '\0';
	return NTFSMAC_DMN_E_NOTIMPL;
}
