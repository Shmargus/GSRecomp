# Both normal developer builds, in order. Nothing builds until the user runs it.
[CmdletBinding()]
param(
    [ValidateRange(1, 100)][int]$CpuPercent = 90,
    [ValidateRange(1, 100)][int]$RamPercent = 90,
    # None builds only the collection launcher (build_launcher.bat).
    [ValidateSet('Both', 'BrokenSeal', 'LostAge', 'None')][string]$Game = 'Both',
    [switch]$CheckOnly,
    [int]$LauncherProcessId = 0,
    [switch]$SkipLauncher
)
$ErrorActionPreference = 'Stop'
$collectionRoot = Split-Path -Parent $PSScriptRoot
$gsRoot = Join-Path $collectionRoot 'The Broken Seal'
$tlaRoot = Join-Path $collectionRoot 'The Lost Age'
$engineRoot = Join-Path $collectionRoot 'engine'
$logRoot = Join-Path $collectionRoot 'local/build-dev'
$buildGs = $Game -in @('Both', 'BrokenSeal')
$buildTla = $Game -in @('Both', 'LostAge')
$originalLocation = Get-Location
$buildMutex = $null
$ownsMutex = $false

function Assert-Idle {
    if ($LauncherProcessId -lt 0) { throw 'LauncherProcessId cannot be negative.' }
    if ($LauncherProcessId -gt 0) {
        if (-not $SkipLauncher) { throw 'LauncherProcessId requires SkipLauncher so the running launcher is not replaced.' }
        $launcher = Get-Process -Id $LauncherProcessId -ErrorAction Stop
        $expectedPath = switch ($launcher.ProcessName) {
            'GoldenSunLauncher' { Join-Path $collectionRoot 'GoldenSunLauncher.exe' }
            'GoldenSunCollection' { Join-Path $gsRoot 'build/gs011_opt/GoldenSunCollection.exe' }
            default { throw 'LauncherProcessId must identify the collection launcher.' }
        }
        if (-not $launcher.Path -or -not (Same-Path $launcher.Path $expectedPath)) {
            throw 'LauncherProcessId does not identify a launcher in this collection.'
        }
    }
    $busy = @(Get-Process -Name cmake,ninja,make,mingw32-make,gcc,'g++',cc,'c++',cc1,cc1plus,lto1,lto-wrapper,ld,collect2,gsr_builder,GoldenSunRecomp,TLARecomp,GoldenSunLauncher,GoldenSunCollection,TLALauncher -ErrorAction SilentlyContinue)
    if ($LauncherProcessId -gt 0) { $busy = @($busy | Where-Object { $_.Id -ne $LauncherProcessId }) }
    if ($busy.Count) {
        $names = ($busy | Select-Object -ExpandProperty ProcessName -Unique) -join ', '
        throw "Close the games and launchers and wait for other builds to finish: $names."
    }
}

function Read-Cache([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "The normal developer configuration is missing: $Path"
    }
    $entries = @{}
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line -match '^([^#/:][^:]*):([^=]+)=(.*)$') {
            $entries[$Matches[1]] = [pscustomobject]@{ Type = $Matches[2]; Value = $Matches[3] }
        }
    }
    if (-not $entries.ContainsKey('CMAKE_HOME_DIRECTORY') -or -not $entries.ContainsKey('CMAKE_GENERATOR')) {
        throw "The developer configuration is incomplete: $Path"
    }
    return $entries
}

