# SPDX-License-Identifier: GPL-2.0-or-later
#
# windows-collect.ps1 - Environment C, Windows side (docs/HARDWARE_TEST_PROTOCOL.md): READ-ONLY record of one volume
# as Windows sees it. It runs queries only (Get-Disk, Get-Volume, fsutil dirty query, fsutil fsinfo, the event log).
# It does not run chkdsk, does not format, does not write to the drive. It writes one text file, and only on a
# folder that is NOT on the drive under test.
#
# Usage (elevated PowerShell):
#   powershell -ExecutionPolicy Bypass -File windows-collect.ps1 -Drive X -Tag 08-win-before -Out C:\hwtest\evidence
# Parameters: -Drive  the drive letter of the disposable volume (one letter); the script refuses C and a letter
#                     whose disk is not on the USB bus, or is a boot or system disk
#             -Tag    name of the snapshot; -Out  folder for the text file (must not be on -Drive)
param(
    [Parameter(Mandatory = $true)][ValidatePattern('^[A-Za-z]$')][string]$Drive,
    [Parameter(Mandatory = $true)][ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Tag,
    [Parameter(Mandatory = $true)][string]$Out
)
$ErrorActionPreference = 'Continue'
$Drive = $Drive.ToUpper()
if ($Drive -eq 'C') { throw 'refused: C: is the system drive' }
$Out = [System.IO.Path]::GetFullPath($Out)   # resolve relative paths against the current directory first
if ($Out.ToUpper().StartsWith("$Drive`:")) { throw 'refused: -Out is on the drive under test' }
New-Item -ItemType Directory -Force -Path $Out | Out-Null

$part = Get-Partition -DriveLetter $Drive -ErrorAction Stop
$disk = Get-Disk -Number $part.DiskNumber -ErrorAction Stop
if ($disk.BusType -ne 'USB') { throw "refused: disk $($disk.Number) is on bus $($disk.BusType), not USB" }
if ($disk.IsBoot -or $disk.IsSystem) { throw "refused: disk $($disk.Number) is a boot or system disk" }

$file = Join-Path $Out "win-$Tag.txt"
& {
    "tag: $Tag"
    "utc: $((Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'))"
    "host: <host>"
    "os: $((Get-CimInstance Win32_OperatingSystem).Caption) $((Get-CimInstance Win32_OperatingSystem).Version)"
    "drive: ${Drive}:"
    "== disk =="
    $disk | Format-List Number, FriendlyName, SerialNumber, Size, BusType, PartitionStyle, IsBoot, IsSystem, IsReadOnly | Out-String
    "== volume =="
    Get-Volume -DriveLetter $Drive | Format-List DriveLetter, FileSystemLabel, FileSystem, DriveType, HealthStatus, Size, SizeRemaining | Out-String
    "== fsutil dirty query =="
    fsutil dirty query "${Drive}:"
    "== fsutil fsinfo ntfsinfo =="
    fsutil fsinfo ntfsinfo "${Drive}:"
    "== fsutil fsinfo volumeinfo =="
    fsutil fsinfo volumeinfo "${Drive}:"
    "== recent Chkdsk / Ntfs / Wininit events (Application log, newest first) =="
    try {
        Get-WinEvent -FilterHashtable @{ LogName = 'Application'; ProviderName = 'Chkdsk', 'Microsoft-Windows-Ntfs', 'Wininit' } -MaxEvents 30 -ErrorAction Stop |
            Format-List TimeCreated, ProviderName, Id, Message | Out-String
    } catch { "no such events: $($_.Exception.Message)" }
    "== recent Ntfs events (System log; providers Ntfs and Microsoft-Windows-Ntfs) =="
    try {
        Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Ntfs', 'Microsoft-Windows-Ntfs' } -MaxEvents 20 -ErrorAction Stop |
            Format-List TimeCreated, Id, LevelDisplayName, Message | Out-String
    } catch { "no such events: $($_.Exception.Message)" }
} *>&1 | Out-File -Encoding utf8 -FilePath $file
Get-Content $file
"wrote $file"
