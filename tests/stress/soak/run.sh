#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# run.sh - soak S-SOAK-01 (T-M5-02, criterion 5.2: 6 hours of mixed workload in the VM, zero hash mismatches, worker
# RSS growth below 10 percent after the first hour). Environment B. Registered default: 10 minutes (baseline at the
# end of minute 1:40, a sixth of the run, as 1 h of 6 h); the criterion run is `run.sh --out DIR --full`
# (6 h, baseline at 1 h). `run.sh --help` lists the options; see tests/stress/soak/README.md.
exec sh "$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)/mix_host.sh" soak "$@"
