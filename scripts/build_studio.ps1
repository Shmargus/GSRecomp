# Build Studio: one window for every developer build, regeneration and
# release script. Opened by "Build Studio.bat". It only starts the existing scripts; they keep their own checks.
param(
    [ValidateSet('Games', 'Regenerate', 'Releases', 'Extras')][string]$Page = ''
)
$ErrorActionPreference = 'Stop'
# The window starts hidden, so an error would otherwise vanish: log it and say so.
trap {
    $text = "$_`r`n$($_.InvocationInfo.PositionMessage)`r`n$($_.ScriptStackTrace)"
    try {
        $dir = Join-Path (Split-Path -Parent $PSScriptRoot) 'local/build-studio'
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        Set-Content -LiteralPath (Join-Path $dir 'startup-error.log') -Value $text -Encoding utf8
    } catch { }
    try {
        Add-Type -AssemblyName PresentationFramework
        [void][Windows.MessageBox]::Show("Build Studio could not start:`r`n`r`n$text", 'Build Studio', 'OK', 'Error')
    } catch { }
    exit 1
}
Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase, System.Xaml
Add-Type -Namespace Gsr -Name Dwm -MemberDefinition @'
[DllImport("dwmapi.dll")] public static extern int DwmSetWindowAttribute(System.IntPtr hwnd, int attr, ref int value, int size);
[DllImport("user32.dll")] public static extern bool ShowWindow(System.IntPtr hwnd, int cmd);
'@

$collectionRoot = Split-Path -Parent $PSScriptRoot
$releaseRoot = Join-Path $collectionRoot 'Releases'
$logRoot = Join-Path $collectionRoot 'local/build-studio'
$settingsPath = Join-Path $collectionRoot 'local/build_studio.json'
$oldReleaseSettings = Join-Path $collectionRoot 'local/release_tool.json'
$ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$cmdExe = Join-Path $env:SystemRoot 'System32\cmd.exe'
$wsl = Join-Path $env:SystemRoot 'System32\wsl.exe'
$distro = 'Ubuntu-24.04'
. (Join-Path $PSScriptRoot 'build_guard.ps1')

# One window at a time; a second copy would show a stale log.
$windowMutex = [Threading.Mutex]::new($false, 'Local\GoldenSunBuildStudio')
try { $owned = $windowMutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $owned = $true }
if (-not $owned) {
    [void][Windows.MessageBox]::Show('Build Studio is already open.', 'Build Studio', 'OK', 'Information')
    exit 0
}

# ---------------------------------------------------------------- environment
$haveWsl = $false
if (Test-Path -LiteralPath $wsl) {
    try {
        $names = (& $wsl --list --quiet 2>$null) -replace "\0", ''
        $haveWsl = [bool]($names | Where-Object { $_.Trim() -eq $distro })
    } catch { }
}
$haveTestRoms = $true
foreach ($game in @('The Broken Seal', 'The Lost Age')) {
    $gameRepo = Join-Path $collectionRoot $game
    $haveRom = $false
    try {
        $local = Get-Content -Raw -LiteralPath (Join-Path $gameRepo 'config/local.json') | ConvertFrom-Json
        $candidate = [string]$local.rom
        if ($candidate -and -not [IO.Path]::IsPathRooted($candidate)) { $candidate = Join-Path $gameRepo $candidate }
        $haveRom = $candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)
    } catch { }
    if (-not $haveRom) { $haveRom = @(Get-ChildItem -LiteralPath $gameRepo -File -Filter '*.gba').Count -eq 1 }
    $haveTestRoms = $haveTestRoms -and $haveRom
}

# ------------------------------------------------------------------- settings
$settings = [ordered]@{
    page = 'Games'; game = 'Both'; light = $false; regen = 'Both'
    platforms = @('Windows', 'Linux'); test = $true; version = ''
}
foreach ($path in @($oldReleaseSettings, $settingsPath)) {
    try {
        $saved = Get-Content -Raw -LiteralPath $path | ConvertFrom-Json
        foreach ($p in $saved.PSObject.Properties) {
            if ($settings.Contains($p.Name)) { $settings[$p.Name] = $p.Value }
        }
    } catch { }
}
if ($Page) { $settings.page = $Page }

function Save-Settings {
    try {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $settingsPath) | Out-Null
        [pscustomobject]$settings | ConvertTo-Json | Set-Content -LiteralPath $settingsPath -Encoding ascii
    } catch { }
}

