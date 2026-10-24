# GSRecomp — The Broken Seal

Part of the Golden Sun Recompiled collection; see the collection
[README](../README.md) and [BUILDING.md](../BUILDING.md).

GSRecomp (Golden Sun Recompiled) is a native PC port of **Golden Sun** for the
Game Boy Advance, made by static recompilation, for Windows and Linux
(including the Steam Deck). The game's ARM/Thumb code is translated ahead of
time into C++ by the collection's shared engine in `../engine/`, then compiled
into a regular program that runs the game without an emulator's interpreter
loop and without a GBA BIOS.

No game data is in this repository or in the releases. You supply your own
legally obtained Golden Sun (USA/Europe) ROM; the game code is generated from
it on your machine, and the launcher checks the ROM's SHA-1 first.

> [!WARNING]
> **Work in progress.** GSRecomp is in public testing (pre-release builds),
> not a finished release. Bugs and visual/audio inaccuracies are still
> present, and compatibility is not yet guaranteed.

Playing, settings shared with The Lost Age, music mods and known issues are
in the collection [README](../README.md); what each version changed is in the
[release notes](https://github.com/Shmargus/GSRecomp/releases). The game is
playable from the start to the ending credits.

## Features

- Native x86-64 build of the game's code, including code the game copies into
  RAM and its overlays. Code that was not prepared ahead of time is compiled
  on the spot the first time it is reached, then remembered.
- Runs without a GBA BIOS.
- Wider views drawn by a native GPU renderer (field, world map and battles),
  with spell effects, weather and scenery extended into the extra margins.
- In-window settings menu (F1): fullscreen, window size, sharp pixels,
  flicker reduction, colour profiles (Raw, Handheld, Handheld lighter, Soft,
  Natural, Warm, Deep, and Custom with Saturation, Hue, Brightness, Warmth and
  Darkening sliders), a 4:3 aspect ratio, experimental screen filters
  (LCD3x, CRT Lottes, xBR, ScaleFX; need OpenGL), volume and the music mod
  switch, fast forward, an FPS/speed counter, and rebindable controls and
  hotkeys, including auto fire for A and B, Quit Game and Soft Reset.
- Seamless mode: after the ending, choose the slot for the Clear Data and go
  straight on to The Lost Age, which imports that slot.
- In-game settings for walking speed, encounter rate, screen size and
  slowdown removal, plus 2x battle speed (Select in battle). Walk Speed does
  not apply in the Colosso trials, on purpose.
- **Hard Mode:** enemies have 1.5x HP and 1.25x Attack and Defence. Asked
  once when starting a new game, changeable in the settings screen, and
  remembered per save (it replaces Auto-Sleep).
- In-game cheat menu (F11 by default): infinite HP/PP and experience, coin
  and drop-chance multipliers.
- Block timing: the translated code keeps its cycle count per block instead
  of per instruction, so heavy spells and summons run far faster
  (`scripts/block_timing.py`, applied by the root `scripts/build_game.ps1`;
  `-PerInstruction` builds the old way for comparison).

## Build requirements

- **Windows 10/11 x64** for the development build below. Linux builds: see
  [BUILDING.md](../BUILDING.md).
- **MSYS2** installed at `C:\msys64` with the MinGW-w64 toolchain
  (`mingw-w64-x86_64-gcc`, `mingw-w64-x86_64-make`) and
  `mingw-w64-x86_64-SDL2`. `scripts/build_game.ps1` expects
  `C:/msys64/mingw64/bin/mingw32-make.exe`.
- **CMake 3.20+** and **Ninja**.
- **Git**. During configuration CMake downloads Dear ImGui, toml++ and
  dr_flac at pinned commits (unless `../engine/third_party/` holds them), so
  internet access is needed the first time.
- **Python 3.10+**, with the `unicorn` package (`pip install unicorn`) for
  `tools/build_stamp_variants.py`.
- **Golden Sun disassembly**, [gsret/goldensun](https://github.com/gsret/goldensun)
  at revision `84a80693003439acdbb78dd84538b0532461ad4b`, built into
  `goldensun.elf` and its overlay binaries. Symbols and overlay code come from it.
- **Your own Golden Sun (USA/Europe) ROM**, SHA-1
  `5c4695205413df7db52b9a184815a07783999971`.

There are no submodules; `../engine/` is an ordinary directory.

## Building

The collection-root `build_dev.bat -Game BrokenSeal` is the primary build
command (the launcher's cog runs the same build). It compiles and links the
game, but it needs two things to exist first: the recompiled code generated
from your ROM, and a configured build directory. From a fresh clone, in this
folder:

1. **Build the recompiler.**
   ```
   cmake -S ../engine -B ../engine/build/relwithdebinfo-msys -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
   cmake --build ../engine/build/relwithdebinfo-msys --target gba_recompile
   ```

2. **Point the scripts at your files.** Create `config/local.json`
   (git-ignored):
   ```json
   {
     "rom": "C:\\path\\to\\Golden Sun.gba",
     "bios": "C:\\path\\to\\gba_bios.bin",
     "elf": "C:\\path\\to\\goldensun\\goldensun.elf",
     "recompile": "C:\\path\\to\\GSRecomp\\engine\\build\\relwithdebinfo-msys\\gba_recompile.exe",
     "disassembly_revision": "84a80693003439acdbb78dd84538b0532461ad4b"
   }
   ```
   `scripts/gs.ps1` currently checks that the `bios` path exists, even though
   the game itself runs without a BIOS.

3. **Generate the recompiled code** into `local/gs011/` (git-ignored). Every
   input address list is in `config/usa/`:
   - Symbols: `tools/import_main_symbols.py` and
     `tools/import_overlay_inventory.py` write `local/symbols/`.
   - Main program: `powershell -File scripts\gs.ps1 -To recompile`
     (writes `local/gs011/main`).
   - Overlays: `python tools/build_all_overlays.py --recompiler <gba_recompile.exe> --compile-only`
     (reads the disassembly's `overlays/` directory).
   - RAM-copied code: one `gba_recompile` run per `config/usa/transient-*.toml`,
     into the matching `local/gs011/transient_*` directory named in
     `CMakeLists.txt`. **This step is not scripted yet.**
   - Generated variants: `tools/build_synthesized_dc8_variant.py`,
     `tools/build_synthesized_dc8_slot2_variant.py`,
     `tools/build_synthesized_dc8_slot3_variant.py` and
     `tools/build_stamp_variants.py` (each takes `--rom`; see `--help`).

   CMake stops with a clear message naming any directory that is missing.

4. **Configure the build directory once.**
   ```
   cmake -S . -B build/gs011_opt -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGSR_BUILD_LOCAL_RUNNER=ON -DGSR_GENERATED_DIR=local/gs011/main
   ```

5. **Run the collection-root `build_dev.bat -Game BrokenSeal`.** It runs
   `scripts/build_game.ps1`, which:
   - re-configures `build/gs011_opt` with link-time optimisation;
   - builds the engine, `GoldenSunEngine.dll` (our code only, with LTO),
     the small `GoldenSunRecomp.exe` that runs it, and the translated game
     code beside them as `GoldenSunGame.dll`;
   - by default caps the build at 90% of CPU and 90% of RAM
     (`-CpuPercent 80 -RamPercent 75` to lower them).

   Then it builds the collection launcher.

**Output:**
- `build\gs011_opt\GoldenSunRecomp.exe`: the game.
- `GoldenSunLauncher.exe`: in the collection root.

Player releases and Linux builds: see [BUILDING.md](../BUILDING.md).

ROMs, BIOS files, saves and anything generated from them must never be
committed; `.gitignore` excludes them.

## License

`../engine/` is an edited copy of [gbarecomp](https://github.com/mstan/gbarecomp)
by Matthew Stan, licensed under the PolyForm Noncommercial License 1.0.0
(`../engine/LICENSE`), so this project is noncommercial. See `LICENSE` for
the rest of the repository.

---

<sub>Portions of this project were developed with the assistance of AI-based development tools.</sub>
