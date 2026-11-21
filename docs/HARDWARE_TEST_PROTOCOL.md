# Hardware test protocol (Environment C, T-M5-10, criterion 5.8)

Status: draft for the human operator. Nothing in this document has been run. Every statement that the repository does
not prove is marked **[U]** (an assumption to check while running; if it is wrong, record that as a finding, do not
bend the step to make it fit).

Purpose: show on real USB media, with real unplugging and with Windows as the judge, what a virtual disk cannot show
(TEST_STRATEGY.md section 2, "What the VM cannot show"; R-19, R-30). Cases `H-USB-01` to `H-USB-11` and `H-FND-01` are the
ones in TEST_STRATEGY.md section 11; `H-SYNC-01`, `H-NAME-01` and `H-PERF-01` are added here for R-30, the Windows
half of U22 and the physical-disk measurement left open by `docs/spikes/M5-perf.md` (their ids are new **[U]**: the
coordinator may rename them). The run resolves U12, U15 and U23 and answers R-26 and R-30 as far as hardware can.

Without a signed run (section 12) the release is labelled "VM-verified only, not production-ready" (IMPLEMENTATION_PLAN.md
criterion 5.8, exit gate M5).

## Contents

1. Who does what, and what is needed
2. Safety rules
3. One-time preparation
4. Common procedures (P1 to P10)
5. Cases `H-USB-01` to `H-USB-07`, `H-FND-01`
6. Cases `H-USB-08` and `H-USB-09`: U12
7. Case `H-USB-10`: U15
8. Case `H-USB-11`: U23
9. Cases `H-SYNC-01`, `H-NAME-01`, `H-PERF-01`
10. Order of work and time
11. Recording results
12. Sign-off

## 1. Who does what, and what is needed

The operator is a person. Agents prepare and later analyse; they do not run any step of this protocol, do not type
the administrator password, and do not sign (AGENTS.md: no write to physical media without human approval; H5).

| Need | Detail |
|---|---|
| Disposable media (H5) | **F**: one USB flash drive. **S**: one USB SSD or HDD. Both hold nothing you want to keep; both will be formatted, partitioned, overwritten and unplugged without ejecting, many times. Each is written down by vendor, model, size and **serial number** before anything else (section 3). 8 GB or more each; F of 64 GB or less keeps the image copies small. |
| Windows machine (H6) | Windows 10 or 11, administrator rights, PowerShell 5.1 or later, a USB port for F and S. A virtual machine with USB pass-through is acceptable only if it is declared in the sign-off **[U]** (pass-through can change cache and removal behaviour). |
| The Mac | Apple silicon, macOS 27, with the developer install of this commit: `packaging/dev-install.sh` (section 3). A second USB port; a direct port, not a hub, unless the device table says otherwise. |
| Other NTFS drivers (H7) | None may be active on the Mac during the run (Paragon NTFS and similar, R-17). Use a Mac or boot volume without them, or switch them off in their own settings for the test window; record which. The agents never switch them off. |
| A GUI session | `H-FND-01`, `H-USB-11` and `H-USB-05` need someone logged in at the Mac screen (Finder, notifications). |
| Staging space on the Mac | At least 5 GiB free on the internal disk: `tree-1g` (1 GiB), `tree-big` (3 GiB), the evidence texts and the `--copy` images (each is the size of the NTFS volume: 64 MiB for the fixtures, the whole formatted size for a Windows-formatted stick; set `--copy-dir` to a disk with room). Never on the device under test. |
| The Windows PC's own disks | Never used for any step. Hibernation and Fast Startup legs use the PC only as the Windows that sees the disposable disk, or a Windows installed on external media (H-USB-05). |
| Time | Plan for two working days (about 16 hours of hands-on time, section 10), with a rest day if the disks heat up. |

## 2. Safety rules

These are hard rules. A step that seems to need breaking one is a reason to stop and ask the coordinator.

1. **Only the two named disks.** Before the first step, unplug every other external disk, card, phone and stick from both machines. Every destructive or repairing step (anything that formats, partitions, clears, writes a device node or a fixture image, runs `chkdsk /f`, enables BitLocker, writes in bulk, wipes, or runs `journal-clear` or `repair`) is preceded by the identity check of P1 (Mac: `confirm_disposable`) or of P2 (Windows: `Confirm-Disposable`, which resolves the drive letter to the disk and its serial) and, for the formatting, clearing, wiping and BitLocker steps, your own typed confirmation that the disk is the disposable one. Never run such a step from shell history or from a script you did not read.
2. **Device numbers change.** `disk4` today is `disk5` after the next plug. Re-run P1 or P2 after every plug before a destructive step. Identify by serial number, model and size, never by number alone.
3. **No internal disk, ever.** `tests/hardware/identify.sh` refuses the boot disk, anything that is not `Device Location: External`, anything over 512 GB. Treat a refusal as final; do not edit the script to get past it.
4. **Never use** `ntfsfix`, `ntfsmac-worker` options `recover`, `remove_hiberfile` or `fsname`, `mount -t ntfs`, `ntfs-3g` or `mkntfs` from a shell on these disks, and never `ntfsmac repair --clear-dirty` outside the case that names it. The product itself never emits `recover` or `remove_hiberfile` (D4); do not add them by hand.
5. **Repair commands are explicit.** The only Mac commands that change an unsafe volume are `ntfsmac journal-clear` (H-USB-08) and `ntfsmac repair --sync-mirror` (H-USB-10 leg C), each typed by you inside `confirm_disposable && ...`. In the dev install (the root-peer fallback of ADR-019, no signed CLI) they run under `sudo`: the command asks for the label shown by `ntfsmac list`, then for the last 8 hex digits of the **NTFS volume serial** (the `serial` field of the probe and of `ntfsmac status`, for example `0x1A2B3C4D5E6F7788` gives `5E6F7788`; this is not the USB serial), and your password is typed in the terminal, not in a system dialog **[U: the exact prompt sequence of the root-peer path]**. Nobody else types that password.
6. **Windows `chkdsk /f` and `/scan` are part of the experiment.** Run them where a case says so, and only there. Do not let Windows fix a volume "in passing": when a prompt "Scan and fix" appears and the case does not say to accept it, close it (H-USB-09 depends on this).
7. **One writer at a time.** The volume is attached to the Mac or to Windows, never both. Eject on one side (or unplug, where the case says so) before plugging into the other.
8. **Never place a Mac `Time Machine`, Spotlight or other backup target on these disks**; do not format them as anything other than what a step says.
9. **No second use of a disposable disk for real data**, before or after. After the run, wipe both with a full format on Windows.
10. **Record everything that is surprising**, even if it is not a pass criterion. A result that is unexplained is INCONCLUSIVE, not PASS.
11. **If a run is interrupted** (power cut, crash, disk disappears at an unplanned moment), write down the time and the state, run the Mac snapshot (P6) and the Windows snapshot (P7), and decide with the coordinator whether the disk is reset (P3) or kept as a finding.

## 3. One-time preparation

### 3.1 The Mac

```sh
git rev-parse HEAD                      # the commit under test; the tree must be clean: git status --short
packaging/dev-install.sh --job-arg --automount-mode=ro   # as your own user; it calls sudo; installs the daemon, CLI and launchd job
ntfsmac doctor                          # keep the output as evidence
ls /Library/Filesystems                 # look for another NTFS driver; record the result
systemextensionsctl list                # macFUSE approved and enabled; no other NTFS extension [U: exact output]
```

**Read-only automount for the whole run, except `H-USB-01`.** With `--automount-mode=ro` the daemon attaches every volume read-only: no volmark, no dirty flag, no `.fseventsd`, nothing written. That keeps every "before Windows" and "after Windows" snapshot free of a Mac write (a clean volume would otherwise be mounted `rw` at once by the default automount, which changes `.fseventsd`, the flags and the log before you can look). A case that needs a read/write session asks for it itself (P4rw). `H-USB-01` tests the default automount and is run first with a plain `packaging/dev-install.sh` (no `--job-arg`); then install again with the line above (`--no-build` is enough) before the next case. Record in the results which mode each case ran in.

The dev install has no switch for `journal-clear` rule 2 (the evidence rule): the product build ships it disabled (ARCHITECTURE.md section 3.5). Do not install a build with test seams (`--allow-seams`) for this run.

Check that the install carries patch 0009 (Finder tags, ADR-025) **[U: no direct query exists]**: the case `H-USB-11` checks it with a write of a `:` stream name before it relies on it.

Set up the working shell once, and use the same shell for the whole session. Replace the values; `SERIAL` is the serial of the disk you are working with, copied from the label or from System Information > USB (`SPUSBDataType` no longer exists on macOS 27; `tests/hardware/identify.sh` reads the serial of the right device from `ioreg`). `confirm_disposable` refuses an empty or "Not Provided" serial: a disk without a USB serial cannot be used for this run.

```sh
cd <your checkout>
export EV=tests/results/evidence/hardware-$(date -u +%Y%m%d)    # evidence directory; make it: mkdir -p $EV
export DISK=disk4 SLICE=disk4s1 SERIAL='<serial of the disposable disk>'
confirm_disposable() {
	case $SLICE in "$DISK"s[0-9]*) ;; *) echo "SLICE $SLICE is not a slice of $DISK" >&2; return 1 ;; esac
	sh tests/hardware/identify.sh "$DISK" --serial "$SERIAL" || return 1
	printf 'Type DESTROY %s to confirm that %s is the disposable disk: ' "$SERIAL" "$DISK"
	read a && [ "$a" = "DESTROY $SERIAL" ]
}
```

Every destructive Mac command in this document is written `confirm_disposable && <command>`. If `confirm_disposable` fails, the command does not run.

### 3.2 The test trees (staging, on the Mac's own disk)

```sh
python3 -I tests/hardware/mktree.py gen ~/hwstage/tree-1g --mib 1024          # about 1 GiB, many small files, MANIFEST.sha256
python3 -I tests/hardware/mktree.py gen ~/hwstage/tree-big --mib 3072 --seed 77   # for the unplug-during-copy cases
```

`tree-1g` is the "tree-1g" of H-USB-01 (TEST_STRATEGY.md names it; no fixture of that name exists in the repository, this is its definition **[U]**). Copy `~/hwstage/tree-1g` to the Windows machine once (network or an external disk that is **not** F or S) so that it can verify by hash too.

### 3.3 The Windows machine

Elevated PowerShell. Create `C:\hwtest\evidence` for output (never on F or S). Copy `tests/hardware/windows-collect.ps1` there.

```powershell
Get-ComputerInfo | Select-Object WindowsProductName, WindowsVersion, OsBuildNumber
powercfg /a                                                  # which sleep states exist; Fast Startup is listed under "Hibernate"
reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Power" /v HiberbootEnabled   # 1 = Fast Startup on
```

Disk identity and the confirmation function (used by every destructive Windows step):