# ------------------------------------------------------------------------ UI
[xml]$xaml = @'
<Window xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        Title="Build Studio" Width="1080" Height="740" MinWidth="900" MinHeight="620"
        WindowStartupLocation="CenterScreen" Background="#121316"
        FontFamily="Segoe UI Variable Text, Segoe UI" FontSize="13" Foreground="#E6E4DF"
        TextOptions.TextFormattingMode="Display" UseLayoutRounding="True">
  <Window.Resources>
    <SolidColorBrush x:Key="Accent" Color="#D6A23E"/>
    <SolidColorBrush x:Key="Muted" Color="#8C8F98"/>
    <SolidColorBrush x:Key="Line" Color="#2A2C32"/>
    <SolidColorBrush x:Key="Surface" Color="#1A1B1F"/>
    <FontFamily x:Key="Icons">Segoe Fluent Icons, Segoe MDL2 Assets</FontFamily>

    <Style x:Key="Primary" TargetType="Button">
      <Setter Property="Foreground" Value="#17130B"/>
      <Setter Property="Background" Value="{StaticResource Accent}"/>
      <Setter Property="FontWeight" Value="SemiBold"/>
      <Setter Property="Padding" Value="20,9"/>
      <Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="Button">
            <Border x:Name="b" Background="{TemplateBinding Background}" CornerRadius="6" Padding="{TemplateBinding Padding}">
              <ContentPresenter HorizontalAlignment="Center" VerticalAlignment="Center"/>
            </Border>
            <ControlTemplate.Triggers>
              <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="b" Property="Background" Value="#E3B357"/></Trigger>
              <Trigger Property="IsPressed" Value="True"><Setter TargetName="b" Property="Background" Value="#BF8F33"/></Trigger>
              <Trigger Property="IsEnabled" Value="False">
                <Setter TargetName="b" Property="Background" Value="#26282D"/>
                <Setter Property="Foreground" Value="#62656C"/>
              </Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>

    <Style x:Key="Secondary" TargetType="Button">
      <Setter Property="Foreground" Value="#E6E4DF"/>
      <Setter Property="Padding" Value="14,7"/>
      <Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="Button">
            <Border x:Name="b" Background="Transparent" BorderBrush="{StaticResource Line}" BorderThickness="1" CornerRadius="6" Padding="{TemplateBinding Padding}">
              <ContentPresenter HorizontalAlignment="Center" VerticalAlignment="Center"/>
            </Border>
            <ControlTemplate.Triggers>
              <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="b" Property="Background" Value="#22242A"/></Trigger>
              <Trigger Property="IsEnabled" Value="False"><Setter Property="Opacity" Value="0.35"/></Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>

    <Style x:Key="Link" TargetType="Button">
      <Setter Property="Foreground" Value="{StaticResource Muted}"/>
      <Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="Button">
            <Border x:Name="b" Background="Transparent" CornerRadius="6" Padding="12,7">
              <ContentPresenter HorizontalAlignment="Left"/>
            </Border>
            <ControlTemplate.Triggers>
              <Trigger Property="IsMouseOver" Value="True">
                <Setter TargetName="b" Property="Background" Value="#1E1F24"/>
                <Setter Property="Foreground" Value="#E6E4DF"/>
              </Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>

    <Style x:Key="Nav" TargetType="RadioButton">
      <Setter Property="Foreground" Value="#A9ABB2"/>
      <Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Margin" Value="0,1"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="RadioButton">
            <Border x:Name="b" Background="Transparent" CornerRadius="6" Padding="12,8">
              <Grid>
                <Border x:Name="mark" Width="3" Height="14" CornerRadius="1.5" Background="{StaticResource Accent}"
                        HorizontalAlignment="Left" Margin="-12,0,0,0" Visibility="Hidden"/>
                <StackPanel Orientation="Horizontal">
                  <TextBlock Text="{TemplateBinding Tag}" FontFamily="{StaticResource Icons}" FontSize="15" Width="18" VerticalAlignment="Center"/>
                  <ContentPresenter Margin="12,0,0,0" VerticalAlignment="Center"/>
                </StackPanel>
              </Grid>
            </Border>
            <ControlTemplate.Triggers>
              <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="b" Property="Background" Value="#1E1F24"/></Trigger>
              <Trigger Property="IsChecked" Value="True">
                <Setter TargetName="b" Property="Background" Value="#232429"/>
                <Setter TargetName="mark" Property="Visibility" Value="Visible"/>
                <Setter Property="Foreground" Value="#F1EFEA"/>
              </Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>

    <Style x:Key="Segment" TargetType="RadioButton">
      <Setter Property="Foreground" Value="#A9ABB2"/>
      <Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="RadioButton">
            <Border x:Name="b" Background="Transparent" CornerRadius="5" Padding="14,6" Margin="2">
              <ContentPresenter HorizontalAlignment="Center"/>
            </Border>
            <ControlTemplate.Triggers>
              <Trigger Property="IsMouseOver" Value="True"><Setter Property="Foreground" Value="#E6E4DF"/></Trigger>
              <Trigger Property="IsChecked" Value="True">
                <Setter TargetName="b" Property="Background" Value="#2C2E35"/>
                <Setter Property="Foreground" Value="#F1EFEA"/>
              </Trigger>
              <Trigger Property="IsEnabled" Value="False"><Setter Property="Opacity" Value="0.4"/></Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>

    <Style TargetType="CheckBox">
      <Setter Property="Foreground" Value="#E6E4DF"/>
      <Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="CheckBox">
            <StackPanel Orientation="Horizontal" Background="Transparent">
              <Border x:Name="box" Width="17" Height="17" CornerRadius="4" BorderThickness="1.5"
                      BorderBrush="#4A4D55" Background="Transparent" VerticalAlignment="Top" Margin="0,1,0,0">
                <Path x:Name="tick" Data="M 3.5 8.5 L 7 12 L 13.5 4.5" Stroke="#17130B" StrokeThickness="2"
                      StrokeStartLineCap="Round" StrokeEndLineCap="Round" StrokeLineJoin="Round" Visibility="Hidden"
                      Margin="-1,-1,0,0"/>
              </Border>
              <ContentPresenter Margin="10,0,0,0" VerticalAlignment="Top"/>
            </StackPanel>
            <ControlTemplate.Triggers>
              <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="box" Property="BorderBrush" Value="#7A7D85"/></Trigger>
              <Trigger Property="IsChecked" Value="True">
                <Setter TargetName="box" Property="Background" Value="{StaticResource Accent}"/>
                <Setter TargetName="box" Property="BorderBrush" Value="{StaticResource Accent}"/>
                <Setter TargetName="tick" Property="Visibility" Value="Visible"/>
              </Trigger>
              <Trigger Property="IsEnabled" Value="False"><Setter Property="Opacity" Value="0.38"/></Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>

    <Style x:Key="Field" TargetType="TextBox">
      <Setter Property="Foreground" Value="#E6E4DF"/>
      <Setter Property="CaretBrush" Value="#E6E4DF"/>
      <Setter Property="SelectionBrush" Value="{StaticResource Accent}"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="TextBox">
            <Border x:Name="b" Background="#141518" BorderBrush="{StaticResource Line}" BorderThickness="1" CornerRadius="6" Padding="8,6">
              <ScrollViewer x:Name="PART_ContentHost"/>
            </Border>
            <ControlTemplate.Triggers>
              <Trigger Property="IsKeyboardFocused" Value="True"><Setter TargetName="b" Property="BorderBrush" Value="{StaticResource Accent}"/></Trigger>
              <Trigger Property="IsEnabled" Value="False"><Setter Property="Opacity" Value="0.4"/></Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>

    <Style TargetType="ScrollBar">
      <Setter Property="Width" Value="10"/>
      <Setter Property="MinWidth" Value="10"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="ScrollBar">
            <Track x:Name="PART_Track" IsDirectionReversed="True">
              <Track.Thumb>
                <Thumb>
                  <Thumb.Template>
                    <ControlTemplate TargetType="Thumb"><Border Background="#34363D" CornerRadius="3" Margin="2"/></ControlTemplate>
                  </Thumb.Template>
                </Thumb>
              </Track.Thumb>
            </Track>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>

    <Style x:Key="H1" TargetType="TextBlock">
      <Setter Property="FontFamily" Value="Segoe UI Variable Display, Segoe UI"/>
      <Setter Property="FontSize" Value="24"/>
      <Setter Property="FontWeight" Value="SemiBold"/>
      <Setter Property="Foreground" Value="#F1EFEA"/>
    </Style>
    <Style x:Key="Lead" TargetType="TextBlock">
      <Setter Property="Foreground" Value="{StaticResource Muted}"/>
      <Setter Property="TextWrapping" Value="Wrap"/>
      <Setter Property="Margin" Value="0,6,0,22"/>
      <Setter Property="MaxWidth" Value="620"/>
      <Setter Property="HorizontalAlignment" Value="Left"/>
    </Style>
    <Style x:Key="Label" TargetType="TextBlock">
      <Setter Property="FontSize" Value="11"/>
      <Setter Property="FontWeight" Value="SemiBold"/>
      <Setter Property="Foreground" Value="#73767E"/>
      <Setter Property="Margin" Value="0,0,0,8"/>
    </Style>
    <Style x:Key="Hint" TargetType="TextBlock">
      <Setter Property="Foreground" Value="{StaticResource Muted}"/>
      <Setter Property="FontSize" Value="12"/>
      <Setter Property="TextWrapping" Value="Wrap"/>
      <Setter Property="Margin" Value="27,3,0,0"/>
    </Style>
    <Style x:Key="Card" TargetType="Border">
      <Setter Property="Background" Value="{StaticResource Surface}"/>
      <Setter Property="BorderBrush" Value="{StaticResource Line}"/>
      <Setter Property="BorderThickness" Value="1"/>
      <Setter Property="CornerRadius" Value="8"/>
      <Setter Property="Padding" Value="18,16"/>
    </Style>
    <Style x:Key="Track" TargetType="Border">
      <Setter Property="Background" Value="#17181C"/>
      <Setter Property="BorderBrush" Value="{StaticResource Line}"/>
      <Setter Property="BorderThickness" Value="1"/>
      <Setter Property="CornerRadius" Value="7"/>
      <Setter Property="HorizontalAlignment" Value="Left"/>
    </Style>
  </Window.Resources>

  <Grid>
    <Grid.ColumnDefinitions>
      <ColumnDefinition Width="232"/>
      <ColumnDefinition Width="*"/>
    </Grid.ColumnDefinitions>

    <!-- Sidebar -->
    <Border Background="#16171A" BorderBrush="#222328" BorderThickness="0,0,1,0">
      <DockPanel Margin="14,20,14,16">
        <StackPanel DockPanel.Dock="Top" Margin="10,0,0,26">
          <TextBlock Text="Build Studio" FontFamily="Segoe UI Variable Display, Segoe UI" FontSize="17" FontWeight="SemiBold" Foreground="#F1EFEA"/>
          <TextBlock Text="Golden Sun Recompiled" Foreground="{StaticResource Muted}" FontSize="12" Margin="0,2,0,0"/>
        </StackPanel>
        <StackPanel DockPanel.Dock="Bottom">
          <Button x:Name="OpenReleases" Style="{StaticResource Link}">
            <StackPanel Orientation="Horizontal">
              <TextBlock Text="&#xE8B7;" FontFamily="{StaticResource Icons}" Width="18" VerticalAlignment="Center"/>
              <TextBlock Text="Releases folder" Margin="12,0,0,0"/>
            </StackPanel>
          </Button>
          <Button x:Name="OpenLogs" Style="{StaticResource Link}">
            <StackPanel Orientation="Horizontal">
              <TextBlock Text="&#xE9F9;" FontFamily="{StaticResource Icons}" Width="18" VerticalAlignment="Center"/>
              <TextBlock Text="Build logs" Margin="12,0,0,0"/>
            </StackPanel>
          </Button>
        </StackPanel>
        <StackPanel>
          <RadioButton x:Name="NavGames" GroupName="nav" Style="{StaticResource Nav}" Tag="&#xE7FC;" Content="Developer Launcher"/>
          <RadioButton x:Name="NavRegenerate" GroupName="nav" Style="{StaticResource Nav}" Tag="&#xE943;" Content="Regenerate code"/>
          <RadioButton x:Name="NavReleases" GroupName="nav" Style="{StaticResource Nav}" Tag="&#xE7B8;" Content="Releases"/>
          <RadioButton x:Name="NavExtras" GroupName="nav" Style="{StaticResource Nav}" Tag="&#xE8EA;" Content="Launcher &amp; Android"/>
        </StackPanel>
      </DockPanel>
    </Border>

    <!-- Main -->
    <Grid Grid.Column="1" Margin="40,32,40,28">
      <Grid.RowDefinitions>
        <RowDefinition Height="Auto"/>
        <RowDefinition Height="Auto"/>
        <RowDefinition Height="*"/>
      </Grid.RowDefinitions>

      <Grid x:Name="Pages">
        <!-- Games -->
        <StackPanel x:Name="PageGames">
          <TextBlock Style="{StaticResource H1}" Text="Developer Launcher"/>
          <TextBlock Style="{StaticResource Lead}" Text="Builds the developer games and the launcher that runs them. Close the launcher and any running game first."/>
          <TextBlock Style="{StaticResource Label}" Text="WHAT TO BUILD"/>
          <Border Style="{StaticResource Track}">
            <StackPanel Orientation="Horizontal">
              <RadioButton x:Name="GameBoth" GroupName="game" Style="{StaticResource Segment}" Content="Both games"/>
              <RadioButton x:Name="GameBrokenSeal" GroupName="game" Style="{StaticResource Segment}" Content="The Broken Seal"/>
              <RadioButton x:Name="GameLostAge" GroupName="game" Style="{StaticResource Segment}" Content="The Lost Age"/>
            </StackPanel>
          </Border>
          <Grid Margin="0,18,0,0" MaxWidth="620" HorizontalAlignment="Left">
            <Grid.ColumnDefinitions><ColumnDefinition Width="180"/><ColumnDefinition Width="180"/><ColumnDefinition Width="180"/></Grid.ColumnDefinitions>
            <StackPanel><TextBlock Text="The Broken Seal" Foreground="{StaticResource Muted}" FontSize="12"/><TextBlock x:Name="BuiltBrokenSeal" Margin="0,3,0,0"/></StackPanel>
            <StackPanel Grid.Column="1"><TextBlock Text="The Lost Age" Foreground="{StaticResource Muted}" FontSize="12"/><TextBlock x:Name="BuiltLostAge" Margin="0,3,0,0"/></StackPanel>
            <StackPanel Grid.Column="2"><TextBlock Text="Launcher" Foreground="{StaticResource Muted}" FontSize="12"/><TextBlock x:Name="BuiltLauncher" Margin="0,3,0,0"/></StackPanel>
          </Grid>
          <StackPanel Margin="0,22,0,0">
            <CheckBox x:Name="Light" Content="Use fewer system resources"/>
            <TextBlock Style="{StaticResource Hint}" Text="Caps the build at half your processor and memory. Slower, and a build that needs more memory will fail."/>
            <CheckBox x:Name="GamesCheckOnly" Content="Only check the tools" Margin="0,12,0,0"/>
            <TextBlock Style="{StaticResource Hint}" Text="Builds and changes nothing."/>
          </StackPanel>
          <Button x:Name="RunGames" Style="{StaticResource Primary}" Content="Build" HorizontalAlignment="Left" Margin="0,24,0,0"/>
        </StackPanel>

        <!-- Regenerate -->
        <StackPanel x:Name="PageRegenerate">
          <TextBlock Style="{StaticResource H1}" Text="Regenerate code"/>
          <TextBlock Style="{StaticResource Lead}" Text="Translates the ROM into game code again. Needed after a ROM-configuration or translator change; build the game afterwards."/>
          <TextBlock Style="{StaticResource Label}" Text="WHICH GAME"/>
          <Border Style="{StaticResource Track}">
            <StackPanel Orientation="Horizontal">
              <RadioButton x:Name="RegenBoth" GroupName="regen" Style="{StaticResource Segment}" Content="Both games"/>
              <RadioButton x:Name="RegenBrokenSeal" GroupName="regen" Style="{StaticResource Segment}" Content="The Broken Seal"/>
              <RadioButton x:Name="RegenLostAge" GroupName="regen" Style="{StaticResource Segment}" Content="The Lost Age"/>
            </StackPanel>
          </Border>
          <Button x:Name="RunRegen" Style="{StaticResource Primary}" Content="Regenerate" HorizontalAlignment="Left" Margin="0,24,0,0"/>
        </StackPanel>

        <!-- Releases -->
        <StackPanel x:Name="PageReleases">
          <TextBlock Style="{StaticResource H1}" Text="Releases"/>
          <TextBlock Style="{StaticResource Lead}" Text="Player packages for both games, written to the Releases folder."/>
          <TextBlock Style="{StaticResource Label}" Text="PLATFORMS"/>
          <Grid MaxWidth="620" HorizontalAlignment="Left">
            <Grid.ColumnDefinitions><ColumnDefinition Width="300"/><ColumnDefinition Width="300"/></Grid.ColumnDefinitions>
            <Grid.RowDefinitions><RowDefinition/><RowDefinition/></Grid.RowDefinitions>
            <StackPanel Margin="0,0,0,14"><CheckBox x:Name="PlatWindows" Content="Windows x64"/><TextBlock x:Name="HintWindows" Style="{StaticResource Hint}"/></StackPanel>
            <StackPanel Grid.Column="1"><CheckBox x:Name="PlatLinux" Content="Linux x64"/><TextBlock x:Name="HintLinux" Style="{StaticResource Hint}"/></StackPanel>
            <StackPanel Grid.Row="1"><CheckBox x:Name="PlatArm64" Content="Linux ARM64"/><TextBlock x:Name="HintArm64" Style="{StaticResource Hint}"/></StackPanel>
            <StackPanel Grid.Row="1" Grid.Column="1"><CheckBox x:Name="PlatAndroid" Content="Android"/><TextBlock x:Name="HintAndroid" Style="{StaticResource Hint}"/></StackPanel>
          </Grid>
          <TextBlock Style="{StaticResource Label}" Text="VERSION" Margin="0,20,0,8"/>
          <TextBox x:Name="Version" Style="{StaticResource Field}" Width="220" HorizontalAlignment="Left"/>
          <StackPanel Margin="0,18,0,0">
            <CheckBox x:Name="TestBuilders" Content="Test both packaged game builders"/>
            <TextBlock x:Name="HintTest" Style="{StaticResource Hint}"/>
            <WrapPanel Margin="0,12,0,0">
              <CheckBox x:Name="LauncherOnly" Content="Launcher only" Margin="0,0,28,0"/>
              <CheckBox x:Name="Ashley" Content="Ashley Edition (desktop)"/>
            </WrapPanel>
          </StackPanel>
          <Button x:Name="RunRelease" Style="{StaticResource Primary}" Content="Make release" HorizontalAlignment="Left" Margin="0,24,0,0"/>
        </StackPanel>

        <!-- Launcher & Android -->
        <StackPanel x:Name="PageExtras">
          <TextBlock Style="{StaticResource H1}" Text="Launcher &amp; Android"/>
          <TextBlock Style="{StaticResource Lead}" Text="Builds that leave the games alone."/>
          <Grid MaxWidth="720" HorizontalAlignment="Left">
            <Grid.ColumnDefinitions><ColumnDefinition Width="*"/><ColumnDefinition Width="16"/><ColumnDefinition Width="*"/></Grid.ColumnDefinitions>
            <Border Style="{StaticResource Card}">
              <DockPanel>
                <TextBlock DockPanel.Dock="Top" Text="Launcher" FontSize="15" FontWeight="SemiBold"/>
                <Button x:Name="RunLauncher" DockPanel.Dock="Bottom" Style="{StaticResource Primary}" Content="Rebuild launcher" HorizontalAlignment="Left" Margin="0,18,0,0"/>
                <TextBlock Style="{StaticResource Hint}" Margin="0,6,0,0" Text="Rebuilds GoldenSunLauncher.exe after launcher changes. Close the launcher first."/>
              </DockPanel>
            </Border>
            <Border Grid.Column="2" Style="{StaticResource Card}">
              <DockPanel>
                <TextBlock DockPanel.Dock="Top" Text="Android app" FontSize="15" FontWeight="SemiBold"/>
                <Button x:Name="RunAndroid" DockPanel.Dock="Bottom" Style="{StaticResource Primary}" Content="Build app" HorizontalAlignment="Left" Margin="0,18,0,0"/>
                <CheckBox x:Name="AndroidCheckOnly" DockPanel.Dock="Bottom" Content="Only check the Android tools" Margin="0,14,0,0"/>
                <TextBlock Style="{StaticResource Hint}" Margin="0,6,0,0" Text="Sideload test copy in Releases\android. Use Releases for one to share."/>
              </DockPanel>
            </Border>
          </Grid>
        </StackPanel>
      </Grid>

      <!-- Run status -->
      <Grid Grid.Row="1" Margin="0,34,0,12">
        <Grid.ColumnDefinitions><ColumnDefinition Width="*"/><ColumnDefinition Width="Auto"/></Grid.ColumnDefinitions>
        <StackPanel VerticalAlignment="Center">
          <StackPanel Orientation="Horizontal">
            <Ellipse x:Name="Dot" Width="8" Height="8" Fill="#4A4D55" VerticalAlignment="Center" Margin="0,1,10,0"/>
            <TextBlock x:Name="Status" Text="Idle" FontSize="15" FontWeight="SemiBold" TextTrimming="CharacterEllipsis"/>
          </StackPanel>
          <TextBlock x:Name="Detail" Foreground="{StaticResource Muted}" FontSize="12" Margin="18,3,0,0" TextTrimming="CharacterEllipsis"/>
        </StackPanel>
        <Button x:Name="Stop" Grid.Column="1" Style="{StaticResource Secondary}" Content="Stop" VerticalAlignment="Center"/>
      </Grid>

      <Grid Grid.Row="2">
        <Grid.RowDefinitions><RowDefinition Height="Auto"/><RowDefinition Height="*"/></Grid.RowDefinitions>
        <ProgressBar x:Name="Progress" Height="3" Minimum="0" Maximum="100" Foreground="{StaticResource Accent}" Background="#222328" BorderThickness="0"/>
        <Border Grid.Row="1" Background="#0D0E10" BorderBrush="#202126" BorderThickness="1,0,1,1" CornerRadius="0,0,8,8">
          <TextBox x:Name="Log" IsReadOnly="True" Background="Transparent" BorderThickness="0" Foreground="#B9BBC1"
                   FontFamily="Cascadia Mono, Consolas" FontSize="12" Padding="14,12" TextWrapping="Wrap"
                   VerticalScrollBarVisibility="Auto" HorizontalScrollBarVisibility="Disabled"
                   SelectionBrush="{StaticResource Accent}" CaretBrush="Transparent"/>
        </Border>
      </Grid>
    </Grid>
  </Grid>
