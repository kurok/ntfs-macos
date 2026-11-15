/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "posix.h"
#include "volmark.h"

int main(int argc, char **argv)
{
	return volmark_main(argc, argv, false);
}
