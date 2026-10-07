# The release window, opened by make_release.bat: pick Windows, Linux or
# both, with or without the test build, and watch make_release.ps1 run.
# Nothing builds until "Make release" is clicked.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -Namespace Gsr -Name Dpi -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();'
[void][Gsr.Dpi]::SetProcessDPIAware()
[System.Windows.Forms.Application]::EnableVisualStyles()

$repo = Split-Path -Parent $PSScriptRoot
$releaseRoot = Join-Path $repo 'GSRecomp-Release'
$settingsPath = Join-Path $repo 'local/release_tool.json'
$logPath = Join-Path $releaseRoot 'make_release.log'
$distro = 'Ubuntu-24.04'
$wsl = Join-Path $env:SystemRoot 'System32\wsl.exe'

# What this PC can do.
$haveWsl = $false
if (Test-Path -LiteralPath $wsl) {
    $names = (& $wsl --list --quiet 2>$null) -replace "`0", ''
    $haveWsl = [bool]($names | Where-Object { $_.Trim() -eq $distro })
}
$rom = $null
try {
    $local = Get-Content -Raw -LiteralPath (Join-Path $repo 'config/local.json') | ConvertFrom-Json
    if ($local.rom -and (Test-Path -LiteralPath $local.rom)) { $rom = $local.rom }
} catch { }

# Last choices.
$saved = @{ target = 'Both'; test = $true; version = '' }
try {
    $j = Get-Content -Raw -LiteralPath $settingsPath | ConvertFrom-Json
    if ($j.target) { $saved.target = $j.target }
    if ($null -ne $j.test) { $saved.test = [bool]$j.test }
    if ($j.version) { $saved.version = [string]$j.version }
} catch { }

# ---- Window ----------------------------------------------------------------
$font = New-Object System.Drawing.Font('Segoe UI', 9.5)
$form = New-Object System.Windows.Forms.Form
$form.Text = 'Golden Sun Recompiled - Make release'
$form.Font = $font
$form.AutoScaleMode = 'Dpi'
$form.StartPosition = 'CenterScreen'
$form.ClientSize = New-Object System.Drawing.Size(760, 600)
$form.MinimumSize = New-Object System.Drawing.Size(620, 480)

$title = New-Object System.Windows.Forms.Label
$title.Text = 'Make a player release'
$title.Font = New-Object System.Drawing.Font('Segoe UI', 13, [System.Drawing.FontStyle]::Bold)
$title.AutoSize = $true
$title.Location = New-Object System.Drawing.Point(16, 12)
$form.Controls.Add($title)

$group = New-Object System.Windows.Forms.GroupBox
$group.Text = 'Build for'
$group.Location = New-Object System.Drawing.Point(16, 48)
$group.Size = New-Object System.Drawing.Size(250, 120)
$form.Controls.Add($group)

$radios = @{}
$y = 24
foreach ($name in @('Windows', 'Linux', 'Both')) {
    $r = New-Object System.Windows.Forms.RadioButton
    $r.Text = $name
    $r.AutoSize = $true
    $r.Location = New-Object System.Drawing.Point(16, $y)
    $group.Controls.Add($r)
    $radios[$name] = $r
    $y += 28
}
if (-not $haveWsl) {
    $radios['Linux'].Enabled = $false
    $radios['Both'].Enabled = $false
    $saved.target = 'Windows'
}
$radios[$saved.target].Checked = $true

$test = New-Object System.Windows.Forms.CheckBox
$test.Text = 'Test the release'
$test.AutoSize = $true
$test.Location = New-Object System.Drawing.Point(290, 56)
$test.Checked = $saved.test -and [bool]$rom
$test.Enabled = [bool]$rom
$form.Controls.Add($test)

# Only the player launchers: players drop the new one over their own and
# nothing rebuilds. Never remembered, so a full release is the default.
$launcherOnly = New-Object System.Windows.Forms.CheckBox
$launcherOnly.Text = 'Launcher only'
$launcherOnly.AutoSize = $true
$launcherOnly.Location = New-Object System.Drawing.Point(450, 56)
$form.Controls.Add($launcherOnly)

$versionLabel = New-Object System.Windows.Forms.Label
$versionLabel.Text = 'Version:'
$versionLabel.AutoSize = $true
$versionLabel.Location = New-Object System.Drawing.Point(290, 90)
$form.Controls.Add($versionLabel)

$version = New-Object System.Windows.Forms.TextBox
$version.Location = New-Object System.Drawing.Point(360, 86)
$version.Size = New-Object System.Drawing.Size(140, 26)
$version.Text = $saved.version
$form.Controls.Add($version)