</Window>
'@

$window = [Windows.Markup.XamlReader]::Load([Xml.XmlNodeReader]::new($xaml))
$ui = @{}
$xaml.SelectNodes('//*[@*[local-name()="Name"]]') | ForEach-Object {
    $name = $_.GetAttribute('Name', 'http://schemas.microsoft.com/winfx/2006/xaml')
    if ($name) { $ui[$name] = $window.FindName($name) }
}

# Dark title bar on Windows 10 2004+ / 11.
$window.Add_SourceInitialized({
    $hwnd = [Windows.Interop.WindowInteropHelper]::new($window).Handle
    $on = 1
    [void][Gsr.Dwm]::DwmSetWindowAttribute($hwnd, 20, [ref]$on, 4)
})
# Started hidden (no console), and Windows applies that to our first window too:
# show it again explicitly and bring it to the front.
$window.Add_ContentRendered({
    $hwnd = [Windows.Interop.WindowInteropHelper]::new($window).Handle
    [void][Gsr.Dwm]::ShowWindow($hwnd, 5)
    [void]$window.Activate()
})

$brush = { param($hex) [Windows.Media.BrushConverter]::new().ConvertFromString($hex) }
$colours = @{ idle = '#4A4D55'; busy = '#D6A23E'; ok = '#5FB37A'; fail = '#E0645C' }

