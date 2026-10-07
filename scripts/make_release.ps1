# Used by make_release.bat. Nothing runs until the user launches it.
#
# Makes the player release: GSRecomp-Release\Golden Sun Recompiled, a clean
# folder with only what a player needs, and a zip of it. It holds no Golden
# Sun code: the player unzips it, starts GoldenSunLauncher.exe and picks
# their ROM, and the launcher builds the game code from that ROM once
# (builder\gsr_builder.exe, about 3 minutes) before it plays.
#
#   GoldenSunLauncher.exe      release launcher (Pick ROM, Quit, first-run build)
#   GoldenSunRecomp.exe        the engine: build\gs011_opt from build_lto.bat
#   SDL2.dll, lib*.dll         runtime libraries the two load
#   builder\gsr_builder.exe    ROM -> GoldenSunGame.dll
#   builder\gba_recompile.exe  the translator
#   builder\data\              main/transient/overlay configs, the build plan
#   builder\engine\            the two headers the game code includes and the
#                              engine's import library
#   builder\mingw64\           a trimmed MinGW-w64 GCC: only the files the
#                              game code's compile and link open
#   overlay_toolchain\include\ the three overlay shim headers the engine's
#                              runtime self-heal compile includes
#
# Not included: GoldenSunGame.dll, any generated code, the ROM, the BIOS,
# gssplash.jpg (official box art).
#
# With -Test (default) it then builds the game code from the ROM in
# config\local.json using ONLY the release folder's builder and toolchain,
# into a scratch folder, to prove the release is complete.
#
# Then (unless -NoLinux) the Linux release in WSL Ubuntu-24.04 through
# scripts/make_release_linux.sh: GSRecomp-Release\linux\Golden Sun Recompiled
# and GoldenSunRecompiled-linux-<date>.zip, tested the same way.
#
# -LauncherOnly builds only the player launchers, zipped as
# GSRecomp-Release\GoldenSunLauncher-windows-<version>.zip and
# GSRecomp-Release\linux\GoldenSunLauncher-linux-<version>.zip. Players drop
# the launcher over their own; it is not part of the game code's fingerprint,
# so nothing rebuilds. The engine is not built or touched.
[CmdletBinding()]
param(
    [switch]$NoTest,
    # The release's version: the tag you give its GitHub release (v0.2-test).
    # The launchers report it and tell players when GitHub has a different
    # one. Empty: "dev", and the launchers never check.
    [string]$Version = '',
    # Skip the Windows release (steps 1-7): Linux only. It then uses the
    # block-timing game code of the last Windows engine build.
    [switch]$NoWindows,
    # Skip the Linux release (step 8, built in WSL Ubuntu).
    [switch]$NoLinux,
    # Only the player launchers (see above).
    [switch]$LauncherOnly,
    # Limits for the engine build (step 1), passed on to build_lto.ps1.
    [ValidateRange(1, 100)][int]$CpuPercent = 90,
    [ValidateRange(1, 100)][int]$RamPercent = 90
)
$ErrorActionPreference = 'Stop'
try {
    if (-not [Environment]::Is64BitProcess) { throw 'Use 64-bit PowerShell.' }
    $repo = Split-Path -Parent $PSScriptRoot
    Set-Location -LiteralPath $repo

    $releaseRoot = Join-Path $repo 'GSRecomp-Release'
    $buildRel    = 'GSRecomp-Release/build'
    $buildDir    = Join-Path $repo $buildRel
    $outDir      = Join-Path $releaseRoot 'Golden Sun Recompiled'
    $mingwRoot   = 'C:/msys64/mingw64'
    $mingw       = "$mingwRoot/bin"
    $corpus      = Join-Path $repo 'local/gs011/main'
    $optDir      = Join-Path $repo 'build/gs011_opt'

    $busy = Get-Process -Name ninja, cc1plus, lto1, ld, collect2, mingw32-make `
        -ErrorAction SilentlyContinue
    if ($busy) { throw 'Another build is running. Wait for it to finish, then run this again.' }
    if ($NoWindows -and $NoLinux) { throw 'Nothing to make: both -NoWindows and -NoLinux were given.' }
    $releaseVersion = if ($Version.Trim()) { $Version.Trim() } else { 'dev' }
    if ($releaseVersion -notmatch '^[A-Za-z0-9._-]+$') { throw "Version '$releaseVersion' may only use letters, digits, '.', '-' and '_'." }
    if ($releaseVersion -eq 'dev') {
        Write-Host 'No version given: the launchers are built as "dev" and never check for updates.' -ForegroundColor Yellow
    } else {
        Write-Host "Version: $releaseVersion (use this as the tag of the GitHub release)."
    }
    if (-not $NoWindows) {
        foreach ($f in @("$mingw/c++.exe", "$mingw/objdump.exe")) {
            if (-not (Test-Path -LiteralPath $f)) { throw "Missing $f (MSYS2 MinGW-w64 toolchain)." }
        }
        $env:PATH = "$mingw;$env:PATH"
        $cmake = (Get-Command cmake.exe -ErrorAction Stop).Source

        if (-not $LauncherOnly) {
        # ---- 1. The engine: build_lto.bat's split build, current -------------
        $engineExe = Join-Path $optDir 'GoldenSunRecomp.exe'
        $engineApi = Join-Path $optDir 'libGoldenSunRecomp_api.a'
        foreach ($f in @($engineExe, $engineApi)) {
            if (-not (Test-Path -LiteralPath $f)) { throw "Missing $f. Run build_lto.bat first." }
        }
        if (-not (Select-String -LiteralPath (Join-Path $optDir 'CMakeCache.txt') -SimpleMatch -Quiet 'GSR_SPLIT_GAME_CODE:BOOL=ON')) {
            throw 'build\gs011_opt is not a split build. Run build_lto.bat first.'
        }
        # Bring it up to date through build_lto.ps1, exactly as build_lto.bat
        # does: it writes the block-timing game code, points the tree at it, sets
        # MAKE so the LTO link runs in parallel, applies the CPU and memory caps
        # and shows its progress. (A bare `cmake --build ... | Out-Null` here hid
        # a full engine rebuild with a serial LTO link and looked hung,
        # 2026-09-30.) Normally quick after build_lto.bat.
        Write-Host 'Checking the engine build is current (build_lto.ps1).'
        & (Join-Path $PSScriptRoot 'build_lto.ps1') -CpuPercent $CpuPercent -RamPercent $RamPercent
        if ($LASTEXITCODE -ne 0) { throw 'The engine build failed. See the build output above.' }
        }

        # ---- 2. Launcher, builder, translator: the release build tree --------
        # Configured like build\gs011_opt but without LTO (none of these three
        # needs it) and with the player launcher.
        New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
        # The bug report service (tools/report_service) stays out of the public
        # source: its address is read from local\report_host.txt (one line, not
        # in git) or $env:GSR_REPORT_HOST. Without it the launcher has no Send
        # report button, which is what a fork gets.
        $reportHost = $env:GSR_REPORT_HOST
        $reportHostFile = Join-Path $repo 'local\report_host.txt'
        if (-not $reportHost -and (Test-Path -LiteralPath $reportHostFile)) {
            $reportHost = (Get-Content -LiteralPath $reportHostFile -TotalCount 1).Trim()
        }
        if (-not $reportHost) {
            Write-Host 'No local\report_host.txt: the launcher is built without Send report.' -ForegroundColor Yellow
        }
        $configure = @(
            '-S', $repo, '-B', $buildDir, '-G', 'Ninja',
            '-DCMAKE_BUILD_TYPE=Release',
            "-DCMAKE_C_COMPILER=$mingw/cc.exe",
            "-DCMAKE_CXX_COMPILER=$mingw/c++.exe",
            "-DSDL2_INCLUDE_DIR=$mingwRoot/include/SDL2",
            "-DSDL2_LIBRARY=$mingwRoot/lib/libSDL2.dll.a",
            '-DGSR_BUILD_LOCAL_RUNNER=ON',
            "-DGSR_GENERATED_DIR=$($corpus -replace '\\', '/')",
            '-DGSR_ENABLE_LTO=OFF',
            '-DGSR_DEBUG_SYMBOLS=OFF',
            '-DGSR_RELEASE_LAUNCHER=ON',
            "-DGSR_RELEASE_VERSION=$releaseVersion",
            "-DGSR_REPORT_HOST=$reportHost"
        )
        & $cmake @configure | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed ($LASTEXITCODE)." }
        if ($LauncherOnly) {
            Write-Host 'Building the player launcher only.'
            & $cmake --build $buildDir --target GoldenSunLauncher
            if ($LASTEXITCODE -ne 0) { throw "Building the launcher failed ($LASTEXITCODE)." }
            $launcherZip = Join-Path $releaseRoot "GoldenSunLauncher-windows-$releaseVersion.zip"
            if (Test-Path -LiteralPath $launcherZip) { Remove-Item -LiteralPath $launcherZip -Force }
            Compress-Archive -LiteralPath (Join-Path $buildDir 'GoldenSunLauncher.exe') -DestinationPath $launcherZip
            Write-Host "Launcher ready: $launcherZip" -ForegroundColor Green
        } else {
        Write-Host 'Building the player launcher, the builder and the translator.'
        & $cmake --build $buildDir --target GoldenSunLauncher gsr_builder gba_recompile
        if ($LASTEXITCODE -ne 0) { throw "Building the release tools failed ($LASTEXITCODE)." }
        $launcher   = Join-Path $buildDir 'GoldenSunLauncher.exe'
        $builderExe = Join-Path $buildDir 'gsr_builder.exe'
        $recompiler = Get-ChildItem -LiteralPath $buildDir -Recurse -Filter 'gba_recompile.exe' |
            Select-Object -First 1 -ExpandProperty FullName
        foreach ($f in @($launcher, $builderExe, $recompiler)) {
            if (-not $f -or -not (Test-Path -LiteralPath $f)) { throw "Build output missing: $f" }
        }

        # ---- 3. A clean release folder -----------------------------------------
        if (Test-Path -LiteralPath $outDir) { Remove-Item -LiteralPath $outDir -Recurse -Force }
        $builderDir = Join-Path $outDir 'builder'
        $dataDir    = Join-Path $builderDir 'data'
        $engineDir  = Join-Path $builderDir 'engine'
        # GCC finds its Windows headers at bin/../../mingw64/include: the folder
        # must be called mingw64.
        $tcDir      = Join-Path $builderDir 'mingw64'
        foreach ($d in @($outDir, $builderDir, $dataDir, (Join-Path $dataDir 'overlays'),
                         (Join-Path $engineDir 'include'), $tcDir)) {
            New-Item -ItemType Directory -Force -Path $d | Out-Null
        }
        Copy-Item -LiteralPath $launcher  -Destination $outDir
        Copy-Item -LiteralPath $engineExe -Destination $outDir
        foreach ($dll in @('SDL2.dll', 'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll')) {
            Copy-Item -LiteralPath (Join-Path $mingw $dll) -Destination $outDir
        }
        Copy-Item -LiteralPath $builderExe -Destination $builderDir
        Copy-Item -LiteralPath $recompiler -Destination $builderDir

        # Data: the build plan and every config it names.
        $usa = Join-Path $repo 'config/usa'
        $plan = Join-Path $usa 'game_code_plan.txt'
        Copy-Item -LiteralPath $plan -Destination $dataDir
        Copy-Item -LiteralPath (Join-Path $usa 'main.toml') -Destination $dataDir
        Copy-Item -LiteralPath (Join-Path $usa 'overlay-registry.inc') -Destination $dataDir
        foreach ($line in Get-Content -LiteralPath $plan) {
            $w = ($line -replace '#.*$', '').Trim() -split '\s+'
            if ($w[0] -eq 'transient') { Copy-Item -LiteralPath (Join-Path $usa $w[2]) -Destination $dataDir }
        }
        Get-ChildItem -LiteralPath (Join-Path $usa 'overlays') -Filter '*.toml' |
            Copy-Item -Destination (Join-Path $dataDir 'overlays')

        # Engine: the headers generated code includes, and the import library
        # built with the shipped engine.
        foreach ($h in @('runtime_arm.h', 'runtime_arm_types.h')) {
            Copy-Item -LiteralPath (Join-Path $repo "gbarecomp/src/armv4t/$h") -Destination (Join-Path $engineDir 'include')
        }
        Copy-Item -LiteralPath $engineApi -Destination $engineDir

        # Self-heal: the overlay shim headers the engine's runtime compile includes.
        $ovInc = Join-Path $outDir 'overlay_toolchain/include'
        New-Item -ItemType Directory -Force -Path $ovInc | Out-Null
        foreach ($h in @('src/runtime/overlay_runtime_arm.h', 'src/runtime/overlay_abi.h',
                         'src/armv4t/runtime_arm_types.h')) {
            $src = Join-Path $repo "gbarecomp/$h"
            if (-not (Test-Path -LiteralPath $src)) { throw "Overlay header missing: $src" }
            Copy-Item -LiteralPath $src -Destination $ovInc
        }

        # ---- 4. Trimmed GCC -----------------------------------------------------
        Write-Host 'Assembling the compiler.'
        $gccVersion = (& "$mingw/g++.exe" -dumpfullversion).Trim()
        $target = (& "$mingw/g++.exe" -dumpmachine).Trim()
        $gccLib = "lib/gcc/$target/$gccVersion"
        $copied = New-Object 'System.Collections.Generic.HashSet[string]'
        function Copy-Tc([string]$rel) {
            $src = Join-Path $mingwRoot $rel
            if (-not (Test-Path -LiteralPath $src)) { throw "Toolchain file missing: $src" }
            $dst = Join-Path $tcDir $rel
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
            Copy-Item -LiteralPath $src -Destination $dst -Force
            [void]$copied.Add($rel)
        }
        $programs = @('bin/g++.exe', "$gccLib/cc1plus.exe", "$gccLib/collect2.exe",
                      "$gccLib/lto-wrapper.exe", "$gccLib/liblto_plugin.dll",
                      "$target/bin/as.exe", "$target/bin/ld.exe")
        $libs = @("$gccLib/crtbegin.o", "$gccLib/crtend.o", "$gccLib/libgcc.a", "$gccLib/libgcc_eh.a",
                  'lib/dllcrt2.o', 'lib/crt2.o', 'lib/libadvapi32.a', 'lib/libgcc_s.a',
                  'lib/libkernel32.a', 'lib/libmingw32.a', 'lib/libmingwex.a', 'lib/libmsvcrt.a',
                  'lib/libmoldname.a', 'lib/libpthread.dll.a', 'lib/libpthread.a', 'lib/libshell32.a',
                  'lib/libstdc++.dll.a', 'lib/libuser32.a', 'lib/bfd-plugins/liblto_plugin.dll')
        foreach ($f in $programs + $libs) { Copy-Tc $f }
        # Every MinGW DLL those programs load, recursively.
        $queue = New-Object System.Collections.Queue
        foreach ($p in $programs) { $queue.Enqueue((Join-Path $mingwRoot $p)) }
        while ($queue.Count -gt 0) {
            $file = $queue.Dequeue()
            $names = & "$mingw/objdump.exe" -p $file | Select-String 'DLL Name: (.+)$' |
                ForEach-Object { $_.Matches[0].Groups[1].Value.Trim() }
            foreach ($n in $names) {
                $rel = "bin/$n"
                if ((Test-Path -LiteralPath (Join-Path $mingwRoot $rel)) -and -not $copied.Contains($rel)) {
                    Copy-Tc $rel
                    $queue.Enqueue((Join-Path $mingwRoot $rel))
                }
            }
        }
        # Headers: exactly the system headers a compile of each kind of generated
        # file opens, with the flags the builder uses.
        $sample = @('main/recompiled_000.cpp', 'main/dispatch_table.cpp', 'main/symbol_map.cpp',
                    'stamps/stamp_registry.cpp') | ForEach-Object { Join-Path (Join-Path $repo 'local/gs011') $_ }
        $engineInc = Join-Path $engineDir 'include'
        $headers = New-Object 'System.Collections.Generic.HashSet[string]'
        foreach ($src in $sample) {
            if (-not (Test-Path -LiteralPath $src)) { continue }
            $deps = & "$mingw/g++.exe" -O2 -g -DNDEBUG '-std=c++20' -Og -g0 '-DGBARECOMP_OUTLINE_BUS=1' `
                "-I$(Split-Path -Parent $src)" "-I$engineInc" -M $src
            if ($LASTEXITCODE -ne 0) { throw "Could not list the headers of $src" }
            foreach ($tok in (($deps -join ' ') -replace '\\\s', ' ' -split '\s+')) {
                $t = $tok -replace '\\', '/'
                $prefix = "$mingwRoot/"
                if ($t.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
                    [void]$headers.Add($t.Substring($prefix.Length))
                } elseif ($t -match '^C:/msys64/mingw64/bin/\.\./(.+)$') {
                    [void]$headers.Add($Matches[1])
                }
            }
        }
        foreach ($h in $headers) {
            $norm = [IO.Path]::GetFullPath((Join-Path $mingwRoot $h)).Substring(([IO.Path]::GetFullPath($mingwRoot)).Length + 1)
            Copy-Tc ($norm -replace '\\', '/')
        }

        # ---- 5. Notes for players and third-party notices ------------------------
        $packages = & 'C:/msys64/usr/bin/pacman.exe' -Q mingw-w64-x86_64-gcc mingw-w64-x86_64-binutils `
            mingw-w64-x86_64-crt mingw-w64-x86_64-headers mingw-w64-x86_64-gmp mingw-w64-x86_64-mpfr `
            mingw-w64-x86_64-mpc mingw-w64-x86_64-isl mingw-w64-x86_64-zlib mingw-w64-x86_64-zstd `
            mingw-w64-x86_64-libiconv mingw-w64-x86_64-gettext-runtime mingw-w64-x86_64-libwinpthread `
            mingw-w64-x86_64-SDL2 2>$null
        @"
