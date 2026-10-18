# Building both games

Open **GoldenSunLauncher.exe** to build and play. Each cartridge shows whether
its game is ready, not built yet, or needs rebuilding. Select an unbuilt game
to choose its ROM and build it in this window; progress stays visible and the
game starts when it is ready. Escape/B cancels the build. The gold cog also
offers **Build game / Rebuild game**, which finishes without starting the game.
Builds run one at a time.

## Readiness

Developer games show **Needs rebuilding** when their build record is missing,
their sources or build settings have changed, or their built files no longer
match. The inputs are listed in `scripts/developer_build_inputs.txt`:

- `engine/` changes affect both games;
- `launcher/` and `The Broken Seal/` changes affect The Broken Seal (its build
  also produces the launcher);
- `The Lost Age/` changes affect The Lost Age.

Checks run once in the background when the launcher opens (**Checking
build...**) and after a build. Cartridge clicks and returning from a game
reuse that result, so restart the launcher after editing sources. Saves and F1
preferences never trigger a rebuild. ROM-configuration changes need the
regeneration step first (GS1 `scripts/gs.ps1 -To recompile`, TLA
`scripts/tla_recompile.ps1`); the records track compiled sources and generated
code, not unregenerated manifests. Translator/code-generation changes also
require regeneration of the affected game code before rebuilding. In
particular, the 0.5.2 tail-transfer fix needs freshly generated code; compiling
an older generated corpus with a newer compiler does not add those transfers.

## Developer builds (Windows)

`build_dev.bat` builds The Broken Seal, then The Lost Age, then the root
launcher; a failure stops it. Close the launcher and games first.

```text
build_dev.bat                    both games and the launcher
build_dev.bat -Game BrokenSeal   one game (also updates the launcher)
build_dev.bat -Game LostAge
build_dev.bat -CheckOnly         check dependencies; builds and writes nothing
```

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

Use GCC 15 or newer for the 0.5.2 guaranteed tail-transfer fix. Select it in
both the translator and game CMake configurations; installing it alone does
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

## Player releases

Double-click **make_release.bat** to open the release window and package both
games (Windows, Linux through WSL Ubuntu, or both). Packages and logs go to
root `Releases/`. `make_release.bat -CheckOnly` checks the release layout
without building. Player packages put the game folders under `games/` beside
the launcher; **Launcher only** packages update the launcher, artwork and
runtime files.

For 0.5.2, set **Version** to `0.5.2`, select Windows and Linux, and enable
the packaged-builder tests. Use a full release for engine/compiler updates;
**Launcher only** does not update either game's engine or bundled builder.
Keep builds sequential. Packaging creates local archives; it does not commit,
push, tag or publish a GitHub release.

The tests prepare each game's code using the packaged tools and your ROM.
The release flow also scans the output for protected game data. ROMs, BIOS
files, saves, generated game code and compiled game libraries must not ship.
Successful packaging does not replace a gameplay check of the release.

The final 0.5.2 source also includes shared F1 controls, alternate controller
bindings and the Linux Soft Reset correction. These change the shared engine
and launcher: build a full Windows/Linux release. The earlier 18 October
ZIPs were repaired for notices only and do not contain these final code changes.
Windows/Linux syntax checks and focused controller/reset checks pass; the
new release's full build, packaged-builder tests and gameplay checks remain
separate from those results.

Both packages include `licenses/ENGINE_LICENSE.txt` (PolyForm Noncommercial)
and `licenses/THIRD_PARTY_ATTRIBUTION.md`, with platform notices in
`THIRD_PARTY_NOTICES.txt`. Keep these files in the download. The previous
Linux `THIRD-PARTY.txt` already contained its toolchain notices; the filename
is now consistent across platforms.

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