# --------------------------------------------------------------- page state
$pages = 'Games', 'Regenerate', 'Releases', 'Extras'
function Show-Page([string]$name) {
    foreach ($p in $pages) {
        $ui["Page$p"].Visibility = if ($p -eq $name) { 'Visible' } else { 'Collapsed' }
    }
    $settings.page = $name
}
foreach ($p in $pages) { $ui["Nav$p"].Add_Checked({ Show-Page $this.Name.Substring(3) }) }
if ($settings.page -notin $pages) { $settings.page = 'Games' }
$ui["Nav$($settings.page)"].IsChecked = $true
Show-Page $settings.page

function Format-Built([string]$path) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return 'Not built yet' }
    $item = Get-Item -LiteralPath $path
    if ($item.Length -eq 0) { return 'Broken (empty file)' }
    return 'Built ' + $item.LastWriteTime.ToString('d MMM, HH:mm')
}
function Update-Built {
    $ui.BuiltBrokenSeal.Text = Format-Built (Join-Path $collectionRoot "The Broken Seal/build/gs011_opt/GoldenSunRecomp.exe")
    $ui.BuiltLostAge.Text = Format-Built (Join-Path $collectionRoot "The Lost Age/build/tla/TLARecomp.exe")
    $ui.BuiltLauncher.Text = Format-Built (Join-Path $collectionRoot 'GoldenSunLauncher.exe')
}
Update-Built

