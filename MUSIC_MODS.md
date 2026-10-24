# Music mods

Golden Sun Recompiled can play your own recordings in place of the games'
music: a remastered soundtrack, an orchestral cover album, anything. This
page explains how to set one up, how to make your own, and how it works.

No music ships with the project. Use recordings you are allowed to use.

The first music mod is the **remastered Golden Sun and Golden Sun: The Lost
Age soundtracks by Vincent Dortel aka [Juste-D](https://www.youtube.com/@JusteDni)**, shared with
his permission. Many thanks to him for the remaster and for his help with
the loop points. Everything below was worked out on his recordings.

* [Download the music mod](https://drive.google.com/file/d/1oMiW5pYbjg3Dl9LHHVEYZLpP3jZoryLo/view?usp=drive_link) (both albums with their
  `loops.txt`, ready to unzip next to the launcher).
* The original albums: [Juste-D's Google Drive](https://drive.google.com/drive/folders/1Q2MWzb_uSvXp0Z3vWthvErnzM3xyL6Em).

## Using a music mod

1. Next to `GoldenSunLauncher.exe`, make a folder `Music` with a folder
   `TBS` (The Broken Seal) and/or `TLA` (The Lost Age) inside it.
2. Put the recordings in, named by their track number (see below), plus the
   mod's `loops.txt` if it has one.
3. Start the launcher. The first time it finds a music folder it asks
   whether to use it. Later you can switch it with **Enable Music Mod**
   under the cogwheel, or in game under **F1 → Audio** (takes effect
   straight away).

```
Golden Sun Recompiled/
  GoldenSunLauncher.exe
  Music/
    TBS/
      1-01 - The First Book.flac
      1-02 - Page One.flac
      ...
      loops.txt
    TLA/
      2-01 - The Second Book.flac
      ...
      loops.txt
```

The Lost Age uses its own folder first. For the songs it borrows from The
Broken Seal (the victory fanfare, Colosso, Isaac's battle theme and about 40
more) it plays the matching recording from `Music/TBS`, so having both
folders gives The Lost Age a complete soundtrack.

## Making your own

### 1. Files

* **Format:** FLAC (16 or 24-bit), mono or stereo, any sample rate. It is
  converted on the fly.
* **Names:** `<disc>-<NN> <anything>.flac`, for example
  `1-09 - An Adept's Home.flac`. Only `NN`, the track number, matters: the
  disc digit and the title are for you. One file per number.
* **Track numbers** follow the order of the Golden Sun soundtrack albums
  most remasters use. Each number is tied to the game song(s) it replaces:

#### The Broken Seal (`Music/TBS`)

| File | Song | File | Song |
|---|---|---|---|
| `1-01` | The First Book | `1-23` | Boss Battle (GS1) |
| `1-02` | Page One | `1-24` | Happy Towns |
| `1-03` | On that night, 3 years ago | `1-25` | Oriental |
| `1-04` | Isaac Battle | `1-26` | Desert Heat |
| `1-05` | Victory | `1-27` | Linked Battle |
| `1-06` | Saturos and Menardi | `1-28` | Kalay |
| `1-07` | Fallen Heroes (GS1) | `1-29` | Set Sail! Through the Karagol Sea |
| `1-08` | Prelude | `1-30` | Crossbone Isle |
| `1-09` | An Adept's Home | `1-31` | Tolbi |
| `1-10` | Sol Sanctum | `1-32` | Having Fun |
| `1-11` | The Elemental Stars | `1-33` | Colosso Starts |
| `1-12` | Sol Sanctum Erupts | `1-34` | Ready for a Challenge |
| `1-13` | The Angarian Journey | `1-35` | Colosso Battle |
| `1-14` | A Little Friendship | `1-36` | Altmiller Cave - Tunnel Ruins |
| `1-15` | Mysterious Caves | `1-37` | In the Presence of a Lord |
| `1-16` | Hopelessness | `1-38` | Venus Lighthouse |
| `1-17` | Mercury Lighthouse | `1-39` | Saturos and Menardi Battle |
| `1-18` | Saturos Battle | `1-40` | Fusion Dragon Battle |
| `1-19` | Winterly Imil | `1-41` | The Final Beacon |
| `1-20` | The Royal Palace | `1-42` | The First Book Extended |
| `1-21` | Forest's Requiem | `1-43` | The Golden Sun Sets |
| `1-22` | Inside Tret - Altin Peak's Flood |  |  |

#### The Lost Age (`Music/TLA`)

| File | Song | File | Song |
|---|---|---|---|
| `2-01` | The Second Book | `2-28` | Apojii Islands |
| `2-02` | Moving Worlds | `2-29` | Aqua Rock |
| `2-03` | Jenna Battle | `2-30` | Yallam |
| `2-04` | Trouble is brewing | `2-31` | Yepp's Song |
| `2-05` | Traversing Weyard | `2-32` | Taopo Swamp |
| `2-06` | Felix Battle | `2-33` | Tundarian Tower |
| `2-07` | Fallen Heroes (GS2 version) | `2-34` | Ankohl Ruins |
| `2-08` | Drums of Daila | `2-35` | There Goes Briggs |
| `2-09` | Kandorean Temple Entrance | `2-36` | Sea of Time |
| `2-10` | Kandorean Temple Labyrinth | `2-37` | Ruins of Lemuria |
| `2-11` | Boss Battle (GS2) | `2-38` | Freezing Kalt |
| `2-12` | Cavernous Shadows | `2-39` | Mystical Shaman Village |
| `2-13` | Little Madra | `2-40` | Place your bets |
| `2-14` | Beneath the Surface | `2-41` | Jupiter Lighthouse |
| `2-15` | Alhafra | `2-42` | Agatio and Karst |
| `2-16` | Non-Adept Battle | `2-43` | Agatio and Karst Battle |
| `2-17` | Air's Rock | `2-44` | Alone |
| `2-18` | A Full Moon in Garoh | `2-45` | Walking forward with determination |
| `2-19` | Scaling Mountains | `2-46` | Magma Rock |
| `2-20` | Nocturnal Ritual | `2-47` | Frozen in Prox |
| `2-21` | Inside the Great Gabomba | `2-48` | Mars Lighthouse |
| `2-22` | Full Speed Ahead | `2-49` | Enemy Appearance |
| `2-23` | Ship Battle | `2-50` | Doom Dragon Battle |
| `2-24` | Izumo in Despair | `2-51` | Sorrow and Regret |
| `2-25` | Gaia Rock | `2-52` | The Golden Sun Rises |
| `2-26` | Dragons Battle | `2-53` | Welcome Home (Not) |
| `2-27` | Festival in Izumo | `2-54` | Welcome Home |

Numbers above 54 in `Music/TLA` (bonus tracks, alternate takes) are loaded
but not played. A missing number just keeps that song's original music.

Each recording should be a faithful arrangement of the game's song at the
game's tempo: intro, then the looping part at least once more, and ideally
the loop played twice before the track ends or fades. That is what lets the
loop be cut cleanly.

### 2. Loop points (`loops.txt`)

The game loops most songs from a fixed point back to an earlier one; a
recording has to loop at the same musical spot or it jumps. Each folder's
`loops.txt` tells the game where, in the recording's own samples:

```
# <track> <jump_from> <jump_to>   play until jump_from, carry on at jump_to
1 3047759 222369
# <track> once                    the game plays this song once; stop at the end
8 once
```

Positions count sample frames at the file's own sample rate (at 44.1 kHz,
one second is 44100). Anything after `#` is a comment. A track with no line
repeats the whole file.

You can write these by hand (an audio editor shows sample positions), or
let the tool work them out from your ROM:

```
python "The Broken Seal/tools/music_loops.py" --rom "The Broken Seal.gba" --music "Music/TBS"
python "The Broken Seal/tools/music_loops.py" --game tla --rom "The Lost Age.gba" --music "Music/TLA"
```

It needs Python 3 with `numpy`, and `ffmpeg` on your PATH (or pass
`--ffmpeg <path>`). It only accepts the supported USA/Europe ROMs, uses
nothing but the songs' note data, and writes `loops.txt` into the folder. Each
line's comment gives the musical loop start and end in file time and how
closely the recording matches itself at the chosen jump (100% = the two
passes are identical there). A low match means the recording differs
between passes; listen to that song's loop.

### 3. Share it

A music mod is the `Music` folder: the recordings plus `loops.txt`. Players
drop it next to the launcher. The loop points only fit those exact files,
so ship them together.

## How it works

**The games' own loop points.** Both games use Nintendo's standard GBA
sound driver ("m4a" or "Sappy"). Every song is note data in the ROM: each
instrument part ends with a jump back to its loop start, or with an end
marker for songs that play once. Reading that data gives the loop start and
end in musical ticks (24 per beat) and the song's tempo.

**Lining up a recording.** The tool finds where the song's first beat sits
in the recording by matching the timing of the game's notes against the
attacks in the audio. Juste-D's remaster follows the game's tempo exactly: in
every looping song the recording repeats after precisely the game's loop
length (we measured it). So the loop length in samples comes straight from
the note data, and the start comes from the alignment. A recording played
at a different tempo will not line up; its matches come out low.

**A clean jump.** The loop length is fixed, but the jump can happen at any
point inside the loop. The tool picks the spot where the recording's next
pass sounds most like the current one, so a cut-off echo or a slightly
different first pass doesn't click. For The Lost Age we matched each album
track to its song in the ROM the same way: rhythm, notes and exact loop
length.

**Playing it.** When the game starts a song, the engine looks the song up
in that game's table (`The Broken Seal/src/local_music_tracks.h`,
`The Lost Age/src/tla_local_music_tracks.h`) and starts the recording. The
game's own song keeps running silently, so its timing, fades and pauses
still apply to the recording, and sound effects are untouched. The first
quarter second of every recording is ready in memory from startup, and the
rest streams from disk (FLAC decoding by
[dr_flac](https://github.com/mackron/dr_libs)). The loop jump happens before
the audio is converted to the output rate, so it is seamless.

**Code:** `engine/src/runtime/host_local_music.cpp` (files, loops,
streaming), `engine/src/runtime/local_music_m4a.h` (taking over a song from
the sound driver), `The Broken Seal/tools/music_loops.py` (loop points).
