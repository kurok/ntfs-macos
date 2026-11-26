/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Fixed names of the daemon status codes (internal.h). */
#include "internal.h"

const char *ntfsmac_dmn_status_name(ntfsmac_dmn_status s)
{
	switch (s) {
	case NTFSMAC_DMN_OK: return "ok";
	case NTFSMAC_DMN_E_NOTIMPL: return "not implemented";
	case NTFSMAC_DMN_E_INVAL: return "invalid argument";
	case NTFSMAC_DMN_E_STATE: return "wrong state";
	case NTFSMAC_DMN_E_BUSY: return "busy";
	case NTFSMAC_DMN_E_IO: return "I/O error";
	case NTFSMAC_DMN_E_TIMEOUT: return "timeout";
	case NTFSMAC_DMN_E_DENIED: return "denied";
	case NTFSMAC_DMN_E_RANGE: return "over a bound";
	}
	return "?";
}
