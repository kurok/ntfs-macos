/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Test build of ntfsmac-volmark: the same code with two extra options
 * (--stop-before-write, --no-alarm). Never installed; the product binary is
 * ntfsmac-volmark (main.c).
 */
#include "posix.h"
#include "volmark.h"

int main(int argc, char **argv)
{
	return volmark_main(argc, argv, true);
}
