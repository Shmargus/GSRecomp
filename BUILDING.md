# Building both games

Open **GoldenSunLauncher.exe** to build and play. Each cartridge shows whether
its game is ready, not built yet, or needs rebuilding. Select an unbuilt game
to choose its ROM and build it in this window; progress stays visible and the
game starts when it is ready. Escape/B cancels the build. The gold cog also
offers **Build game / Rebuild game**, which finishes without starting the game.
Builds run one at a time.

The build dialogue includes **Use fewer system resources**, off by default.
Checking it caps the complete build and its child processes at 50% of CPU
capacity and 50% of installed RAM; building may take longer. The choice
applies to both games when **Build both** is selected, and is offered again
for the next build or rebuild. Leaving it unchecked keeps normal behaviour.
Other applications use additional resources. A hard memory limit can cause
a build to fail if it cannot fit; it does not merely slow memory allocation.

Windows uses job limits for both developer and player builds. Linux player
builds use systemd 254+ user scopes with CPU and memory controllers enabled;
the builder verifies the actual cgroup limits before touching its work folder.
If those controls are unavailable, the limited build stops with an error;
normal builds remain available. The existing Linux developer-build entry
points are unchanged. Titles, checkbox, warning and buttons follow the
selected game's flag in all five languages.
Player packages need the updated launcher and both updated builders; a
launcher-only update does not include the new builder support.

After a launcher-only update, a player build record without a library
checksum needs one rebuild. The launcher records the checksum after the
older version-2 builder successfully rebuilds and its output is verified.

## Build Studio

**Build Studio.bat** opens one window for every developer build script:
the developer launcher and games (`build_dev.ps1`), code regeneration (GS1 `gs.ps1 -To recompile`,
TLA `tla_recompile.ps1`), player releases (`make_release.ps1`), the launcher
alone (`build_dev.ps1 -Game None`) and the Android app (`build_apk.py`). It
only starts those scripts with the options picked; their own checks still
apply. One build runs at a time, and the window refuses to start one while
any build process is running. Each run's output is shown live and kept in
`local/build-studio/`; choices are remembered in `local/build_studio.json`.
The pages are **Developer Launcher** (both games or one, plus the launcher),
**Regenerate code**, **Releases** and **Launcher & Android**. From a shell, the
scripts in `scripts/` take the same options.

## Readiness

Developer games show **Needs rebuilding** when their build record is missing,
their sources or build settings have changed, or their built files no longer
match. The inputs are listed in `scripts/developer_build_inputs.txt`:

- `engine/` and `shared_game/` changes affect both games;
- `The Broken Seal/` changes affect The Broken Seal;
- `The Lost Age/` changes affect The Lost Age.

The cartridge indicators track game builds. The launcher cog builds the
selected game. After changing `launcher/`, close the launcher and use
Build Studio's **Rebuild launcher**.

Checks run once in the background when the launcher opens (**Checking
build...**) and after a build. Cartridge clicks and returning from a game
reuse that result, so restart the launcher after editing sources. Saves and F1
preferences never trigger a rebuild. ROM-configuration changes need the
regeneration step first (GS1 `scripts/gs.ps1 -To recompile`, TLA
`scripts/tla_recompile.ps1`); the records track compiled sources and generated
code, not unregenerated manifests. Translator/code-generation changes also
require regeneration of the affected game code before rebuilding; compiling
older generated code with a newer translator does not pick them up.

## Developer builds (Windows)

Developer launcher sessions always capture host profiling, cost probe and
present cadence, including when Test variables is off. These three boxes
are fixed on in developer settings; other tests remain optional.