$versionHint = New-Object System.Windows.Forms.Label
$versionHint.Text = 'e.g. v0.2-test (the GitHub tag)'
$versionHint.AutoSize = $true
$versionHint.ForeColor = [System.Drawing.Color]::DimGray
$versionHint.Location = New-Object System.Drawing.Point(508, 90)
$form.Controls.Add($versionHint)

$info = New-Object System.Windows.Forms.Label
$info.Location = New-Object System.Drawing.Point(290, 118)
$info.Size = New-Object System.Drawing.Size(450, 60)
$info.Anchor = 'Top, Left, Right'
$form.Controls.Add($info)

function Update-Info {
    $lines = @()
    $test.Enabled = [bool]$rom -and -not $launcherOnly.Checked
    if ($launcherOnly.Checked) {
        $lines += 'Only the launchers, zipped on their own, for players to drop over their old one. The game and engine are not built.'
    } elseif ($rom) {
        $lines += 'The test builds the game once from your ROM with only the release''s own tools, about 4 minutes per platform. It is deleted afterwards.'
    } else {
        $lines += 'No test: config\local.json names no ROM that exists.'
    }
    if (-not $haveWsl) {
        $lines += "Linux needs WSL $distro, which is not installed."
    } elseif ($radios['Linux'].Checked) {
        $lines += 'Linux only uses the game code of your last Windows build (build_lto.bat).'
    }
    $info.Text = $lines -join "`r`n`r`n"
}
foreach ($r in $radios.Values) { $r.Add_CheckedChanged({ Update-Info }) }
$launcherOnly.Add_CheckedChanged({ Update-Info })
Update-Info

$makeBtn = New-Object System.Windows.Forms.Button
$makeBtn.Text = 'Make release'
$makeBtn.Size = New-Object System.Drawing.Size(130, 34)
$makeBtn.Location = New-Object System.Drawing.Point(16, 182)
$form.Controls.Add($makeBtn)
$form.AcceptButton = $makeBtn

$stopBtn = New-Object System.Windows.Forms.Button
$stopBtn.Text = 'Stop'
$stopBtn.Size = New-Object System.Drawing.Size(90, 34)
$stopBtn.Location = New-Object System.Drawing.Point(156, 182)
$stopBtn.Enabled = $false
$form.Controls.Add($stopBtn)

$openBtn = New-Object System.Windows.Forms.Button
$openBtn.Text = 'Open release folder'
$openBtn.Size = New-Object System.Drawing.Size(160, 34)
$openBtn.Location = New-Object System.Drawing.Point(256, 182)
$form.Controls.Add($openBtn)

$status = New-Object System.Windows.Forms.Label
$status.AutoSize = $false
$status.Location = New-Object System.Drawing.Point(430, 190)
$status.Size = New-Object System.Drawing.Size(314, 24)
$status.Anchor = 'Top, Left, Right'
$status.Text = 'Ready.'
$form.Controls.Add($status)

$log = New-Object System.Windows.Forms.TextBox
$log.Multiline = $true
$log.ReadOnly = $true
$log.ScrollBars = 'Both'
$log.WordWrap = $false
$log.Font = New-Object System.Drawing.Font('Consolas', 9)
$log.BackColor = [System.Drawing.Color]::White
$log.Location = New-Object System.Drawing.Point(16, 228)
$log.Size = New-Object System.Drawing.Size(728, 356)
$log.Anchor = 'Top, Bottom, Left, Right'
$form.Controls.Add($log)

# ---- Running make_release.ps1 ----------------------------------------------
$script:proc = $null
$script:reader = $null
$script:linuxPart = $false
$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 300

function Set-Running([bool]$running) {
    $makeBtn.Enabled = -not $running
    $stopBtn.Enabled = $running
    $group.Enabled = -not $running
    $version.Enabled = -not $running
    $test.Enabled = (-not $running) -and [bool]$rom -and -not $launcherOnly.Checked
    $launcherOnly.Enabled = -not $running
}

function Read-Log {
    if (-not $script:reader) { return }
    $text = $script:reader.ReadToEnd()
    if ($text) {
        # Keep the box responsive on long builds: the last ~400 KB only.
        if ($log.TextLength + $text.Length -gt 400000) {
            $log.Text = $log.Text.Substring([Math]::Max(0, $log.TextLength - 200000))
        }
        $log.AppendText(($text -replace "`r?`n", "`r`n"))
    }
}

$timer.Add_Tick({
    Read-Log
    if ($script:proc -and $script:proc.HasExited) {
        $timer.Stop()
        Read-Log
        $code = $script:proc.ExitCode
        $script:reader.Close(); $script:reader = $null
        $script:proc = $null
        Set-Running $false
        if ($code -eq 0) {
            $status.ForeColor = [System.Drawing.Color]::DarkGreen
            $status.Text = 'Release ready.'
        } else {
            $status.ForeColor = [System.Drawing.Color]::DarkRed
            $status.Text = "Stopped (exit code $code). See the log."
        }
    }
})

