/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * U-SBX-12: this file uses the test seams of the spawn module. It compiles only if
 * NTFSMAC_TEST_SEAMS is defined; the CMake test of that name compiles it without the definition
 * and expects the compiler to refuse it, and with the definition and expects it to pass, so that
 * the first failure can only be the seams (T-M4-06, audit L5).
 */
#include "spawn_config.h"

int main(void)
{
	ntfsmac_spawn_config c;

	c.sandbox_dir = NULL;
	c.sandbox_exec_path = "/bin/true";
	c.sandbox_owner = 1;
	c.after_check_hook = NULL;
	return 0;
}