Golden Sun Recompiled
=====================

1. Start GoldenSunLauncher.exe.
2. Click "Pick ROM" and choose your own Golden Sun (USA, Europe) ROM.
3. The first time, the game is prepared from your ROM. This takes a few
   minutes and happens only once. Then the game starts.

After that, "Pick ROM" starts the game straight away.

This download contains no Golden Sun code or data. Everything from the game
comes from your ROM, on your PC.

Settings: press F1 in the game.
"@ | Set-Content -LiteralPath (Join-Path $outDir 'README.txt') -Encoding ascii

        @"
Third-party software in this folder
===================================

SDL2 (SDL2.dll): zlib license. https://www.libsdl.org/

The builder\mingw64 folder and the lib*.dll files beside the game are
unmodified binaries from the MSYS2 MinGW-w64 packages listed below. GCC and
binutils are under the GNU GPL version 3 (the GCC runtime libraries under the
GCC Runtime Library Exception); GMP, MPFR and MPC under the GNU LGPL; ISL
under the MIT license; zlib under the zlib license; zstd under the BSD
license; libiconv and gettext-runtime under the GNU LGPL; the MinGW-w64
runtime and headers under the licenses in their packages. The source code of
each exact package version is available from
https://repo.msys2.org/mingw/sources/ (and from us on request).