function Same-Path([string]$Left, [string]$Right) {
    return [IO.Path]::GetFullPath($Left).TrimEnd('\', '/') -ieq [IO.Path]::GetFullPath($Right).TrimEnd('\', '/')
}

function Remap-Value([string]$Value, [object[]]$Mappings) {
    # Preserve already-relocated paths in a partially updated cache. The old
    # GS root is now the collection parent and otherwise matches those too.
    $mapped = $Value
    for ($index = 0; $index -lt $Mappings.Count; ++$index) {
        $current = $Mappings[$index].New -replace '\\', '/'
        $pattern = [regex]::Escape($current.TrimEnd('/')).Replace('/', '[\\/]') + '(?=$|[\\/;"\s])'
        $mapped = [regex]::Replace($mapped, $pattern, "__GSR_CURRENT_ROOT_${index}__", [Text.RegularExpressions.RegexOptions]::IgnoreCase)
    }
    # Reserve old-root placeholders before expanding them, so replacements
    # cannot be remapped again through another old prefix.
    for ($index = 0; $index -lt $Mappings.Count; ++$index) {
        $old = $Mappings[$index].Old -replace '\\', '/'
        $pattern = [regex]::Escape($old.TrimEnd('/')).Replace('/', '[\\/]') + '(?=$|[\\/;"\s])'
        $mapped = [regex]::Replace($mapped, $pattern, "__GSR_MOVED_ROOT_${index}__", [Text.RegularExpressions.RegexOptions]::IgnoreCase)
    }
    for ($index = 0; $index -lt $Mappings.Count; ++$index) {
        $mapped = $mapped.Replace("__GSR_MOVED_ROOT_${index}__", ($Mappings[$index].New -replace '\\', '/'))
        $mapped = $mapped.Replace("__GSR_CURRENT_ROOT_${index}__", ($Mappings[$index].New -replace '\\', '/'))
    }
    return $mapped
}

function Set-Setting($Settings, [string]$Name, [string]$Type, [string]$Value) {
    $Settings[$Name] = [pscustomobject]@{ Type = $Type; Value = $Value }
}

function Get-Plan([string]$Name, [string]$Source, [string]$BuildDir, $Cache, [object[]]$Mappings) {
    $settings = @{}
    $changed = 0
    foreach ($key in $Cache.Keys) {
        if ($key -in @('GSR_LAUNCHER_OUTPUT_DIR', 'TLA_WITH_LAUNCHER')) { continue }
        $entry = $Cache[$key]
        # Generated INTERNAL/STATIC metadata belongs to the previous tree.
        if ($entry.Type -notin @('BOOL', 'STRING', 'PATH', 'FILEPATH', 'UNINITIALIZED')) { continue }
        $value = Remap-Value $entry.Value $Mappings
        if ($value -cne $entry.Value) { ++$changed }
        $type = if ($entry.Type -eq 'UNINITIALIZED') { 'STRING' } else { $entry.Type }
        Set-Setting $settings $key $type $value
    }
    Set-Setting $settings 'GSR_RELEASE_VERSION' 'STRING' 'dev'
    Set-Setting $settings 'GBARECOMP_STATIC_RELEASE' 'BOOL' 'OFF'
    Set-Setting $settings 'GBARECOMP_ROOT' 'PATH' ($engineRoot -replace '\\', '/')
    Set-Setting $settings 'CMAKE_RUNTIME_OUTPUT_DIRECTORY' 'PATH' ($BuildDir -replace '\\', '/')
    # Existing fetched sources survive --fresh. Reuse them instead of running
    # an old FetchContent sub-build whose cache still names the previous folder.
    foreach ($dependency in @(@('IMGUI', 'imgui'), @('TOMLPLUSPLUS', 'tomlplusplus'))) {
        $key = 'FETCHCONTENT_SOURCE_DIR_' + $dependency[0]
        $sourceDir = Join-Path $gsRoot ('build/gs011_opt/_deps/' + $dependency[1] + '-src')
        if ((-not $settings.ContainsKey($key) -or -not $settings[$key].Value) -and
            (Test-Path -LiteralPath $sourceDir -PathType Container)) {
            Set-Setting $settings $key 'PATH' ($sourceDir -replace '\\', '/')
        }
    }
    if ($Name -eq 'gs') {
        Set-Setting $settings 'GSR_RELEASE_LAUNCHER' 'BOOL' 'OFF'
        Set-Setting $settings 'GSR_BUILD_LOCAL_RUNNER' 'BOOL' 'ON'
    } else {
        Set-Setting $settings 'TLA_RELEASE_LAUNCHER' 'BOOL' 'OFF'
        Set-Setting $settings 'TLA_PLAYER_RELEASE' 'BOOL' 'OFF'
        # Retired: the player builder now lives at the collection root.
        $null = $settings.Remove('TLA_COLLECTION_ROOT')
    }
    return [pscustomobject]@{
        Name = $Name; Source = $Source; BuildDir = $BuildDir; Settings = $settings
        Generator = $Cache['CMAKE_GENERATOR'].Value
        Fresh = -not (Same-Path $Cache['CMAKE_HOME_DIRECTORY'].Value $Source)
        Remapped = $changed; CachePath = Join-Path $BuildDir 'CMakeCache.txt'
    }
}

function Require-File([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "$Description is missing: $Path" }
}

function Validate-Plan($Plan) {
    Require-File (Join-Path $Plan.Source 'CMakeLists.txt') 'Project configuration'
    foreach ($key in @('CMAKE_CXX_COMPILER', 'CMAKE_MAKE_PROGRAM', 'SDL2_LIBRARY')) {
        if (-not $Plan.Settings.ContainsKey($key) -or -not $Plan.Settings[$key].Value) { throw "$($Plan.Name): $key is not configured." }
        Require-File $Plan.Settings[$key].Value $key
    }
    foreach ($key in @('SDL2_INCLUDE_DIR', 'FETCHCONTENT_SOURCE_DIR_IMGUI', 'FETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS')) {
        if ($Plan.Settings.ContainsKey($key) -and $Plan.Settings[$key].Value -and
            -not (Test-Path -LiteralPath $Plan.Settings[$key].Value -PathType Container)) {
            throw "$($Plan.Name): dependency folder is missing: $($Plan.Settings[$key].Value)"
        }
    }
    Write-Host "$($Plan.Name): $($Plan.BuildDir)"
    Write-Host "  Preserve $($Plan.Settings.Count) settings; remap $($Plan.Remapped); fresh configuration: $($Plan.Fresh)."
    foreach ($key in @('CMAKE_CXX_COMPILER', 'SDL2_LIBRARY', 'FETCHCONTENT_SOURCE_DIR_IMGUI', 'FETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS', 'GBARECOMP_ROOT')) {
        if ($Plan.Settings.ContainsKey($key) -and $Plan.Settings[$key].Value) { Write-Host "  ${key}: $($Plan.Settings[$key].Value)" }
    }
}

function Cmake-Literal([string]$Value) {
    $equals = '='
    while ($Value.Contains("]$equals]")) { $equals += '=' }
    return "[$equals[$Value]$equals]"
}

function Invoke-NativeLogged([string]$Executable, [string[]]$NativeArguments, [string]$LogFile) {
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $Executable @NativeArguments 2>&1 | ForEach-Object { "$_" } |
            Tee-Object -FilePath $LogFile | ForEach-Object { Write-Host $_ }
        $nativeExit = $LASTEXITCODE
    } finally { $ErrorActionPreference = $previousPreference }
    if ($nativeExit -ne 0) { throw "Command failed ($nativeExit). See $LogFile" }
}

