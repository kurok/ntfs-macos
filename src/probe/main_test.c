/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Test build of the probe: the same code with two extra options (--scan-budget-ms,
 * --no-alarm). Never installed; the product binary is ntfsmac-probe (main.c).
 */
#include "posix.h"
#include "probe_core.h"

int main(int argc, char **argv)
{
	return ntfsmac_probe_cli(argc, argv, true);
}
