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
code, not unregenerated manifests.

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

- `The Broken Seal/build/gs011_opt/GoldenSunRecomp.exe` + `GoldenSunGame.dll`
- `The Lost Age/build/tla/TLARecomp.exe` + `TLAGame.dll`
- `GoldenSunLauncher.exe` at the root (copied from Broken Seal's
  `GoldenSunCollection.exe`)

## Linux developer builds

The Lost Age: `bash "The Lost Age/scripts/tla_build.sh" --rom <rom>` writes
its own build record. For other manual Linux builds, record the inputs before
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
