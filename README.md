# GSRecomp

GSRecomp (Golden Sun Recompiled) is a native PC port of **Golden Sun** for the
Game Boy Advance, made by static recompilation, for Windows and Linux
(including the Steam Deck). The game's ARM/Thumb code is translated ahead of
time into C++ by the `gbarecomp` engine included in `gbarecomp/`, then compiled
into a regular program that runs the game without an emulator's interpreter
loop and without a GBA BIOS.

No game data is in this repository or in the releases. You supply your own
legally obtained Golden Sun (USA/Europe) ROM; the game code is generated from
it on your machine, and the launcher checks the ROM's SHA-1 first.

> [!WARNING]
> **Work in progress.** GSRecomp is in public testing (pre-release builds),
> not a finished release. Bugs and visual/audio inaccuracies are still
> present, and compatibility is not yet guaranteed.

## Status

- **Latest release: [0.4.1](https://github.com/Shmargus/GSRecomp/releases)**
  (pre-release), for Windows and Linux. 0.4.1 adds a crash catcher for a
  rare crash when leaving a room (seen leaving Sol Sanctum): it saves what
  the game was doing the moment it goes wrong and adds it to the bug
  report. 0.4 added colour profiles with Custom sliders, a 4:3 picture,
  experimental screen filters, a memory snapshot in bug reports after a
  crash, and a fix for the black bars on the world map with AMD graphics on
  Linux.
- The game is playable from start to the ending credits.
- Known issues:
  - A few spell effects (Spark Plasma, Destruct Ray, Supernova, Thor's
    thunder wave) still stop at the original screen edge in the widescreen
    view.
  - Very rarely a single frame flashes pink (a frame the new widescreen
    drawing cannot handle yet); the game carries on.
  - The Walk Speed setting does not apply in the Colosso trials (kept on
    purpose).
  - A crash in the Vale storm opening or during a battle has been reported
    a few times; the cause is not found yet. Press **Send report** if it
    happens.
  - The Mars Star looks garbled when you take it out in Sol Sanctum.

## Playing

1. Download the Windows or Linux zip from
   [Releases](https://github.com/Shmargus/GSRecomp/releases) and unzip it.
2. Start `GoldenSunLauncher` (`GoldenSunLauncher.exe` on Windows) and pick
   your Golden Sun ROM. The launcher remembers it.
3. On first start, and again after an update, the launcher builds the game
   code from your ROM, with a progress bar. A compiler is included in the
   download, so nothing else needs to be installed (Linux needs SDL2 and
   OpenGL from the system; SteamOS and most desktops have both).
4. Press **Play**, or tick "Start the game automatically".

If something goes wrong, press **F12** in the game right after it: the game
keeps the last two seconds, and when it closes the launcher packs a bug
report you can send with **Send report**. The launcher also tells you when a
newer release is out and shows its notes in a "What's new" box.

The save file sits next to your ROM, with the same name and a `.sav`
extension. A save from an emulator that the game cannot read gets an
explanation from the launcher instead of a silent exit.

## Features

- Native x86-64 build of the game's code, including code the game copies into
  RAM and its overlays. Code that was not prepared ahead of time is compiled
  on the spot the first time it is reached, then remembered.
- Runs without a GBA BIOS.
- Expanded widescreen view drawn by a native GPU renderer (field, world map and
  battles), with spell effects extended into the extra margins.
- In-window settings menu (F1): fullscreen, window size, sharp pixels,
  flicker reduction, colour profiles (Raw, Handheld, Handheld lighter,
  Soft, Natural, Warm, Deep, and Custom with Saturation, Hue, Brightness,
  Warmth and Darkening sliders), a 4:3 aspect ratio (the middle of the
  wide view), experimental screen filters (LCD3x, CRT Lottes, xBR, ScaleFX;
  need OpenGL), volume, fast forward, an FPS/speed counter, and
  rebindable controls and hotkeys, including auto fire for A and B and a
  Quit Game hotkey.
- Optional in-game settings for walking speed, encounter rate, screen size and slowdown
  removal, plus 2x battle speed (Select in battle).
- **Hard Mode:** enemies have 1.5x HP and 1.25x Attack and Defence. Asked
  once when starting a new game, changeable in the settings screen, and
  remembered per save (it replaces Auto-Sleep, which is gone).
- In-game cheat menu (F11 by default): infinite HP/PP and experience, coin
  and drop-chance multipliers.
- Block timing: the translated code keeps its cycle count per block instead
  of per instruction, so heavy spells and summons run far faster
  (`tools/block_timing.py`, applied by `build_lto.bat`;
  `build_lto.bat -PerInstruction` builds the old way for comparison).

## Build requirements

- **Windows 10/11 x64** for the development build below. Linux builds are
  made with `scripts/make_release_linux.sh` (see "Linux" further down).
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
   - builds the engine, `GoldenSunRecomp.exe` (our code only, with LTO),
     and the translated game code beside it as `GoldenSunGame.dll`;
   - by default caps the build at 90% of CPU and 90% of RAM
     (`build_lto.bat -CpuPercent 80 -RamPercent 75` to lower them).

   A full link takes 15-25 minutes.

**Output:**
- `build\gs011_opt\GoldenSunRecomp.exe`: the game.
- `GoldenSunLauncher.exe`: in the repository root.

### Releases

`make_release.bat` opens a release window that builds the player downloads
for Windows, Linux (through WSL Ubuntu 24.04) or both, into
`GSRecomp-Release/`; run `build_lto.bat` first. Each download has the
launcher, engine, builder, translator and a trimmed compiler, and no game
code. By default it then builds the game from your ROM using only the
release's own tools, to prove the download is complete. The release scripts
read the bug-report service address from `local/report_host.txt` (not in
git); without it the launcher has no Send report button.

- **Linux by hand:** `bash scripts/make_release_linux.sh` on a 64-bit Linux
  machine or WSL (built and tested on Ubuntu 24.04) with GCC, CMake, Ninja and the
  SDL2/zlib development packages. It makes
  `GSRecomp-Release/linux/GoldenSunRecompiled-linux-<date>.zip` with the
  launcher, the engine, the builder and a trimmed copy of that machine's GCC.
  `--launcher-only` makes a launcher-only update zip. Players need a glibc at
  least as new as the build machine's (2.38 on Ubuntu 24.04).

## Running

Start `GoldenSunLauncher.exe`. On first launch it asks you to select your
Golden Sun ROM, checks its SHA-1, remembers the path in
`local\launcher-rom.txt`, and starts the game. No BIOS is needed to play.
The launcher's activity is logged to `logs\launcher.log`.

ROMs, BIOS files, saves and anything generated from them must never be committed;
`.gitignore` excludes them.

## License

`gbarecomp/` is an edited copy of [gbarecomp](https://github.com/mstan/gbarecomp)
by Matthew Stan, licensed under the PolyForm Noncommercial License 1.0.0
(`gbarecomp/LICENSE`), so this project is noncommercial. See `LICENSE` for
the rest of the repository.

---

<sub>Portions of this project were developed with the assistance of AI-based development tools.</sub>
