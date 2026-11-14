# SPDX-License-Identifier: GPL-2.0-or-later
"""Run cases and produce the case records of a result file."""

import os
import shutil
import sys
import tempfile
import time
import traceback

from .core import Ctx, Fail, Skip


def run_cases(cases, env, run_id, results_dir, transport, tmp_root=None, quiet=False, meta=None):
    """Run cases in order. Evidence goes to <results_dir>/evidence/<run_id>/<ID>.*;
    returned evidence paths are relative to results_dir. `meta`, if given, collects ctx.meta of every case."""
    records = []
    ev_rel = "evidence/%s" % run_id
    ev_dir = os.path.join(results_dir, "evidence", run_id)
    for c in cases:
        work = tempfile.mkdtemp(prefix="ntfsmac-case-", dir=tmp_root)
        ctx = Ctx(c, env, work, ev_dir, transport, quiet=quiet)
        t0 = time.monotonic()
        status, note = "pass", None
        ctx.log("start %s (env %s)" % (c.id, env))
        try:
            c.func(ctx)
        except Skip as e:
            msg = str(e).strip()
            if msg:
                status, note = "skip", msg
            else:
                status, note = "error", "skip raised without a note"
        except Fail as e:
            status, note = "fail", str(e) or "failed"
        except AssertionError as e:
            status, note = "fail", str(e) or "assertion failed"
        except Exception as e:
            status, note = "error", "%s: %s" % (type(e).__name__, e)
            ctx.log("traceback:\n" + traceback.format_exc())
        dur = round(time.monotonic() - t0, 3)
        if meta is not None:
            meta.update(ctx.meta)
        if ctx.close() and status == "pass":
            status, note = "error", "cleanup failed (see log)"
        ctx.log_final("end %s: %s%s" % (c.id, status, (" - " + note) if note else ""))
        shutil.rmtree(work, ignore_errors=True)
        evidence = sorted("%s/%s" % (ev_rel, fn) for fn in os.listdir(ev_dir) if fn.startswith(c.id + "."))
        records.append({"id": c.id, "status": status, "duration_s": dur, "evidence": evidence, "note": note})
        if not quiet:
            sys.stderr.write("%-5s %s %.1fs%s\n" % (status.upper(), c.id, dur, (" (" + note + ")") if note else ""))
    return records