$makeBtn.Add_Click({
    $busy = Get-Process -Name ninja, cc1plus, lto1, ld, collect2, mingw32-make -ErrorAction SilentlyContinue
    if ($busy) {
        [void][System.Windows.Forms.MessageBox]::Show($form,
            'Another build is running. Wait for it to finish first.', 'Make release', 'OK', 'Warning')
        return
    }
    $target = @('Windows', 'Linux', 'Both') | Where-Object { $radios[$_].Checked } | Select-Object -First 1
    try {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $settingsPath) | Out-Null
        @{ target = $target; test = $test.Checked; version = $version.Text.Trim() } | ConvertTo-Json | Set-Content -LiteralPath $settingsPath -Encoding ascii
    } catch { }

    $switches = @()
    if ($target -eq 'Windows') { $switches += '-NoLinux' }
    if ($target -eq 'Linux') { $switches += '-NoWindows' }
    if (-not $test.Checked) { $switches += '-NoTest' }
    if ($launcherOnly.Checked) { $switches += '-LauncherOnly' }
    $versionText = $version.Text.Trim()
    if ($versionText -and $versionText -notmatch '^[A-Za-z0-9._-]+$') {
        [void][System.Windows.Forms.MessageBox]::Show($form,
            "The version may only use letters, digits, '.', '-' and '_' (for example v0.2-test).",
            'Make release', 'OK', 'Warning')
        return
    }
    if (-not $versionText) {
        $answer = [System.Windows.Forms.MessageBox]::Show($form,
            "No version filled in. The launchers will then never tell players about updates.`r`n`r`nMake the release anyway?",
            'Make release', 'YesNo', 'Question')
        if ($answer -ne 'Yes') { return }
    } else {
        $switches += "-Version $versionText"
    }
    $script:linuxPart = $target -ne 'Windows'

    New-Item -ItemType Directory -Force -Path $releaseRoot | Out-Null
    Set-Content -LiteralPath $logPath -Value '' -Encoding ascii
    $log.Clear()
    $ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $releaseScript = Join-Path $PSScriptRoot 'make_release.ps1'
    $inner = '"{0}" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "{1}" {2} > "{3}" 2>&1' -f `
        $ps, $releaseScript, ($switches -join ' '), $logPath
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = Join-Path $env:SystemRoot 'System32\cmd.exe'
    $psi.Arguments = '/d /s /c "' + $inner + '"'
    $psi.WorkingDirectory = $repo
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $script:proc = [System.Diagnostics.Process]::Start($psi)
    $fs = New-Object System.IO.FileStream($logPath, 'Open', 'Read', 'ReadWrite')
    $script:reader = New-Object System.IO.StreamReader($fs, [System.Text.Encoding]::UTF8)

    $status.ForeColor = [System.Drawing.Color]::Black
    $status.Text = @{ Windows = 'Making the Windows release...'; Linux = 'Making the Linux release...';
                      Both = 'Making the Windows and Linux releases...' }[$target]
    Set-Running $true
    $timer.Start()
})

function Stop-Release {
    if (-not $script:proc) { return }
    & (Join-Path $env:SystemRoot 'System32\taskkill.exe') /T /F /PID $script:proc.Id 2>$null | Out-Null
    if ($script:linuxPart -and $haveWsl) {
        # Ending wsl.exe does not end what it started inside Linux.
        & $wsl -d $distro -u root --exec pkill -f -e 'make_release_linux|gsr_builder|ninja' 2>$null | Out-Null
    }
}

$stopBtn.Add_Click({
    $answer = [System.Windows.Forms.MessageBox]::Show($form,
        "Stop the release build?`r`n`r`nStopping while the Windows engine links can leave a broken GoldenSunRecomp.exe in build\gs011_opt; build_lto.bat makes it again.",
        'Make release', 'YesNo', 'Warning')
    if ($answer -eq 'Yes') { Stop-Release }
})

$openBtn.Add_Click({
    New-Item -ItemType Directory -Force -Path $releaseRoot | Out-Null
    Start-Process explorer.exe -ArgumentList "`"$releaseRoot`""
})

$form.Add_FormClosing({
    param($sender, $e)
    if ($script:proc -and -not $script:proc.HasExited) {
        $answer = [System.Windows.Forms.MessageBox]::Show($form,
            'A release is still being made. Stop it and close?', 'Make release', 'YesNo', 'Warning')
        if ($answer -ne 'Yes') { $e.Cancel = $true; return }
        Stop-Release
    }
    $timer.Stop()
})

[void]$form.ShowDialog()