$ui["Game$($settings.game)"].IsChecked = $true
if (-not ($ui.GameBoth.IsChecked -or $ui.GameBrokenSeal.IsChecked -or $ui.GameLostAge.IsChecked)) { $ui.GameBoth.IsChecked = $true }
$ui["Regen$($settings.regen)"].IsChecked = $true
if (-not ($ui.RegenBoth.IsChecked -or $ui.RegenBrokenSeal.IsChecked -or $ui.RegenLostAge.IsChecked)) { $ui.RegenBoth.IsChecked = $true }
$ui.Light.IsChecked = [bool]$settings.light
$ui.TestBuilders.IsChecked = [bool]$settings.test
$ui.Version.Text = [string]$settings.version

$platforms = [ordered]@{
    Windows = 'Native Windows ZIP'
    Linux   = 'Built in WSL Ubuntu 24.04'
    Arm64   = 'Emulated in WSL; takes longer'
    Android = 'Signed APK'
}
foreach ($key in $platforms.Keys) {
    $ui["Plat$key"].IsChecked = $key -in @($settings.platforms)
    $hint = $platforms[$key]
    if (-not $haveWsl -and $key -in @('Linux', 'Arm64')) { $hint = "Needs WSL $distro" }
    $ui["Hint$key"].Text = $hint
}