function Configure-Plan($Plan, [string]$RunDir) {
    Assert-Idle
    $cacheBackup = Join-Path $RunDir "$($Plan.Name)-CMakeCache-before.txt"
    Copy-Item -LiteralPath $Plan.CachePath -Destination $cacheBackup
    $preload = Join-Path $RunDir "$($Plan.Name)-settings.cmake"
    $lines = @('# Preserved and relocated developer cache entries.')
    foreach ($key in @($Plan.Settings.Keys | Sort-Object)) {
        $entry = $Plan.Settings[$key]
        $literal = Cmake-Literal $entry.Value
        $lines += "set($key $literal CACHE $($entry.Type) `"Developer build setting`" FORCE)"
    }
    [IO.File]::WriteAllLines($preload, $lines, (New-Object Text.UTF8Encoding($false)))
    $arguments = @('-S', $Plan.Source, '-B', $Plan.BuildDir, '-G', $Plan.Generator, '-C', $preload)
    if ($Plan.Fresh) { $arguments += '--fresh' }
    Invoke-NativeLogged $script:cmake $arguments (Join-Path $RunDir "$($Plan.Name)-configure.log")
}

try {
    if (-not [Environment]::Is64BitProcess) { throw 'Use 64-bit PowerShell.' }
    if ($Game -eq 'None' -and $SkipLauncher) { throw 'Nothing to build: no game and no launcher.' }
    Assert-Idle
    $cmake = (Get-Command cmake.exe -ErrorAction Stop).Source
    $python = (Get-Command python.exe -ErrorAction Stop).Source
    $buildStampScript = Join-Path $PSScriptRoot 'developer_build_stamp.py'
    Require-File $buildStampScript 'Developer build record writer'
    Require-File (Join-Path $PSScriptRoot 'developer_build_inputs.txt') 'Developer build input specification'
    $powershell = Join-Path $PSHOME 'powershell.exe'
    $buildGameScript = Join-Path $PSScriptRoot 'build_game.ps1'
    $gsBuild = Join-Path $gsRoot 'build/gs011_opt'
    $tlaBuild = Join-Path $tlaRoot 'build/tla'
    $gsCache = Read-Cache (Join-Path $gsBuild 'CMakeCache.txt')
    $mappings = @([pscustomobject]@{ Old = $gsCache['CMAKE_HOME_DIRECTORY'].Value; New = $gsRoot })
    if ($buildTla) {
        $tlaCache = Read-Cache (Join-Path $tlaBuild 'CMakeCache.txt')
        $mappings += [pscustomobject]@{ Old = $tlaCache['CMAKE_HOME_DIRECTORY'].Value; New = $tlaRoot }
    }
    $mappings = @($mappings | Sort-Object { $_.Old.Length } -Descending)
    $gsPlan = Get-Plan 'gs' $gsRoot $gsBuild $gsCache $mappings
    Validate-Plan $gsPlan
    $generatedFolders = @()
    if ($buildGs) {
        Require-File $buildGameScript 'Game build script'
        $generatedFolders += Join-Path $gsRoot 'local/gs011'
    }
    if ($buildTla) {
        $tlaPlan = Get-Plan 'tla' $tlaRoot $tlaBuild $tlaCache $mappings
        Validate-Plan $tlaPlan
        $generatedFolders += @((Join-Path $tlaRoot 'local/tla/main'), (Join-Path $tlaRoot 'local/tla/reloc'))
    }
    foreach ($folder in $generatedFolders) {
        if (-not (Test-Path -LiteralPath $folder -PathType Container)) { throw "Generated game code is missing: $folder" }
    }
    $compilerBin = Split-Path -Parent $gsPlan.Settings['CMAKE_CXX_COMPILER'].Value
    Require-File (Join-Path $compilerBin 'mingw32-make.exe') 'LTO make tool'
    $runtimeDlls = @{}
    foreach ($dll in @('SDL2.dll', 'zlib1.dll', 'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll')) {
        $locations = @((Join-Path $compilerBin $dll))
        if ($dll -eq 'SDL2.dll') {
            $libraryDir = Split-Path -Parent $gsPlan.Settings['SDL2_LIBRARY'].Value
            $locations = @((Join-Path $libraryDir "../bin/$dll")) + $locations
        }
        $found = @($locations | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf })
        if (-not $found.Count) { throw "Collection launcher dependency is missing: $dll" }
        $runtimeDlls[$dll] = [IO.Path]::GetFullPath($found[0])
    }
    $launcherStep = if ($SkipLauncher) { 'collection launcher skipped' } else { 'then the collection launcher' }
    Write-Host "CPU/RAM budgets: $CpuPercent% / $RamPercent%. Selected games: $Game; $launcherStep."
    if ($CheckOnly) {
        Write-Host 'Validation passed. No configuration, files or builds changed.'
        exit 0
    }
    $buildMutex = New-Object Threading.Mutex($false, 'Local\GoldenSunCollectionBuild')
    try { $ownsMutex = $buildMutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $ownsMutex = $true }
    if (-not $ownsMutex) { throw 'Another collection build or release is running.' }
    Assert-Idle
    $runDir = Join-Path $logRoot (Get-Date -Format 'yyyyMMdd-HHmmss-fff')
    $null = New-Item -ItemType Directory -Path $runDir -Force
    $env:PATH = "$compilerBin;$env:PATH"
    $env:MAKE = (Join-Path $compilerBin 'mingw32-make.exe') -replace '\\', '/'
    Set-Location -LiteralPath $collectionRoot

    Configure-Plan $gsPlan $runDir
    if ($buildGs) {
    & $python $buildStampScript --root $collectionRoot --game BrokenSeal --build-dir $gsBuild --begin --snapshot (Join-Path $runDir 'gs-inputs.txt')
    if ($LASTEXITCODE -ne 0) { throw "The Broken Seal input snapshot failed ($LASTEXITCODE)." }
    & $powershell -NoProfile -ExecutionPolicy Bypass -File $buildGameScript -Game BrokenSeal -CpuPercent $CpuPercent -RamPercent $RamPercent -LogFile (Join-Path $runDir 'gs-build.log')
    if ($LASTEXITCODE -ne 0) { throw "The Broken Seal build failed ($LASTEXITCODE)." }
    & $python $buildStampScript --root $collectionRoot --game BrokenSeal --build-dir $gsBuild --finish --snapshot (Join-Path $runDir 'gs-inputs.txt')
    if ($LASTEXITCODE -ne 0) { throw "The Broken Seal build record failed ($LASTEXITCODE)." }
    }
    if ($buildTla) {
    Configure-Plan $tlaPlan $runDir
    & $python $buildStampScript --root $collectionRoot --game LostAge --build-dir $tlaBuild --begin --snapshot (Join-Path $runDir 'tla-inputs.txt')
    if ($LASTEXITCODE -ne 0) { throw "The Lost Age input snapshot failed ($LASTEXITCODE)." }
    & $powershell -NoProfile -ExecutionPolicy Bypass -File $buildGameScript -Game LostAge -CpuPercent $CpuPercent -RamPercent $RamPercent -LogFile (Join-Path $runDir 'tla-build.log')
    if ($LASTEXITCODE -ne 0) { throw "The Lost Age build failed ($LASTEXITCODE)." }
    & $python $buildStampScript --root $collectionRoot --game LostAge --build-dir $tlaBuild --finish --snapshot (Join-Path $runDir 'tla-inputs.txt')
    if ($LASTEXITCODE -ne 0) { throw "The Lost Age build record failed ($LASTEXITCODE)." }
    }
    # Both session implementations are built into this one collection launcher.
    Assert-Idle
    if (-not $SkipLauncher) {
        $jobs = [Math]::Max(1, [int][Math]::Floor([Environment]::ProcessorCount * $CpuPercent / 100.0))
        Invoke-NativeLogged $cmake @('--build', $gsBuild, '--target', 'GoldenSunCollection', '--parallel', "$jobs") (Join-Path $runDir 'collection-build.log')
        $selector = Join-Path $gsBuild 'GoldenSunCollection.exe'
        Require-File $selector 'Built collection launcher'
    }
    Assert-Idle
    foreach ($dll in $runtimeDlls.Keys) {
        if (-not $SkipLauncher) { Copy-Item -LiteralPath $runtimeDlls[$dll] -Destination (Join-Path $collectionRoot $dll) -Force }
        # Engines remain in their own build trees and must also be runnable
        # without inheriting a toolchain PATH from a development terminal.
        if ($buildGs) { Copy-Item -LiteralPath $runtimeDlls[$dll] -Destination (Join-Path $gsBuild $dll) -Force }
        if ($buildTla) { Copy-Item -LiteralPath $runtimeDlls[$dll] -Destination (Join-Path $tlaBuild $dll) -Force }
    }
    if (-not $SkipLauncher) { Copy-Item -LiteralPath $selector -Destination (Join-Path $collectionRoot 'GoldenSunLauncher.exe') -Force }
    $ready = if ($SkipLauncher) { "Selected developer games ($Game) are ready" } else { "Selected developer games ($Game) and the collection launcher are ready" }
    Write-Host "${ready}. Logs: $runDir"
    exit 0
} catch {
    Write-Host "Developer build stopped: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
} finally {
    if ($ownsMutex) { $buildMutex.ReleaseMutex() }
    if ($null -ne $buildMutex) { $buildMutex.Dispose() }
    Set-Location -LiteralPath $originalLocation
}
