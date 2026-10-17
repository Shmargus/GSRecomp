# Builds the combined player release. No build runs until the user starts it.
# Root GoldenSunLauncher handles both games; each game's engine and
# ROM-only builder lives under games/The Broken Seal or games/The Lost Age.
# -LauncherOnly updates the root launcher, its artwork and runtime dependencies.
# No ROM, BIOS, generated game code or game library is shipped.
[CmdletBinding()]
param(
    [switch]$NoTest,
    # Inspect the relocated layout without configuring, building or writing.
    [switch]$CheckOnly,
    # The release's version: the tag you give its GitHub release (v0.2-test).
    # The launcher reports it and tells players when GitHub has a different
    # one. Empty: "dev", and the launcher never checks.
    [string]$Version = '',
    # One release feed for the collection and both games.
    [string]$Repository = 'Shmargus/GSRecomp',
    # Skip the Windows release (steps 1-7): Linux only. It then uses the
    # block-timing game code of the last Windows engine build.
    [switch]$NoWindows,
    # Skip the Linux release (step 8, built in WSL Ubuntu).
    [switch]$NoLinux,
    # Only the collection launcher (see above).
    [switch]$LauncherOnly,
    # Include Ashley's F1 sticker and dedication in this full release only.
    [switch]$AshleyEdition,
    # Limits for the engine build (step 1), passed on to build_game.ps1.
    [ValidateRange(1, 100)][int]$CpuPercent = 90,
    [ValidateRange(1, 100)][int]$RamPercent = 90
)
$ErrorActionPreference = 'Stop'
$buildMutex = $null
$ownsMutex = $false
try {
    if (-not [Environment]::Is64BitProcess) { throw 'Use 64-bit PowerShell.' }
    $collectionRoot = Split-Path -Parent $PSScriptRoot
    $repo = Join-Path $collectionRoot 'The Broken Seal'
    Set-Location -LiteralPath $repo

    $tlaRepo = Join-Path $collectionRoot 'The Lost Age'
    # The shared engine lives at the collection root, beside both games.
    $engineRoot = Join-Path $collectionRoot 'engine'
    $engineCMake = $engineRoot -replace '\\', '/'
    if (-not (Test-Path -LiteralPath (Join-Path $tlaRepo 'CMakeLists.txt'))) {
        throw "The Lost Age folder is missing beside The Broken Seal."
    }
    $releaseRoot = Join-Path $collectionRoot 'Releases'
    $buildRel    = 'build/release-collection'
    $buildDir    = Join-Path $repo $buildRel
    $outDir      = Join-Path $releaseRoot 'Golden Sun Recompiled'
    $gameOut     = Join-Path $outDir 'games/The Broken Seal'
    $tlaOut      = Join-Path $outDir 'games/The Lost Age'
    $tlaBuild    = Join-Path $tlaRepo 'build/release'
    function Get-FreshCacheOption([string]$directory) {
        $cachePath = Join-Path $directory 'CMakeCache.txt'
        if (-not (Test-Path -LiteralPath $cachePath)) { return }
        $savedDirectory = (Get-Content -LiteralPath $cachePath |
            Where-Object { $_ -match '^CMAKE_CACHEFILE_DIR:INTERNAL=' } |
            Select-Object -First 1) -replace '^[^=]+=', ''
        if ($savedDirectory -and [IO.Path]::GetFullPath($savedDirectory).TrimEnd('\', '/') -ine
            [IO.Path]::GetFullPath($directory).TrimEnd('\', '/')) {
            # Keep a moved configuration recoverable; generated metadata will
            # be replaced only when a release build is started.
            Copy-Item -LiteralPath $cachePath -Destination ($cachePath + '.before-root-move') -Force
            return '--fresh'
        }
    }
    function Get-ExistingDependencyOptions {
        foreach ($dependency in @(@('IMGUI', 'imgui'), @('TOMLPLUSPLUS', 'tomlplusplus'))) {
            foreach ($directory in @($buildDir, (Join-Path $repo 'build/gs011_opt'))) {
                $source = Join-Path $directory ('_deps/' + $dependency[1] + '-src')
                if (Test-Path -LiteralPath $source -PathType Container) {
                    '-DFETCHCONTENT_SOURCE_DIR_' + $dependency[0] + '=' + ($source -replace '\\', '/')
                    break
                }
            }
        }
    }
    # Only delete resolved directories within this release's workspace.
    function Remove-ReleaseTree([string]$path) {
        $full = [IO.Path]::GetFullPath($path)
        $allowed = [IO.Path]::GetFullPath($releaseRoot).TrimEnd('\') + '\'
        if (-not $full.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to remove a directory outside the release root: $full"
        }
        if (Test-Path -LiteralPath $full) { Remove-Item -LiteralPath $full -Recurse -Force }
    }
    function Get-ReleaseTestRom([string]$gameRepo) {
        $localFile = Join-Path $gameRepo 'config/local.json'
        if (Test-Path -LiteralPath $localFile) {
            $settings = Get-Content -Raw -LiteralPath $localFile | ConvertFrom-Json
            if ($settings.rom) {
                $candidate = [string]$settings.rom
                if (-not [IO.Path]::IsPathRooted($candidate)) { $candidate = Join-Path $gameRepo $candidate }
                if (Test-Path -LiteralPath $candidate) { return [IO.Path]::GetFullPath($candidate) }
            }
        }
        # A moved checkout may still have the old absolute path in local.json.
        $roms = @(Get-ChildItem -LiteralPath $gameRepo -File -Filter '*.gba')
        if ($roms.Count -eq 1) { return $roms[0].FullName }
        throw "Set a valid ROM path in $localFile to test this game."
    }
    # File-name checks cannot see ROM bytes compiled into a program; this reads
    # every shipped file's content against both ROMs. The unmodified GCC
    # folders (mingw64, toolchain) are third-party and left out. On a match the
    # given zip is removed so nothing with ROM bytes is left to upload.
    function Assert-NoRomBytes([string]$folder, [string]$zip = '') {
        Write-Host "Checking $folder for ROM bytes."
        & $python (Join-Path $PSScriptRoot 'release_rom_scan.py') `
            --rom (Get-ReleaseTestRom $repo) --rom (Get-ReleaseTestRom $tlaRepo) `
            --skip mingw64 --skip toolchain $folder
        if ($LASTEXITCODE -ne 0) {
            if ($zip -and (Test-Path -LiteralPath $zip)) { Remove-Item -LiteralPath $zip -Force }
            if ($LASTEXITCODE -eq 3) { throw "ROM bytes found in $folder; the release must not ship them." }
            throw "The ROM scan could not run ($python, exit $LASTEXITCODE); see the error above."
        }
    }
    function Copy-CollectionFiles([string]$destination) {
        New-Item -ItemType Directory -Force -Path (Join-Path $destination 'assets') | Out-Null
        Copy-Item -LiteralPath (Join-Path $buildDir 'GoldenSunCollection.exe') -Destination (Join-Path $destination 'GoldenSunLauncher.exe')
        foreach ($asset in @('launcher_logo.png', 'cartridge_broken_seal.png', 'cartridge_lost_age.png')) {
            Copy-Item -LiteralPath (Join-Path $collectionRoot "assets/$asset") -Destination (Join-Path $destination 'assets')
        }
        foreach ($dll in @('SDL2.dll', 'zlib1.dll', 'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll')) {
            Copy-Item -LiteralPath (Join-Path $mingw $dll) -Destination $destination
        }
    }
    $mingwRoot   = 'C:/msys64/mingw64'
    $mingw       = "$mingwRoot/bin"
    $corpus      = Join-Path $repo 'local/gs011/main'
    $optDir      = Join-Path $repo 'build/gs011_opt'

    $busy = Get-Process -Name ninja, cc1plus, lto1, ld, collect2, mingw32-make `
        -ErrorAction SilentlyContinue
    if ($busy) { throw 'Another build is running. Wait for it to finish, then run this again.' }
    if ($NoWindows -and $NoLinux) { throw 'Nothing to make: both -NoWindows and -NoLinux were given.' }
    if ($AshleyEdition -and $LauncherOnly) { throw 'Ashley Edition needs a full release; turn off Launcher only.' }
    $releaseVersion = if ($Version.Trim()) { $Version.Trim() } else { 'dev' }
    if ($Repository -notmatch '^[A-Za-z0-9-]+/[A-Za-z0-9_.-]+$') {
        throw 'Repository must be a GitHub owner/repository name.'
    }
    if ($releaseVersion -notmatch '^[A-Za-z0-9._-]+$') { throw "Version '$releaseVersion' may only use letters, digits, '.', '-' and '_'." }
    if ($CheckOnly) {
        foreach ($path in @((Join-Path $repo 'CMakeLists.txt'), (Join-Path $tlaRepo 'CMakeLists.txt'),
                            (Join-Path $PSScriptRoot 'build_game.ps1'),
                            (Join-Path $PSScriptRoot 'make_release_linux.sh'),
                            (Join-Path $collectionRoot 'assets/launcher_logo.png'),
                            (Join-Path $collectionRoot 'assets/cartridge_broken_seal.png'),
                            (Join-Path $collectionRoot 'assets/cartridge_lost_age.png'))) {
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required file is missing: $path" }
        }
        Write-Host "The Broken Seal source: $repo"
        Write-Host "The Lost Age source: $tlaRepo"
        Write-Host "Broken Seal release tools: $buildDir"
        Write-Host "Lost Age release tools: $tlaBuild"
        Write-Host "Combined packages: $releaseRoot"
        Write-Host 'Layout validation passed. No configuration, files or builds changed.'
        exit 0
    }
    $buildMutex = New-Object Threading.Mutex($false, 'Local\GoldenSunCollectionBuild')
    try { $ownsMutex = $buildMutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $ownsMutex = $true }
    if (-not $ownsMutex) { throw 'Another collection build or release is running.' }
    if ($releaseVersion -eq 'dev') {
        Write-Host 'No version given: the launcher is built as "dev" and never checks for updates.' -ForegroundColor Yellow
    } else {
        Write-Host "Version: $releaseVersion (use this as the tag of the GitHub release)."
    }
    # The ROM scan needs numpy; MSYS2's python, put first on PATH below, lacks it.
    $python = (Get-Command python.exe -ErrorAction Stop).Source
    if (-not $NoWindows) {
        foreach ($f in @("$mingw/c++.exe", "$mingw/objdump.exe")) {
            if (-not (Test-Path -LiteralPath $f)) { throw "Missing $f (MSYS2 MinGW-w64 toolchain)." }
        }
        $env:PATH = "$mingw;$env:PATH"
        $cmake = (Get-Command cmake.exe -ErrorAction Stop).Source

        if (-not $LauncherOnly) {
        # ---- 1. The engine: the split build, current --------------------------
        $engineExe = Join-Path $optDir 'GoldenSunRecomp.exe'
        $engineDll = Join-Path $optDir 'GoldenSunEngine.dll'
        $engineApi = Join-Path $optDir 'libGoldenSunRecomp_api.a'
        foreach ($f in @($engineExe, $engineDll, $engineApi)) {
            if (-not (Test-Path -LiteralPath $f)) { throw "Missing $f. Run build_dev.bat -Game BrokenSeal first." }
        }
        if (-not (Select-String -LiteralPath (Join-Path $optDir 'CMakeCache.txt') -SimpleMatch -Quiet 'GSR_SPLIT_GAME_CODE:BOOL=ON')) {
            throw 'build\gs011_opt is not a split build. Run build_dev.bat -Game BrokenSeal first.'
        }
        $engineBuildRel = 'build/gs011_opt'
        $engineCache = Get-Content -LiteralPath (Join-Path $optDir 'CMakeCache.txt')
        $savedHome = ($engineCache | Where-Object { $_ -match '^CMAKE_HOME_DIRECTORY:INTERNAL=' }) -replace '^CMAKE_HOME_DIRECTORY:INTERNAL=', ''
        $currentHome = $repo -replace '\\', '/'
        if ($savedHome.TrimEnd('/') -ne $currentHome.TrimEnd('/')) {
            # The checkout moved into The Broken Seal. Keep the existing build
            # untouched and recreate its scalar configuration in a release tree.
            $engineBuildRel = 'build/release-engine'
            $optDir = Join-Path $repo $engineBuildRel
            $seed = @('-S', $repo, '-B', $optDir, '-G', 'Ninja')
            foreach ($line in $engineCache) {
                if ($line -match '^((?:GSR_|SDL2_|GBARECOMP_|FETCHCONTENT_SOURCE_DIR_)[A-Za-z0-9_]*|CMAKE_(?:C_COMPILER|CXX_COMPILER|BUILD_TYPE|C_FLAGS[A-Za-z0-9_]*|CXX_FLAGS[A-Za-z0-9_]*)):(BOOL|STRING|PATH|FILEPATH)=(.*)$') {
                    $key = $Matches[1]; $kind = $Matches[2]; $value = $Matches[3]
                    if ($savedHome -and $value.StartsWith($savedHome, [StringComparison]::OrdinalIgnoreCase)) {
                        $value = $currentHome + $value.Substring($savedHome.Length)
                    }
                    $seed += "-D${key}:${kind}=$value"
                }
            }
            $seed += "-DGBARECOMP_ROOT:PATH=$engineCMake"
            $seed += '-DGSR_RELEASE_LAUNCHER=ON'
            $seed += @(Get-FreshCacheOption $optDir)
            $seed += @(Get-ExistingDependencyOptions)
            Write-Host 'The Broken Seal folder moved: preparing a separate release engine build.'
            & $cmake @seed | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "The Broken Seal release engine configuration failed ($LASTEXITCODE)." }
        }
        $engineExe = Join-Path $optDir 'GoldenSunRecomp.exe'
        $engineDll = Join-Path $optDir 'GoldenSunEngine.dll'
        $engineApi = Join-Path $optDir 'libGoldenSunRecomp_api.a'
        # Bring it up to date through build_game.ps1: it writes the block-timing
        # game code, points the tree at it, sets MAKE so the LTO link runs in
        # parallel, applies the CPU and memory caps and shows its progress. (A
        # bare `cmake --build ... | Out-Null` hid a full engine rebuild with a
        # serial LTO link and looked hung.) Normally quick after build_dev.bat.
        Write-Host 'Checking the engine build is current (build_game.ps1).'
        $powershell = Join-Path $PSHOME 'powershell.exe'
        if (-not (Test-Path -LiteralPath $powershell)) { $powershell = Join-Path $PSHOME 'pwsh.exe' }
        & $powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build_game.ps1') -Game BrokenSeal -BuildDir $engineBuildRel -CpuPercent $CpuPercent -RamPercent $RamPercent
        if ($LASTEXITCODE -ne 0) { throw 'The engine build failed. See the build output above.' }
        }

        # ---- 2. Launcher, builder, translator: the release build tree --------
        # Configured like build\gs011_opt but without LTO (none of these three
        # needs it) and with the player launcher.
        New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
        # The bug report service (The Broken Seal/tools/report_service) stays out of the public
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
            "-DGBARECOMP_ROOT=$engineCMake",
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
            "-DGSR_RELEASE_REPOSITORY=$Repository",
            "-DGSR_REPORT_HOST=$reportHost"
        )
        $configure += @(Get-FreshCacheOption $buildDir)
        $configure += @(Get-ExistingDependencyOptions)
        & $cmake @configure | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed ($LASTEXITCODE)." }
        function Get-ReleaseDependencySource([string]$name, [string]$directory) {
            $entry = Get-Content -LiteralPath (Join-Path $buildDir 'CMakeCache.txt') |
                Where-Object { $_ -match "^FETCHCONTENT_SOURCE_DIR_${name}:(PATH|STRING)=" } |
                Select-Object -First 1
            if ($entry) {
                $source = $entry -replace '^[^=]+=', ''
                if ($source -and (Test-Path -LiteralPath $source -PathType Container)) { return $source }
            }
            $vendored = if ($name -eq 'TOMLPLUSPLUS') { 'tomlpp' } else { $directory }
            foreach ($source in @((Join-Path $buildDir "_deps/$directory-src"), (Join-Path $engineRoot "third_party/$vendored"))) {
                if (Test-Path -LiteralPath $source -PathType Container) { return $source }
            }
            throw "The $name source directory is missing from the configured release dependencies."
        }
        if (-not $LauncherOnly) {
        $imguiSource = Get-ReleaseDependencySource 'IMGUI' 'imgui'
        $tomlSource = Get-ReleaseDependencySource 'TOMLPLUSPLUS' 'tomlplusplus'
        $tlaConfigure = @(
            '-S', $tlaRepo, '-B', $tlaBuild, '-G', 'Ninja',
            '-DCMAKE_BUILD_TYPE=RelWithDebInfo', "-DGBARECOMP_ROOT=$engineCMake",
            "-DCMAKE_C_COMPILER=$mingw/cc.exe", "-DCMAKE_CXX_COMPILER=$mingw/c++.exe",
            "-DSDL2_INCLUDE_DIR=$mingwRoot/include/SDL2", "-DSDL2_LIBRARY=$mingwRoot/lib/libSDL2.dll.a",
            "-DFETCHCONTENT_SOURCE_DIR_IMGUI=$imguiSource",
            "-DFETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS=$tomlSource",
            "-DTLA_GENERATED_DIR=$tlaRepo/local/tla_block/main",
            # build_game.ps1 also enables LTO; keep it on here so building
            # release tools never invalidates the engine's existing objects.
            '-DTLA_SPLIT_GAME_CODE=ON', '-DTLA_ENABLE_LTO=ON',
            '-DTLA_RELEASE_LAUNCHER=ON', '-DTLA_PLAYER_RELEASE=ON',
            "-DGSR_RELEASE_VERSION=$releaseVersion", "-DGSR_REPORT_HOST=$reportHost",
            "-DGSR_RELEASE_REPOSITORY=$Repository"
        )
        $tlaConfigure += @(Get-FreshCacheOption $tlaBuild)
        & $cmake @tlaConfigure | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "The Lost Age release configuration failed ($LASTEXITCODE)." }
        }
        if ($LauncherOnly) {
            Write-Host 'Building the collection launcher.'
            & $cmake --build $buildDir --target GoldenSunCollection
            if ($LASTEXITCODE -ne 0) { throw "Building the launcher failed ($LASTEXITCODE)." }
            $launcherStage = Join-Path $releaseRoot 'launcher-update'
            Remove-ReleaseTree $launcherStage
            Copy-CollectionFiles $launcherStage
            Assert-NoRomBytes $launcherStage
            $launcherZip = Join-Path $releaseRoot "GoldenSunLauncher-windows-$releaseVersion.zip"
            if (Test-Path -LiteralPath $launcherZip) { Remove-Item -LiteralPath $launcherZip -Force }
            Compress-Archive -Path (Join-Path $launcherStage '*') -DestinationPath $launcherZip
            Remove-ReleaseTree $launcherStage
            Write-Host "Launcher ready: $launcherZip" -ForegroundColor Green
        } else {
        Write-Host 'Building the player launcher, the builder and the translator.'
        & $cmake --build $buildDir --target GoldenSunCollection gsr_builder gba_recompile
        if ($LASTEXITCODE -ne 0) { throw "Building the release tools failed ($LASTEXITCODE)." }
        Write-Host 'Building The Lost Age engine through its speed-build script.'
        $powershell = Join-Path $PSHOME 'powershell.exe'
        if (-not (Test-Path -LiteralPath $powershell)) { $powershell = Join-Path $PSHOME 'pwsh.exe' }
        $tlaBuildRelative = 'build/release'
        & $powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build_game.ps1') -Game LostAge -BuildDir $tlaBuildRelative -CpuPercent $CpuPercent -RamPercent $RamPercent
        if ($LASTEXITCODE -ne 0) { throw 'The Lost Age engine build failed.' }
        Set-Location -LiteralPath $repo
        & $cmake --build $tlaBuild --target tla_builder gba_recompile
        if ($LASTEXITCODE -ne 0) { throw "The Lost Age release tools failed ($LASTEXITCODE)." }
        $launcher   = Join-Path $buildDir 'GoldenSunCollection.exe'
        $builderExe = Join-Path $buildDir 'gsr_builder.exe'
        $recompiler = Get-ChildItem -LiteralPath $buildDir -Recurse -Filter 'gba_recompile.exe' |
            Select-Object -First 1 -ExpandProperty FullName
        foreach ($f in @($launcher, $builderExe, $recompiler)) {
            if (-not $f -or -not (Test-Path -LiteralPath $f)) { throw "Build output missing: $f" }
        }

        # ---- 3. A clean release folder -----------------------------------------
        Remove-ReleaseTree $outDir
        Copy-CollectionFiles $outDir
        $builderDir = Join-Path $gameOut 'builder'
        $dataDir    = Join-Path $builderDir 'data'
        $engineDir  = Join-Path $builderDir 'engine'
        # GCC finds its Windows headers at bin/../../mingw64/include: the folder
        # must be called mingw64.
        $tcDir      = Join-Path $builderDir 'mingw64'
        foreach ($d in @($gameOut, $builderDir, $dataDir, (Join-Path $dataDir 'overlays'),
                         (Join-Path $engineDir 'include'), $tcDir)) {
            New-Item -ItemType Directory -Force -Path $d | Out-Null
        }
        Copy-Item -LiteralPath $engineExe, $engineDll -Destination $gameOut
        if ($AshleyEdition) {
            Copy-Item -LiteralPath (Join-Path $collectionRoot 'assets/ashley_edition/edition_badge.bmp'),
                                  (Join-Path $collectionRoot 'assets/ashley_edition/edition_badge.txt') -Destination $gameOut
        }
        foreach ($dll in @('SDL2.dll', 'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll')) {
            Copy-Item -LiteralPath (Join-Path $mingw $dll) -Destination $gameOut
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
            Copy-Item -LiteralPath (Join-Path $engineRoot "src/armv4t/$h") -Destination (Join-Path $engineDir 'include')
        }
        Copy-Item -LiteralPath $engineApi -Destination $engineDir

        # Self-heal: the overlay shim headers the engine's runtime compile includes.
        $ovInc = Join-Path $gameOut 'overlay_toolchain/include'
        New-Item -ItemType Directory -Force -Path $ovInc | Out-Null
        foreach ($h in @('src/runtime/overlay_runtime_arm.h', 'src/runtime/overlay_abi.h',
                         'src/armv4t/runtime_arm_types.h')) {
            $src = Join-Path $engineRoot $h
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
        $sample += @('main/recompiled_000.cpp', 'main/dispatch_table.cpp', 'main/symbol_map.cpp') |
            ForEach-Object { Join-Path (Join-Path $tlaRepo 'local/tla_block') $_ }
        foreach ($kind in @('recompiled.cpp', 'dispatch_table.cpp', 'expected.cpp')) {
            $representative = Get-ChildItem -LiteralPath (Join-Path $tlaRepo 'local/tla_block/reloc') -Recurse -File -Filter $kind |
                Select-Object -First 1 -ExpandProperty FullName
            if ($representative) { $sample += $representative }
        }
        $engineInc = Join-Path $engineDir 'include'
        $headers = New-Object 'System.Collections.Generic.HashSet[string]'
        # Union both corpora's distinct standard includes before trimming GCC.
        $headerProbe = Join-Path $releaseRoot 'collection-header-probe.cpp'
        $includeLines = foreach ($root in @((Join-Path $repo 'local/gs011'), (Join-Path $tlaRepo 'local/tla_block'))) {
            Get-ChildItem -LiteralPath $root -Recurse -File -Include '*.cpp', '*.h' |
                Select-String -Pattern '^\s*#\s*include\s*<' | ForEach-Object { $_.Line.Trim() }
        }
        $includeLines | Sort-Object -Unique | Set-Content -LiteralPath $headerProbe -Encoding ascii
        $sample += $headerProbe
        foreach ($src in $sample) {
            if (-not (Test-Path -LiteralPath $src)) { continue }
            $runtimeInc = if ($src.StartsWith($tlaRepo, [StringComparison]::OrdinalIgnoreCase)) {
                Join-Path $engineRoot 'src/armv4t'
            } else { $engineInc }
            $deps = & "$mingw/g++.exe" -O2 -g -DNDEBUG '-std=c++20' -Og -g0 '-DGBARECOMP_OUTLINE_BUS=1' `
                "-I$(Split-Path -Parent $src)" "-I$runtimeInc" -M $src
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
        Remove-Item -LiteralPath $headerProbe -Force
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

Start GoldenSunLauncher.exe in the collection's top folder and select
The Broken Seal. On first play, choose your own Golden Sun (USA, Europe) ROM.
Preparation happens once; later selections start the game directly.

This download contains no Golden Sun code or data. Everything from the game
comes from your ROM, on your PC.

Settings: press F1 in the game.
"@ | Set-Content -LiteralPath (Join-Path $gameOut 'README.txt') -Encoding ascii

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
"@ | Set-Content -LiteralPath (Join-Path $gameOut 'THIRD_PARTY_NOTICES.txt') -Encoding ascii

        # The Lost Age: its own config/runtime API, using shared engine headers.
        $tlaBuilderDir = Join-Path $tlaOut 'builder'
        $tlaData = Join-Path $tlaBuilderDir 'data'
        $tlaEngine = Join-Path $tlaBuilderDir 'engine'
        New-Item -ItemType Directory -Force -Path $tlaOut, (Join-Path $tlaData 'reloc'), (Join-Path $tlaEngine 'include') | Out-Null
        Copy-Item -LiteralPath (Join-Path $tlaBuild 'TLARecomp.exe'), (Join-Path $tlaBuild 'TLAEngine.dll') -Destination $tlaOut
        if ($AshleyEdition) {
            Copy-Item -LiteralPath (Join-Path $collectionRoot 'assets/ashley_edition/edition_badge.bmp'),
                                  (Join-Path $collectionRoot 'assets/ashley_edition/edition_badge.txt') -Destination $tlaOut
        }
        Copy-Item -LiteralPath (Join-Path $tlaBuild 'tla_builder.exe') -Destination (Join-Path $tlaBuilderDir 'gsr_builder.exe')
        $tlaTranslator = Get-ChildItem -LiteralPath $tlaBuild -Recurse -Filter 'gba_recompile.exe' | Select-Object -First 1 -ExpandProperty FullName
        if (-not $tlaTranslator) { throw 'The Lost Age translator is missing.' }
        Copy-Item -LiteralPath $tlaTranslator -Destination $tlaBuilderDir
        Copy-Item -LiteralPath (Join-Path $tlaRepo 'config/tla/main.toml') -Destination $tlaData
        Get-ChildItem -LiteralPath (Join-Path $tlaRepo 'config/tla/reloc') -Filter '*.toml' | Copy-Item -Destination (Join-Path $tlaData 'reloc')
        foreach ($h in @('runtime_arm.h', 'runtime_arm_types.h')) {
            Copy-Item -LiteralPath (Join-Path $engineRoot "src/armv4t/$h") -Destination (Join-Path $tlaEngine 'include')
        }
        Copy-Item -LiteralPath (Join-Path $tlaBuild 'libTLARecomp_api.a') -Destination $tlaEngine
        $tlaOverlayInc = Join-Path $tlaOut 'overlay_toolchain/include'
        New-Item -ItemType Directory -Force -Path $tlaOverlayInc | Out-Null
        foreach ($h in @('src/runtime/overlay_runtime_arm.h', 'src/runtime/overlay_abi.h', 'src/armv4t/runtime_arm_types.h')) {
            Copy-Item -LiteralPath (Join-Path $engineRoot $h) -Destination $tlaOverlayInc
        }
        Copy-Item -LiteralPath $tcDir -Destination $tlaBuilderDir -Recurse
        foreach ($dll in @('SDL2.dll', 'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll')) {
            Copy-Item -LiteralPath (Join-Path $mingw $dll) -Destination $tlaOut
        }
        Copy-Item -LiteralPath (Join-Path $gameOut 'THIRD_PARTY_NOTICES.txt') -Destination $outDir
        @'
Golden Sun Recompiled
=====================

Start GoldenSunLauncher.exe and click The Broken Seal or The Lost Age.
The selected cartridge breathes and glows. On first play, accept the preparation
prompt and choose your own matching USA/Europe ROM. Later selections start
the game directly. New releases are offered before starting either game.
Selected .gba files are copied into ROMs beside the launcher. Existing matching
.sav files and savestates are copied into Saves; the originals are kept.
Conflicting imports are preserved in Saves/Imported. Each game keeps its own
settings in its folder under games/.
The root launcher handles preparation, updates and starting both games.
Keep the games folders beside it; each contains that game's engine and tools.
Settings: press F1 in the game.
This download contains no game code, ROM, BIOS or generated game library.
'@ | Set-Content -LiteralPath (Join-Path $outDir 'README.txt') -Encoding ascii

        # ---- 6. Checks -------------------------------------------------------------
        foreach ($forbidden in @('GoldenSunGame.dll', 'TLAGame.dll', 'libGoldenSunGame.so', 'libTLAGame.so', 'gssplash.jpg', '*.gba', '*.sav', '*.state*', '*.ini', 'recompiled*.cpp', 'bios*.bin')) {
            $hit = Get-ChildItem -LiteralPath $outDir -Recurse -Filter $forbidden -ErrorAction SilentlyContinue
            if ($hit) { throw "The release folder must not contain $forbidden ($($hit[0].FullName))." }
        }
        Assert-NoRomBytes $outDir
        $size = (Get-ChildItem -LiteralPath $outDir -Recurse -File | Measure-Object Length -Sum).Sum
        Write-Host ('Release folder: {0:N0} MB' -f ($size / 1MB))

        if (-not $NoTest) {
            $testRom = Get-ReleaseTestRom $repo
            $testDir = Join-Path $releaseRoot 'release-test'
            Remove-ReleaseTree $testDir
            New-Item -ItemType Directory -Force -Path $testDir | Out-Null
            Write-Host 'Testing: building the game code with only the release folder''s tools.'
            $clock = [Diagnostics.Stopwatch]::StartNew()
            & (Join-Path $builderDir 'gsr_builder.exe') --rom $testRom --out $testDir --work (Join-Path $testDir 'work') |
                Where-Object { $_ -match '^@(stage|error|done)' } | ForEach-Object { Write-Host "  $_" }
            $ok = $LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath (Join-Path $testDir 'GoldenSunGame.dll'))
            Write-Host ('  {0:N1} min' -f $clock.Elapsed.TotalMinutes)
            if (-not $ok) { throw "The release test build failed. Log: $(Join-Path $testDir 'work\build-log.txt')" }
            Remove-ReleaseTree $testDir
            Write-Host 'Testing The Lost Age with only its packaged tools.'
            New-Item -ItemType Directory -Force -Path $testDir | Out-Null
            $tlaTestRom = Get-ReleaseTestRom $tlaRepo
            & (Join-Path $tlaBuilderDir 'gsr_builder.exe') --rom $tlaTestRom --out $testDir --work (Join-Path $testDir 'work') |
                Where-Object { $_ -match '^@(stage|error|done)' } | ForEach-Object { Write-Host "  $_" }
            if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $testDir 'TLAGame.dll'))) {
                throw "The Lost Age release test failed. Log: $(Join-Path $testDir 'work/build-log.txt')"
            }
            Remove-ReleaseTree $testDir
            Write-Host '  Both releases build their game from the ROM on their own.' -ForegroundColor Green
        }

        # ---- 7. Zip ------------------------------------------------------------------
        $editionSuffix = if ($AshleyEdition) { '-Ashley' } else { '' }
        $zip = Join-Path $releaseRoot ('GoldenSunRecompiled{0}-{1}.zip' -f $editionSuffix, (Get-Date -Format 'yyyy-MM-dd'))
        if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
        Compress-Archive -Path $outDir -DestinationPath $zip -CompressionLevel Optimal
        Write-Host ''
        Write-Host "Release ready: $outDir" -ForegroundColor Green
        Write-Host ('Zip for GitHub: {0} ({1:N0} MB)' -f $zip, ((Get-Item -LiteralPath $zip).Length / 1MB)) -ForegroundColor Green
        }
    }

    # ---- 8. Linux release (WSL Ubuntu) ------------------------------------------
    # scripts/make_release_linux.sh inside WSL: the Linux engine, launcher,
    # builder and a trimmed Linux GCC, into root Releases\linux (a folder
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
            $linuxArgs = @('scripts/make_release_linux.sh', '--version', $releaseVersion,
                           '--repository', $Repository)
            if ($AshleyEdition) { $linuxArgs += '--ashley-edition' }
            if ($LauncherOnly) {
                $linuxArgs += '--launcher-only'
            } elseif ($NoTest) {
                $linuxArgs += '--no-test'
            } else {
                # The ROM path goes over as GSR_ROM through WSLENV: Windows
                # PowerShell would split it at its spaces as an argument.
                $romFull = Get-ReleaseTestRom $repo
                $env:GSR_ROM = '/mnt/' + $romFull.Substring(0, 1).ToLower() +
                    ($romFull.Substring(2) -replace '\\', '/')
                $tlaRomFull = Get-ReleaseTestRom $tlaRepo
                $env:TLA_ROM = '/mnt/' + $tlaRomFull.Substring(0, 1).ToLower() + ($tlaRomFull.Substring(2) -replace '\\', '/')
                $env:WSLENV = (@($env:WSLENV, 'GSR_ROM', 'TLA_ROM') | Where-Object { $_ }) -join ':'
            }
            & $wsl -d $distro -u root --cd $collectionRoot --exec bash @linuxArgs
            if ($LASTEXITCODE -ne 0) {
                Write-Host "The Linux release failed (the Windows release above is complete)." -ForegroundColor Red
                exit 1
            }
            if (-not $LauncherOnly) {
                $linuxZip = Get-ChildItem -LiteralPath (Join-Path $releaseRoot 'linux') -File -Filter 'GoldenSunRecompiled-linux*.zip' |
                    Sort-Object LastWriteTime | Select-Object -Last 1
                Assert-NoRomBytes (Join-Path $releaseRoot 'linux/Golden Sun Recompiled') $(if ($linuxZip) { $linuxZip.FullName } else { '' })
            }
            Write-Host "Linux release ready: $(Join-Path $releaseRoot 'linux')" -ForegroundColor Green
        }
    }
    exit 0
} catch {
    Write-Host "Release stopped: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
} finally {
    if ($ownsMutex) { $buildMutex.ReleaseMutex() }
    if ($null -ne $buildMutex) { $buildMutex.Dispose() }
}