function Get-Choice([string]$prefix) {
    foreach ($c in 'Both', 'BrokenSeal', 'LostAge') { if ($ui["$prefix$c"].IsChecked) { return $c } }
    return 'Both'
}
function Get-SelectedPlatforms {
    $platforms.Keys | Where-Object { $ui["Plat$_"].IsEnabled -and $ui["Plat$_"].IsChecked }
}

# ---------------------------------------------------------------- job state
$script:job = $null          # @{ Name; Staged; Proc; Reader; Pending; Log }
$script:stage = 0
$script:stageCount = 0
$script:phase = ''
$script:failure = ''
$script:lastLine = ''
$script:stopRequested = $false
$script:clock = [Diagnostics.Stopwatch]::new()
$sep = "  $([char]0xB7)  "

function Update-Controls {
    $idle = $null -eq $script:job
    foreach ($b in 'RunGames', 'RunRegen', 'RunRelease', 'RunLauncher', 'RunAndroid') { $ui[$b].IsEnabled = $idle }
    foreach ($c in 'GameBoth', 'GameBrokenSeal', 'GameLostAge', 'RegenBoth', 'RegenBrokenSeal', 'RegenLostAge',
                   'Light', 'GamesCheckOnly', 'Version', 'LauncherOnly', 'AndroidCheckOnly') { $ui[$c].IsEnabled = $idle }
    $ui.Light.IsEnabled = $idle -and -not $ui.GamesCheckOnly.IsChecked
    foreach ($key in $platforms.Keys) {
        $ui["Plat$key"].IsEnabled = $idle -and ($haveWsl -or $key -notin @('Linux', 'Arm64')) -and
            (-not $ui.LauncherOnly.IsChecked -or $key -ne 'Android')
    }
    $selected = @(Get-SelectedPlatforms)
    $desktop = @($selected | Where-Object { $_ -ne 'Android' })
    $ui.TestBuilders.IsEnabled = $idle -and $desktop.Count -and $haveTestRoms -and -not $ui.LauncherOnly.IsChecked
    $ui.Ashley.IsEnabled = $idle -and $desktop.Count -and -not $ui.LauncherOnly.IsChecked
    $ui.RunRelease.IsEnabled = $idle -and $selected.Count -gt 0
    $ui.HintTest.Text = if ($ui.LauncherOnly.IsChecked) { 'Packages only the desktop launcher, artwork and runtime files.' }
        elseif (-not $haveTestRoms) { 'Needs a ROM for each game.' }
        else { 'Runs each packaged builder on your ROMs.' }
    $ui.RunGames.Content = if ($ui.GamesCheckOnly.IsChecked) { 'Check tools' } else { 'Build' }
    $ui.RunAndroid.Content = if ($ui.AndroidCheckOnly.IsChecked) { 'Check tools' } else { 'Build app' }
    $ui.Stop.Visibility = if ($idle) { 'Collapsed' } else { 'Visible' }
    $ui.Stop.IsEnabled = -not $script:stopRequested
}

function Set-State([string]$kind, [string]$text, [string]$detail) {
    $ui.Dot.Fill = & $brush $colours[$kind]
    $ui.Status.Text = $text
    if ($PSBoundParameters.ContainsKey('detail')) { $ui.Detail.Text = $detail }
}

function Update-Progress {
    if (-not $script:job) { return }
    $elapsed = $script:clock.Elapsed.ToString('hh\:mm\:ss')
    if ($script:job.Staged -and $script:stageCount -gt 0) {
        $done = [Math]::Max(0, [Math]::Min($script:stageCount, $script:stage - 1))
        $ui.Progress.IsIndeterminate = $false
        $ui.Progress.Value = [Math]::Floor(100 * $done / $script:stageCount)
        $ui.Detail.Text = "$done of $($script:stageCount) stages done$sep$elapsed"
    } else {
        $ui.Detail.Text = "Running for $elapsed"
    }
}

