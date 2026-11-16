/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>

#include "cli.h"

int main(int argc, char **argv)
{
	return ntfsmac_cli_main(argc, argv, stdout, stderr);
}