```powershell
Get-Disk | Format-Table Number, FriendlyName, SerialNumber, Size, BusType, PartitionStyle, IsBoot, IsSystem
function Confirm-Disposable {
    # -DiskNumber N or -Letter X (the letter is resolved to its disk); exact serial match; -Typed asks you to type DESTROY.
    param([int]$DiskNumber = -1, [string]$Letter, [Parameter(Mandatory = $true)][string]$Serial, [switch]$Typed)
    $Serial = $Serial.Trim()
    if ($Serial.Length -lt 6) { throw 'serial is empty or too short' }
    if ($Letter) { $DiskNumber = (Get-Partition -DriveLetter $Letter -ErrorAction Stop).DiskNumber }
    if ($DiskNumber -lt 0) { throw 'give -DiskNumber or -Letter' }
    $d = Get-Disk -Number $DiskNumber -ErrorAction Stop
    $d | Format-List Number, FriendlyName, SerialNumber, Size, BusType, IsBoot, IsSystem
    if ($d.BusType -ne 'USB' -or $d.IsBoot -or $d.IsSystem) { throw 'not an external USB data disk' }
    if ($d.Size -gt 512GB) { throw "disk is larger than 512 GB ($($d.Size) bytes)" }
    if (-not $d.SerialNumber -or $d.SerialNumber.Trim() -ne $Serial) { throw "serial '$($d.SerialNumber)' is not exactly '$Serial'" }
    if ($Typed -and (Read-Host "Type DESTROY $Serial to confirm disk $DiskNumber is the disposable one") -ne "DESTROY $Serial") { throw 'not confirmed' }
    "disk $DiskNumber confirmed"
}
```

Call it before **every** step that writes in a way you cannot undo, with the drive letter once the volume has one: `Confirm-Disposable -Letter X -Serial $S` before each `chkdsk X: /f`, before leg D's bulk writes, and `Confirm-Disposable -Letter X -Serial $S -Typed` (or `-DiskNumber $N -Typed`) before P3, `Enable-BitLocker`, and the final wipe (section 10). `$S` is the Windows form of the serial, exactly as `Get-Disk` prints it (trimmed). Windows may print a USB serial in another form than macOS (padded, hex of the ASCII, or the bridge's own number) **[U]**. If the strings differ, write both forms in the device table, compare vendor, model and exact size on both machines, and put the Windows form in `$S`. Do not weaken the check; a disk whose serial Windows cannot show is not usable.

### 3.4 The fixture images for `H-USB-10`

The images are made by the coordinator (or by you, if Docker is available) in Environment A, on the Mac that has the checkout, before the run:

```sh
sh tests/fixtures/gen/mirror.sh --only mirror-split --export build/hw-fixtures   # mirror-split (record 0)
sh tests/fixtures/gen/vmk.sh --export build/hw-fixtures                         # vmk-split and vmk-split-clear (record 3)
find build/hw-fixtures -name '*.img' -exec shasum -a 256 {} +                   # record these values at generation
```

`build/` is git-ignored. The images are not bit-identical across generations (NTFS-3G stamps wall-clock times), so the SHA-256 recorded at generation is the identity of the image used; write it in the results and re-check it with `shasum -a 256` before every P9. `mirror-split` has the same content as the sweep's `smallfiles-c68.img.gz`; `mirror.sh --only sweep --export DIR` regenerates the sweep images (`sweep-smallfiles-c68` **[U: file name]**) and is the way to get that exact one; the protocol uses `mirror-split` for legs A and C. The exported file names below `build/hw-fixtures` are **[U]**; use `find`. Each is a 64 MiB bare NTFS volume.

### 3.5 Privacy of the evidence

The collectors print `<host>` instead of the machine name. Before anything under `tests/results/` is committed, check every evidence file for the host names, user names and home paths of both machines, and replace them by `<host>`, `<user>`, `<home>`:

```sh
grep -rIn -e "$(uname -n)" -e "$(uname -n | cut -d. -f1)" -e "$USER" "$EV"   # and the Windows machine name and user, by hand
```

USB serial numbers of the disposable media stay in the results (TEST_STRATEGY.md section 10 asks for them). The operator is named in the sign-off by GitHub handle or initials, not by full name.

### 3.6 The device table

Fill in the device table of the results file (section 11) now: vendor, model, serial (Mac form and Windows form), size, USB version, port, cable, and the Windows `DriveType` that `Get-Volume` shows later (`Removable` or `Fixed`: this decides what Windows does at shutdown and at plug-in, **[U]** and matters for `H-USB-05`).

## 4. Common procedures

Names used in the cases. **Mac** commands run in the prepared shell of 3.1; **Win** commands in elevated PowerShell. `X:` is the drive letter Windows gives the volume.

**P1. Identify the disk (Mac).** Plug the device. Wait 15 s.
```sh
diskutil list external                       # find the disk of the right size and model
export DISK=disk4 SLICE=disk4s1              # set from the line above; the slice is the NTFS partition
sh tests/hardware/identify.sh $DISK --serial "$SERIAL"
```
It must print `OK` and `serial matches the one given`. If it prints REFUSED, stop.

**P2. Identify the disk (Windows).** `Get-Disk | Format-Table Number, FriendlyName, SerialNumber, Size, BusType, IsBoot, IsSystem`, then `Get-Volume` for the letter. `$N` is the disk number.

**P3. Make a Windows-clean volume.** Used before every case that needs "a volume Windows last touched cleanly" (this also makes the `$LogFile` a Windows-written one, which U12 needs: the synthetic logs of `logfile_mark --init` are not a baseline, TEST_STRATEGY.md section 3.2). On Windows:
```powershell
$N = 3; $S = '<serial, Windows form>'                      # from P2
Confirm-Disposable -DiskNumber $N -Serial $S -Typed       # types DESTROY; stops on any mismatch
Clear-Disk -Number $N -RemoveData -RemoveOEM -Confirm:$false
Initialize-Disk -Number $N -PartitionStyle MBR
$p = New-Partition -DiskNumber $N -UseMaximumSize -AssignDriveLetter -MbrType IFS
Format-Volume -DriveLetter $p.DriveLetter -FileSystem NTFS -NewFileSystemLabel HWTEST -Confirm:$false
$X = $p.DriveLetter
Set-Content -Path "${X}:\W-baseline.txt" -Value "written by Windows $(Get-Date -Format o)"
New-Item -ItemType Directory "${X}:\winfolder" | Out-Null
Copy-Item "${X}:\W-baseline.txt" "${X}:\winfolder\copy.txt"
Write-VolumeCache -DriveLetter $X                         # flush
fsutil dirty query "${X}:"                                # must say NOT Dirty
```
Then P8 (eject on Windows). The default NTFS cluster size is used (4096 on these sizes **[U]**; record it: `fsutil fsinfo ntfsinfo X:`). MBR is used on purpose (type 0x07), the same layout as P9.

**P4. Plug into the Mac and check the attach.**
```sh
ntfsmac list
ntfsmac status $SLICE                                     # state, mount (rw or ro), mountpoint, reason, journal member
mount | grep "$SLICE "
tail -n 40 /var/log/ntfsmacd.log                          # the daemon's decision in words [U: exact lines]
```
Record the whole output. With the read-only automount of 3.1 the expected state of a Windows-clean volume is `Mounted`, `mount: ro` (only `H-USB-01`, installed with the default automount, expects `rw`). If Apple's own driver mounted it read-only first (U21), record that and run `diskutil unmount $DISK`; the daemon then decides. Re-run P1 after **every** plug before a command that writes, because the disk number can change.

**P4rw. Ask for a read/write session** (cases that write to the volume). After P4 shows `Mounted`, `ro`: `ntfsmac unmount $SLICE`, wait for `Appeared`, then `ntfsmac mount $SLICE`; the volume must show `mount: rw`. Record the replies (a volume that the policy keeps read-only, with its reason, is a finding, not something to override). The mount point is `MP=$(ntfsmac status $SLICE | sed -n 's/^  mountpoint: //p')` **[U: field name as the hook of the perf tests parses it]**.

