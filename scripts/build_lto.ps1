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
    [ValidateRange(1, 100)][int]$RamPercent = 90
)
$ErrorActionPreference = 'Stop'
try {
    if (-not [Environment]::Is64BitProcess) { throw 'Use 64-bit PowerShell.' }
    $repo = Split-Path -Parent $PSScriptRoot
    Set-Location -LiteralPath $repo
    $buildDir = Join-Path $repo 'build/gs011_opt'
    if (-not (Test-Path -LiteralPath (Join-Path $buildDir 'CMakeCache.txt'))) {
        throw 'The normal build/gs011_opt configuration is missing. Set it up first.'
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
}
'@
    $jobHandle = [GoldenSunBuildLimits]::Apply([uint32]$CpuPercent, $memoryLimit)
    Write-Host "LTO build: CPU cap $CpuPercent%; parallel jobs $jobs."
    Write-Host ('Build memory limit: {0:N1} GiB ({1}% of installed RAM, committed memory).' -f ($memoryLimit / 1GB), $RamPercent)
    Write-Host 'These are maximums, not usage targets. Other apps use additional memory.'

    $configureOutput = & $cmake -S $repo -B $buildDir '-DGSR_ENABLE_LTO=ON' "-DGSR_LTO_JOBS=$jobs" 2>&1
    $configureExit = $LASTEXITCODE
    $configureOutput | ForEach-Object { Write-Host $_ }
    if ($configureExit -ne 0) { throw "CMake configuration failed ($configureExit)." }
    # This project otherwise degrades gracefully to no LTO; manual LTO builds
    # should instead stop loudly if IPO support was not accepted.
    if (($configureOutput -join "`n") -match 'building without it') {
        throw 'This toolchain cannot enable LTO; build was not started.'
    }
    & $cmake --build $buildDir --target GoldenSunRecomp --parallel $jobs
    exit $LASTEXITCODE
} catch {
    Write-Host "Build stopped: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