function Read-Log([switch]$Final) {
    $j = $script:job
    if (-not $j.Reader -and (Test-Path -LiteralPath $j.Log)) {
        try {
            $fs = [IO.File]::Open($j.Log, 'Open', 'Read', 'ReadWrite')
            $j.Reader = [IO.StreamReader]::new($fs, [Text.Encoding]::UTF8)
        } catch [IO.IOException] { return }
    }
    if (-not $j.Reader) { return }
    $chunk = $j.Reader.ReadToEnd()
    if (-not $chunk -and -not $Final) { return }
    $lines = @($j.Pending + $chunk -split '\r?\n')
    $j.Pending = $lines[-1]
    if ($Final) { $j.Pending = '' } else { $lines = $lines | Select-Object -SkipLast 1 }
    $visible = foreach ($line in $lines) {
        $marker = $line.Trim()
        if ($marker -match '^@release-plan ([1-9]\d*)$') { $script:stageCount = [int]$Matches[1]; continue }
        if ($marker -match '^@release-stage (.+)$') {
            $script:stage++
            $script:phase = $Matches[1]
            if (-not $script:stopRequested) { $ui.Status.Text = $script:phase }
            continue
        }
        if ($line -match '(?:Release stopped|Android build stopped|Developer build stopped): (.+)$') { $script:failure = $Matches[1] }
        if ($line.Trim()) { $script:lastLine = $line.Trim() }
        if ($line -or -not $Final) { $line }
    }
    if ($null -ne $visible) {
        $append = ($visible -join "`r`n") + "`r`n"
        # Keep the view light on long builds; the log file has everything.
        if ($ui.Log.Text.Length + $append.Length -gt 400000) {
            $ui.Log.Text = $ui.Log.Text.Substring([Math]::Max(0, $ui.Log.Text.Length - 200000))
        }
        $ui.Log.AppendText($append)
        $ui.Log.ScrollToEnd()
    }
}

$timer = [Windows.Threading.DispatcherTimer]::new()
$timer.Interval = [TimeSpan]::FromMilliseconds(300)
$timer.Add_Tick({
    if (-not $script:job) { $timer.Stop(); return }
    Read-Log
    Update-Progress
    if (-not $script:job.Proc.HasExited) { return }
    $timer.Stop()
    Read-Log -Final
    $code = $script:job.Proc.ExitCode
    $name = $script:job.Name
    $script:clock.Stop()
    $elapsed = $script:clock.Elapsed.ToString('hh\:mm\:ss')
    $script:job.Reader.Dispose()
    $script:job.Proc.Dispose()
    $script:job = $null
    $ui.Progress.IsIndeterminate = $false
    if ($script:stopRequested) {
        $ui.Progress.Value = 0
        Set-State 'idle' "$name stopped" "After $elapsed"
    } elseif ($code -eq 0) {
        $ui.Progress.Value = 100
        Set-State 'ok' "$name finished" "Took $elapsed"
    } else {
        $ui.Progress.Value = 0
        $why = if ($script:failure) { $script:failure } elseif ($script:lastLine) { $script:lastLine } else { "Exit code $code" }
        Set-State 'fail' "$name failed" $why
    }
    Update-Built
    Update-Controls
})

