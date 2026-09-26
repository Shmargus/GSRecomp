# What is in this repository, and who owns it

Plain answers first, because the licences here are not all the same and one of
them limits what this project may ever be.

## The short version

| Part | Whose | Licence | In this repo? |
|---|---|---|---|
| The game: Golden Sun, its ROM, BIOS, art, music, text | Nintendo / Camelot Software Planning | Not ours to license | **No, and never** |
| The engine, `gbarecomp/` | Matthew Stan (mstan) | PolyForm Noncommercial 1.0.0 | Yes, an edited copy |
| Code ported into the engine from others | see "Inside the engine" below | MIT / Apache-2.0 / MPL-2.0 | Yes, inside `gbarecomp/src/` |
| Everything else: `src/`, `tools/`, `tests/`, `config/`, `scripts/`, the docs | this project | not yet selected — see `LICENSE` | Yes |
| Dear ImGui, toml++, SDL2 | their authors | MIT, MIT, zlib | No — fetched or installed at build time |

**The practical consequence: this project is noncommercial.** The engine's
licence permits use, modification and redistribution for noncommercial purposes
only. That covers the whole build, because nothing here runs without the engine.

## The game

No ROM, BIOS, save, savestate, screenshot of copyrighted art, or extracted
asset is in this repository, and none may be added. You supply your own legally
obtained ROM and BIOS; both are hash-verified locally.

The recompiled C the build produces is derived from your ROM. It is written to
`generated/` and `local/`, which are not tracked here and must not be
published. `docs/LEGAL.md` is the rule for all of this.

What this repository does hold about the game is our own analysis: symbol
names, addresses, function boundaries and hardware observations in `config/`
and `symbols/` — facts measured about the program, not its content.

## The engine, `gbarecomp/`

Upstream is [`mstan/gbarecomp`](https://github.com/mstan/gbarecomp), copyright
Matthew Stan, licensed **PolyForm Noncommercial 1.0.0** — the full text is at
`gbarecomp/LICENSE` and stays there.

That licence allows what this repository does: use it, change it, and pass it
on, **for any purpose other than a commercial one**. It requires that anyone
who receives a copy also receives the licence, which is why `gbarecomp/LICENSE`
is committed alongside the code rather than referenced.

`gbarecomp/` here is **not upstream**. It is a copy that diverged on
2026-07-17 and now carries 12 commits of our own — about 47,000 added lines
across 130 files, in the PPU, the ARM runtime, the audio shadow and mixer, the
overlay loader, the host window and the recompiler tool. `gbarecomp/MODIFICATIONS.md`
describes what changed and why. Building against upstream will not reproduce
this project.

Where a fix belongs: anything about ARMv4T, GBA hardware or the recompiler goes
inside `gbarecomp/` and is a candidate to send upstream. Anything about Golden
Sun stays outside it.

### Inside the engine

The engine itself ports code from two other projects, credited in full in
`gbarecomp/THIRD_PARTY_ATTRIBUTION.md`, which is kept intact:

- **[JRickey/gba-recomp](https://github.com/JRickey/gba-recomp)**, MIT OR
  Apache-2.0, used with the author's permission — the MP2K driver detection,
  the screen-colour simulation, the cartridge RTC, and the audio shadow
  verifier.
- **[mGBA](https://github.com/mgba-emu/mgba)**, MPL-2.0 — the BIOS
  high-level-emulation routines in `gbarecomp/src/runtime/bios_hle.{h,cpp}`.
  Those files remain governed by the MPL-2.0; its text is at
  <https://mozilla.org/MPL/2.0/>. **Note:** upstream's attribution file says
  mGBA is vendored at `third_party/mgba`. It is not vendored in this copy —
  only the re-implemented files above are here, and mGBA's own source is at the
  link. The oracle target that would link mGBA is not part of this copy either.

## Our own code

`src/`, `tools/`, `tests/`, `config/`, `scripts/`, and the documentation are
this project's work. `LICENSE` has not selected a licence for them, so all
rights are reserved by default: people may read this repository, but nobody may
reuse our code until a licence is chosen. Whatever is chosen cannot escape the
engine's noncommercial terms while the engine is part of the build.

## Build-time dependencies, not in this repository

CMake fetches these at configure time, each pinned to an exact version, or uses
a local copy if one is present:

| Library | Use | Licence |
|---|---|---|
| Dear ImGui v1.91.9-docking | the in-game configuration UI | MIT |
| toml++ v3.4.0 | reading `game.toml` and the symbol metadata | MIT |

And these must be installed on the machine: a MinGW-w64 C++ toolchain, SDL2
(zlib licence), OpenGL, Python 3.10+, CMake and Ninja.

## If any of this changes

Selling, sponsoring or otherwise monetising this project is the one thing the
engine's licence forbids, and it would have to be taken up with mstan first.
Adding a dependency means adding a row here. Adding game data means breaking
`docs/LEGAL.md`, which is not a thing to do.
