/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stub of the session channel watcher. T-M4-13 replaces this file. The stub fails closed: no session is ever proven clean. */
#include "internal.h"

void ntfsmac_errwatch_init(ntfsmac_errwatch *w)
{
	if (w == NULL)
		return;
	w->flags = 0;
	w->records = 0;
	w->line_len = 0;
	w->line[0] = '\0';
}

ntfsmac_dmn_status ntfsmac_errwatch_feed(ntfsmac_errwatch *w, const char *buf,
					 size_t len)
{
	(void)buf;
	(void)len;
	if (w != NULL)
		w->flags |= NTFSMAC_EW_PROTOCOL_ERROR;
	return NTFSMAC_DMN_E_NOTIMPL;
}

void ntfsmac_errwatch_eof(ntfsmac_errwatch *w)
{
	if (w != NULL)
		w->flags |= NTFSMAC_EW_EOF;
}

bool ntfsmac_errwatch_clean_proof(const ntfsmac_errwatch *w, bool unmount_seen_first,
				  bool re_adopted, int worker_exit_status)
{
	(void)w;
	(void)unmount_seen_first;
	(void)re_adopted;
	(void)worker_exit_status;
	return false;
}