function Start-Build([string]$name, [string]$command, [switch]$Staged) {
    if ($script:job) { return }
    $busy = @(Get-GsrBuildProcesses)
    if ($busy.Count) {
        $names = ($busy | Select-Object -ExpandProperty ProcessName -Unique) -join ', '
        [void][Windows.MessageBox]::Show($window, "Another build is running ($names). Wait for it to finish first.",
            'Build Studio', 'OK', 'Warning')
        return
    }
    New-Item -ItemType Directory -Force -Path $logRoot | Out-Null
    $slug = ($name -replace '[^A-Za-z0-9]+', '-').Trim('-').ToLowerInvariant()
    $log = Join-Path $logRoot ("{0}-{1}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $slug)
    [IO.File]::WriteAllText($log, '')
    try {
        $psi = [Diagnostics.ProcessStartInfo]::new()
        $psi.FileName = $cmdExe
        $psi.Arguments = '/d /s /c "(' + $command + ') > "' + $log + '" 2>&1"'
        $psi.WorkingDirectory = $collectionRoot
        $psi.UseShellExecute = $false
        $psi.CreateNoWindow = $true
        $psi.EnvironmentVariables['PYTHONUNBUFFERED'] = '1'
        $psi.EnvironmentVariables['PYTHONIOENCODING'] = 'utf-8'
        $proc = [Diagnostics.Process]::Start($psi)
    } catch {
        Set-State 'fail' "Could not start: $name" $_.Exception.Message
        return
    }
    $script:job = @{ Name = $name; Staged = [bool]$Staged; Proc = $proc; Reader = $null; Pending = ''; Log = $log }
    $script:stage = 0; $script:stageCount = 0; $script:phase = ''
    $script:failure = ''; $script:lastLine = ''; $script:stopRequested = $false
    $script:clock.Restart()
    $ui.Log.Clear()
    $ui.Progress.Value = 0
    $ui.Progress.IsIndeterminate = $true
    Set-State 'busy' $name 'Starting'
    Save-Settings
    Update-Controls
    $timer.Start()
}

function Stop-Build {
    if (-not $script:job -or $script:job.Proc.HasExited) { return }
    $script:stopRequested = $true
    Set-State 'busy' "Stopping $($script:job.Name)"
    Update-Controls
    & (Join-Path $env:SystemRoot 'System32\taskkill.exe') /T /F /PID $script:job.Proc.Id 2>$null | Out-Null
    if ($script:phase.StartsWith('Linux ') -and $haveWsl) {
        # Ending wsl.exe does not end its Linux children.
        & $wsl -d $distro -u root --exec pkill -f -e 'make_release_linux|gsr_builder|ninja' 2>$null | Out-Null
    }
}

function Quote([string]$path) { '"' + $path + '"' }
function PsFile([string]$script, [string]$arguments = '') {
    '{0} -NoLogo -NoProfile -ExecutionPolicy Bypass -File {1} {2}' -f (Quote $ps), (Quote $script), $arguments
}

# --------------------------------------------------------------------- jobs
$ui.RunGames.Add_Click({
    $game = Get-Choice 'Game'
    $settings.game = $game
    $settings.light = [bool]$ui.Light.IsChecked
    $arguments = "-Game $game"
    if ($ui.GamesCheckOnly.IsChecked) { $arguments += ' -CheckOnly' }
    elseif ($ui.Light.IsChecked) { $arguments += ' -CpuPercent 50 -RamPercent 50' }
    $label = switch ($game) { 'Both' { 'Both games' } 'BrokenSeal' { 'The Broken Seal' } 'LostAge' { 'The Lost Age' } }
    $name = if ($ui.GamesCheckOnly.IsChecked) { 'Tool check' } else { "$label build" }
    Start-Build $name (PsFile (Join-Path $PSScriptRoot 'build_dev.ps1') $arguments)
})

$ui.RunRegen.Add_Click({
    $which = Get-Choice 'Regen'
    $settings.regen = $which
    $gs = PsFile (Join-Path $collectionRoot 'The Broken Seal/scripts/gs.ps1') '-To recompile'
    $tla = PsFile (Join-Path $collectionRoot 'The Lost Age/scripts/tla_recompile.ps1')
    switch ($which) {
        'BrokenSeal' { Start-Build 'The Broken Seal code' $gs }
        'LostAge'    { Start-Build 'The Lost Age code' $tla }
        default      { Start-Build 'Game code' "$gs && $tla" }
    }
})

$ui.RunRelease.Add_Click({
    $selected = @(Get-SelectedPlatforms)
    if (-not $selected.Count) { return }
    $versionText = $ui.Version.Text.Trim()
    if ($versionText -and $versionText -notmatch '^[A-Za-z0-9._-]+$') {
        [void][Windows.MessageBox]::Show($window, 'Use letters, digits, periods, hyphens and underscores in the version.',
            'Build Studio', 'OK', 'Warning')
        return
    }
    if (-not $versionText) {
        $answer = [Windows.MessageBox]::Show($window, 'Build without a version? The launcher will skip update checks.',
            'Build Studio', 'YesNo', 'Question')
        if ($answer -ne 'Yes') { return }
    }
    $switches = @('-ReleaseProgress')
    if ('Windows' -notin $selected) { $switches += '-NoWindows' }
    if ('Linux' -notin $selected) { $switches += '-NoLinux' }
    if ('Arm64' -in $selected) { $switches += '-LinuxArm64' }
    if ('Android' -in $selected) { $switches += '-Android' }
    if (-not ($ui.TestBuilders.IsEnabled -and $ui.TestBuilders.IsChecked)) { $switches += '-NoTest' }
    if ($ui.LauncherOnly.IsChecked) { $switches += '-LauncherOnly' }
    if ($ui.Ashley.IsEnabled -and $ui.Ashley.IsChecked) { $switches += '-AshleyEdition' }
    if ($versionText) { $switches += "-Version:$versionText" }
    # Remember choices that are greyed out only for this run.
    $settings.platforms = @($platforms.Keys | Where-Object { $ui["Plat$_"].IsChecked })
    $settings.test = [bool]$ui.TestBuilders.IsChecked
    $settings.version = $versionText
    New-Item -ItemType Directory -Force -Path $releaseRoot | Out-Null
    Start-Build 'Release' (PsFile (Join-Path $PSScriptRoot 'make_release.ps1') ($switches -join ' ')) -Staged
})

$ui.RunLauncher.Add_Click({
    Start-Build 'Launcher build' (PsFile (Join-Path $PSScriptRoot 'build_dev.ps1') '-Game None')
})

$ui.RunAndroid.Add_Click({
    $apk = Quote (Join-Path $PSScriptRoot 'android/build_apk.py')
    if ($ui.AndroidCheckOnly.IsChecked) { Start-Build 'Android tool check' "python $apk --check-tools" }
    else { Start-Build 'Android app' "python $apk" }
})

$ui.Stop.Add_Click({
    $answer = [Windows.MessageBox]::Show($window,
        "Stop $($script:job.Name)? If it stops while linking, that game may need building again.",
        'Build Studio', 'YesNo', 'Warning')
    if ($answer -eq 'Yes') { Stop-Build }
})

$ui.OpenReleases.Add_Click({
    New-Item -ItemType Directory -Force -Path $releaseRoot | Out-Null
    Start-Process explorer.exe -ArgumentList (Quote $releaseRoot)
})
$ui.OpenLogs.Add_Click({
    New-Item -ItemType Directory -Force -Path $logRoot | Out-Null
    Start-Process explorer.exe -ArgumentList (Quote $logRoot)
})

foreach ($c in 'PlatWindows', 'PlatLinux', 'PlatArm64', 'PlatAndroid', 'LauncherOnly', 'GamesCheckOnly', 'AndroidCheckOnly') {
    $ui[$c].Add_Checked({ Update-Controls })
    $ui[$c].Add_Unchecked({ Update-Controls })
}

$window.Add_Closing({
    param($sender, $e)
    if ($script:job -and -not $script:job.Proc.HasExited) {
        $answer = [Windows.MessageBox]::Show($window, "Stop $($script:job.Name) and close?", 'Build Studio', 'YesNo', 'Warning')
        if ($answer -ne 'Yes') { $e.Cancel = $true; return }
        Stop-Build
    }
    $timer.Stop()
    Save-Settings
})

Update-Controls
[void]$window.ShowDialog()
$windowMutex.ReleaseMutex()
