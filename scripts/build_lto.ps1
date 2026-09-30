# Used by build_lto.bat. Nothing runs until the user launches it.
# CPU is a Windows job hard cap; memory is a job-wide committed-memory cap
# sized from physical RAM, not a promise of a particular Task Manager reading.
# Other applications are outside these limits. Exceeding the memory budget
# can fail an allocation/build; it does not throttle allocations until space frees.
# References:
# https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_cpu_rate_control_information
# https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_extended_limit_information
[CmdletBinding()]
param(
    [ValidateRange(1, 100)][int]$CpuPercent = 90,
    [ValidateRange(1, 100)][int]$RamPercent = 90,
    # Relative to the repository. make_release.ps1 points this at the
    # release build tree; everything else uses the normal one.
    [string]$BuildDir = 'build/gs011_opt',
    # Compile jobs. 0 = the CPU-based count below. make_release.ps1 lowers
    # it for from-scratch builds, where many of the ~6 MB generated game-code
    # files compile at once and together outgrow the memory limit.
    [int]$CompileJobs = 0,
    # When set, the whole build output is also written here.
    [string]$LogFile = '',
    # Comparison builds only (build_nolto_test.bat): the same build with
    # link-time optimisation off, to measure what LTO buys in play.
    [switch]$NoLto,
    # build_lto_nosymbols.bat: the same LTO build without debug information.
    [switch]$NoSymbols,
    # Optimisation level for the game's translated code (GSR_GAME_CODE_OPT).
    # Og is the normal, quick-to-compile level; build_lto_o2.bat passes O2
    # to measure whether heavy spells (Ragnarok, Storm Ray) run faster.
    # Passed on every build so a comparison level never sticks.
    [ValidateSet('Og', 'O1', 'O2', 'O3')][string]$GameCodeOpt = 'Og',
    # Block timing is the default (Jimmy, 2026-09-30; ROADMAP.md "block
    # bookkeeping"): tools/block_timing.py rewrites local\gs011 into
    # local\gs011_block and the tree builds that. -PerInstruction builds the
    # per-instruction game code (local\gs011) instead, for comparison only.
    # Switching either way recompiles all the game code once.
    [switch]$PerInstruction
)
$ErrorActionPreference = 'Stop'
try {
    if (-not [Environment]::Is64BitProcess) { throw 'Use 64-bit PowerShell.' }
    $repo = Split-Path -Parent $PSScriptRoot
    Set-Location -LiteralPath $repo
    $buildDir = Join-Path $repo $BuildDir
    if (-not (Test-Path -LiteralPath (Join-Path $buildDir 'CMakeCache.txt'))) {
        throw "The $BuildDir configuration is missing. Set it up first."
    }
    $mingw = 'C:/msys64/mingw64/bin'
    if (-not (Test-Path -LiteralPath "$mingw/mingw32-make.exe")) {
        throw "Missing $mingw/mingw32-make.exe"
    }
    $env:PATH = "$mingw;$env:PATH"
    # Forward slashes are required by GCC's LTO wrapper.
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
    public static IntPtr Apply(uint cpuPercent, ulong memoryBytes) {
        IntPtr job = CreateJobObject(IntPtr.Zero, null);
        if (job == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
        try {
            Extended limits = new Extended();
            limits.BasicInfo.Flags = 0x200; // JOB_OBJECT_LIMIT_JOB_MEMORY
            limits.JobMemory = new UIntPtr(memoryBytes);
            Set(job, 9, limits); // JobObjectExtendedLimitInformation
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
    $jobHandle = [GoldenSunBuildLimits]::Apply([uint32]$CpuPercent, $memoryLimit)
    $kind = if ($NoLto) { 'No-LTO comparison build' } else { 'LTO build' }
    if ($NoSymbols) { $kind += ' without debug symbols' }
    $kind += ", game code -$GameCodeOpt"
    Write-Host "${kind}: CPU cap $CpuPercent%; parallel jobs $jobs."
    Write-Host ('Build memory limit: {0:N1} GiB ({1}% of installed RAM, committed memory).' -f ($memoryLimit / 1GB), $RamPercent)
    Write-Host 'These are maximums, not usage targets. Other apps use additional memory.'

    # Which game code the tree builds: every game-code folder setting in the
    # tree's cache (GSR_GAME_CODE_ROOT, GSR_GENERATED_DIR, the overlay,
    # stamp, transient and synth folders) is pointed at the chosen copy.
    $codeName = if ($PerInstruction) { 'gs011' } else { 'gs011_block' }
    if (-not $PerInstruction) {
        Write-Host 'Writing the block-timing game code (local\gs011_block)...'
        & python (Join-Path $repo 'tools/block_timing.py') 'local/gs011' 'local/gs011_block'
        if ($LASTEXITCODE -ne 0) { throw "block_timing.py failed ($LASTEXITCODE)." }
    }
    $codeRoot = ($repo -replace '\\', '/') + "/local/$codeName"
    $codeArgs = @()
    foreach ($line in Get-Content -LiteralPath (Join-Path $buildDir 'CMakeCache.txt')) {
        if ($line -match '^(GSR_[A-Z0-9_]+):PATH=.*/local/gs011(_block)?(/.*)?$') {
            $codeArgs += "-D$($Matches[1]):PATH=$codeRoot$($Matches[3])"
        }
    }
    if ($codeArgs.Count -eq 0) { throw 'No game-code folder settings in the tree cache.' }
    Write-Host "Game code: local\$codeName ($($codeArgs.Count) folder settings)."

    $ltoFlag = if ($NoLto) { '-DGSR_ENABLE_LTO=OFF' } else { '-DGSR_ENABLE_LTO=ON' }
    $symbolFlag = if ($NoSymbols) { '-DGSR_DEBUG_SYMBOLS=OFF' } else { '-DGSR_DEBUG_SYMBOLS=ON' }
    $configureOutput = & $cmake -S $repo -B $buildDir $ltoFlag $symbolFlag "-DGSR_LTO_JOBS=$jobs" "-DGSR_GAME_CODE_OPT=-$GameCodeOpt" @codeArgs 2>&1
    $configureExit = $LASTEXITCODE
    $configureOutput | ForEach-Object { Write-Host $_ }
    if ($configureExit -ne 0) { throw "CMake configuration failed ($configureExit)." }
    # This project otherwise degrades gracefully to no LTO; manual LTO builds
    # should instead stop loudly if IPO support was not accepted.
    if (-not $NoLto -and ($configureOutput -join "`n") -match 'building without it') {
        throw 'This toolchain cannot enable LTO; build was not started.'
    }
    # A split build (GSR_SPLIT_GAME_CODE) also has the game code as
    # GoldenSunGame.dll beside the exe.
    $targets = @('GoldenSunRecomp')
    if (Select-String -LiteralPath (Join-Path $buildDir 'CMakeCache.txt') -SimpleMatch -Quiet 'GSR_SPLIT_GAME_CODE:BOOL=ON') {
        $targets += 'GoldenSunGame'
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
    exit $buildExit
} catch {
    Write-Host "Build stopped: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
