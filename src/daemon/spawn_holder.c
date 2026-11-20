/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "spawn_holder.h"

#include <stddef.h>

bool ntfsmac_holder_is_worker_leftover(const ntfsmac_holder *h, pid_t worker_pid,
				       const ntfsmac_exe_id *known, unsigned nknown)
{
	unsigned i;

	if (h == NULL)
		return false;
	if (worker_pid > 1) {
		if (h->pid == worker_pid)
			return true;
		for (i = 0; i < h->nancestors && i < NTFSMAC_HOLDER_ANC_MAX; i++)
			if (h->ancestors[i] == worker_pid)
				return true;
	}
	if (!h->exe_known || known == NULL)
		return false;
	for (i = 0; i < nknown; i++)
		if (known[i].dev == h->exe_id.dev && known[i].ino == h->exe_id.ino)
			return true;
	return false;
}
