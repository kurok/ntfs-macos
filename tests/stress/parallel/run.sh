#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# run.sh - parallel S-PAR-01 (T-M5-02, criterion 5.3: 16 writer processes for 30 minutes, zero mismatches, no hang
# longer than 60 s). Environment B. Registered default: 5 minutes with 16 writers; the criterion run is
# `run.sh --out DIR --full` (30 min). `run.sh --help` lists the options; see tests/stress/parallel/README.md.
# The workload and the checks are tests/stress/soak/mixlib.py (kind "par"); the host and guest drivers are shared.
exec sh "$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)/../soak/mix_host.sh" par "$@"
