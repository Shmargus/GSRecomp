<#
.SYNOPSIS
  One command for the whole Golden Sun loop: regenerate -> recompile -> build -> verify.

.DESCRIPTION
  This exists so day-to-day work is a single command instead of a sequence
  of tool invocations spread across two checkouts. It hides the GSRecomp /
  gbarecomp split, not the evidence: every step prints what it actually ran
  and every acceptance number comes from real output.

  Steps, in order:
    1. regenerate  config/usa/main.toml from the ELF via tools/build_main_toml.py
    2. recompile   the main corpus with the patched gba_recompile (HARD GATE:
                   zero data_range collisions)
    3. build       build/gs011/GoldenSunRecomp.exe
    4. verify      the strict-static tracks, comparing semantic fingerprints
                   against config/usa/acceptance-baseline.json

  Any step can be run alone; -From/-To run a contiguous slice.

.EXAMPLE
  .\scripts\gs.ps1                      # full loop
  .\scripts\gs.ps1 -From verify         # just re-run the tracks
  .\scripts\gs.ps1 -Frames 21600        # push the frontier track further
  .\scripts\gs.ps1 -To recompile        # regenerate + gate, no 35-minute build

.NOTES
  Private paths (ROM, BIOS, ELF) are read from config/local.json, which is
  gitignored. Nothing here embeds ROM, BIOS or asset bytes.
