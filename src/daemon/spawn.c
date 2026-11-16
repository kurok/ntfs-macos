/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stub of the spawn and supervision module. T-M4-01 replaces this file. */
#include "internal.h"

ntfsmac_dmn_status ntfsmac_spawn_probe(const ntfsmac_probe_run *run,
				       ntfsmac_probe_verdict *verdict)
{
	(void)run;
	(void)verdict;
	return NTFSMAC_DMN_E_NOTIMPL;
}

ntfsmac_dmn_status ntfsmac_spawn_volmark(const ntfsmac_volmark_run *run,
					 bool set, int *exit_code)
{
	(void)run;
	(void)set;
	(void)exit_code;
	return NTFSMAC_DMN_E_NOTIMPL;
}

ntfsmac_dmn_status ntfsmac_spawn_worker(const ntfsmac_worker_run *run,
					ntfsmac_child *child, bool *mounted_ro)
{
	(void)run;
	(void)child;
	(void)mounted_ro;
	return NTFSMAC_DMN_E_NOTIMPL;
}

ntfsmac_dmn_status ntfsmac_child_poll(ntfsmac_child *child, bool *exited,
				      int *exit_status)
{
	(void)child;
	(void)exited;
	(void)exit_status;
	return NTFSMAC_DMN_E_NOTIMPL;
}

ntfsmac_dmn_status ntfsmac_child_kill_group(ntfsmac_child *child)
{
	(void)child;
	return NTFSMAC_DMN_E_NOTIMPL;
}
