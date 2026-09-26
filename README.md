# GSRecomp

GSRecomp (Golden Sun Recompiled) is a native Windows x86-64 port of **Golden Sun** for the Game
Boy Advance, made by static recompilation. The game's ARM/Thumb code is translated ahead of time into C++
by the `gbarecomp` engine included in `gbarecomp/`, then compiled into a regular PC
executable that runs the game without an emulator's interpreter loop and without a
GBA BIOS.

No game data is in this repository. You supply your own legally obtained Golden
Sun (USA/Europe) ROM; the build generates the recompiled code from it on your
machine, and the launcher checks the ROM's SHA-1 before starting.

> [!WARNING]
> **Work in progress.** GSRecomp is under active development and is not a
> finished release. Features may be incomplete or unstable, bugs and
> visual/audio inaccuracies are still present, and compatibility is not yet
> guaranteed.

## Features

- Native x86-64 build of the game's code, including code the game copies into
  RAM and its overlays.
- Runs without a GBA BIOS.
- Launcher with a ROM picker and SHA-1 check.
- Expanded widescreen view drawn by a native GPU renderer (field, world map and
  battles), with spell effects extended into the extra margins.
- In-window settings menu (F1): fullscreen, window scale, integer scaling,
  colour profiles, LCD ghosting, volume, turbo, and rebindable controls and
  hotkeys.
- Optional in-game settings for walking speed, encounter rate, screen size and slowdown
  removal, plus 2x battle speed (Select in battle).
- In-game cheat menu (F11 by default): infinite HP/PP and experience, coin
  and drop-chance multipliers.

## Build requirements

- **Windows 10/11 x64.** The build is only set up and tested there.
- **MSYS2** installed at `C:\msys64` with the MinGW-w64 toolchain
  (`mingw-w64-x86_64-gcc`, `mingw-w64-x86_64-make`) and
  `mingw-w64-x86_64-SDL2`. `scripts/build_lto.ps1` expects
  `C:/msys64/mingw64/bin/mingw32-make.exe`.
- **CMake 3.20+** and **Ninja**.
- **Git**. During configuration CMake downloads Dear ImGui and toml++ at
  pinned commits, so internet access is needed the first time.
- **Python 3.10+**, with the `unicorn` package (`pip install unicorn`) for
  `tools/build_stamp_variants.py`.
- **Golden Sun disassembly**, [gsret/goldensun](https://github.com/gsret/goldensun)
  at revision `84a80693003439acdbb78dd84538b0532461ad4b`, built into
  `goldensun.elf` and its overlay binaries. Symbols and overlay code come from it.
- **Your own Golden Sun (USA/Europe) ROM**, SHA-1
  `5c4695205413df7db52b9a184815a07783999971`.

There are no submodules; `gbarecomp/` is an ordinary directory.

## Building

`build_lto.bat` is the primary build command. It compiles and links the game, but
it needs two things to exist first: the recompiled code generated from your ROM,
and a configured build directory. From a fresh clone:

1. **Build the recompiler.**
   ```
   cmake -S gbarecomp -B gbarecomp/build/relwithdebinfo-msys -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
   cmake --build gbarecomp/build/relwithdebinfo-msys --target gba_recompile
   ```

2. **Point the scripts at your files.** Create `config/local.json`
   (git-ignored):
   ```json
   {
     "rom": "C:\\path\\to\\Golden Sun.gba",
     "bios": "C:\\path\\to\\gba_bios.bin",
     "elf": "C:\\path\\to\\goldensun\\goldensun.elf",
     "recompile": "C:\\path\\to\\GSRecomp\\gbarecomp\\build\\relwithdebinfo-msys\\gba_recompile.exe",
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

5. **Run `build_lto.bat`** (double-click it or run it from a terminal). It
   runs `scripts/build_lto.ps1`, which:
   - re-configures `build/gs011_opt` with link-time optimisation;
   - builds the `GoldenSunRecomp` target;
   - by default caps the build at 90% of CPU and 90% of RAM
     (`build_lto.bat -CpuPercent 80 -RamPercent 75` to lower them).

   A full link takes 15-25 minutes.

**Output:**
- `build\gs011_opt\GoldenSunRecomp.exe`: the game.
- `GoldenSunLauncher.exe`: in the repository root.

## Running

Start `GoldenSunLauncher.exe`. On first launch it asks you to select your
Golden Sun ROM, checks its SHA-1, remembers the path in
`local\launcher-rom.txt`, and starts the game. No BIOS is needed to play.

ROMs, BIOS files, saves and anything generated from them must never be committed;
`.gitignore` excludes them.

## License

`gbarecomp/` is an edited copy of [gbarecomp](https://github.com/mstan/gbarecomp)
by Matthew Stan, licensed under the PolyForm Noncommercial License 1.0.0
(`gbarecomp/LICENSE`), so this project is noncommercial. See `LICENSE` for
the rest of the repository.

---

<sub>Portions of this project were developed with the assistance of AI-based development tools.</sub>
