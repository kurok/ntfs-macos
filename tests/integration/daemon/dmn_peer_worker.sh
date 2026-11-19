#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-11, I-DMN-19 and I-DMN-21 "a request sent from inside the worker's process group": installed
# by dmn_priv_guest.sh in place of the worker binary, together with a worker sandbox profile that
# allows everything (the real profile denies the XPC lookup, so the request would never reach the
# daemon; this suite wants the daemon's own check 2 to decide). The daemon starts it like a worker,
# as the console user and in its own process group. It runs the command found in
# /var/tmp/dmn-peer.cmd, writes the output to /var/tmp/dmn-peer.out, and then waits so that it is
# still alive while the daemon answers. Disposable guest only; never part of the product.
[ "$(/usr/sbin/sysctl -n kern.hv_vmm_present 2>/dev/null)" = 1 ] || { echo "$0: guest only" >&2; exit 2; }
cmd=$(cat /var/tmp/dmn-peer.cmd 2>/dev/null)
sh -c "$cmd" >/var/tmp/dmn-peer.out 2>&1
echo "exit $?" >>/var/tmp/dmn-peer.out
sleep 60
