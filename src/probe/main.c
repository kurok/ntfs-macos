/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "posix.h"
#include "probe_core.h"

int main(int argc, char **argv)
{
	return ntfsmac_probe_cli(argc, argv, false);
}
