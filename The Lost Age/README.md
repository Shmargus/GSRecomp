# TLARecomp

Golden Sun: The Lost Age Recompiled: a native Windows and Linux port of
**Golden Sun: The Lost Age** for the Game Boy Advance, made by static
recompilation. The game's ARM/Thumb code is translated ahead of time into C++
through the collection's shared engine in `../engine/`, and compiled into a PC
program that runs without a GBA BIOS. Started from Golden Sun Recompiled 0.2.3.

Part of the Golden Sun Recompiled collection; see the collection
[README](../README.md) and [BUILDING.md](../BUILDING.md).

No game data is in this repository. You supply your own Golden Sun: The Lost
Age (USA/Europe) ROM, SHA-1 `b500663220cb9bf56b9f8e8c0c544f1d6fa3a824`.

> [!WARNING]
> **Work in progress.** Not a finished release.

## Windows build

- Windows 10/11 x64, MSYS2 at `C:\msys64` with MinGW-w64 GCC, SDL2,
  CMake 3.20+ and Ninja.
- Python 3.10+ with `unicorn` and `capstone`.

1. Create `config/local.json` (git-ignored) naming the ROM (`rom`) and
   `gba_recompile.exe` (`recompile`), built with
   `cmake --build build/tla --target gba_recompile`.
2. `scripts/tla_recompile.ps1`: translates the ROM into `local/tla/`
   (rerun after any `config/tla/main.toml` change).
3. Collection-root `build_dev.bat -Game LostAge` (or the launcher's cog)
   builds `build/tla/TLARecomp.exe`, `build/tla/TLAGame.dll` and the root
   launcher.

Start collection-root `GoldenSunLauncher.exe` and select The Lost Age to play.
There is no separate TLA launcher. ROMs and progress live in root `ROMs`/`Saves`.

## Linux build

Like GSRecomp, Linux builds a native `TLARecomp` engine, separate
`libTLAGame.so` game library. The collection owns the SDL2/ImGui launcher. The engine uses
LTO; translated game code uses `-Og` without LTO. No Wine or Windows
compiler is involved. Windows and Linux use the same ROM translation
pipeline, with separate generated code and build directories.

On Ubuntu/Debian, install the build dependencies:

```sh
sudo apt install build-essential cmake ninja-build git pkg-config \
    libsdl2-dev libgl1-mesa-dev zlib1g-dev python3-venv
python3 -m venv .venv
. .venv/bin/activate
python -m pip install unicorn capstone
```

Build from the repository root (CMake 3.21+ and Python 3.10+):

```sh
bash scripts/tla_build.sh --rom "/path/to/Golden Sun - The Lost Age (USA, Europe).gba"
```

This builds the game's Linux engine/library only. Build the unified Linux
launcher through the collection's root `scripts/make_release_linux.sh --dev`
flow, then launch `GoldenSunLauncher` from the resulting collection.

The first build fetches the engine's pinned ImGui and toml++ dependencies,
builds the native recompiler, checks the ROM SHA-1 and generates game code.
Later builds reuse it. Add `--recompile` after changing `config/tla/` or
the recompiler, and `--jobs N` to choose concurrent compiler jobs (default
one). `CMAKE_BUILD_PARALLEL_LEVEL` also sets that default. `PYTHON` can
select a different Python interpreter.

Select The Lost Age in the collection launcher and choose its ROM. Install `zenity` or
`kdialog` for a desktop file chooser, or enter its path in the launcher.
Graphics settings and diagnostic toggles are available there; F1 opens
the game's video, audio and control settings. F12 saves the last two
seconds. Game settings, cache and logs remain in that game's collection folder;
its `logs/latest.txt` identifies the latest session. Progress stays in root `Saves`.

Combined Windows/Linux player releases, first-run ROM preparation and shared
update checks are managed by collection-root release scripts.

For a headless check:

```sh
./build/tla-linux/TLARecomp --rom "/path/to/game.gba" \
    --no-window --frames 1200 --dump-png local/linux-smoke.png
```

ROMs, BIOS files, saves and anything generated from them must never be
committed; `.gitignore` excludes them.

## Import a Golden Sun team

Leave **Import Golden Sun save files** checked in the launcher. Start a new
TLA game, answer **Yes** to **Load a Golden Sun save file?**, then select
your GS1 `.sav` file in the Windows file browser. It always opens, with a
validated GS1 clear save preselected when found in Documents, the game
folder, or the previous import location. TLA saves are excluded. Music
continues while browsing. Choose the cleared slot using the game's Up/Down
and A controls; B cancels. Unfinished or invalid slots are excluded. The
file must be a standard 64 KB battery save,
including Clear Data saved after the ending, rather than a save state.

TLA applies its own full link-transfer rules to the team, Djinn, items,
coins and story flags. The normal Isaac naming screen still follows import.
The GS1 file is read only. Cancelling returns to the question. Uncheck the launcher option for the original Link/Password
menu. On Linux, the save picker requires `zenity` or `kdialog`.

## License

`../engine/` is an edited copy of [gbarecomp](https://github.com/mstan/gbarecomp)
by Matthew Stan, licensed under the PolyForm Noncommercial License 1.0.0
(`../engine/LICENSE`), so this project is noncommercial. See `LICENSE` for
the rest of the repository.

---

<sub>Portions of this project were developed with the assistance of AI-based development tools.</sub>
