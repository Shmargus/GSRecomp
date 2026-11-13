# The developer/release game build for both games (replaces The Broken Seal's
# build_lto.ps1 and The Lost Age's tla_build.ps1; see
# Archive/2026-10-13-build-scripts/MANIFEST.txt). Nothing runs until the user
# launches it. Per-game differences are in the $games table below; the shared
# flow is the rest.
# CPU is a Windows job hard cap; memory is a job-wide committed-memory cap
# sized from physical RAM, not a promise of a particular Task Manager reading.
# Other applications are outside these limits. Exceeding the memory budget
# can fail an allocation/build; it does not throttle allocations until space frees.
# References:
# https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_cpu_rate_control_information
# https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_extended_limit_information
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('BrokenSeal', 'LostAge')][string]$Game,
    [ValidateRange(1, 100)][int]$CpuPercent = 90,
    [ValidateRange(1, 100)][int]$RamPercent = 90,
    [switch]$CpuCapInherited,
    # Relative to the game's folder. Empty = the game's normal tree below.
    # make_release.ps1 points this at the release build tree.
    [string]$BuildDir = '',
    # Compile jobs. 0 = the CPU-based count below. (Broken Seal only.)
    # make_release.ps1 lowers it for from-scratch builds, where many of the
    # ~6 MB generated game-code files compile at once and together outgrow
    # the memory limit.
    [int]$CompileJobs = 0,
    # When set, the whole build output is also written here.
    [string]$LogFile = '',
    # Preserve the parent's interpreter when the compiler changes PATH.
    [string]$PythonExecutable = '',
    # Optimisation level for the game's translated code (<Prefix>_GAME_CODE_OPT).
    # Og is the normal, quick-to-compile level; O2 measures whether heavy
    # spells run faster. Passed on every build so a comparison level never sticks.
    [ValidateSet('Og', 'O1', 'O2', 'O3')][string]$GameCodeOpt = 'Og',
    # Block timing is the default: scripts/block_timing.py rewrites the per-instruction game
    # code into its _block copy and the tree builds that. -PerInstruction
    # builds the per-instruction game code instead, for comparison only.
    # Switching either way recompiles all the game code once.
    [switch]$PerInstruction,
    # Pick (and if needed make) the game code for the
    # collection's ROM, record it in local\game_code_revision.txt, and stop.
    # build_dev.ps1 runs this before recording the build's inputs.
    [switch]$PrepareOnly,
    # The clean ROM to build from (build_dev.ps1 -BuildRom). Empty = the
    # collection's ROM.
    [string]$BuildRom = ''
)
$ErrorActionPreference = 'Stop'