#>
[CmdletBinding()]
param(
    [ValidateSet('regenerate', 'recompile', 'build', 'verify')]
    [string]$From = 'regenerate',

    [ValidateSet('regenerate', 'recompile', 'build', 'verify')]
    [string]$To = 'verify',

    # Frontier track length. The regression tracks are always run at their
    # own pinned lengths regardless of this.
    [int]$Frames = 10800,

    # Skip the 5,400-frame regression tracks. Use only for a quick look;
    # never for an acceptance claim.
    [switch]$SkipRegressions,

    # Link with LTO. Roughly a 25-minute link, and the only configuration
    # whose throughput numbers are comparable to the recorded fps figures.
    [switch]$Lto
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot
$Order = @('regenerate', 'recompile', 'build', 'verify')
$FromIndex = $Order.IndexOf($From)
$ToIndex = $Order.IndexOf($To)

if ($FromIndex -gt $ToIndex) {
    throw "-From '$From' comes after -To '$To'; nothing to do."
}

function Test-Stage([string]$Name) {
    $index = $Order.IndexOf($Name)
    return ($index -ge $FromIndex -and $index -le $ToIndex)
}

function Write-Step([string]$Message) {
    Write-Host ""
    Write-Host "=== $Message" -ForegroundColor Cyan
}

function Write-Ran([string]$CommandLine) {
    Write-Host "    $CommandLine" -ForegroundColor DarkGray
}

# --- configuration -------------------------------------------------------
# Private absolute paths live outside the repo's tracked files.

$ConfigPath = Join-Path $RepoRoot 'config/local.json'
if (-not (Test-Path $ConfigPath)) {
    throw @"
Missing $ConfigPath.

Create it (it is gitignored) with the private paths for this machine:

{
  "rom":       "C:\\path\\to\\Golden Sun.gba",
  "bios":      "C:\\path\\to\\gba_bios.bin",
  "elf":       "C:\\path\\to\\goldensun-disasm\\goldensun.elf",
  "recompile": "C:\\path\\to\\gbarecomp\\build-mingw2\\gba_recompile.exe",
  "disassembly_revision": "<git rev of the disassembly checkout>"
}
"@
}

$Config = Get-Content $ConfigPath -Raw | ConvertFrom-Json

foreach ($key in @('rom', 'bios', 'elf', 'recompile', 'disassembly_revision')) {
    if (-not $Config.PSObject.Properties.Name.Contains($key)) {
        throw "config/local.json is missing required key '$key'."
    }
}

foreach ($key in @('rom', 'bios', 'elf', 'recompile')) {
    if (-not (Test-Path $Config.$key)) {
        throw "config/local.json '$key' points at a missing file: $($Config.$key)"
    }
}

# The in-tree gba_recompile under build/ has been stale before and reported
# hundreds of phantom data_range collisions against an unmodified config.
# Refuse to run one from build/ rather than let staleness read as a regression.
if ($Config.recompile -match [regex]::Escape((Join-Path $RepoRoot 'build'))) {
    throw @"
config/local.json 'recompile' points inside this repo's build/ tree:
  $($Config.recompile)
That copy has been stale in the past and reports false data_range collisions.
Point it at the gbarecomp checkout's own build output instead.
"@
}

$MainToml   = Join-Path $RepoRoot 'config/usa/main.toml'
$BuildDir   = Join-Path $RepoRoot 'build/gs011'
$CorpusDir  = Join-Path $RepoRoot 'local/gs011/main'
$Runner     = Join-Path $BuildDir 'GoldenSunRecomp.exe'
$LogDir     = Join-Path $BuildDir 'gs-logs'
$BaselineFile = Join-Path $RepoRoot 'config/usa/acceptance-baseline.json'

New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

# --- 1. regenerate -------------------------------------------------------

if (Test-Stage 'regenerate') {
    Write-Step 'regenerate  config/usa/main.toml'

    $before = if (Test-Path $MainToml) {
        (Get-FileHash $MainToml -Algorithm SHA256).Hash.ToLower()
    } else { '(absent)' }

    $genArgs = @(
        (Join-Path $RepoRoot 'tools/build_main_toml.py')
        '--rom',             $Config.rom
        '--elf',             $Config.elf
        '--corpus',          (Join-Path $RepoRoot 'local/symbols/main-symbols.json')
        '--data-exceptions', (Join-Path $RepoRoot 'config/usa/main-data-exceptions.json')
        '--output',          $MainToml
        '--disassembly-revision', $Config.disassembly_revision
    )
    Write-Ran "python tools/build_main_toml.py --output config/usa/main.toml ..."
    & python @genArgs
    if ($LASTEXITCODE -ne 0) { throw "build_main_toml.py failed ($LASTEXITCODE)." }

    $after = (Get-FileHash $MainToml -Algorithm SHA256).Hash.ToLower()
    Write-Host "    main.toml sha256 $after"
    if ($before -eq $after) {
        Write-Host "    unchanged (inputs did not move)" -ForegroundColor DarkGray
    } else {
        Write-Host "    was            $before" -ForegroundColor DarkGray
    }
}

# --- 2. recompile --------------------------------------------------------

if (Test-Stage 'recompile') {
    Write-Step 'recompile   main corpus  (gate: zero data_range collisions)'

    $log = Join-Path $LogDir 'recompile.log'
    Write-Ran "$($Config.recompile) --config config/usa/main.toml --out local/gs011/main"

    # cmd.exe does the redirect. Piping a native exe's stderr through
    # PowerShell wraps each line in an ErrorRecord (NativeCommandError) and
    # sets $? false even on exit 0 - which previously lost this log entirely
    # on the one run where it mattered.
    $quoted = '"{0}" --rom "{1}" --config "{2}" --out "{3}" --max-functions 40000 > "{4}" 2>&1' -f `
        $Config.recompile, $Config.rom, $MainToml, $CorpusDir, $log
    & cmd.exe /c $quoted
    $recompileExit = $LASTEXITCODE
    $collisions = @(Select-String -Path $log -SimpleMatch 'control-flow entries into')

    $summary = Select-String -Path $log -Pattern 'TOTAL emitted|thumb literal hooks|long_branch_calls|midfn_aliases|alias_seeds_dropped|data_ranges_honored'
    foreach ($line in $summary) { Write-Host "    $($line.Line.Trim())" }

    if ($collisions.Count -gt 0) {
        # Print the offending entry addresses too, not just the count - the
        # "entered at ... via branch ..." lines are what identify the defect.
        Write-Host ""
        $detail = Select-String -Path $log -Pattern 'entered at 0x|data_range \[0x'
        foreach ($c in $collisions) { Write-Host "    $($c.Line.Trim())" -ForegroundColor Red }
        foreach ($d in $detail | Select-Object -First 20) {
            Write-Host "    $($d.Line.Trim())" -ForegroundColor Red
        }
        throw "GATE FAILED: control-flow entries into [[data_range]]. Full log: $log"
    }
    if ($recompileExit -ne 0) {
        throw "gba_recompile exited $recompileExit. Full log: $log"
    }

    # A TOML conditional-branch override is only effective when the emitted
    # instruction contains the runtime chokepoint. Keep stale generated
    # shards from silently making the runner's callback unreachable.
    $conditionalBranchSites = @(
        '0x0800B27E', '0x0800B324', '0x0800B328',
        '0x0800B3D2', '0x0800B3DC', '0x0800B3E6', '0x0800B3EC',
        '0x0800C6FA', '0x0800C702', '0x0800C708',
        '0x0801CFC6',
        '0x080C26BE'
    )
    $generatedShards = @(Get-ChildItem -LiteralPath $CorpusDir -Filter 'recompiled_*.cpp' -File)
    foreach ($site in $conditionalBranchSites) {
        $needle = "runtime_conditional_branch(${site}u"
        $hook = @($generatedShards | Select-String -SimpleMatch $needle)
        if ($hook.Count -eq 0) {
            throw "GATE FAILED: generated corpus is missing runtime conditional-branch hook at $site. Regenerate with the current gba_recompile."
        }
        Write-Host "    generated conditional-branch hook present: $site" -ForegroundColor Green
    }
    # Instant text's fourth Message-speed choice needs these three hooks.
    $valueHooks = @(
        'runtime_thumb_alu_immediate(0x0801D072u',
        'runtime_thumb_literal(0x08016E56u',
        'runtime_thumb_literal(0x08093332u',
        'runtime_thumb_alu_immediate(0x080180D0u',
        'runtime_thumb_literal(0x0801D6AEu',
        'runtime_thumb_alu_immediate(0x08021732u',
        'runtime_thumb_alu_immediate(0x0801D6B8u',
        'runtime_thumb_alu_immediate(0x0801D6C6u',
        'runtime_thumb_literal(0x0801D39Cu',
        'runtime_thumb_literal(0x0801D5B6u',
        'runtime_thumb_literal(0x0801D772u',
        'runtime_thumb_literal(0x0801D7CAu',
        'runtime_thumb_literal(0x0808AF84u',
        'runtime_thumb_literal(0x08003318u'
    )
    foreach ($needle in $valueHooks) {
        $hook = @($generatedShards | Select-String -SimpleMatch $needle)
        if ($hook.Count -eq 0) {
            throw "GATE FAILED: generated corpus is missing $needle. Regenerate with the current gba_recompile."
        }
        Write-Host "    generated hook present: $needle" -ForegroundColor Green
    }
    # Func_dc8's second self-modifying slot must be a dispatch boundary, not
    # an AOT entry with the ROM placeholders baked in (the runner refuses
    # such a corpus at startup).
    $dispatchFiles = @(Get-ChildItem -LiteralPath $CorpusDir -Filter 'dispatch_table*.cpp' -File)
    foreach ($slot in @('0x03000A5C', '0x03000BD8')) {
        $hit = @($dispatchFiles | Select-String -SimpleMatch "{${slot}u, 0u")
        if ($hit.Count -ne 0) {
            throw "GATE FAILED: main corpus has an AOT entry at $slot; main.toml must declare it a runtime_code_entry."
        }
        Write-Host "    Func_dc8 slot at $slot is a dispatch boundary" -ForegroundColor Green
    }
    Write-Host "    gate passed: zero data_range collisions" -ForegroundColor Green
}

# --- 3. build ------------------------------------------------------------

if (Test-Stage 'build') {
    Write-Step "build       GoldenSunRecomp.exe  (LTO: $([bool]$Lto))"

    $ltoFlag = if ($Lto) { 'ON' } else { 'OFF' }
    $cmakeArgs = @(
        '-S', $RepoRoot,
        '-B', $BuildDir,
        '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release',
        '-DGSR_BUILD_LOCAL_RUNNER=ON',
        "-DGBARECOMP_ROOT=$(Join-Path $RepoRoot 'gbarecomp')",
        "-DGSR_GENERATED_DIR=$(Join-Path $RepoRoot 'local/gs011/main')",
        '-DSDL2_INCLUDE_DIR=C:/msys64/mingw64/include/SDL2',
        '-DSDL2_LIBRARY=C:/msys64/mingw64/lib/libSDL2.dll.a',
        "-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=$ltoFlag"
    )
    Write-Ran "cmake -S . -B build/gs011 -G Ninja -DCMAKE_BUILD_TYPE=Release -DGSR_BUILD_LOCAL_RUNNER=ON -DGSR_GENERATED_DIR=local/gs011/main -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=$ltoFlag"
    & cmake @cmakeArgs | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)." }

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    Write-Ran "cmake --build build/gs011 --target GoldenSunRecomp"
    & cmake --build $BuildDir --target GoldenSunRecomp *>&1 |
        Tee-Object -FilePath (Join-Path $LogDir 'build.log') |
        Select-String -Pattern 'error|FAILED|Linking' | ForEach-Object { Write-Host "    $_" }
    if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE). Log: $LogDir\build.log" }

    $sw.Stop()
    $size = (Get-Item $Runner).Length
    Write-Host ("    linked {0:N0} bytes in {1:mm\:ss}" -f $size, $sw.Elapsed)
}

# --- 4. verify -----------------------------------------------------------

# One strict-static track. Returns a hashtable of the semantic fingerprints
# plus the boundary, if it stopped at one.
function Invoke-Track {
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][int]$TrackFrames,
        # $null means the no-input track: the variable must be genuinely
        # ABSENT, because an empty value silently enables a demo track.
        [string]$DemoInput
    )

    # Never run two runners at once: a concurrent run has silently eaten
    # another run's entire stdout before.
    $running = @(Get-Process -Name 'GoldenSunRecomp' -ErrorAction SilentlyContinue)
    if ($running.Count -gt 0) {
        throw "A GoldenSunRecomp.exe is already running (PID $($running[0].Id)). Refusing to run two at once."
    }

    $log = Join-Path $LogDir "track-$Name.log"

    $env:GBARECOMP_STRICT_STATIC = '1'
    if ([string]::IsNullOrEmpty($DemoInput)) {
        Remove-Item Env:\GBARECOMP_DEMO_INPUT -ErrorAction SilentlyContinue
        if (Test-Path Env:\GBARECOMP_DEMO_INPUT) {
            throw "GBARECOMP_DEMO_INPUT is still set; the no-input track would be invalid."
        }
    } else {
        $env:GBARECOMP_DEMO_INPUT = $DemoInput
    }

    Write-Host ""
    Write-Host "    [$Name] $TrackFrames frames, strict-static, self-heal off"

    # Redirect via cmd.exe. A strict-static abort writes to stderr, and
    # piping native stderr through PowerShell turns each line into an
    # ErrorRecord - which made a perfectly normal "found the next boundary"
    # result look like a script crash.
    $quoted = '"{0}" --bios "{1}" --rom "{2}" --no-window --frames {3} > "{4}" 2>&1' -f `
        $Runner, $Config.bios, $Config.rom, $TrackFrames, $log
    & cmd.exe /c $quoted
    $trackExit = $LASTEXITCODE

    Remove-Item Env:\GBARECOMP_STRICT_STATIC -ErrorAction SilentlyContinue
    Remove-Item Env:\GBARECOMP_DEMO_INPUT -ErrorAction SilentlyContinue

    $text = Get-Content $log -Raw
    $result = @{ name = $Name; frames = $TrackFrames; exit = $trackExit; log = $log }

    # Semantic invariants. These are what "no regression" means; a
    # FULLY_STATIC headline on its own does not establish it.
    foreach ($field in @('cycles', 'steps', 'ppu_frames', 'ppu_vcount', 'trace_events',
                         'dispatch_misses', 'interpreted_insns', 'healed_native',
                         'unmapped', 'io_unhandled')) {
        if ($text -match "$field=(\d+)") { $result[$field] = [int64]$Matches[1] }
    }
    foreach ($field in @('final_pc')) {
        if ($text -match "$field=(0x[0-9a-fA-F]+)") { $result[$field] = $Matches[1] }
    }
    foreach ($field in @('pal_nonzero', 'vram_nonzero', 'oam_nonzero')) {
        if ($text -match "$field=(\d+/\d+)") { $result[$field] = $Matches[1] }
    }
    if ($text -match 'self_heal_coverage=(\w+)') { $result['coverage'] = $Matches[1] }

    if ($text -match 'STRICT_STATIC dispatch miss for pc=(0x[0-9A-Fa-f]+) \((\w+)\)') {
        $result['boundary_pc'] = $Matches[1]
        $result['boundary_mode'] = $Matches[2]
    }

    return $result
}

if (Test-Stage 'verify') {
    Write-Step 'verify      strict-static tracks'

    if (-not (Test-Path $Runner)) { throw "No runner at $Runner. Run the build stage first." }

    $baseline = if (Test-Path $BaselineFile) {
        Get-Content $BaselineFile -Raw | ConvertFrom-Json
    } else { $null }

    $results = @()
    if (-not $SkipRegressions) {
        $results += Invoke-Track -Name 'campaign-5400' -TrackFrames 5400 -DemoInput 'campaign'
        $results += Invoke-Track -Name 'noinput-5400'  -TrackFrames 5400
    }
    $results += Invoke-Track -Name "campaign-$Frames" -TrackFrames $Frames -DemoInput 'campaign'

    # trace_events legitimately moves when interior resume aliases are added.
    # Everything else here is architectural state and must not move.
    $semantic = @('cycles', 'steps', 'final_pc', 'ppu_frames', 'ppu_vcount',
                  'pal_nonzero', 'vram_nonzero', 'oam_nonzero',
                  'unmapped', 'io_unhandled')

    Write-Host ""
    Write-Host "=== results" -ForegroundColor Cyan
    $regressions = @()

    foreach ($r in $results) {
        $coverage = if ($r.ContainsKey('coverage')) { $r['coverage'] } else { '(none)' }
        $colour = if ($coverage -eq 'FULLY_STATIC') { 'Green' } else { 'Yellow' }
        Write-Host ("  {0,-18} {1,-12} exit={2} trace_events={3}" -f `
            $r['name'], $coverage, $r['exit'],
            $(if ($r.ContainsKey('trace_events')) { $r['trace_events'] } else { '?' })) -ForegroundColor $colour

        if ($r.ContainsKey('boundary_pc')) {
            Write-Host ("      boundary: {0} ({1})" -f $r['boundary_pc'], $r['boundary_mode']) -ForegroundColor Yellow
            Write-Host  "      resolve:  python tools/resolve_miss_functions.py --elf <elf> $($r['log'])"
        }

        if ($null -ne $baseline -and $baseline.PSObject.Properties.Name.Contains($r['name'])) {
            $b = $baseline.$($r['name'])
            foreach ($field in $semantic) {
                if (-not $b.PSObject.Properties.Name.Contains($field)) { continue }
                if (-not $r.ContainsKey($field)) { continue }
                if ("$($r[$field])" -ne "$($b.$field)") {
                    $regressions += "$($r['name']).$field : baseline $($b.$field) -> now $($r[$field])"
                }
            }
            if ($r.ContainsKey('trace_events') -and $b.PSObject.Properties.Name.Contains('trace_events')) {
                $delta = $r['trace_events'] - $b.trace_events
                if ($delta -ne 0) {
                    Write-Host ("      trace_events moved {0:+#;-#;0} (semantic invariants unchanged)" -f $delta) -ForegroundColor DarkGray
                }
            }
        }
    }

    if ($regressions.Count -gt 0) {
        Write-Host ""
        Write-Host "REGRESSION - semantic invariants moved:" -ForegroundColor Red
        foreach ($line in $regressions) { Write-Host "  $line" -ForegroundColor Red }
        throw "Acceptance failed: $($regressions.Count) semantic invariant(s) changed."
    }

    if ($null -eq $baseline) {
        Write-Host ""
        Write-Host "  No $BaselineFile - nothing to compare against." -ForegroundColor Yellow
        Write-Host "  Write the current numbers there once you trust them." -ForegroundColor Yellow
    } else {
        Write-Host ""
        Write-Host "  semantic invariants match baseline" -ForegroundColor Green
    }
}

Write-Host ""
Write-Host "done." -ForegroundColor Cyan
