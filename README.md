# Golden Sun Recompiled

Native PC ports of **Golden Sun** (The Broken Seal) and **Golden Sun: The Lost
Age** for the Game Boy Advance, made by static recompilation, for Windows and
Linux (including the Steam Deck). Each game's ARM/Thumb code is translated
ahead of time into C++ by the shared engine in `engine/` (an edited copy of
gbarecomp), then compiled into native code without requiring a GBA BIOS.
Code first discovered during play can temporarily run through an interpreter
while it is compiled into native code.

No game data is in this repository or in the releases. You supply your own
legally obtained USA/Europe ROMs; the game code is generated from them on your
machine. The launcher checks each ROM's SHA-1 unless you enable
**Skip the ROM check**; the supported ROMs remain the USA/Europe versions.

> [!WARNING]
> **Work in progress.** Both games are in public testing (pre-release
> builds). Bugs and visual/audio inaccuracies are still present.

## Playing

1. Download the Windows or Linux zip from
   [Releases](https://github.com/Shmargus/GSRecomp/releases) and unzip it.
2. Start `GoldenSunLauncher` (`GoldenSunLauncher.exe` on Windows), pick The
   Broken Seal or The Lost Age, and choose that game's ROM. It remembers each.
3. On first start, and after an update, the launcher builds the game code from
   your ROM with a progress bar. A compiler is included; Linux needs SDL2 and
   OpenGL from the system.
4. Select the cartridge to play. Settings are in the launcher's gold cog and
   in-game on F1.

If something goes wrong, press **F12** in the game right after it: the game
keeps the last two seconds, and the launcher packs a bug report you can send
with **Send report**. ROMs live in `ROMs/`, saves and savestates in `Saves/`.
Control bindings live in `Controls/` and are shared; other F1 settings remain
per game. On first launch, existing bindings from that game seed the shared
settings.

Each game's README describes its features, known issues and game-specific
build steps: [The Broken Seal](The%20Broken%20Seal/README.md) and
[The Lost Age](The%20Lost%20Age/README.md).

## 0.5.2

0.5.2 includes the TLA map-opening crash fix, TBS world-map
cave flicker fix, further TLA sprite/scenery fixes and sailing Walk Speed.
Seamless mode lets you choose the Clear Data slot, and TLA's F1 → Game offers
**Update from The Broken Seal** for an existing saved adventure before the
reunion. The update keeps a backup; load the updated slot to use it.

F1 keyboard, controller and hotkey bindings carry between games. Under
**Controls**, use the **Controller** and **Alternate** columns to bind the
D-pad and control stick at the same time. Bind **Soft Reset** under
**Hotkeys** to restart the current game at its title screen; after Seamless
handover it restarts TLA without repeating the transfer. Unsaved progress
is lost. Soft Reset is unassigned by default.

Both games restore asynchronous GPU readback. The launcher shows its version
and accepts any ROM filename with the required `.gba` extension. Linux
releases build and bundle with GCC 15 or newer.
See the [release notes](https://github.com/Shmargus/GSRecomp/releases) for
the contents of each download. Updating can require the launcher to prepare
your game code again from your ROM.

## Known issues

- Loading a savestate in a larger view than it was made in can still cause
  garbled graphics across the screen.
- TLA: some Gondowan Cliffs scenery outside the original view is garbled;
  snow scenery can appear late, and sailing scenery remains around the map
  screen.
- TLA: Raging Flood has shape, edge-cutoff and slowdown issues. Great
  Gabomba has a text-shaped effect and ritual slowdown.
- TLA: startup/loading pauses remain under investigation. One Djinn-joining
  animation stutter was reported in playtesting and has not been reproduced.
- TBS: intro rain does not fill the wider view, and Kolima's pond-drain
  scenery does not follow the water properly.
- Performance improvements are still being investigated for both games,
  including Turbo speed in TLA. A speedup from the readback changes on slower
  graphics cards has not been measured.

## Layout

```text
GoldenSunLauncher.exe   the one launcher for both games
engine/                 shared engine and translator (edited gbarecomp)
launcher/               launcher source: game selection, builds, settings, reports
player_builder/         builder that player packages run on the player's ROM
scripts/                shared build, readiness and release scripts
build_dev.bat           developer build of both games and the launcher
build_launcher.bat      developer build of the launcher only
make_release.bat        player release window
The Broken Seal/        GS1: runner, ROM configuration, hooks, tools, build tree
The Lost Age/           TLA: runner, ROM configuration, hooks, tools, build tree
assets/                 launcher artwork
ROMs/  Saves/           your ROMs and progress (never committed)
Controls/               shared keyboard, controller and hotkey bindings
```

Generic ARMv4T/GBA engine fixes go in `engine/` and rebuild both games. Code
both games share (the field renderer in `engine/src/field/`, the F12 history,
the options file) lives there too and takes each game's addresses from that
game. Each game's addresses, hooks and generated code stay in its own folder.

## Building

See [BUILDING.md](BUILDING.md). In short: the launcher builds a game from its
cartridge or the gold cog, and `build_dev.bat` builds both developer games
and the launcher.

## License

`engine/` is an edited copy of [gbarecomp](https://github.com/mstan/gbarecomp)
by Matthew Stan, licensed under the PolyForm Noncommercial License 1.0.0
(`engine/LICENSE`), so this project is noncommercial. See `LICENSE` for the
rest of the repository.

---

<sub>Portions of this project were developed with the assistance of AI-based development tools.</sub>