For language testing, Settings > either game > Save testing offers **Use
English saves (test copy)**. It starts with a fresh copy of that game's English
in-game save and keeps test saves/states under `Saves/English save tests`.
English and normal language-specific progress stays untouched; existing
savestates are not copied across ROM languages. Existing savestates for every
language must be retained through player updates and flag changes; the English
filenames remain compatible. Test-copy mode is developer-only, including when
a player launcher finds old developer settings. F2 states require their original
ROM language; sharing a filename does not convert the captured game session. Restart game keeps the current
test copy; starting again from the selector makes a fresh one. The checkbox is
off by default, remembered per game, and independent of Test variables.
Rebuild the launcher (Build Studio, **Rebuild launcher**) after changing launcher sources.

Music mods (`Music/TBS`, `Music/TLA`, see [MUSIC_MODS.md](MUSIC_MODS.md))
work the same in developer and player builds; `Music/` is never committed.
The engine decodes FLAC with dr_flac: a copy in `engine/third_party/dr_libs`
if present, otherwise CMake fetches the pinned commit.

Build Studio's **Developer Launcher** page runs `scripts/build_dev.ps1`: The
Broken Seal, then The Lost Age, then the root launcher; a failure stops it.
Close the launcher and games first. From a shell:

```text
scripts/build_dev.ps1                    both games and the launcher
scripts/build_dev.ps1 -Game BrokenSeal   one game (also updates the launcher)
scripts/build_dev.ps1 -Game LostAge
scripts/build_dev.ps1 -Game None         the launcher only
scripts/build_dev.ps1 -CheckOnly         check dependencies; builds and writes nothing
```

The Broken Seal builds for the language its launcher flag picks
(`local/broken-seal-language.txt`; English, Spanish, Italian, French or German). Each
language has its own ROM and saves: `ROMs/The Broken Seal.gba`, or the same
name with ` (Spain)`, ` (Italy)`, ` (France)` or ` (Germany)` before `.gba`. USA/Europe uses
`local/gs011`, Spain `local/es/gs011`, Italy `local/it/gs011`, France `local/fr/gs011`, and Germany
`local/de/gs011`. `scripts/build_game.ps1` makes localized code with the player
builder from `config/es`, `config/it`, `config/fr` or `config/de` the first time and whenever
the ROM, configs or gba_recompile change (`local/game_code_revision.txt`
records the choice). Switching ROM recompiles all the game code.

The Lost Age has the same five language flags, saved independently in
`local/lost-age-language.txt`. Localized ROMs use `ROMs/The Lost Age` with
` (Spain)`, ` (Italy)`, ` (France)` or ` (Germany)` before `.gba`; each has its
own saves/states. Game code lives under `The Lost Age/local/es/tla`,
`local/it/tla`, `local/fr/tla` or `local/de/tla`. Windows developer builds and
Windows/Linux player builders use the corresponding `config/es`, `config/it`,
`config/fr` or `config/de`. After changing launcher sources, close it and rebuild
the launcher before selecting a new flag and building The Lost Age.

The build-one/both question uses the flag of the game you selected. Save
conversion questions use the flag of the game just built, including when the
two games have different flags. Titles, messages and buttons follow that
language. Rebuild the launcher after changing this wording.

After a successful build for a new language, the launcher offers to convert
that game's English in-game save if it exists and there is no save for the new
language. This option is available in both player and developer launchers.
The prompt warns in all five languages that only in-game saves transfer;
F-key savestates (F2, etc.) do NOT carry over and remain in their original
language. It also warns that characters move to the nearest sanctuary.
Accepting keeps the English original and creates a separate save for the
chosen language; existing savestates remain untouched. Declining is remembered
for that game/language, and existing language saves are never replaced.

The Lost Age's harvested RAM code (code the game ran from RAM in play
sessions) is most of its compile, so developer builds leave it out by
default, as player releases do; that code then compiles while playing.
Launcher Settings > The Lost Age > **Pre-built RAM code** turns it back on.
Changing it needs a rebuild.

The script uses 90% of CPU/RAM by default (`-CpuPercent`, `-RamPercent`),
keeps each game's existing build settings, points both at the root `engine/`
and records a verified build. A build started from the launcher keeps the
running launcher and skips replacing it.