**P5. Unplug (the experiment, not an accident).** Pull the USB connector out of the Mac's port by hand, at once, without eject, without `unmount`, without sleep. Note the second: have `date -u +%T` typed and press Enter at the moment. For "while idle" cases: no process of yours touches the volume for 60 s before the pull. Do not plug back before 10 s have passed. After the pull, record:
```sh
ntfsmac list                                              # the volume is gone
ps -axo pid,comm | grep ntfsmac-worker                   # no worker left
sudo ls -l /var/db/ntfs-macos/journal                    # the journal entry of this serial is still there (the directory is root-only)
tail -n 60 /var/log/ntfsmacd.log
```
Also write what the screen showed (a "Disk Not Ejected Properly" notification, the product's notification, a dialog).

**P6. Mac snapshot** (read-only). Takes the flags, `current_lsn`, state and journal as the Mac sees them:
```sh
sh tests/hardware/collect.sh --disk $DISK --slice $SLICE --serial "$SERIAL" --tag <case>-<moment> --out "$EV" [--copy [--copy-dir ~/hwimages]]
```
It prints `state`, `reason_codes`, `volume_flags`, `logfile`, `logfile_lsn` (this is `current_lsn`, from the product's own read-only probe), `hiberfile`, the journal entries (read by a root shell: the entry's `logfile_lsn_at_start` is the value the evidence rule compares with) and writes `$EV/<tag>.txt`. `--copy` also copies the NTFS volume only (the size the probe reports, not the rest of the partition) to `~/hwimages/<tag>.img` (default `--copy-dir`, outside the repository; `*.img` is ignored by `.gitignore` anyway) and prints its SHA-256, so that the exact flags and restart pages can be dumped later in Environment A (`logfile_mark IMG --dump`, `setvolflags IMG --show`). Use `--copy` where a case says so; images are not committed, only their hashes go into the evidence text (TEST_STRATEGY.md section 10).

**P7. Windows snapshot** (read-only):
```powershell
powershell -ExecutionPolicy Bypass -File C:\hwtest\windows-collect.ps1 -Drive X -Tag <case>-<moment> -Out C:\hwtest\evidence
```
It records disk and volume identity, `fsutil dirty query`, `fsutil fsinfo ntfsinfo` and `volumeinfo`, and the recent `Chkdsk`, `Ntfs`, `Microsoft-Windows-Ntfs` and `Wininit` events (host name printed as `<host>`). It never runs `chkdsk`.

**P8. Eject on Windows.** `Write-VolumeCache -DriveLetter X`, then the tray icon "Safely Remove Hardware", wait for "can be safely removed", then pull the cable.

**P11. Windows scan after a scenario.** After every scenario that ends with the disk on Windows (the case says where), with the tag of the case:
```powershell
chkdsk X: /scan *>&1 | Tee-Object C:\hwtest\evidence\<tag>-chkdsk-scan.txt
fsutil dirty query X:
```
Keep the file; the verdict (the last 10 lines, verbatim) goes in the record table. `/scan` is the scan after a scenario; `/f` is run only where a case names it, always as `chkdsk X: /f /x` (`/x` dismounts the volume first, so there is no hidden "force dismount (Y/N)" question), after `Confirm-Disposable -Letter X -Serial $S`.

**P9. Put a fixture image on a disk (Mac, destructive).** Used by `H-USB-10`. The disk is first partitioned with one MBR partition of type 0x07, then the image is copied into that partition's raw node. A bare image has no partition table; Windows normally does not mount a bare NTFS volume on a partition-less stick, so a partition is made **[U]**; the image is smaller than the partition, which the volume-fits-device check allows.
```sh
sh tests/hardware/identify.sh $DISK --serial "$SERIAL"                       # first, to see the disk
confirm_disposable && diskutil unmountDisk $DISK
confirm_disposable && sudo diskutil partitionDisk $DISK MBR ExFAT HWIMG R     # one 0x07 partition
sudo fdisk /dev/$DISK                                                         # check: partition 1 has id 07 [U]
export SLICE=${DISK}s1                                                        # set BEFORE the next confirm_disposable
confirm_disposable && diskutil unmountDisk $DISK
shasum -a 256 <image>                                                         # equals the value recorded at generation
confirm_disposable && sudo dd if=<image> of=/dev/r$SLICE bs=1m && sync
diskutil eject $DISK                                                          # then unplug and plug again; run P1 again
```
`confirm_disposable` checks again on each line because the disk number may have changed after the partitioning, and it refuses a `$SLICE` that does not belong to `$DISK`. After the copy the slice holds the image bytes at its start and zeros or the old partition content after the image **[U: ExFAT metadata of the temporary format remains beyond the 64 MiB; it is not read by NTFS]**. The NTFS boot sector's "hidden sectors" field does not match the partition start; whether Windows accepts that is **[U]**: if Windows does not assign a drive letter, or reports RAW, record it as the result of that leg and use the fallback: boot the stick on the Mac only (legs A and B then produce no Windows result; say so).

**P10. Which side is the writer.** Between steps, say aloud which machine has the stick. A wrong guess is how a volume gets written twice.

## 5. Cases `H-USB-01` to `H-USB-07`, `H-FND-01`

Run `H-USB-01` to `H-USB-07` on **F** and on **S** where the case says "F+S". A single device is enough for `H-USB-05` and `H-USB-07` (marked). Unplug cases are repeated (the point of the cut matters).

### H-USB-01 Format on Windows, copy in on the Mac, eject, verify on Windows

Devices: F+S. Goal: the basic round trip: insert, read/write, eject; the Windows view is clean; the data are equal. Resolves: criterion 5.8 (insert, write, eject), D1 and D5 on hardware, part of R-19.

Steps
0. Mac: this case runs first, with the default automount: `packaging/dev-install.sh --no-build` without `--job-arg` (3.1). Afterwards install again with `--job-arg --automount-mode=ro`.
1. Win: P3 (Windows-clean volume). Win: P7 with tag `01-win-formatted`.
2. Mac: plug (P1), P4. Expect `Mounted`, `rw` (the automount under test). P6, tag `01-attached` (the on-disk flag is now set by the session marker: record `volume_flags`; expected `DIRTY`, ARCHITECTURE.md section 3.6).
3. Mac: copy and time it: `time cp -R ~/hwstage/tree-1g "$MP/"` then `sync`. Verify on the Mac: `python3 -I tests/hardware/mktree.py verify "$MP/tree-1g" ~/hwstage/tree-1g/MANIFEST.sha256`.
4. Mac: `ls -la "$MP"` (record extra entries such as `.fseventsd`, `.Spotlight-V100`, `.Trashes`, `._*`, `.DS_Store`: R-26).
5. Mac: eject through the product: `ntfsmac unmount $SLICE` (record the reply and the last 30 lines of `/var/log/ntfsmacd.log`: expect a clean end, no `W` record, no write error), then P6 tag `01-ejected` (expected: flag clear, `state: clean`, no journal entry), then `diskutil eject $DISK`.
6. Win: plug. Record any prompt (expected none). P7 tag `01-win-after`. Verify by hash: 
```powershell
Get-Content C:\hwtest\tree-1g\MANIFEST.sha256 | ForEach-Object { $h,$p = $_ -split '  ',2; $f = Join-Path "X:\tree-1g" $p; if (-not (Test-Path -LiteralPath $f)) { "MISSING $p" } elseif ((Get-FileHash -Algorithm SHA256 -LiteralPath $f).Hash.ToLower() -ne $h) { "BAD $p" } }; "verify done"
```
   Then P11 with tag `01` (`chkdsk /scan` and `fsutil dirty query`).

Record

| Step | Item | Value |
|---|---|---|
| 2 | `ntfsmac status` (state, mount), flags in P6 | |
| 3 | copy time, MiB/s; Mac verify output | |
| 4 | root listing (extra entries) | |
| 5 | unmount reply; log lines; flags and state after | |
| 6 | Windows prompt; hash result (count of MISSING/BAD); `chkdsk /scan` verdict (verbatim last 10 lines); `fsutil dirty query` | |

Pass: Mac verify `OK`; Windows loop prints nothing but `verify done`; `chkdsk /scan` reports no errors; `fsutil dirty query` says NOT Dirty; after the product unmount the volume probes `clean` with no journal entry. Fail: any of these not true. If Windows shows a "Scan and fix" prompt after a clean product eject, that is a FAIL of the clean-session proof on hardware (record the flags in `01-ejected`).

### H-USB-02 Files written on Windows, read on the Mac

Devices: F+S. Goal: Windows data are read correctly; Windows file names (including non-ASCII) open on the Mac. Resolves: part of U22 (Windows side, with `H-NAME-01`).

Steps
1. Win: P3. Make files with Windows-written names and a manifest (on the Windows internal disk), then copy them onto the volume:
```powershell
$d = 'C:\hwtest\tree-w'; New-Item -ItemType Directory $d -Force | Out-Null
$rng = [Security.Cryptography.RandomNumberGenerator]::Create()
1..200 | ForEach-Object { $b = New-Object byte[] (Get-Random -Minimum 1024 -Maximum 4194304); $rng.GetBytes($b); [IO.File]::WriteAllBytes("$d\f$_.bin", $b) }
foreach ($n in ('caf' + [char]0xE9 + '.txt'), ([string][char]0x65E5 + [char]0x672C + [char]0x8A9E + '.txt'), 'with space.txt', 'UPPER.TXT') { Set-Content -LiteralPath (Join-Path $d $n) -Value "name test $n" }
New-Item -ItemType Directory "$d\sub dir" | Out-Null; Set-Content "$d\sub dir\inner.txt" 'inner'
Set-Location $d
Get-ChildItem -Recurse -File | Where-Object Name -ne MANIFEST.sha256 | ForEach-Object { '{0}  {1}' -f (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash.ToLower(), ($_.FullName.Substring($d.Length + 1) -replace '\\', '/') } | Set-Content -Encoding utf8 MANIFEST.sha256
Copy-Item -Recurse $d "${X}:\tree-w"                      # $X from P3
Copy-Item "$d\MANIFEST.sha256" "C:\hwtest\evidence\02-MANIFEST.sha256"
Write-VolumeCache -DriveLetter $X
```
   Win: P7 tag `02-win-written`; P8.
2. Mac: plug, P1, P4. With the read-only automount of 3.1 the volume is already `Mounted`, `mount: ro`, and the Mac has written nothing (without that install a clean volume would first be mounted `rw` by the default automount, and the snapshot below would not be untouched). Copy the manifest from the Windows machine to the Mac (network; not via F or S) as `~/hwstage/02-MANIFEST.sha256`.
3. Mac: `python3 -I tests/hardware/mktree.py verify "$MP/tree-w" ~/hwstage/02-MANIFEST.sha256`. `ls "$MP/tree-w" | cat -v` (the listing shows names as macOS shows them; U22: NFD in listings). `cat "$MP/tree-w/sub dir/inner.txt"`.
4. Mac: `ntfsmac unmount $SLICE`; P6 tag `02-after-ro` (flags and `logfile_lsn` must equal `02-win-written`: no DIRTY, state clean); `diskutil eject $DISK`. Win: plug, P11 with tag `02`.

Record: the verify output (count `OK n`, or BAD/MISSING lines); the listing of the four special names as printed; flags before and after the read-only session.

Pass: `OK n` with every file (names with `e` acute and the Japanese name found through the manifest's own spelling); the read-only Mac session changed nothing: flags and `logfile_lsn` equal before and after it. Fail: any BAD or MISSING; or a changed `logfile_lsn` after a read-only mount.

### H-USB-03 Unplug while idle (mounted read/write)

Devices: F+S, 3 repetitions each (the third on a volume that has been used for a while, not freshly formatted). Goal: the unclean case of a Mac session, idle. Resolves: R-19 (real unplug), part of D3. This case ends where `H-USB-08` starts (section 6).

Steps
1. Win: P3, eject.
2. Mac: plug, P1, P4, P4rw (`Mounted`, `rw`).
3. Mac: write a synced set and record it: `mkdir -p "$EV/03-run$N"`; `perl tests/stress/disconnect/wl.pl base "$MP$SUB" "$EV/03-run$N" 5` (`$N` = repetition; `SUB=` empty for repetitions 1 and 2; for repetition 3, on a volume that has been used for a while, `SUB=/rep3` after `mkdir "$MP/rep3"`, because `wl.pl base` needs a `base` directory that does not exist yet). Use `"$MP$SUB"` as the path in `wl.pl verify` too. It writes `base/` with six files, fsyncs each file and its directory and the volume, and records `relpath sha256` in `$EV/03-run$N/base.list` after the fsyncs.
4. Mac: P6 tag `03-run$N-pre` (the volume is mounted read/write: record `volume_flags`: expected `DIRTY` set by the session; `logfile_lsn` = `L0`).
5. Wait 60 s with no access to the volume, then P5 (unplug). Record the time and what the screen showed.
6. Mac: wait 10 s, plug again, P1, P4 (read-only, as installed). P6 tag `03-run$N-replug` (expected: `state: dirty_flag`, `volume_flags` contains `DIRTY`, journal entry present with its `logfile_lsn_at_start`). Because the automount mode alone already gives `ro`, now ask for read/write: P4rw. It must **not** give `rw`: record the policy's answer and reason (dirty flag, journal entry). Verify the synced set on the read-only mount: `perl tests/stress/disconnect/wl.pl verify "$MP$SUB" "$EV/03-run$N"` (expect `OK 6`).
7. Mac: `ntfsmac unmount $SLICE` (if mounted), `diskutil eject $DISK`.
8. Win: plug. Do **not** click anything in a prompt yet: write down its exact words (expected: "Scan and fix" offered). P7 tag `03-run$N-win-first`. Then choose neither prompt option (close the prompt) and run P11 with tag `03-run$N` (on a volume with the dirty bit set, `/scan` may refuse or do less than on a clean one **[U]**: record what it says). Verify the synced set by hash (the Windows form of `wl.pl verify`):
```powershell
Get-Content C:\hwtest\evidence\03-run1-base.list | ForEach-Object { $p,$h = $_ -split ' '; $f = "X:\$p"; if (-not (Test-Path $f)) { "MISSING $p" } elseif ((Get-FileHash -Algorithm SHA256 $f).Hash.ToLower() -ne $h) { "BAD $p" } }; "verify done"
```
   (copy `base.list` to the Windows machine first). P7 tag `03-run$N-win-scan`. P8.
9. Mac, every repetition including the first: plug, P1, P6 with `--copy` tag `03-run$N-after-scan` (what did `/scan` do to the flags and the LSN; for repetition 1 this snapshot is the "before" of `H-USB-08` and must exist before `H-USB-08` step 2), eject. Repetition 1 then continues with section 6 from step 2; for the other repetitions reset the disk with P3 for the next repetition.

Record

| Step | Item | Value |
|---|---|---|
| 3 | `wl.pl base` result, number of files in `base.list` | |
| 4 | flags and `logfile_lsn` while mounted rw (`L0`) | |
| 5 | time of the pull; screen after the pull; worker gone? journal entry still present? | |
| 6 | `ntfsmac status` after replug (state, mount, reason, journal member); P6 flags, state, `logfile_lsn`, the journal entry's `logfile_lsn_at_start`; P4rw answer and reason; `wl.pl verify` | |
| 8 | Windows prompt (exact words); `chkdsk /scan` output; Windows hash check; P7 `fsutil dirty query` | |
| 9 | flags and `logfile_lsn` after `/scan` | |

Pass: after the pull the next Mac attach is `ro`, a request for `rw` is refused by the policy (not only by the automount mode), and the synced set verifies; the Windows prompt or `chkdsk` shows the unclean state (dirty bit set); `chkdsk /scan` reports no lost data in the synced set (every file of `base.list` present with equal hash on Windows). Fail: the next Mac attach is `rw`; a synced file is missing or wrong on either side; Windows finds damage beyond the idle session (record verbatim).

### H-USB-04 Unplug during a large copy

Devices: F+S, 3 repetitions each, the pull at about 25 %, 50 % and 75 % of the copy. Goal: the unclean case with a write in flight; files finished and synced before the copy are intact; Windows repairs at most the file in flight. Resolves: R-19; D3; R-33 on real hardware (the copy creates MFT records, so a `mirror_split` is possible here and is recorded if it appears).

Steps
1. Win: P3, eject. Mac: plug, P1, P4, P4rw (`rw`).
2. Mac: synced set: `mkdir -p "$EV/04-run$N"; perl tests/stress/disconnect/wl.pl base "$MP" "$EV/04-run$N" 5`.
3. Mac: start the big copy in a second terminal: `cp -R ~/hwstage/tree-big "$MP/big"`. In the first terminal watch progress: `du -sk "$MP/big"`; at the planned fraction of 3,072 MiB (786432, 1572864 or 2359296 KiB) do P5. Record the exact byte count seen last and the time.
4. Mac: plug again, P1, P4 (read-only as installed), P6 tag `04-run$N-replug` (`--copy` on repetition 1), then P4rw: it must not give `rw` (policy: dirty flag, or refusal for `mirror_split`/`corrupt`; record the reason). Verify the synced set: `wl.pl verify "$MP" "$EV/04-run$N"` (`OK 6`). `ls -R "$MP/big" | head` and `ls -l` of the largest partial file; record how many files of `big` are present and complete: `python3 -I tests/hardware/mktree.py verify "$MP/big" ~/hwstage/tree-big/MANIFEST.sha256` (the BAD and MISSING lines list the files in flight and not started; record the counts; this is not a pass criterion).
5. Mac: eject (`ntfsmac unmount` if mounted, `diskutil eject $DISK`).
6. Win: plug; write down the prompt. P7 tag `04-run$N-win-first`. Then `Confirm-Disposable -Letter X -Serial $S` and `chkdsk X: /f /x *>&1 | Tee-Object C:\hwtest\evidence\04-run$N-chkdsk-f.txt`. Re-run the synced-set hash check of H-USB-03 step 8. P11 with tag `04-run$N` (expect clean). P7 tag `04-run$N-win-after`. P8.
7. Mac: plug; P4 and P6 tag `04-run$N-after-chkdsk` (expected: `clean` or `dirty_flag` with only `MODIFIED_BY_CHKDSK`; see `H-USB-08`). If the repetition's replug state in step 4 was `mirror_split` or `corrupt`, say so in the table and keep `$EV/04-run$N-replug.txt`.

Record

| Step | Item | Value |
|---|---|---|
| 2 | `wl.pl base` records | |
| 3 | fraction and bytes at the pull; time | |
| 4 | `ntfsmac status` (state, mount, reason); probe state (`dirty_flag`, `mirror_split`, `corrupt`, ...); synced-set verify; complete/partial/missing counts of `big` | |
| 6 | Windows prompt; chkdsk /f text (verbatim: the lines naming files, orphans, MFT, mirror, log); hash check of the synced set; `/scan` after | |
| 7 | Mac state and flags after chkdsk | |

Pass: next Mac attach is `ro` (or refused with `mirror_split`/`corrupt`, recorded); every file in the synced set is intact on the Mac (before Windows) and on Windows (after `chkdsk /f`); `chkdsk /f` repairs at most the file in flight (no loss of files of the synced set, no volume-wide damage report); `chkdsk /scan` after `/f` is clean. Fail: a synced file is lost or changed; `chkdsk` reports damage to anything but the in-flight file; the Mac attaches `rw` after the pull.

### H-USB-05 Hibernated Windows and Fast Startup

Device: S (or F if it is a fixed disk to Windows). Goal: a volume Windows left in a hibernation or Fast Startup state is mounted read-only on the Mac with the reason shown, and Windows resumes normally afterwards. Resolves: the "real" evidence for `U-PROBE-*` (TEST_STRATEGY.md section 3.4), U19 (Windows part, if the system-volume leg is done).

What is possible with a data disk **[U]**: Fast Startup is a hibernation of the kernel session only (a logged-off, partly hibernated system), not of the user session: at shutdown Windows saves the kernel state, so a volume it keeps mounted is left with an open log; for a removable disk Windows dismounts the volume at shutdown, for a fixed disk it may leave the volume in the hibernated session with a Windows 10-or-later restart-page version 2.0 in `$LogFile` (the probe's `fast_startup`). A data disk has no `hiberfil.sys` in its root, so the `hibernated` verdict (a `hiberfil.sys` on the volume) can only be produced with a Windows system volume on the external disk, which is optional.

Steps, leg 1 (Fast Startup, data disk)
1. Win: `Get-Disk -Number $N | Format-List BusType` and `Get-Volume -DriveLetter X | Format-List DriveType` (record `Removable` or `Fixed`). Confirm Fast Startup is on (3.3). P3, then put a file on it with `Set-Content`, leave a document open on it (a text file open in Notepad) and keep the stick plugged in.
2. Win: `shutdown /s /t 0` (a normal shutdown: this is the Fast Startup one; **not** `/full`). Wait until the machine is off (power light off, fans stopped). Then pull the stick. Do not turn the machine on yet.
3. Mac: plug, P1, P4, P6 tag `05-fast-replug`. Expected: `ro` with the reason shown, state `fast_startup` or `unclean_log` or `dirty_flag` (record which; any `rw` is a FAIL). If the state is `clean`, Windows dismounted the volume at shutdown (removable disk): record it, and the leg is INCONCLUSIVE for the Fast Startup state (not a failure). `ntfsmac unmount` if mounted, `diskutil eject $DISK`.
4. Win: turn the machine on with the stick unplugged, wait for the desktop (it resumes from the hibernated session), plug the stick. Record whether Windows resumes normally, whether Notepad's file is still there, any prompt. P7 tag `05-fast-after`. P11 with tag `05`.

Steps, leg 2 (hibernation with the volume holding `hiberfil.sys`; only if the operator has Windows installed on external media and the coordinator wants it)
1. (Only a Windows installed on external media; never the PC's internal Windows or its internal disk.) Boot that Windows from the external disk, run `powercfg /h on`, then `shutdown /h`. Wait until the machine is off.
2. Mac: P1, P4, P6 tag `05-hib-replug`. Expected: `ro`, state `hibernated`, reason shown. Do not mount `rw` by any means. `diskutil eject $DISK`.
3. Boot the same Windows from the disk again; it must resume the hibernated session. Record. P11 does not apply (the volume is the running system's); record `chkdsk C: /scan` only if you choose to.

Record: leg 1 and 2: Windows `DriveType`; what Windows did at shutdown; Mac state and reason text; P6 flags, `logfile` (`v2_fast_startup`?), `hiberfile`; whether Windows resumed normally.

Pass: the Mac attach is never `rw` while the volume is in a hibernated or Fast Startup state; the reason is shown; Windows resumes normally and its files are intact. If the volume probes `clean` because Windows dismounted it, that leg is INCONCLUSIVE and leg 2 or the synthetic fixtures carry the evidence (release notes say "unsafe-state detection verified on synthetic images only", TEST_STRATEGY.md section 3.4). Fail: `rw` mount of such a volume; Windows cannot resume or reports damage after the Mac looked at the volume.

### H-USB-06 Sleep and wake the Mac with the volume mounted

Devices: F+S. Goal: a mounted volume survives sleep and wake, or fails in a defined way. Resolves: R-19 (sleep/wake).

Steps
1. Win: P3, eject. Mac: plug, P1, P4, P4rw (`rw`). `mkdir -p "$EV/06-run"; perl tests/stress/disconnect/wl.pl base "$MP" "$EV/06-run" 5`.
2. Mac: `pmset -g log | tail -n 5` (note the time); put the Mac to sleep: Apple menu, Sleep (or `pmset sleepnow`). Wait 2 minutes (the USB port may stay powered or not **[U]**; note whether the stick's light stayed on).
3. Wake the Mac. At once: `ntfsmac list; ntfsmac status $SLICE; tail -n 40 /var/log/ntfsmacd.log`; then `perl tests/stress/disconnect/wl.pl verify "$MP" "$EV/06-run"`. Write a new file and read it back: `date > "$MP/after-wake.txt"; sync; cat "$MP/after-wake.txt"`. Repeat sleep and wake once more, now with a `cp` of a 500 MiB file started before sleeping and finishing after waking (`cp -R ~/hwstage/tree-1g/large "$MP/large-copy"` in a second terminal, sleep, wake, wait, `shasum -a 256` of both).
4. If the volume disappeared across sleep (USB reset), the Mac treats it as an unplug: record the state of the next attach (it is the case of `H-USB-03`); then `diskutil eject`, and Win: P11 with tag `06-unclean`.
5. Normal end: `ntfsmac unmount $SLICE`, P6 tag `06-end` (expected `clean`), `diskutil eject $DISK`, Win: plug, P11 with tag `06`.

Record: whether the volume stayed mounted; verify output; the new file; the copy hash equality; the log lines; if it vanished: next attach state.

Pass: the volume is usable after wake, hashes equal, the end state is clean and `chkdsk /scan` clean; or, if USB power is cut by sleep, the volume reappears or disappears cleanly and the next attach is `ro` with the synced set intact (that is `H-USB-03`). Fail: the volume is mounted but I/O hangs or returns errors without disappearing; data differ; an `rw` attach after an unclean wake.

### H-USB-07 BitLocker volume

Device: F (one device is enough). Goal: a BitLocker volume is not mounted and the message is clear. Resolves: R-19 (media), `U-PROBE-*` for `bitlocker`.

Steps
1. Win: P3 (BitLocker needs Windows 10/11 Pro or Enterprise to turn on; Home can only open a BitLocker To Go volume **[U]**: on Home, use another Windows or mark the case NOT DONE). Then `Confirm-Disposable -Letter X -Serial $S -Typed`. Turn BitLocker on for the volume with a password (Settings, or `Enable-BitLocker -MountPoint X: -PasswordProtector -EncryptionMethod XtsAes128 -UsedSpaceOnly` and enter a password at the prompt; **[U]** the exact cmdlet options on this Windows). Wait until `manage-bde -status X:` says `Fully Encrypted` (or `Used Space Only Encrypted`). Eject (P8). Never type this password on the Mac.
2. Mac: plug, P1, P4. Expected: not mounted by the product (`Failed` or equivalent, reason `BITLOCKER`/unsupported), a clear message; macOS may offer to initialise or eject the disk: **choose Ignore or Eject, never Initialize**. P6 tag `07-attached` (`bitlocker: True`, state `unsupported`). Record the message text (`ntfsmac status`, notification, Finder dialog). `mount | grep "$SLICE "` must show nothing from the product.
3. Mac: `diskutil eject $DISK`. Win: plug, unlock with the password, read the file you wrote, `manage-bde -status X:`, P11 with tag `07` (after the unlock); remove encryption if you want to reuse the stick: `manage-bde -off X:` (wait), or run P3.

Pass: no mount by the product, message shown and understandable (words recorded), Windows unlocks the volume and the data are intact. Fail: any mount; a message that does not say why; Windows cannot unlock.

### H-FND-01 Finder

Devices: F+S (the flash drive is the minimum). A GUI session is needed. Goal: ordinary Finder use works and leaves what Windows can read. Resolves: part of R-26 (what macOS writes onto the volume), the open question of `mdutil -i off` and `.metadata_never_index` (RISK_REGISTER.md R-26: "needs a GUI session, T-M5-10").

Steps
1. Win: P3, eject. Mac: plug; the volume appears in the Finder sidebar (record that and the name Finder shows; U14: it may be the mount-point leaf). P4, P4rw (Finder can only write to a `rw` volume).
2. Finder: drag-copy a folder (use `~/hwstage/tree-1g/medium` or any 200 MB folder) onto the volume; rename the folder; rename a file inside; copy a second folder; move one file to the Trash; Empty Trash (Finder, Empty Trash); then click the eject button next to the volume in the Finder sidebar. Note any dialog or error at each action and its wording.
3. Before the eject: `ls -la "$MP"`, `ls -la "$MP/.Trashes" 2>&1 | head`, `mdutil -s "$MP"`, `xattr -l "$MP"/*`. Record the extra files macOS created (`.DS_Store`, `._*`, `.fseventsd`, `.Spotlight-V100`, `.Trashes`).
4. After the eject: Mac: `ntfsmac list` (the volume should be `Appeared` or gone; record), P6 tag `fnd-ejected` (expected clean flag, no journal entry). `tail -n 40 /var/log/ntfsmacd.log` (the eject came through the product's unmount path: record that it ended clean).
5. Win: plug; `cmd /c dir /a X:\` and `Get-ChildItem -Force -Recurse X:\ | Select-Object FullName, Attributes | Out-File C:\hwtest\evidence\fnd-listing.txt`; compare the tree with what you made (names, renames, the file in the Trash is gone, the folder copy is complete: hash check as in `H-USB-01` against the manifest of the copied subfolder: generate it on the Mac with the same `mktree.py gen` manifest and use only its lines for that folder). P7 tag `fnd-win`, P11 with tag `fnd`.

Record: each Finder action and its result; the list of macOS-created files at root and below; `mdutil -s` text; eject result; Windows listing differences; `chkdsk /scan` verdict.

Pass: every action succeeds without an error dialog; eject through Finder ends the session clean (flag cleared, no journal entry); Windows shows the same tree and `chkdsk /scan` is clean. The list of macOS-created files is the input for the expected-differences list of the integrity tests and for the `mdutil` decision (it is recorded, not a pass criterion). Fail: an action fails or hangs; the eject leaves the volume dirty; Windows reports damage.

## 6. Cases `H-USB-08` and `H-USB-09`: U12 (the evidence rule of the journal)

These two cases together settle U12 (ARCHITECTURE.md section 3.5, section 11): does Windows advance `current_lsn`, does `chkdsk` clear `VOLUME_IS_DIRTY` and which flags it leaves, and does a plain Windows read/write mount leave our `VOLUME_IS_DIRTY` set. The logs are written by Windows (P3 formats the volume on Windows and mounts it there once), not synthetic ones. Device: F first; S if time allows.

### H-USB-08 Windows `chkdsk /f` after an unclean Mac session

Starts from the end of `H-USB-03` repetition 1 (the disk is on Windows after step 8 of that case, `/scan` done, no `/f` yet). If the chain was broken (a disk reset in between), redo steps 1 to 7 of `H-USB-03` first and stop before its step 8.

Steps
1. Mac (this is the "before" record, taken before Windows touched the volume with `/f`; it is the Mac snapshot `03-run1-after-scan` that `H-USB-03` step 9 took after Windows' `/scan`; it must exist, and the disk must not have been mounted by the Mac `rw` since). Fix these values in the table: `volume_flags` (all of them, by name), `logfile_lsn` (= `current_lsn`), `state`. Record two reference values for the evidence rule: `L0`, the LSN seen while the session was mounted (`03-run1-pre`), and the journal entry's `logfile_lsn_at_start` (in the journal section of `03-run1-replug.txt` and `03-run1-after-scan.txt`; this is the value rule 2 compares with, it may differ from `L0`). Note that `/scan` may already have advanced the LSN; the record shows the step in which it moved.
2. Win: plug. P7 tag `08-win-before`. `Confirm-Disposable -Letter X -Serial $S`, then `chkdsk X: /f /x *>&1 | Tee-Object C:\hwtest\evidence\08-chkdsk-f.txt` (the full output is kept). Then at once: `fsutil dirty query X:`, `Write-VolumeCache -DriveLetter X`, P7 tag `08-win-after`. Also copy out the event log lines of the run (P7 does). P8.
3. Mac: plug, P1. With the read-only automount nothing has mounted it read/write; P6 with `--copy` tag `08-after` (the probe is read-only, and the journal entry is present): record `volume_flags` (all, by name), `logfile_lsn` (`L1`), `state`, and `ntfsmac status` (journal member). The computed answers (write them in the table):
   - D1 Dirty cleared by chkdsk? (`DIRTY` in `volume_flags` before, absent after)
   - D2 LSN changed by Windows? (`L1` different from `L0` and from the value before `/f`)
   - D3 Flags left set: every name in `volume_flags` after (for example `MODIFIED_BY_CHKDSK`, `MOUNTED_ON_NT4`, anything else): record each.
   - D4 Probe state after: `clean`, `dirty_flag`, other. A lone `MODIFIED_BY_CHKDSK` must probe `clean` (N-04).
   - D5 Would the evidence rule fire on this attach? Rule 2 requires: journal entry `marker=set`, fresh probe state exactly `clean`, `logfile_lsn` present and different from the entry's `logfile_lsn_at_start` (compute against that value and, for the record, against `L0` too), same serial and size. Evaluate it by hand from `ntfsmac status` (journal member) and the snapshot. **The product build cannot enable the rule** (no switch; section 3.1): this is a computed answer, and the coordinator decides about enabling it from it **[U]**.
4. Mac, rule disabled (the product as installed): `ntfsmac list` shows the attach `ro` (automount mode and the journal entry). `confirm_disposable && sudo ntfsmac journal-clear $SLICE`: it shows the label and asks for it, then for the last 8 hex digits of the NTFS volume serial, then for your password in the terminal (rule 5 of section 2). Type them yourself. Record the whole reply, including any warning that the cleared flag may be a lost flush (it appears when the LSN equals `logfile_lsn_at_start`, ARCHITECTURE.md section 3.5, path 3). Then `ntfsmac unmount $SLICE` (the volume is mounted `ro`), wait for `Appeared`, and `ntfsmac mount $SLICE`: expect `rw`. P6 tag `08-cleared`. Then `ntfsmac unmount $SLICE` and `diskutil eject $DISK`; P6 tag `08-unmounted` (clean, no journal entry).
5. Win: plug; P11 with tag `08` (expect clean).

Record

| Item | Before `/f` (Mac, step 1) | After `/f` (Mac, step 3) |
|---|---|---|
| `volume_flags` (all, by name) | | |
| `logfile_lsn` (`current_lsn`); also `L0` and the entry's `logfile_lsn_at_start` | | |
| `logfile` state | | |
| probe `state` and `reason_codes` | | |
| `ntfsmac status` (state, mount, reason, journal member) | | |
| Windows `fsutil dirty query` (before, after `/f`) | | |
| Text of `chkdsk /f` (verbatim, main findings) | | |

Pass (resolves the positive half of U12): `DIRTY` is cleared by `chkdsk /f`; `logfile_lsn` differs from `L0` after Windows; every other flag left set is recorded, and the probe gives `clean` (or only the informational flags); with the journal entry present the attach is `ro`; `journal-clear` succeeds in the state `clean`; the next `mount` is `rw`. D5 is recorded either way. Fail: `DIRTY` still set after `/f` (then rule 2 cannot rely on it and `chkdsk` is not the clearing path: finding); `logfile_lsn` unchanged by Windows (the evidence rule would never fire: finding); a flag outside the informational ones left set (finding for the policy); `journal-clear` refused in a `clean` state.

### H-USB-09 A plain Windows mount, no `chkdsk`: our flag must stay set

Device: F. Goal: the negative half of U12: a Windows read/write mount that replays or advances the log, but does not run `chkdsk`, must leave our `VOLUME_IS_DIRTY` set (otherwise rule 2 would accept a volume that nothing checked). Resolves: U12 (negative case); R-32.

Steps
1. Make a second unclean Mac session, as in `H-USB-03` steps 1 to 7, with the evidence tag prefix `09` (a new P3, a fresh `wl.pl base` run in `$EV/09-run`, P6 tags `09-pre` and `09-replug`, P4rw refused after the replug). Fix `volume_flags` (expected `DIRTY`), `logfile_lsn` `L0'` from `09-pre`, the journal entry's `logfile_lsn_at_start`, and the state. Eject.
2. Win: plug. When Windows offers "Scan and fix", choose **Continue without scanning** (or close the prompt) **[U: the wording of the choices on this Windows]**. Do not run `chkdsk`. P7 tag `09-win-before` (this runs `fsutil dirty query`: record `Dirty`).
3. Win: write one file and flush: `Set-Content -Path X:\w09.txt -Value "plain windows mount $(Get-Date -Format o)"; Write-VolumeCache -DriveLetter X`. Record whether the write worked (Windows may mount a dirty volume read-only or refuse **[U]**; if it refused, the LSN may not have changed and the case is INCONCLUSIVE on the LSN part but still answers the flag). P7 tag `09-win-after` (`fsutil dirty query` again). Eject with P8.
4. Mac: plug, P1 (read-only automount: nothing mounted `rw`), P6 with `--copy` tag `09-after`: `volume_flags`, `logfile_lsn` (`L1'`), state; `ntfsmac status` (the attach must be `ro`, reason `dirty_flag`; record the exact reason text and the journal member). Then `diskutil eject $DISK`.
5. Win: plug again, after the snapshot of step 4 and not before: P11 with tag `09-final` (what a scan makes of the volume; record it). Then reset to a Windows-clean volume with P3 before the next case.

Record

| Item | Before Windows (Mac, step 1) | After the plain Windows mount (Mac, step 4) |
|---|---|---|
| `volume_flags` | | |
| `logfile_lsn` | | |
| probe `state` | | |
| `ntfsmac status` (mount, reason, journal member) | | |
| Windows `fsutil dirty query` (before and after the write) | | |
| Windows prompt wording; did the write succeed | | |

Pass: after the plain Windows mount `DIRTY` is still set (the Mac probe says `dirty_flag`), the attach is `ro` with the evidence rule disabled (the product as installed) and also by the computed rule (the entry is `marker=set`, but the probe is not `clean`, so rule 2 would not fire). `logfile_lsn` may have changed; that must be recorded: it is the reason the LSN alone is not enough. Fail: `DIRTY` cleared by a plain mount: then rule 2 stays disabled permanently, and the finding goes to the coordinator at once (ARCHITECTURE.md section 3.5: rule 2 stays disabled unless both `H-USB-08` and `H-USB-09` pass).

Together: `H-USB-08` and `H-USB-09` both pass resolves U12 (the Windows steps of T-M5-10). If either fails or is INCONCLUSIVE, U12 stays open and the coordinator decides about rule 2. This protocol does not decide that.

## 7. Case `H-USB-10`: Windows and the stale `$MFTMirr` (U15)

Device: F. Goal: what Windows `chkdsk /f` does with a mirror-split volume and a volmark-split volume, and whether the Mac repair is accepted by Windows. Resolves: U15 (Windows half; the Apple half is resolved), part of R-33 and R-36.

Images and hashes: write the three SHA-256 values you were given (and check them with `shasum -a 256`): `mirror-split`, `vmk-split`, and the second copy of `mirror-split` for leg C (the same file).

### Leg A: `mirror-split` (record 0; the sweep's `smallfiles-c68.img.gz`)
1. Mac: P1; P9 with the `mirror-split` image (P9 ends by ejecting). Plug again, P1 again (the disk number may have changed). P4: record what the product does (expected: refuses, state `Failed`/`mirror_split`; the daemon may leave the disk to Apple's driver, which mounts it read-only: U15 Apple half: record whether it did, and `diskutil unmount $DISK` if so). P6 with `--copy` tag `10a-mac-before` (expected state `mirror_split`, reason `MFTMIRR_STALE`, flags, `logfile_lsn`). Eject.
2. Win: plug. Record: does a drive letter appear, is the volume `NTFS` or `RAW`, any prompt. P7 tag `10a-win-before`. `Confirm-Disposable -Letter X -Serial $S`, then `chkdsk X: /f /x *>&1 | Tee-Object C:\hwtest\evidence\10a-chkdsk-f.txt`. Record whether the output mentions the master file table mirror, any correction text (verbatim). P11 with tag `10a`. P7 tag `10a-win-after`. P8.
3. Mac: plug, P1, P4. With the read-only automount the volume attaches `ro` and nothing is written before the snapshot. P6 with `--copy` tag `10a-mac-after`. Expected: the probe state is `clean` or `dirty_flag`, **never** `corrupt` or `mirror_split`; `ntfsmac list`. If `clean`, read the root listing on the `ro` mount (do not ask for `rw`). Eject.

### Leg B: `vmk-split` (record 3)
Repeat leg A with the `vmk-split` image, tags `10b-...`. Before-state: `mirror_split` (`MFTMIRR_STALE`, with `FLAG_DIRTY` from the overlay view: TEST_STRATEGY.md section 3.2). If you were given `vmk-split-clear` (the reverse direction), do it as leg B2 with tags `10b2-...`.

### Leg C: the Mac repair, then Windows `chkdsk /scan`
1. Mac: P1; P9 with the `mirror-split` image again; plug, P1 again. P4 (state `mirror_split`; if Apple's driver mounted it, `diskutil unmount $DISK` first, otherwise `repair --sync-mirror` answers "could not be checked just now (is it mounted by another driver?)", U15). P6 tag `10c-before`.
2. Mac: `confirm_disposable && sudo ntfsmac repair --sync-mirror $SLICE` (you type the label, the last 8 hex digits of the NTFS volume serial, and your password in the terminal; rule 5 of section 2). Record the whole reply. Expected: the repair succeeds, the reply names the fresh state (`clean`, or `dirty_flag` if the flag was set). P6 with `--copy` tag `10c-after`. Eject.
3. Win: plug. P7 tag `10c-win`. P11 with tag `10c` (the verdict that matters). Also, if the scan is clean, `Confirm-Disposable -Letter X -Serial $S` and `chkdsk X: /f /x` once, and record whether it changes anything. P8.

### Leg D: does a Windows-only volume ever probe `mirror_split`?
(Windows 10 and later no longer update the full `$MFTMirr`, `volume.c:997`; ARCHITECTURE.md U15.) 
1. Win: P3, then `Confirm-Disposable -Letter X -Serial $S` and grow the MFT hard: `1..30000 | ForEach-Object { New-Item -ItemType File -Path "X:\g\f$_.txt" -Force | Out-Null }` (30,000 files; first `New-Item -ItemType Directory X:\g`), then delete half, `Write-VolumeCache`, P8. Do it three times, with a Mac probe between rounds.
2. Mac: plug each time, P1, P4, P6 tag `10d-round$R`. Record `state`. Eject. After the last round Win: P11 with tag `10d`.

Record

| Leg | Windows saw (letter, NTFS/RAW, prompt) | `chkdsk /f` text about the mirror and corrections | Mac probe before (state, reasons) | Mac probe after (state, reasons, flags, LSN) |
|---|---|---|---|---|
| A mirror-split | | | | |
| B vmk-split | | | | |
| B2 vmk-split-clear (if given) | | | | |
| C Mac repair | n/a | `/scan` verdict; `/f` effect | | |
| D Windows-only, rounds 1 to 3 | | n/a | n/a | |

Pass: legs A and B: Windows mounts and `chkdsk /f` repairs or accepts the mirror (whatever it reports is recorded), and the next Mac probe gives `clean` or `dirty_flag`, never `corrupt` or `mirror_split`; leg C: `repair --sync-mirror` succeeds and `chkdsk /scan` reports no mirror error; leg D: every probe is `clean` (a `mirror_split` or `corrupt` from a Windows-only volume is a FAIL with high priority: NTFS-3G would then refuse volumes Windows considers healthy). Fail: after `chkdsk /f` the Mac probe says `corrupt` or `mirror_split`; `chkdsk /scan` after the Mac repair reports a mirror or MFT error; the repair writes beyond what ADR-021 says (compare the `--copy` images before and after later in Environment A with `logfile_mark --dump` and a byte comparison of `$MFT` and `$MFTMirr` records 0 to 3).

## 8. Case `H-USB-11`: Finder tags and comments as `:` streams (U23, ADR-025)

Device: F. Goal: does Windows `chkdsk` keep, drop or flag the streams whose names contain a colon (`com.apple.metadata:_kMDItemUserTags`, `com.apple.metadata:kMDItemFinderComment`) that patch 0009 lets the Mac write. Resolves: U23 (ADR-025: if `chkdsk` drops or flags them, patch 0009 is reverted).

Steps
1. Win: P3, `Set-Content X:\tagfile.txt 'tag test'`, `New-Item -ItemType Directory X:\tagdir`, eject.
2. Mac: plug, P1, P4, P4rw (`rw`). First check that the `:` stream name is accepted (patch 0009): `xattr -w com.apple.metadata:hwtest 1 "$MP/tagfile.txt"`; `xattr -l "$MP/tagfile.txt"`. If this fails (`Operation not permitted` or `Invalid argument`), patch 0009 is not in this build: stop the case, NOT DONE, report to the coordinator. Otherwise remove it: `xattr -d com.apple.metadata:hwtest "$MP/tagfile.txt"`.
3. Finder: tag `tagfile.txt` (right-click, a colour tag) and the folder `tagdir`; add a comment to the file (Get Info, Comments). Mac: `xattr -l "$MP/tagfile.txt" "$MP/tagdir"` (expect `com.apple.metadata:_kMDItemUserTags` and `com.apple.metadata:kMDItemFinderComment`); `xattr -px com.apple.metadata:_kMDItemUserTags "$MP/tagfile.txt" > $EV/11-tags-before.hex`.
4. Mac: eject through the product (`ntfsmac unmount $SLICE`, `diskutil eject $DISK`); P6 tag `11-ejected` (clean).
5. Win: plug. List the streams: `cmd /c dir /r X:\` and `Get-Item -Stream * X:\tagfile.txt, X:\tagdir | Format-Table PSChildName, Stream, Length`. Run P11 with tag `11` (`/scan`), then `Confirm-Disposable -Letter X -Serial $S` and `chkdsk X: /f /x *>&1 | Tee-Object C:\hwtest\evidence\11-chkdsk-f.txt`; list the streams again; P7 tag `11-win`. P8.
6. Mac: plug; P1, P4; `xattr -l "$MP/tagfile.txt" "$MP/tagdir"`; `xattr -px com.apple.metadata:_kMDItemUserTags "$MP/tagfile.txt" | cmp - $EV/11-tags-before.hex` (equal); open Finder: are the tag and the comment still shown? P6 tag `11-after`.

Record: the stream names Windows lists before and after `chkdsk`; every line of `chkdsk /scan` and `/f` that mentions a stream, an attribute, an index or a name; the `xattr` output before and after; Finder shows the tag and comment (yes/no).

Pass: `chkdsk /scan` and `/f` report no error about the streams, the streams are still there on Windows and on the Mac afterwards with the same bytes, the Finder tag and comment still show. Fail: `chkdsk` reports, deletes or renames a stream, or the tag disappears: then patch 0009 is reverted and tags are documented as not stored (ADR-025). Windows listing the stream by a different spelling (for example the name cut at the colon) is not a failure by itself; record it.

## 9. Cases `H-SYNC-01`, `H-NAME-01`, `H-PERF-01`

### H-SYNC-01 Does a flush reach the medium? (R-30, `DKIOCSYNCHRONIZE` on USB bridges)

Devices: F+S. Goal: R-30 says no device node accepts `F_FULLFSYNC` and that `DKIOCSYNCHRONIZE` works on virtual disks but "whether USB bridges and drives honour it is [U]". Two checks: (a) the flush has a cost that looks like a flush; (b) a file whose `fsync` returned survives an immediate pull of the cable.

Steps, part (b) first (the decisive one)
1. Win: P3, eject. Mac: plug, P1, P4, P4rw (`rw`). In a second terminal start an unsynced large write that keeps the drive cache full through the pull: `dd if=/dev/zero of="$MP/cachefill.bin" bs=1m count=2048` (it will fail with an I/O error at the pull; that is expected).
2. `mkdir -p "$EV/sync-run$N"`. Start the workload that fsyncs file after file and holds still after file 3: `perl tests/stress/disconnect/wl.pl fsync "$MP" "$EV/sync-run$N" 7 3`. Every file is written, fsynced and its directory fsynced, and only then recorded in `$EV/sync-run$N/fs.K`. When `$EV/sync-run$N/fs.3` exists, the writer is idle after the fsyncs of file 3 returned. Pull the cable (P5) **now** (the writer has just finished the fsyncs; the busy tail of `wl.pl` between files is the unsynced traffic, so the pull at the hold point is right after the last durable claim). Repeat `N = 1..5` on each device After the pull press Ctrl-C in the terminals of `wl.pl` and `dd` (they may already have died with a write error). The volume is reset with P3 between repetitions (do not continue on a replugged volume).
3. Mac: plug, P1, P4 (`ro`). `perl tests/stress/disconnect/wl.pl verify "$MP" "$EV/sync-run$N"` (every recorded file must hash equal; the records are `fs.1` to `fs.3`). Eject. Win: `Confirm-Disposable -Letter X -Serial $S`, `chkdsk X: /f /x` once (record), P11 with tag `sync-run$N`, check the same files by hash (H-USB-03 step 8 form with `fs.*` files; copy the records over).

Steps, part (a)
4. Mac: with the volume mounted `rw` on a fresh P3 volume, time 200 small synced writes: 
```sh
python3 -I - "$MP" <<'EOF'
import os, sys, time, fcntl
d = sys.argv[1]; full = len(sys.argv) > 2; t = []   # a second argument: F_FULLFSYNC instead of fsync
fd = os.open(d + "/fsync-timing.bin", os.O_WRONLY | os.O_CREAT)
for i in range(200):
    os.pwrite(fd, b"x" * 4096, i * 4096)
    a = time.perf_counter()
    if full: fcntl.fcntl(fd, fcntl.F_FULLFSYNC)
    else: os.fsync(fd)
    t.append(time.perf_counter() - a)
t.sort(); print("fsync median %.2f ms, p95 %.2f ms, max %.2f ms" % (t[100]*1000, t[190]*1000, t[-1]*1000))
os.close(fd)
EOF
```
   Record the numbers. For comparison, on the same stick unmounted: `sudo dd if=/dev/zero of=...` is **not** used (it writes the device); the comparison is the same loop on the Mac's internal disk with `F_FULLFSYNC` (`python3 -I - "$HOME" full`, the real flush of an internal SSD) and on the volume itself with `fsync` (the product's flush path adds `DKIOCSYNCHRONIZE`) and the same loop on Windows with the native driver (`[IO.File]` with `Flush($true)`, record the median **[U]**: only an order of magnitude is meaningful). A median under about 0.2 ms on a USB flash drive or HDD is a sign that the flush does nothing **[U: threshold is a hint, not a pass criterion]**.
5. Mac: `grep -i "flush" /var/log/ntfsmacd.log | tail` (patch 0006 reports a failed flush as `W flush`; record whether any appeared).

Record

| Device | Repetition | Last file fsynced before the pull (K) | Files verified OK / total | Windows check after `chkdsk /f` | fsync median / p95 (ms) | `W flush` seen |
|---|---|---|---|---|---|---|

Pass: for every repetition on every device, every file whose record exists is present with the right hash on the Mac and on Windows. Fail: any lost or changed recorded file: R-30 is real on that device: the finding goes to the coordinator at once with the device model and bridge. The timing is recorded, not judged.

### H-NAME-01 Windows can open what the Mac made, and the reverse (U22, Windows part)

Device: F. Goal: U22 says the Mac stores names as NFC and Windows' view is unconfirmed; ADR-023 depends on it.

Steps
1. Win: P3, eject. Mac: plug, P1, P4, P4rw (`rw`). Create names on the Mac (`$MP`): `touch "$MP/caf$(printf '\303\251').txt"` (NFC e-acute), the same word in NFD `touch "$MP/cafe$(printf '\314\201').txt"` (this may be refused or merged: record), `touch "$MP/$(printf '\346\227\245\346\234\254').txt"`, a name with emoji `touch "$MP/smile-$(printf '\360\237\230\200').txt"`, and a file created by Finder named by typing `é` with the NFD input method. `ls -la "$MP" | cat -v`. P6 tag `name-mac`. Eject.
2. Win: plug. `Get-ChildItem X:\ | ForEach-Object { $_.Name; ($_.Name.ToCharArray() | ForEach-Object { '{0:X4}' -f [int]$_ }) -join ' ' }` (the code units show NFC or NFD), then `Get-Content -LiteralPath "X:\café.txt"` for the typed NFC name. P11 with tag `name`. Create `X:\win-café.txt`, P8.
3. Mac: plug, P1, P4, P4rw; `ls "$MP" | cat -v`; `cat "$MP/win-caf$(printf '\303\251').txt"`; `touch "$MP/new-after-win.txt"`; `ntfsmac unmount $SLICE`; eject. Win: P11 with tag `name-final`.

Record: the code units Windows shows for each Mac-made name; whether Windows opens it; whether the Mac opens the Windows-made name; any `chkdsk` message.

Pass: Windows lists each Mac-made name as NFC (and opens it by that spelling), the Mac opens the Windows-made name, `chkdsk /scan` is clean. Fail: Windows shows NFD or garbled names, or `chkdsk` objects to a name.

### H-PERF-01 Throughput of the passive-I/O product mount on a physical disk

Devices: F and S. Goal: `docs/spikes/M5-perf.md` found the 28 to 89 times slowdown to be a priority inversion between the worker and `diskimages-helper`, fixed by passive disk I/O (`ntfsmac_spawn_io_passive()`, commit 365b689), and measured it only on disk images: "A physical USB or internal disk has no such helper; whether it was affected was not measured (Environment C)." This case measures the product mount on the physical disks, against the same disk's buffered node.

Prerequisite: the dev install of the commit tested; a disk laid out for this case: like P3 (`Confirm-Disposable -DiskNumber $N -Serial $S -Typed`, `Clear-Disk`, `Initialize-Disk -PartitionStyle MBR`), but with two partitions: the NTFS volume takes all but 3 GiB, and a 2 GiB **scratch** partition stays unformatted and without a drive letter (the target of the raw write baseline):
```powershell
$p = New-Partition -DiskNumber $N -Size ((Get-Disk -Number $N).Size - 3GB) -AssignDriveLetter -MbrType IFS
Format-Volume -DriveLetter $p.DriveLetter -FileSystem NTFS -NewFileSystemLabel HWTEST -Confirm:$false
New-Partition -DiskNumber $N -UseMaximumSize -MbrType IFS | Out-Null   # scratch: no letter, no format
```
then the P3 baseline file and flush, P8. On the Mac the NTFS volume is `$SLICE` and the scratch partition is `$SCRATCH` (`diskNs2`; check `diskutil list $DISK`), mounted `rw` by the product (P4, P4rw), at least 2 GiB free beyond the files. No other disk traffic on the Mac (Spotlight indexing of the volume off if you know how **[U]**; at least do not browse it).

Steps
1. Product mount, the benchmark of tests/perf (the same tool and parameters as the Environment B run; the criterion parameters of `tests/perf/README.md` are 5 repetitions, 1 GiB sequential, 16,384 random operations, 10,000 small files, directories of 10,000 and 100,000 entries; on a USB flash drive this takes hours, so the standard run below is the minimum and the full one is optional **[U]**):
```sh
mkdir -p "$EV"
export HW_SLICE=$SLICE HW_SERIAL="$SERIAL"     # the hook runs identify.sh with the serial at every mount
python3 -I tests/perf/bench.py --hook "sh $PWD/tests/hardware/perf_hook.sh" \
	--out "$EV/perf-$DEVICE.json" --label "C: product mount on $DEVICE" --emit-json \
	--reps 3 --seq-mib 1024 --rnd-ops 16384 --small-files 4000 --dir-entries 10000 \
	--meta device="$DEVICE" --meta serial="$SERIAL" --meta commit="$(git rev-parse HEAD)" \
	--meta macos="$(sw_vers -productVersion)" > "$EV/perf-$DEVICE.log" 2>&1
python3 -I -c 'import json,sys; d=json.load(open(sys.argv[1])); [print(k, v.get("median"), v.get("unit"), v.get("min"), v.get("max"), v.get("errors")) for k, v in d["phases"].items()]' "$EV/perf-$DEVICE.json"
```
   (`DEVICE` is `F` or `S`.) The hook mounts and unmounts through `ntfsmac` (the cold-cache step of each read phase). The benchmark writes only below `<mount>/perfbench` on this disposable volume. It exits 0 only if every phase of every repetition finished with verified data.
2. Passive-I/O check on the real disk, during a write phase (second terminal, while step 1 runs the sequential write): `sudo fs_usage -w -f diskio 2>&1 | grep ntfsmac-worker | head -n 40 > "$EV/perf-$DEVICE-fsusage.txt"`. Record the flags shown in brackets (the spike saw `[AP]`-type passive flags on images; whether the same shows on a USB disk is **[U]**).
3. Device baselines, volume unmounted (`ntfsmac unmount $SLICE`; `mount | grep "$SLICE "` shows nothing). The NTFS volume is never written. The write baseline goes to the **scratch partition** (destructive for it only; it holds nothing), the read baselines come from the scratch partition (the same device, raw and buffered nodes; its position on the disk differs from the volume's, which matters on an HDD **[U]**) and from the volume itself (read-only):
```sh
export SCRATCH=${DISK}s2
diskutil list $DISK                                                    # SCRATCH must be the 2 GiB unformatted partition
[ "$SCRATCH" != "$SLICE" ] && confirm_disposable && case $SCRATCH in "$DISK"s[0-9]*) sudo dd if=/dev/zero of=/dev/r$SCRATCH bs=1m count=1024 ;; esac   # raw write baseline, MB/s from dd
sudo purge; sudo dd if=/dev/r$SCRATCH of=/dev/null bs=1m count=1024    # raw read baseline (scratch)
sudo purge; sudo dd if=/dev/$SLICE of=/dev/null bs=1m count=1024       # buffered node of the volume, what the worker reads
sudo purge; sudo dd if=/dev/r$SLICE of=/dev/null bs=1m count=1024      # raw node of the volume
```
   Record the MB/s that `dd` prints for each line. `dd` with `bs=1m` on the raw node is a sequential 1 MiB stream without a flush at the end; the product's sequential write phase ends with `fsync`: that difference favours the baseline and is part of the comparison.
4. Optional reference on Windows, native driver, same device: write and read the same `large` folder (about 1 GiB) on `X:`: `Measure-Command { Copy-Item -Recurse C:\hwtest\tree-1g\large X:\ref; Write-VolumeCache -DriveLetter X }` for the write and a copy back for the read; record MiB/s **[U]**: Windows' cache makes this a rough number. It is the other write comparison if the scratch partition cannot be used.

Record

| Device | Phase | Product median (unit) | min / max | Baseline (raw write of scratch; raw read of scratch, buffered and raw node of the volume) | Ratio baseline/product | `fsync` time of the phase |
|---|---|---|---|---|---|---|
| F | sequential write (1 GiB, fsync at the end) | | | | | |
| F | sequential read (cold) | | | | | |
| F | random 4 KiB write / read | | | n/a | n/a | |
| F | small files create / stat / read / delete | | | n/a | n/a | |
| F | directory of 10,000 list / stat | | | n/a | n/a | |
| S | (same rows) | | | | | |

Pass (the same 2x rule as `tests/perf/README.md` and the report, each direction against its own baseline): the product's sequential write is not more than 2x slower than the raw write baseline of the scratch partition (or than the Windows native write, if that is the baseline used), the sequential read is not more than 2x slower than the raw and buffered read baselines, and the phases all finish with verified data. Reads are never compared with writes. A larger ratio is a finding to be explained, with the `fs_usage` flags. This is a measurement case: there is no speed threshold other than that rule. Fail: the benchmark does not finish; a verification error (data returned wrongly); a slowdown of the 28x kind seen on images.

## 10. Order of work and time

A workable order (each line leaves the disk reset or ready for the next). The hours are hands-on estimates that include the P3 resets, plugging between machines and writing the record tables; a 1 GiB copy to a slow flash drive alone can take 10 minutes **[U]**.

| Order | Cases | Disks | Hours |
|---|---|---|---|
| 1 | Section 3 preparation, `H-USB-01` (default automount install), then the read-only install, `H-USB-02` | F, then S | 3 |
| 2 | `H-USB-10` legs A to D, `H-USB-11`, `H-NAME-01` | F | 4 |
| 3 | `H-FND-01`, `H-USB-06` | F, S | 3 |
| 4 | `H-PERF-01` (the standard run; the full criterion run is optional and takes many hours) | F, S | 4 |
| 5 | `H-USB-03` (3 repetitions per disk), the chain `H-USB-03` repetition 1 then `H-USB-08` | F, S | 4 |
| 6 | `H-USB-03` again, then `H-USB-09` | F | 1.5 |
| 7 | `H-USB-04` (3 repetitions per disk), `H-SYNC-01` (5 repetitions per disk) | F, S | 5 |
| 8 | `H-USB-05`, `H-USB-07` | S (05), F (07) | 2 (leg 2 of 05 only with Windows on an external disk) |
| 9 | Wipe, sign-off | F, S | 1 |

Total about 27 hours: three working days, or two long ones. Stop at any clean point; the case table of section 11 records NOT DONE for the rest. The release statement follows criterion 5.8 (section 12).

**Final wipe** (not a pass criterion; the disks are not reused). On Windows, for each disk:
```powershell
$N = <disk number>; $S = '<serial, Windows form>'
Confirm-Disposable -DiskNumber $N -Serial $S -Typed
Clear-Disk -Number $N -RemoveData -RemoveOEM -Confirm:$false
Initialize-Disk -Number $N -PartitionStyle MBR
$p = New-Partition -DiskNumber $N -UseMaximumSize -AssignDriveLetter -MbrType IFS
Format-Volume -DriveLetter $p.DriveLetter -FileSystem NTFS -Full -Confirm:$false
```

## 11. Recording results

Per case, the record tables of the sections above are filled in the results file (copy of `tests/hardware/results-template.md`), together with the evidence files.

| What | Where |
|---|---|
| Results file | `tests/results/hardware/<UTC date>-<sha7>.md` (TEST_STRATEGY.md section 10: "written from the protocol, with device serial numbers and the human's confirmation"), copied from `tests/hardware/results-template.md` |
| Evidence | `tests/results/evidence/hardware-<UTC date>/`: the P6 snapshot texts (`<tag>.txt`), the Windows snapshot texts (`win-<tag>.txt`) and `chkdsk` outputs copied from `C:\hwtest\evidence`, `base.list` and `fs.*` files, `perf-*.json` and logs. Disk images (`--copy`) are written to `~/hwimages` (outside the repository; `*.img` is git-ignored too) and are never committed: their SHA-256 is printed into the evidence text (the evidence rule: "images are not stored, only their hashes"). Keep them until the coordinator has analysed them in Environment A |
| Serial numbers | In the results file (the device table) as the TEST_STRATEGY.md requires. These are the serials of disposable media, not personal data; the host name of the Mac and Windows machine is replaced by `<host>` as for the other evidence (section 10, aggregated runs) |
| `Hardware-verified:` lines | Written only at sign-off (section 12). `tests/harness/report.py` reads the files named `<UTC date>-<sha7>.md` directly in `tests/results/hardware/` (other names, and commits that are not an ancestor of `HEAD`, are ignored) and takes, from each line at the start of a line of the form `Hardware-verified: ID, ID`, the listed ids (`hardware_verified()`, `HW_RE`). The README status table (`python3 tests/harness/report.py readme`) shows `Verified (hardware)` for a **feature id of `tests/harness/features.json`** that appears there, unless a VM or Linux case of that feature fails. Case ids such as `H-USB-01` are not feature ids and have no effect on the table; they are written on a second line for the record |
| Which feature ids | Only a feature whose behaviour the signed cases showed on hardware. Mapping decided by the coordinator: `integrity`, `file-io` and `daemon` after the cases named in 1 to 3 below pass; `xattr` after `H-USB-11`; `names` after `H-NAME-01` and `H-USB-02`; `rename-delete` after `H-FND-01`. **`sync` is not mapped from `H-SYNC-01`** (a few pulls cannot prove durability; the case records R-30 evidence only). Cases with no feature (`H-USB-05`, `H-USB-07`, `H-USB-10`, `H-PERF-01`) get case-id lines only. (1) `integrity`, `file-io`: `H-USB-01`, `H-USB-02`, `H-FND-01`; (2) `daemon`: `H-USB-03`, `H-USB-04`, `H-USB-08`, `H-USB-09`; (3) the features above for `xattr`, `names`, `rename-delete`. `features.json` has these ids (checked against `tests/harness/features.json`: `names` and `xattr` exist) |
| Limits of the `Hardware-verified:` line | The line is bound to the commit in the file name: `report.py` reads only files named `<UTC date>-<sha7>.md` and ignores the lines of a file with another name or whose commit is not an ancestor of `HEAD`; the README status table shows that commit in its last column. The line still stays true for later commits that descend from it, which is why the table shows the commit and not only the state (M5-F11) |
| Findings | A FAIL or INCONCLUSIVE has a paragraph in the results file and goes to the coordinator for RISK_REGISTER.md and ARCHITECTURE.md section 11 (U12, U15, U23). The agents update those documents, not the operator |
| Follow-ups (not done by this protocol) | (1) Evidence-rule replay: run `ntfsmac_policy_evidence_holds()` on the `--copy` images of `08-after` and `09-after` together with the copied journal entry on the host, to turn the hand computation of D5 into a mechanical check. (2) TEST_STRATEGY.md section 11 words `H-USB-08` and `H-USB-09` as "with the evidence rule enabled" although the product build cannot enable it; the wording needs amending once the coordinator decides how the rule is switched on |
| Gate | The result file is a text file under `tests/results/hardware/`; `scripts/ci-local.sh --verify-result` checks the JSON files of the VM environments only and does not read it **[U]**. The milestone audit reads this file for criterion 5.8 |

## 12. Sign-off

The sign-off is the last section of the results file (the template has it). It is written and signed by the operator,
not by an agent, after the run. It must hold:

| Item | Content |
|---|---|
| Operator | GitHub handle or initials (not the full name) |
| Date (UTC) | the date of the last case |
| Mac | model, macOS version and build, macFUSE version, NTFS-3G version of the worker |
| Windows | edition, version and build, machine (or VM with USB pass-through, declared) |
| Devices | vendor, model, serial (Mac and Windows form), size, for F and S |
| Commit | the full hash tested (`git rev-parse HEAD`) and "tree clean" |
| Per case | PASS, FAIL, NOT DONE or INCONCLUSIVE for `H-USB-01` to `H-USB-11`, `H-FND-01`, `H-SYNC-01`, `H-NAME-01`, `H-PERF-01`, each with the devices it ran on |
| Statement | "I carried out the steps myself, on the disposable media named above, and the results are as I observed them." |
| Criterion 5.8 | met, not met, or partly (list the cases). **Met** means `H-USB-01` to `H-USB-04` (insert, read/write, eject, unplug while idle and during a write, Windows `chkdsk` after each, P11) PASS on **both** F and S. **T-M5-10 done** needs, in addition, `H-USB-08`, `H-USB-09`, `H-USB-10` and `H-USB-11` executed and recorded (any result, with the findings written up; they run on F only). Run on one device only: `H-USB-05` (S), `H-USB-07` (F), `H-USB-08` to `H-USB-11` (F), `H-NAME-01` (F). Run on both: `H-USB-01` to `H-USB-04`, `H-USB-06`, `H-FND-01`, `H-SYNC-01`, `H-PERF-01`. No case may FAIL without a finding |
| Hardware-verified lines | The lines for `tests/harness/report.py`, only for what passed (section 11) |

Without a signed file, or with criterion 5.8 "not met", the release notes say "VM-verified only, not production-ready" (brief section 9).
