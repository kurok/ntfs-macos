/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * T-M4-11, I-DMN-23: a probe that is wrong on purpose. It is the real command line of
 * src/probe/cli.c with one change: cli.c calls dmn_stub_run (cli.c alone is built with
 * -Dntfsmac_probe_run=dmn_stub_run), which runs the real checks and then reports the volume `clean`, whatever they found.
 * The label, serial and geometry stay the real ones, so the daemon's binding and the journal key
 * work as with the real probe. Installed in place of ntfsmac-probe in a disposable guest only;
 * it is never part of the product.
 */
#include <unistd.h>

#include "probe_core.h"

int dmn_stub_run(int fd, const ntfsmac_probe_opts *opts, ntfsmac_probe_verdict *v);

int dmn_stub_run(int fd, const ntfsmac_probe_opts *opts, ntfsmac_probe_verdict *v)
{
	int rc;

#ifdef DMN_STUB_SLOW
	/* A slow probe (a loaded host): the real verdict, DMN_STUB_SLOW seconds late. */
	sleep(DMN_STUB_SLOW);
	return ntfsmac_probe_run(fd, opts, v);
#endif
	rc = ntfsmac_probe_run(fd, opts, v);
	if (rc != NTFSMAC_PROBE_EXIT_OK)
		return rc;
	v->state = NTFSMAC_PROBE_STATE_CLEAN;
	v->reason_codes = 0;
	v->volume_flags = 0;
	v->logfile = NTFSMAC_PROBE_LOGFILE_CLEAN;
	if (!v->has_logfile_lsn) {
		v->has_logfile_lsn = true;
		v->logfile_lsn = UINT64_C(0x100400);
	}
	v->hiberfile = NTFSMAC_PROBE_HIBERFILE_ABSENT;
	v->nonnfc_count = 0;
	v->nonnfc_path_count = 0;
	return NTFSMAC_PROBE_EXIT_OK;
}
