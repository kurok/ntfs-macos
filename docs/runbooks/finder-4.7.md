# Runbook: M4 criterion 4.7 (Finder)

Criterion 4.7 (`docs/IMPLEMENTATION_PLAN.md`, M4): the volume is visible on the desktop and in the Finder
sidebar, and a 1,000-file tree copied in and out with Finder has equal hashes. The checks are
`I-FND-01..02` (`docs/TEST_STRATEGY.md`, section 4). They drive Finder with `osascript`, and macOS asks a
person once to allow that. The approval is given once, in a separate base guest
`ntfsmac-golden-finder`, and every run clones that guest, so the runs need no person. Nothing touches a
disk of the host: the volume is an image file attached inside a disposable guest, and the guest is
deleted at the end. `ntfsmac-golden` is only cloned, never changed.

## Why a separate base guest

The first attended runs (`--attended`, below) failed although the person clicked `Allow`. The guest log
of one of them showed the prompt raised at once, no answer reaching the guest for the whole wait, then
Finder's request ending with "timeout was 1" and a deny entry, so the next events were refused (-1743)
with no new prompt. The same prompt in a freshly booted guest, with no suite set-up before it, was
answered in 28 s. So the approval is now given in a guest that does nothing else, asked again until it is
answered, and kept in that guest's disk.

## Once: build the base guest (a person clicks Allow)

```sh
bash vm/macos/provision-finder-base.sh            # --rebuild makes it again
```

It clones `ntfsmac-golden` to `ntfsmac-golden-finder`, opens its window and prints
`>>> ACTION NEEDED (try N of 5)`. Click `Allow` on the '"tart-guest-agent" wants access to control
"Finder"' dialog. If no answer arrives in 120 s, the script clears the guest user's Apple-event entries
(`tccutil reset AppleEvents`, inside the guest only) and asks again. It then checks that a second event is
answered with no prompt, and stops the guest. If no try succeeds, it deletes `ntfsmac-golden-finder`.
Rebuild it whenever `ntfsmac-golden` is rebuilt.

## Before you start

- The release build exists: `cmake --preset release && cmake --build --preset release`.
- The golden guest `ntfsmac-golden` exists (`tart list`), and fewer than two guests are running.
- A fixture directory with the clean image (Docker must run):
  `sh tests/fixtures/gen/clean.sh --only clean-64m-4k --export /tmp/fx`
- The NTFS-3G source is in `third_party/cache` (`scripts/fetch-deps.sh ntfs-3g`), or you pass a
  `--deps-tar` file from an earlier run to skip the build in the guest.

## The command (run it in a host terminal, from the repository root)

```sh
sh tests/integration/daemon/dmn_vm.sh --finder-base --fixtures /tmp/fx --deps-tar /tmp/ntfsmac-deps.tar \
    --out /tmp/finder-4.7.out finder
```

`--finder-base` clones `ntfsmac-golden-finder` and makes the Finder cases required. Add `--graphics` to
watch the guest window. The older `--attended` (clone `ntfsmac-golden` and wait for a click before the
suite) still works when the click arrives; it is what lost the click above.

If `/tmp/ntfsmac-deps.tar` does not exist yet, the script builds NTFS-3G in the guest (about 10 minutes
more) and saves the tar there.

## What you will see with `--attended`

1. The terminal prints the clone and start of the guest, then a guest window opens (the desktop of the
   user `admin`, with a Finder window).
2. The terminal prints this line and waits, up to 5 minutes:

   `dmn_vm: >>> ACTION NEEDED: in the VM window, click Allow on the '"tart-guest-agent" wants access to control "Finder"' prompt`

   A dialog with that text appears in the guest window.
3. **Click `Allow`** in the guest window. Do this once. (`Don't Allow`, or no click in 5 minutes, makes the
   Finder cases FAIL; run the command again.)
4. The terminal prints `Finder automation allowed`, and the suite runs alone. Do not click or type in the
   guest window until the end; the screenshots show its screen. Finder windows open and close by
   themselves, and Finder copies the 1,000 files in and out (a few minutes).
   With `--finder-base` steps 2 to 4 are replaced by one line, `Finder automation allowed (from
   ntfsmac-golden-finder)`.
5. The terminal prints `PASS` or `FAIL` for each check, then `result: 0 check(s) failed` and
   `######## suite finder: exit 0`. The guest window closes and the guest is deleted.

Total time: about 10 to 15 minutes with `--deps-tar`.

## Where the results land

- Evidence: `tests/results/evidence/m4-gate/finder/DATE-COMMIT/` (change it with `--evidence DIR`):
  - `fnd01-desktop.png`: the desktop with the volume icon.
  - `fnd01-sidebar.png`: a Finder window of the volume with the sidebar.
  - `fnd02-source`, `fnd02-volume`, `fnd02-out` with the suffixes `.raw`, `.lines`, `.count`: file count and
    SHA-256 of each file, for the source, the volume and the copy-out.
- Output: `/tmp/finder-4.7.out` (the `--out` file).
- Look at the two PNG files before you accept 4.7: the check only proves that a screenshot was taken, a
  person judges that the volume icon and the sidebar entry show in it. If the volume shows under another
  name than the mount point leaf (`macFUSE Volume`, U14), that is expected.

## Pass rule

Every `PASS`, no `FAIL`, no `NOT RUNNABLE` line, exit status 0. With `--attended` a refused permission is
a `FAIL`, and so it is with `--finder-base`. Without either (for example `dmn_vm.sh finder`, unattended) the Finder cases are reported
`NOT RUNNABLE` and never count as passed; they do not make 4.7 pass.

## If something goes wrong

- `Finder automation was NOT allowed in the clone of ntfsmac-golden-finder`: the base guest has no
  approval (for example `ntfsmac-golden` was rebuilt, or tart-guest-agent changed). Run
  `bash vm/macos/provision-finder-base.sh --rebuild`. Never edit a permission database on the host.
- The guest is left behind after an interrupted run: `sh vm/macos/tart.sh delete NAME` (`tart list` shows
  `ntfsmac-dmn-*`). Never delete `ntfsmac-golden`; delete `ntfsmac-golden-finder` only to rebuild it.