Package versions:
$($packages -join "`r`n")
"@ | Set-Content -LiteralPath (Join-Path $outDir 'THIRD_PARTY_NOTICES.txt') -Encoding ascii

        # ---- 6. Checks -------------------------------------------------------------
        foreach ($forbidden in @('GoldenSunGame.dll', 'gssplash.jpg', '*.gba', '*.sav', '*.state*', '*.ini', 'recompiled*.cpp', 'bios*.bin')) {
            $hit = Get-ChildItem -LiteralPath $outDir -Recurse -Filter $forbidden -ErrorAction SilentlyContinue
            if ($hit) { throw "The release folder must not contain $forbidden ($($hit[0].FullName))." }
        }
        $size = (Get-ChildItem -LiteralPath $outDir -Recurse -File | Measure-Object Length -Sum).Sum
        Write-Host ('Release folder: {0:N0} MB' -f ($size / 1MB))

        if (-not $NoTest) {
            $local = Get-Content -Raw -LiteralPath (Join-Path $repo 'config/local.json') | ConvertFrom-Json
            $testDir = Join-Path $releaseRoot 'release-test'
            if (Test-Path -LiteralPath $testDir) { Remove-Item -LiteralPath $testDir -Recurse -Force }
            New-Item -ItemType Directory -Force -Path $testDir | Out-Null
            Write-Host 'Testing: building the game code with only the release folder''s tools.'
            $clock = [Diagnostics.Stopwatch]::StartNew()
            & (Join-Path $builderDir 'gsr_builder.exe') --rom $local.rom --out $testDir --work (Join-Path $testDir 'work') |
                Where-Object { $_ -match '^@(stage|error|done)' } | ForEach-Object { Write-Host "  $_" }
            $ok = $LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath (Join-Path $testDir 'GoldenSunGame.dll'))
            Write-Host ('  {0:N1} min' -f $clock.Elapsed.TotalMinutes)
            if (-not $ok) { throw "The release test build failed. Log: $(Join-Path $testDir 'work\build-log.txt')" }
            Remove-Item -LiteralPath $testDir -Recurse -Force
            Write-Host '  The release builds the game from the ROM on its own.' -ForegroundColor Green
        }

        # ---- 7. Zip ------------------------------------------------------------------
        $zip = Join-Path $releaseRoot ('GoldenSunRecompiled-{0}.zip' -f (Get-Date -Format 'yyyy-MM-dd'))
        if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
        Compress-Archive -Path $outDir -DestinationPath $zip -CompressionLevel Optimal
        Write-Host ''
        Write-Host "Release ready: $outDir" -ForegroundColor Green
        Write-Host ('Zip for GitHub: {0} ({1:N0} MB)' -f $zip, ((Get-Item -LiteralPath $zip).Length / 1MB)) -ForegroundColor Green
        }
    }

    # ---- 8. Linux release (WSL Ubuntu) ------------------------------------------
    # scripts/make_release_linux.sh inside WSL: the Linux engine, launcher,
    # builder and a trimmed Linux GCC, into GSRecomp-Release\linux (a folder
    # and a zip made on Linux, which keeps the programs' executable bit). It
    # uses the block-timing game code step 1 just brought up to date.
    if (-not $NoLinux) {
        $distro = 'Ubuntu-24.04'
        $wsl = Join-Path $env:SystemRoot 'System32\wsl.exe'
        $have = (Test-Path -LiteralPath $wsl) -and
            ((& $wsl --list --quiet 2>$null) -replace "`0", '' | Where-Object { $_.Trim() -eq $distro })
        if (-not $have -and $NoWindows) {
            throw "The Linux release needs WSL $distro, which is not installed."
        } elseif (-not $have) {
            Write-Host "Linux release skipped: WSL $distro is not installed." -ForegroundColor Yellow
        } else {
            Write-Host ''
            Write-Host "Linux release (WSL $distro)."
            $linuxArgs = @('scripts/make_release_linux.sh', '--version', $releaseVersion)
            if ($LauncherOnly) {
                $linuxArgs += '--launcher-only'
            } elseif ($NoTest) {
                $linuxArgs += '--no-test'
            } else {
                # The ROM path goes over as GSR_ROM through WSLENV: Windows
                # PowerShell would split it at its spaces as an argument.
                $local = Get-Content -Raw -LiteralPath (Join-Path $repo 'config/local.json') | ConvertFrom-Json
                $romFull = [IO.Path]::GetFullPath($local.rom)
                $env:GSR_ROM = '/mnt/' + $romFull.Substring(0, 1).ToLower() +
                    ($romFull.Substring(2) -replace '\\', '/')
                $env:WSLENV = (@($env:WSLENV, 'GSR_ROM') | Where-Object { $_ }) -join ':'
            }
            & $wsl -d $distro -u root --cd $repo --exec bash @linuxArgs
            if ($LASTEXITCODE -ne 0) {
                Write-Host "The Linux release failed (the Windows release above is complete)." -ForegroundColor Red
                exit 1
            }
            Write-Host "Linux release ready: $(Join-Path $releaseRoot 'linux')" -ForegroundColor Green
        }
    }
    exit 0
} catch {
    Write-Host "Release stopped: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