Outputs:

- `The Broken Seal/build/gs011_opt/GoldenSunRecomp.exe` + `GoldenSunEngine.dll`
  + `GoldenSunGame.dll`
- `The Lost Age/build/tla/TLARecomp.exe` + `TLAEngine.dll` + `TLAGame.dll`

On Windows the `.exe` is a small player program; the engine is the
`...Engine.dll` plug-in it runs, and the game code is the `...Game.dll`
built from the ROM. On Linux the engine is still the program itself.
- `GoldenSunLauncher.exe` at the root (copied from Broken Seal's
  `GoldenSunCollection.exe`)

## Linux developer builds

Use GCC 15 or newer: the generated code relies on its guaranteed tail
transfers. Select it in both the translator and game CMake configurations; installing it alone does
not change an existing configuration's cached compiler. The player-release
script selects the compiler explicitly for its own build trees.

The Lost Age: `bash "The Lost Age/scripts/tla_build.sh" --rom <rom>` writes
its own build record; `--recompile` regenerates its separate Linux corpus.
For other manual Linux builds, record the inputs before
building and repeat with `--finish` only after success:

```text
python3 scripts/developer_build_stamp.py --root "<collection>" --game LostAge --build-dir "<build-directory>" --snapshot "<temporary-file>" --begin
```

Use `BrokenSeal` for the first game.

## Android launcher (sideload)

Use Build Studio's **Launcher & Android > Build app** (`scripts/android/build_apk.py`). It makes
`Releases/android/GoldenSunRecompiled.apk`: the collection launcher, both
engines, both player builders and the translator for 64-bit ARM phones, a
Clang/lld that runs on the phone, and the builder data. Like the PC player
release it holds no game code: the player picks each ROM on the phone and the
launcher builds the game there. `--check-games` also compiles this PC's
generated code for both games the way the phone will and checks every guest
jump became a host jump (Clang has no guarantee like GCC 15's; measured: none
missed in The Lost Age's 211 files). `--package-only`
repacks without compiling.

How it maps onto the PC launcher (`launcher/android_host.h`): the collection
window, a game session and the game are three activities in three processes
(`scripts/android/java`), so each starts clean as a PC program does. The
collection lives in the app's private storage (`files/collection`, PC layout);
the programs ship as `lib*.so` in the app's library folder (the only place
Android runs programs from) and are linked into each game folder. On the
phone the engine is a plug-in (`libgs_engine.so`, `libtla_engine.so`); the
built game library links against it and the game process loads it
(`launcher/android_main.cpp`, engine `game_module.cpp`). Code the game builds
in RAM is compiled while playing by the app's own Clang and lld (engine
`overlay_compile.cpp`), as the PC does with g++.

Tools under `%LOCALAPPDATA%\Android`: NDK r29 (`Sdk/ndk/29.0.14206865`),
build-tools 36.1.0, platform android-36, Temurin JDK 21 (`jdk-21`), the SDL
2.32.10 source (`SDL2-2.32.10`) and the LLVM 21.1.8 source (`llvm`: llvm,
clang, lld). The first run builds LLVM's table generators with MinGW GCC and
then Clang+lld for the phone (`llvm-build/`, about an hour). The APK is
signed with `local/android/gsrecomp-sideload.keystore`, made on first use;
keep it, or a new APK will not install over the old one (and its saves).
For tools installed elsewhere, put the absolute Android folder path (the
parent of `Sdk`, `jdk-21` and `llvm`) in `local/android/toolchain.txt`.
Needs Android 9 and OpenGL ES 3.1. The app icon is made from
`assets/launcher_logo.png` at build time (Pillow).

## Player releases