# What differs between the games. Everything not named here is shared.
#  Root          game folder under the collection root (the CMake source dir)
#  BuildDir      default build tree, relative to Root
#  Prefix        CMake option prefix (<Prefix>_ENABLE_LTO, _LTO_JOBS, _GAME_CODE_OPT)
#  BusyCheck     refuse to start if another build's processes are running
#  CodeName      per-instruction game-code folder under Root/local
#  BlockParts    sub-folders block_timing.py rewrites ('' = the folder itself)
#  Revisions     the game's ROM revision list, relative to Root
#  LanguageFile  the launcher's language choice for the game, under local/
#  Targets       CMake targets (BrokenSeal adds GoldenSunGame and its player
#                program on a split build)
$games = @{
    BrokenSeal = @{
        Root = 'The Broken Seal'; BuildDir = 'build/gs011_opt'; Prefix = 'GSR'
        BusyCheck = $false; CodeName = 'gs011'; BlockParts = @('')
        Revisions = 'src/rom_revisions.inc'; LanguageFile = 'broken-seal-language.txt'
        Targets = @('GoldenSunRecomp')
    }
    LostAge = @{
        Root = 'The Lost Age'; BuildDir = 'build/tla'; Prefix = 'TLA'
        BusyCheck = $true; CodeName = 'tla'; BlockParts = @('main', 'reloc')
        Revisions = 'src/tla_rom_revisions.inc'; LanguageFile = 'lost-age-language.txt'
        Targets = @('TLARecomp', 'TLAGame', 'TLAPlayer')
        # Most memory one game-code file takes to compile: reloc/harv56 at
        # -Og, 1.27 GiB peak commit (measured 2026-10-23, the largest file
        # since the 17 October sharding; 6.4 GiB before it). Sets how many
        # compile at once (TLA_GAME_CODE_COMPILE_JOBS).
        GameCodeCompileGiB = 1.3
    }
}
$cfg = $games[$Game]
try {
    if (-not [Environment]::Is64BitProcess) { throw 'Use 64-bit PowerShell.' }
    if ($Game -ne 'BrokenSeal' -and $CompileJobs -gt 0) {
        throw '-CompileJobs is only supported for BrokenSeal.'
    }
    $python = if ($PythonExecutable) { (Get-Item -LiteralPath $PythonExecutable -ErrorAction Stop).FullName } else { (Get-Command python.exe -ErrorAction Stop).Source }
    $repo = Join-Path (Split-Path -Parent $PSScriptRoot) $cfg.Root
    if (-not $BuildDir) { $BuildDir = $cfg.BuildDir }
    $prefix = $cfg.Prefix
    Set-Location -LiteralPath $repo
    $buildDir = Join-Path $repo $BuildDir
    if (-not (Test-Path -LiteralPath (Join-Path $buildDir 'CMakeCache.txt'))) {
        throw "The $BuildDir configuration is missing. Set it up first."
    }
    if ($cfg.BusyCheck) {
        # Two builds at once corrupt each other's output.
        $busy = Get-Process -Name ninja, cc1plus, lto1, lto-wrapper -ErrorAction SilentlyContinue
        if ($busy) { throw 'Another build is running. Wait for it to finish.' }
    }
    $mingw = 'C:/msys64/mingw64/bin'
    if (-not (Test-Path -LiteralPath "$mingw/mingw32-make.exe")) {
        throw "Missing $mingw/mingw32-make.exe"
    }
    $env:PATH = "$mingw;$env:PATH"
    # GCC's parallel LTO runs its jobs through make; forward slashes are required.
    $env:MAKE = "$mingw/mingw32-make.exe"
    $cmake = (Get-Command cmake.exe -ErrorAction Stop).Source
    $jobs = [Math]::Max(1, [int][Math]::Floor([Environment]::ProcessorCount * $CpuPercent / 100.0))
    $ram = [uint64](Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory
    if ($ram -eq 0) { throw 'Could not read physical RAM.' }
    $memoryLimit = [uint64][Math]::Floor($ram * ($RamPercent / 100.0))

    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class GoldenSunBuildLimits {
    [StructLayout(LayoutKind.Sequential)] struct Basic {
        public long ProcessTime, JobTime;
        public uint Flags;
        public UIntPtr MinWorkingSet, MaxWorkingSet;
        public uint ActiveProcesses;
        public UIntPtr Affinity;
        public uint Priority, Scheduling;
    }
    [StructLayout(LayoutKind.Sequential)] struct IO {
        public ulong ReadOps, WriteOps, OtherOps, ReadBytes, WriteBytes, OtherBytes;
    }
    [StructLayout(LayoutKind.Sequential)] struct Extended {
        public Basic BasicInfo;
        public IO IoInfo;
        public UIntPtr ProcessMemory, JobMemory, PeakProcessMemory, PeakJobMemory;
    }
    [StructLayout(LayoutKind.Sequential)] struct Cpu {
        public uint Flags, Rate;
    }
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr CreateJobObject(IntPtr attributes, string name);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool SetInformationJobObject(IntPtr job, int kind, IntPtr info, uint size);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    static void Set<T>(IntPtr job, int kind, T value) where T : struct {
        int size = Marshal.SizeOf(typeof(T));
        IntPtr data = Marshal.AllocHGlobal(size);
        try {
            Marshal.StructureToPtr(value, data, false);
            if (!SetInformationJobObject(job, kind, data, (uint)size))
                throw new Win32Exception(Marshal.GetLastWin32Error());
        } finally { Marshal.FreeHGlobal(data); }
    }
    public static IntPtr Apply(uint cpuPercent, ulong memoryBytes, bool cpuCapInherited) {
        IntPtr job = CreateJobObject(IntPtr.Zero, null);
        if (job == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
        try {
            Extended limits = new Extended();
            limits.BasicInfo.Flags = 0x200; // JOB_OBJECT_LIMIT_JOB_MEMORY
            limits.JobMemory = new UIntPtr(memoryBytes);
            Set(job, 9, limits); // JobObjectExtendedLimitInformation
            // Nested CPU quotas multiply. The launcher's outer cap already
            // covers preparation, engine checks, CMake and every compiler.
            if (!cpuCapInherited)
                Set(job, 15, new Cpu { Flags = 0x1 | 0x4, Rate = cpuPercent * 100 });
            // Attach before starting CMake: all its descendants inherit caps.
            if (!AssignProcessToJobObject(job, GetCurrentProcess()))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            return job; // Keep open for this PowerShell process's lifetime.
        } catch { CloseHandle(job); throw; }
    }
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool QueryInformationJobObject(IntPtr job, int kind, IntPtr info, uint size, IntPtr returned);
    // Most committed memory the whole build (every compiler and linker
    // process together) used at once, in bytes.
    public static ulong PeakBytes(IntPtr job) {
        int size = Marshal.SizeOf(typeof(Extended));
        IntPtr data = Marshal.AllocHGlobal(size);
        try {
            if (!QueryInformationJobObject(job, 9, data, (uint)size, IntPtr.Zero)) return 0;
            Extended info = (Extended)Marshal.PtrToStructure(data, typeof(Extended));
            return info.PeakJobMemory.ToUInt64();
        } finally { Marshal.FreeHGlobal(data); }
    }
}
'@
    $jobHandle = [GoldenSunBuildLimits]::Apply([uint32]$CpuPercent, $memoryLimit, [bool]$CpuCapInherited)
    $kind = if ($PrepareOnly) { 'Preparing the game code' } else { "LTO build, game code -$GameCodeOpt" }
    if ($Game -eq 'LostAge' -and $PerInstruction) { $kind += ', per-instruction timing' }
    Write-Host "${kind}: CPU cap $CpuPercent%; parallel jobs $jobs."
    if ($Game -eq 'BrokenSeal') {
        Write-Host ('Build memory limit: {0:N1} GiB ({1}% of installed RAM, committed memory).' -f ($memoryLimit / 1GB), $RamPercent)
        Write-Host 'These are maximums, not usage targets. Other apps use additional memory.'
    } else {
        Write-Host ('Build memory limit: {0:N1} GiB ({1}% of installed RAM).' -f ($memoryLimit / 1GB), $RamPercent)
    }

    # Each game plays the collection's ROM for the language the flag under
    # its cartridge picks (local\<LanguageFile>, launcher collection_storage.h:
    # separate ROM files per language). Every revision in the game's list
    # (Revisions; the first is USA/Europe) has its own game code: a localized
    # one is made here from config\<code>, and made again when the ROM, its
    # inputs or gba_recompile change. Switching revision recompiles all the
    # game code once.
    $revisions = @(foreach ($line in Get-Content -LiteralPath (Join-Path $repo $cfg.Revisions)) {
        if ($line -match '^GSR_ROM_REVISION\("([a-z]{2})", "([^"]+)", "([0-9a-f]{40})"') {
            @{ Code = $Matches[1]; Name = $Matches[2]; Sha1 = $Matches[3] }
        }
    })
    if ($revisions.Count -eq 0) { throw "No ROM revisions read from $($cfg.Revisions)." }
    $collection = Split-Path -Parent $repo
    $languageFile = Join-Path $collection "local/$($cfg.LanguageFile)"
    $language = if (Test-Path -LiteralPath $languageFile -PathType Leaf) { "$(Get-Content -LiteralPath $languageFile -TotalCount 1)".Trim() } else { '' }
    $chosen = @($revisions | Where-Object { $_.Code -ceq $language })
    $romName = if ($chosen.Count -and $chosen[0] -ne $revisions[0]) { "$($cfg.Root) ($($chosen[0].Name))" } else { $cfg.Root }
    $rom = if ($BuildRom) { $BuildRom } else { Join-Path $collection "ROMs/$romName.gba" }
    if (-not (Test-Path -LiteralPath $rom -PathType Leaf)) { throw "The collection ROM is missing: $rom" }
    $romSha1 = (Get-FileHash -LiteralPath $rom -Algorithm SHA1).Hash.ToLowerInvariant()
    $revision = @($revisions | Where-Object { $_.Sha1 -eq $romSha1 })[0]
    if (-not $revision) { throw "The ROM (SHA-1 $romSha1) is not a supported $($cfg.Root) ROM. Build from a clean ROM." }
    $localized = $revision -ne $revisions[0]
    $corpus = if ($localized) { "$($revision.Code)/$($cfg.CodeName)" } else { $cfg.CodeName }
    Write-Host "ROM: $($revision.Name) (game code local\$corpus)."
    if ($localized) {
        $corpusDir = Join-Path $repo "local/$corpus"
        $configDir = Join-Path $repo "config/$($revision.Code)"
        # Each input as "<name> <SHA-1>", names relative to $Root (as each
        # game's stamps have always written them, so no game code is remade).
        function Get-InputLines([string]$Root, $Files, [switch]$ForwardSlashes) {
            foreach ($file in $Files | Sort-Object FullName) {
                $name = $file.FullName.Substring($Root.Length + 1)
                if ($ForwardSlashes) { $name = $name.Replace('\', '/') }
                "$name $((Get-FileHash -LiteralPath $file.FullName -Algorithm SHA1).Hash)"
            }
        }
        if ($Game -eq 'BrokenSeal') {
            # The player builder and its translator, as a release ships them
            # (make_release.ps1); quick when already current.
            & $cmake --build $buildDir --target gsr_builder gba_recompile
            $built = $LASTEXITCODE
            $recompiler = Join-Path $buildDir 'gba_recompile.exe'
            $inputs = @(Get-InputLines $configDir (Get-ChildItem -LiteralPath $configDir -Recurse -File) -ForwardSlashes)
        } else {
            & $cmake --build $buildDir --target gba_recompile
            $built = $LASTEXITCODE
            $recompiler = Join-Path $buildDir 'gbarecomp_build/gba_recompile.exe'
            # "harvest OFF": The Lost Age's RAM code always compiles while
            # playing, as in player releases (the line keeps existing stamps valid).
            $inputs = @("harvest OFF") + @(Get-InputLines $repo (
                Get-ChildItem -LiteralPath $configDir, (Join-Path $repo 'tools') -Recurse -File |
                    Where-Object { $_.Extension -in @('.toml', '.py') }))
        }
        if ($built -ne 0) { throw "Building the game-code translator failed ($built)." }
        if (-not (Test-Path -LiteralPath $recompiler -PathType Leaf)) { throw "gba_recompile.exe was not built: $recompiler" }
        $sourceText = ((@("rom $romSha1", "gba_recompile $((Get-FileHash -LiteralPath $recompiler -Algorithm SHA1).Hash)") + $inputs) -join "`n") + "`n"
        $stampFile = Join-Path $corpusDir 'generated-from.txt'
        $made = if (Test-Path -LiteralPath $stampFile -PathType Leaf) { [IO.File]::ReadAllText($stampFile) } else { '' }
        if ($made -cne $sourceText) {
            Write-Host "Making the $($revision.Name) game code from the ROM (local\$corpus)..."
            if ($Game -eq 'BrokenSeal') {
                $work = Join-Path (Split-Path -Parent $corpusDir) 'builder_work'
                & (Join-Path $buildDir 'gsr_builder.exe') --generate-only --keep-work --rom $rom `
                    --data (Join-Path $repo 'config') --work $work --recompiler $recompiler `
                    --engine $buildDir --toolchain (Split-Path -Parent $mingw) --jobs $jobs
                if ($LASTEXITCODE -ne 0) { throw "Making the $($revision.Name) game code failed ($LASTEXITCODE). Log: $work\build-log.txt" }
                # Replaced like the USA corpus is by its own regeneration.
                if (Test-Path -LiteralPath $corpusDir) { Remove-Item -LiteralPath $corpusDir -Recurse -Force }
                Move-Item -LiteralPath (Join-Path $work 'gen') -Destination $corpusDir
            } else {
                $generationArgs = @((Join-Path $repo 'tools/tla_recompile.py'), '--rom', $rom, '--recompile', $recompiler, '--out', $corpusDir, '--no-harvest')
                & $python @generationArgs
                if ($LASTEXITCODE -ne 0) { throw "Making the $($revision.Name) game code failed ($LASTEXITCODE)." }
            }
            [IO.File]::WriteAllText($stampFile, $sourceText)
        }
    }
    # Read by scripts/developer_build_stamp.py and the launcher
    # (collection_build_state.h): which game code this tree builds.
    $record = "rom_sha1=$romSha1`ncorpus=$corpus`n"
    $recordFile = Join-Path $repo 'local/game_code_revision.txt'
    $old = if (Test-Path -LiteralPath $recordFile -PathType Leaf) { [IO.File]::ReadAllText($recordFile) } else { '' }
    if ($old -cne $record) { [IO.File]::WriteAllText($recordFile, $record) }
    if ($PrepareOnly) { exit 0 }

    # Which game code the tree builds: the per-instruction copy
    # (local\<corpus>) or its block-timing copy (local\<corpus>_block),
    # refreshed here (only changed files are rewritten, so only those recompile).
    $codeName = if ($PerInstruction) { $corpus } else { "${corpus}_block" }
    if (-not $PerInstruction) {
        Write-Host "Writing the block-timing game code (local\$codeName)..."
        foreach ($part in $cfg.BlockParts) {
            $from = "local/$corpus"; $to = "local/$codeName"
            if ($part) { $from += "/$part"; $to += "/$part" }
            & $python (Join-Path $PSScriptRoot 'block_timing.py') $from $to
            $partLabel = if ($part) { " on $part" } else { '' }
            if ($LASTEXITCODE -ne 0) { throw "block_timing.py failed$partLabel ($LASTEXITCODE)." }
        }
    }
    $codeRoot = ($repo -replace '\\', '/') + "/local/$codeName"

    $configureArgs = @('-S', $repo, '-B', $buildDir, "-D${prefix}_ENABLE_LTO=ON")
    if ($Game -eq 'BrokenSeal') {
        # Every game-code folder setting in the tree's cache (GSR_GAME_CODE_ROOT,
        # GSR_GENERATED_DIR, the overlay, stamp, transient and synth folders) is
        # pointed at the chosen copy.
        $codeArgs = @()
        foreach ($line in Get-Content -LiteralPath (Join-Path $buildDir 'CMakeCache.txt')) {
            if ($line -match '^(GSR_[A-Z0-9_]+):PATH=.*/local/(?:[a-z]{2}/)?gs011(_block)?(/.*)?$') {
                $codeArgs += "-D$($Matches[1]):PATH=$codeRoot$($Matches[3])"
            }
        }
        if ($codeArgs.Count -eq 0) { throw 'No game-code folder settings in the tree cache.' }
        Write-Host "Game code: local\$codeName ($($codeArgs.Count) folder settings)."
        $configureArgs += @('-DGSR_DEBUG_SYMBOLS=ON', "-DGSR_LTO_JOBS=$jobs", "-DGSR_GAME_CODE_OPT=-$GameCodeOpt",
            "-DGSR_GAME_ROM_SHA1=$romSha1") + $codeArgs
    } else {
        # As many game-code files as fit the memory limit, at least one.
        $gameCodeJobs = [Math]::Max(1, [Math]::Min($jobs,
            [int][Math]::Floor($memoryLimit / ($cfg.GameCodeCompileGiB * 1GB))))
        Write-Host "Game-code files compiled at once: $gameCodeJobs."
        $configureArgs += @('-DTLA_SPLIT_GAME_CODE=ON', "-DTLA_LTO_JOBS=$jobs", "-DTLA_GAME_CODE_OPT=-$GameCodeOpt",
            "-DTLA_GAME_CODE_COMPILE_JOBS=$gameCodeJobs",
            "-DTLA_GENERATED_DIR:PATH=$codeRoot/main", "-DTLA_GAME_ROM_SHA1=$romSha1")
    }
    $configureOutput = & $cmake @configureArgs 2>&1
    $configureExit = $LASTEXITCODE
    $configureOutput | ForEach-Object { Write-Host $_ }
    if ($configureExit -ne 0) { throw "CMake configuration failed ($configureExit)." }
    # This project otherwise degrades gracefully to no LTO; manual LTO builds
    # should instead stop loudly if IPO support was not accepted.
    if (($configureOutput -join "`n") -match 'building without it') {
        throw 'This toolchain cannot enable LTO; build was not started.'
    }
    $targets = @($cfg.Targets)
    # A split Broken Seal build (GSR_SPLIT_GAME_CODE) also has the game code as
    # GoldenSunGame.dll and the engine as a plug-in run by the player program
    # GoldenSunRecomp.exe (target GoldenSunPlayer). (The Lost Age always splits.)
    if ($Game -eq 'BrokenSeal' -and
        (Select-String -LiteralPath (Join-Path $buildDir 'CMakeCache.txt') -SimpleMatch -Quiet 'GSR_SPLIT_GAME_CODE:BOOL=ON')) {
        $targets += 'GoldenSunGame', 'GoldenSunPlayer'
    }
    $compileJobs = if ($CompileJobs -gt 0) { [Math]::Min($CompileJobs, $jobs) } else { $jobs }
    if ($compileJobs -ne $jobs) { Write-Host "Compile jobs: $compileJobs." }
    $clock = [Diagnostics.Stopwatch]::StartNew()
    if ($LogFile) {
        # Compiler messages arrive on stderr. Windows PowerShell turns
        # redirected stderr lines into errors, which 'Stop' would treat as a
        # failure, so relax it for this call only and log them as text.
        $ErrorActionPreference = 'Continue'
        & $cmake --build $buildDir --target @targets --parallel $compileJobs 2>&1 |
            ForEach-Object { "$_" } | Tee-Object -FilePath $LogFile
        $buildExit = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
    } else {
        & $cmake --build $buildDir --target @targets --parallel $compileJobs
        $buildExit = $LASTEXITCODE
    }
    $clock.Stop()
    # Build cost, for comparing LTO against a player-sized build.
    $summary = '{0}: {1:N1} min, peak memory {2:N1} GiB, exit {3}' -f $kind,
        $clock.Elapsed.TotalMinutes, ([GoldenSunBuildLimits]::PeakBytes($jobHandle) / 1GB), $buildExit
    Write-Host $summary
    if ($LogFile) { Add-Content -LiteralPath $LogFile -Value $summary }
    if ($Game -eq 'LostAge' -and $buildExit -eq 0) {
        Write-Host 'Built the Lost Age engine and game library. Launch GoldenSunLauncher.exe from the collection root to play.'
    }
    exit $buildExit
} catch {
    Write-Host "Build stopped: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
