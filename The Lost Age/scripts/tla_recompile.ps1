# Translate using the same pipeline as Linux. Reads config/local.json.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
& python (Join-Path $RepoRoot 'tools/tla_recompile.py')
if ($LASTEXITCODE -ne 0) { throw "ROM translation failed with exit code $LASTEXITCODE." }
