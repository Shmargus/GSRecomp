# Compatibility entry; the collection build is owned by the parent folder.
[CmdletBinding()]
param(
    [switch]$NoTest,
    [switch]$CheckOnly,
    [string]$Version = '',
    [string]$Repository = 'Shmargus/GSRecomp',
    [switch]$NoWindows,
    [switch]$NoLinux,
    [switch]$LauncherOnly,
    [switch]$AshleyEdition,
    [int]$CpuPercent = 90,
    [int]$RamPercent = 90
)
& (Join-Path $PSScriptRoot '../../scripts/make_release.ps1') @PSBoundParameters
exit $LASTEXITCODE