Open Build Studio's **Releases** page and select any combination of Windows x64,
Linux x64, Linux ARM64 and Android. The page shows the active build stage,
completed stages, elapsed time and live output. Packages and logs go to
root `Releases/`. `scripts/make_release.ps1 -CheckOnly` checks the release layout
without building. Player packages put the game folders under `games/` beside
the launcher; **Launcher only** packages update the launcher, artwork and
runtime files.

Set **Release version** to the GitHub tag and enable the packaged-builder
tests for desktop packages. Use a full release for engine/compiler updates;
**Launcher only** does not update either game's engine or bundled builder.
Keep builds sequential. Packaging creates local archives; it does not commit,
push, tag or publish a GitHub release.

**Android** (select it in the window, or use `-Android`): Android runs after
any selected desktop builds. Android prerequisites are checked before any
selected platform starts. `scripts/android/build_apk.py --release` makes
`Releases/GoldenSunRecompiled-android-<date>.apk`, with the release version as
the app's version and not open to adb debugging. Needs the Android tools
under "Android launcher (sideload)". The ROM scan reads inside the APK and
inside the collection it unpacks on the phone. For phone testing with adb,
keep using Build Studio's **Build app** (a debuggable APK).

The release prints how long each part took when it ends (or stops).

The tests prepare each game's code using the packaged tools and your ROM.
The release flow also scans the output for protected game data. ROMs, BIOS
files, saves, generated game code and compiled game libraries must not ship.
Successful packaging does not replace a gameplay check of the release.

Both packages include the player notes from `scripts/release/` (one copy
for both platforms): `README.txt` at the top and `Music/README.txt` about
music mods. They also include `licenses/ENGINE_LICENSE.txt` (PolyForm
Noncommercial), `licenses/THIRD_PARTY_ATTRIBUTION.md` and the platform's
`THIRD_PARTY_NOTICES.txt`. Keep these files in the download.

Linux by hand: `bash scripts/make_release_linux.sh` on a 64-bit Linux
machine or WSL (built and tested on Ubuntu 24.04) with GCC 15, CMake, Ninja
and the SDL2/zlib development packages makes
`Releases/linux/GoldenSunRecompiled-linux-<date>.zip`; `--launcher-only`
makes a launcher-only update. Players need a glibc at least as new as the
build machine's (2.38 on Ubuntu 24.04).

**Linux ARM64** (select it in the window, or use `-LinuxArm64`):
`scripts/make_release_linux_arm64.sh` runs the same Linux script inside an
ARM Ubuntu 24.04 that QEMU emulates in WSL, so the launcher, engines,
builders and bundled GCC 15 are 64-bit ARM. Output goes to
`Releases/linux-arm64/GoldenSunRecompiled-linux-arm64-<date>.zip`. The first
run installs `qemu-user-static` and `mmdebstrap` in WSL and creates the ARM
system in `/root/gsr-arm64-root` (`--prepare` does only that). Emulation
makes it far slower than the x86 Linux build. The launcher's update check
picks the zip with "arm64" in its name on ARM Linux and skips it elsewhere.
On a real ARM Linux machine, run `make_release_linux.sh` directly.

TLA player-release mode excludes the private harvested play cache from both
the runtime table and compilation. Developer builds retain it. This skips
unused release work; it does not reduce the code generated from a player's
ROM. Raw local caches contain game code and are not release assets.

Linux releases require GCC 15 or newer for guaranteed tail transfers
([GCC 15 release notes](https://gcc.gnu.org/gcc-15/changes.html)). The
release script defaults to `gcc-15` / `g++-15`, uses them for both games and
bundles that same C++ compiler for players. `CC` and `CXX` can select another
installed GCC 15+ pair; an older compiler is rejected before building.
When the selected compiler differs from a cached build's compiler, the script
reapplies the full release configuration after CMake resets its cached options.
On the Ubuntu 24.04 release host, install them with:

```text
sudo add-apt-repository ppa:ubuntu-toolchain-r/test
sudo apt-get update
sudo apt-get install gcc-15 g++-15
```
