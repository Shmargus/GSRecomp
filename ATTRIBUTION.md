# What is in this repository, and who owns it

Plain answers first, because the licences here are not all the same and one of
them limits what this project may ever be.

## The short version

| Part | Whose | Licence | In this repo? |
|---|---|---|---|
| The games: Golden Sun and Golden Sun: The Lost Age, their ROMs, BIOS, art, music, text | Nintendo / Camelot Software Planning | Not ours to license | **No, and never** |
| The engine, `engine/` | Matthew Stan (mstan) | PolyForm Noncommercial 1.0.0 | Yes, an edited copy |
| Code ported into the engine from others | see "Inside the engine" below | MIT / Apache-2.0 / MPL-2.0 / public domain | Yes, inside `engine/src/` |
| Everything else: `launcher/`, `player_builder/`, `scripts/`, each game folder, the docs | this project | not yet selected; see `LICENSE` | Yes |
| Dear ImGui, toml++, dr_flac, SDL2 | their authors | MIT, MIT, public domain or MIT-0, zlib | No: fetched or installed at build time |
| Music mods (recordings in `Music/`) | their makers | their own terms | No, and never |

**The practical consequence: this project is noncommercial.** The engine's
licence permits use, modification and redistribution for noncommercial purposes
only. That covers the whole build, because nothing here runs without the engine.

## The games

No ROM, BIOS, save, savestate, screenshot of copyrighted art, or extracted
asset is in this repository, and none may be added. You supply your own legally
obtained ROMs; the launcher checks their SHA-1.

The C++ code the build produces is derived from your ROM. It is written to each
game's `local/` and build folders, which are not tracked here and must not be
published, and player releases never contain it.

What this repository does hold about the games is our own analysis: symbol
names, addresses, function boundaries and hardware observations in each game's
`config/` and `src/`: facts measured about the programs, not their content.

## The engine, `engine/`

Upstream is [`mstan/gbarecomp`](https://github.com/mstan/gbarecomp), copyright
Matthew Stan, licensed **PolyForm Noncommercial 1.0.0**; the full text is at
`engine/LICENSE` and stays there.

That licence allows what this repository does: use it, change it, and pass it
on, **for any purpose other than a commercial one**. It requires that anyone
who receives a copy also receives the licence, which is why `engine/LICENSE`
is committed alongside the code, and why every player release carries it in
`licenses/`.

`engine/` here is **not upstream**: it is a copy that diverged on 2026-07-17
and has been changed extensively since (the PPU, the ARM runtime, audio, the
overlay loader, the host window, the field renderer and the recompiler).
`engine/MODIFICATIONS.md` describes what changed and why. Building against
upstream will not reproduce this project.

Where a fix belongs: anything about ARMv4T, GBA hardware or the recompiler goes
inside `engine/`. Anything about Golden Sun stays in the game folders.

### Inside the engine

The engine ports code from other projects, credited in full in
`engine/THIRD_PARTY_ATTRIBUTION.md`:

- **[JRickey/gba-recomp](https://github.com/JRickey/gba-recomp)**, MIT OR
  Apache-2.0, used with the author's permission: the MP2K driver detection,
  the screen-colour simulation, the cartridge RTC, and the audio shadow
  verifier.
- **[mGBA](https://github.com/mgba-emu/mgba)**, MPL-2.0: the BIOS
  high-level-emulation routines in `engine/src/runtime/bios_hle.{h,cpp}`.
  Those files remain governed by the MPL-2.0; its text is at
  <https://mozilla.org/MPL/2.0/>. mGBA itself is not part of this repository.
- **Screen filters**, four GLSL shaders from libretro's
  [glsl-shaders](https://github.com/libretro/glsl-shaders), ported into
  `engine/src/runtime/screen_filter.cpp`: LCD3x by Gigaherz (public domain),
  xBR-lv2 by Hyllian (MIT), CRT Lottes by Timothy Lottes (public domain) and
  ScaleFX by Sp00kyFox (MIT). The MIT notices are kept verbatim above those
  shaders, as the licence requires.

## Our own code

Everything outside `engine/` is this project's work. `LICENSE` has not
selected a licence for it, so all rights are reserved by default: people may
read this repository, but nobody may reuse our code until a licence is chosen.
Whatever is chosen cannot escape the engine's noncommercial terms while the
engine is part of the build.

## Build-time dependencies, not in this repository

CMake fetches these at configure time, each pinned to an exact version, or uses
a local copy in `engine/third_party/` if one is present:

| Library | Use | Licence |
|---|---|---|
| Dear ImGui v1.91.9-docking | the launcher and the in-game menus | MIT |
| toml++ v3.4.0 | reading `game.toml` and the symbol metadata | MIT |
| dr_flac v0.13.4 | playing music mods' FLAC files | public domain or MIT No Attribution |

And these must be installed on the machine: a C++ toolchain (MinGW-w64 on
Windows, GCC 15+ on Linux), SDL2 (zlib licence), OpenGL, Python 3.10+, CMake
and Ninja. Player releases list the exact packages they bundle in
`THIRD_PARTY_NOTICES.txt`.

## Credits

- **Vincent Dortel aka [Juste-D](https://www.youtube.com/@JusteDni)**, whose
  remastered soundtracks are the first music mod, shared with his permission
  and kept outside this repository (see [MUSIC_MODS.md](MUSIC_MODS.md)).

## If any of this changes

Selling, sponsoring or otherwise monetising this project is the one thing the
engine's licence forbids, and it would have to be taken up with mstan first.
Adding a dependency means adding a row here. Adding game data is never allowed.
