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

Each game's README describes its features and game-specific build steps:
[The Broken Seal](The%20Broken%20Seal/README.md) and
[The Lost Age](The%20Lost%20Age/README.md). The
[release notes](https://github.com/Shmargus/GSRecomp/releases) list what each
version changed; updating can make the launcher prepare your game code again.

## Features

- **Wider views.** Wide and Extra Wide show more of each room and the world
  map around the original screen, with effects, weather and scenery carried
  into the extra space.
- **Shared settings.** Keyboard, controller and hotkey bindings carry between
  both games. Under F1 → Controls, the **Controller** and **Alternate**
  columns bind the D-pad and stick together; **Soft Reset** (F1 → Hotkeys,
  unassigned by default) restarts the current game at its title screen.
- **Seamless mode.** Finish The Broken Seal and carry straight on into The
  Lost Age with your Clear Data (launcher cog). The Lost Age's F1 → Game also
  offers **Update from The Broken Seal** for an existing save, keeping a
  backup.
- **Music mods.** Recordings in the `Music` folder can replace the
  soundtrack (FLAC or WAV), looping where the game loops each song. The
  remastered Golden Sun soundtracks by Vincent Dortel aka
  [Juste-D](https://www.youtube.com/@JusteDni) are available as a
  [ready-made music mod](https://drive.google.com/file/d/1oMiW5pYbjg3Dl9LHHVEYZLpP3jZoryLo/view?usp=drive_link),
  shared with his permission. See [Music mods](MUSIC_MODS.md) to use or make
  one.

## Known issues

- Loading a savestate made at a different screen size (for example Extra
  Wide, loaded in Original) can still garble graphics, such as Felix turning
  into a mess of lines. Switching the screen size in F1 and back redraws the
  screen and fixes it.
- TLA, music mods: the opening scene before Venus Lighthouse keeps the
  original music.
- TLA: Raging Flood has the wrong shape, gets cut off at the edges and causes
  slowdown.
- TLA: Great Gabomba has an effect that can show bits of text, and slowdown
  during its ritual.
- TLA: Supernova doesn't fill the wider view yet, and some other summon
  graphics have smaller issues at the edges.
- TLA: world-map quakes can still glitch at the edges; Jupiter Lighthouse's
  beacon stops short of the top of the wider view.
- TLA: walking can occasionally stutter; we haven't reproduced it yet.
- TBS: Kolima's pond scenery doesn't follow the water as it drains.
- Performance is still being worked on for both games, including Turbo speed
  in TLA.

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
MUSIC_MODS.md           how music mods work and how to make one
ATTRIBUTION.md          who owns what, licences and credits
The Broken Seal/        GS1: runner, ROM configuration, hooks, tools, build tree
The Lost Age/           TLA: runner, ROM configuration, hooks, tools, build tree
assets/                 launcher artwork
ROMs/  Saves/           your ROMs and progress (never committed)
Controls/               shared keyboard, controller and hotkey bindings
Music/                  optional music mods (never committed)
```

Generic ARMv4T/GBA engine fixes go in `engine/` and rebuild both games. Code
both games share (the field renderer in `engine/src/field/`, the F12 history,
the options file) lives there too and takes each game's addresses from that
game. Each game's addresses, hooks and generated code stay in its own folder.

## Building

See [BUILDING.md](BUILDING.md). In short: the launcher builds a game from its
cartridge or the gold cog, and `build_dev.bat` builds both developer games
and the launcher.

## Credits

- **Vincent Dortel aka [Juste-D](https://www.youtube.com/@JusteDni)**, for his remastered Golden
  Sun and Golden Sun: The Lost Age soundtracks, his permission to share them
  as a music mod, and his help with the loop points.
- **Matthew Stan**, for [gbarecomp](https://github.com/mstan/gbarecomp), the
  recompiler this project's engine is built on (see License).
- **David Reid**, for [dr_flac](https://github.com/mackron/dr_libs), which
  plays the music mods' FLAC files.

## License

`engine/` is an edited copy of [gbarecomp](https://github.com/mstan/gbarecomp)
by Matthew Stan, licensed under the PolyForm Noncommercial License 1.0.0
(`engine/LICENSE`), so this project is noncommercial. See `LICENSE` for the
rest of the repository and [ATTRIBUTION.md](ATTRIBUTION.md) for who owns what.

---

<sub>Portions of this project were developed with the assistance of AI-based development tools.</sub>
