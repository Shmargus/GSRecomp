#!/usr/bin/env python3
"""Find where each replacement recording loops, and write loops.txt for it.

The game's own songs carry their loop: every track of an m4a sequence ends
in GOTO back to a label, so the ROM gives the loop start and end in ticks
(24 per quarter note, tempo from the sequence). This tool lines each
recording up with its sequence and turns those ticks into sample positions:

  1. Sequence: loop ticks and tempo, read from the song header that the
     game's table (GS1 src/local_music_tracks.h, TLA
     src/tla_local_music_tracks.h) maps to the recording's album number.
  2. Offset: the recording's onset envelope is matched against the
     sequence's note onsets near the first sound, which places tick 0.
  3. Loop length: the sequence's loop length at its own tempo. Recordings
     made from the sequence repeat at exactly that length (measured; see
     FACTS.md "Local music loop points").
  4. Jump point: the loop length is fixed, but where to jump is free. The
     tool picks the spot inside the loop where the recording's next pass
     sounds most like the current one (level-sensitive match over 2048
     samples), so a cut reverb tail or a differently rendered first pass
     does not click.

loops.txt (in the music folder) holds, per album track:
    <track> <jump_from> <jump_to>     play to jump_from, continue at jump_to
    <track> once                      the game's song ends; play once
in the file's own sample frames. Comments give the musical loop start and
end in file time. The engine reads it (host_local_music.cpp).

Needs numpy and ffmpeg (to decode the FLAC files).
  python tools/music_loops.py --rom "../ROMs/The Broken Seal.gba" \\
      --music "../Music/TBS"
  python tools/music_loops.py --game tla --rom "../ROMs/The Lost Age.gba" \\
      --music "../Music/TLA"
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
COLLECTION = os.path.dirname(REPO)
# --game: approved ROM SHA-1, name, song header -> album track table.
GAMES = {
    'tbs': ('5c4695205413df7db52b9a184815a07783999971', 'The Broken Seal',
            os.path.join(REPO, 'src', 'local_music_tracks.h')),
    'tla': ('b500663220cb9bf56b9f8e8c0c544f1d6fa3a824', 'The Lost Age',
            os.path.join(COLLECTION, 'The Lost Age', 'src',
                         'tla_local_music_tracks.h')),
}
RATE = 44100
ONSET_HOP, ONSET_FFT = 256, 2048
SEAM_WINDOW = 2048

# m4a wait lengths for 0x80..0xB0 (and note lengths for 0xD0..0xFF).
WAITS = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
         19, 20, 21, 22, 23, 24, 28, 30, 32, 36, 40, 42, 44, 48, 52, 54, 56,
         60, 64, 66, 68, 72, 76, 78, 80, 84, 88, 90, 92, 96]
ONE_ARG = {0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xC0, 0xC1, 0xC2, 0xC3,
           0xC4, 0xC5, 0xC8}


def u32(rom: bytes, offset: int) -> int:
    return struct.unpack_from('<I', rom, offset)[0]


def parse_track(rom: bytes, start: int) -> dict:
    """Walk one track to its GOTO/FINE: note onsets, tempos, loop ticks."""
    pos, tick = start, 0
    seen: dict[int, int] = {}
    onsets, tempos = [], []
    running = None
    calls: list[int] = []
    for _ in range(200000):
        if not calls:
            seen.setdefault(pos, tick)
        cmd = rom[pos]
        if cmd < 0x80:
            if running is None:
                raise ValueError(f'running status without a command at {pos:X}')
            cmd = running
        else:
            pos += 1
            if cmd >= 0xBD:
                running = cmd
        if 0x80 <= cmd <= 0xB0:
            tick += WAITS[cmd - 0x80]
        elif cmd == 0xB1:                                   # FINE
            return dict(onsets=onsets, tempos=tempos, loop=None, end=tick)
        elif cmd == 0xB2:                                   # GOTO
            target = u32(rom, pos) - 0x08000000
            pos += 4
            if calls or target not in seen:
                raise ValueError(f'unsupported GOTO at {pos - 5:X}')
            return dict(onsets=onsets, tempos=tempos,
                        loop=(seen[target], tick), end=tick)
        elif cmd == 0xB3:                                   # PATT
            calls.append(pos + 4)
            pos = u32(rom, pos) - 0x08000000
        elif cmd == 0xB4:                                   # PEND
            if calls:
                pos = calls.pop()
        elif cmd == 0xB9:                                   # MEMACC
            pos += 3
        elif cmd in ONE_ARG:
            if cmd == 0xBB:
                tempos.append(rom[pos] * 2)
            pos += 1
        elif cmd == 0xCD:                                   # XCMD
            pos += 2
        elif cmd == 0xCE:                                   # EOT
            if rom[pos] < 0x80:
                pos += 1
        elif cmd == 0xCF or cmd >= 0xD0:                    # TIE / note
            velocity = 127
            if rom[pos] < 0x80:
                pos += 1
                if rom[pos] < 0x80:
                    velocity = rom[pos]
                    pos += 1
                    if cmd != 0xCF and rom[pos] < 0x80:
                        pos += 1
            onsets.append((tick, velocity))
        else:
            raise ValueError(f'unknown command {cmd:02X} at {pos - 1:X}')
    raise ValueError(f'track at {start:X} does not end')


def parse_song(rom: bytes, header: int) -> dict:
    base = header - 0x08000000
    tracks = [parse_track(rom, u32(rom, base + 8 + 4 * i) - 0x08000000)
              for i in range(rom[base])]
    loops = {t['loop'] for t in tracks}
    tempos = {b for t in tracks for b in t['tempos']}
    if len(loops) != 1 or len(tempos) != 1:
        raise ValueError(f'song {header:X}: loops {loops}, tempos {tempos}')
    return dict(loop=loops.pop(), bpm=tempos.pop(),
                end=max(t['end'] for t in tracks),
                onsets=[o for t in tracks for o in t['onsets']])


def recordings(table: str) -> list[tuple[int, int, str]]:
    text = open(table, encoding='utf-8').read()
    return [(int(h, 16), int(n), name) for h, n, name in re.findall(
        r'\{0x([0-9A-Fa-f]+)u, (\d+)u\}, // song \d+: (.*)', text)]


def decode(ffmpeg: str, path: str) -> np.ndarray:
    raw = subprocess.run([ffmpeg, '-v', 'error', '-i', path, '-f', 'f32le',
                          '-ac', '2', '-ar', str(RATE), '-'],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.float32).reshape(-1, 2)


def file_rate(ffprobe: str, path: str) -> int:
    out = subprocess.run([ffprobe, '-v', 'error', '-select_streams', 'a:0',
                          '-show_entries', 'stream=sample_rate', '-of',
                          'csv=p=0', path], capture_output=True, check=True)
    return int(out.stdout.strip())


def onset_envelope(x: np.ndarray) -> np.ndarray:
    count = 1 + (len(x) - ONSET_FFT) // ONSET_HOP
    window = np.hanning(ONSET_FFT).astype(np.float32)
    env = np.zeros(count, np.float32)
    previous = None
    for first in range(0, count, 2048):
        index = np.arange(first, min(count, first + 2048))
        frames = np.stack([x[i * ONSET_HOP:i * ONSET_HOP + ONSET_FFT]
                           for i in index]) * window
        mag = np.log1p(100 * np.abs(np.fft.rfft(frames, axis=1)))
        before = mag[:1] if previous is None else previous[None]
        env[index] = np.maximum(np.diff(np.vstack([before, mag]), axis=0),
                                0).sum(axis=1)
        previous = mag[-1]
    env -= np.convolve(env, np.ones(64) / 64, mode='same')
    return np.maximum(env, 0)


def place_tick_zero(x: np.ndarray, song: dict, seconds_per_tick: float,
                    end_tick: int) -> int:
    """Sample where the sequence's tick 0 falls in the recording."""
    level = np.convolve(np.abs(x), np.ones(441) / 441, 'same')
    first_sound = int(np.argmax(level > level.max() * 0.01))
    first_note = min(t for t, _ in song['onsets'])
    guess = first_sound - int(first_note * seconds_per_tick * RATE)
    frames = int(end_tick * seconds_per_tick * RATE / ONSET_HOP) + 1
    ref = np.zeros(frames, np.float32)
    for tick, velocity in song['onsets']:
        i = int(round(tick * seconds_per_tick * RATE / ONSET_HOP))
        if tick < end_tick and i < frames:
            ref[i] += velocity / 127.0
    ref = np.convolve(ref, np.hanning(9), mode='same')
    env = onset_envelope(x)
    best = None
    radius = int(0.3 * RATE) // ONSET_HOP
    for lag in range(guess // ONSET_HOP - radius, guess // ONSET_HOP + radius + 1):
        a, b = max(0, lag), min(len(env), lag + frames)
        if b <= a:
            continue
        score = float((env[a:b] * ref[a - lag:b - lag]).sum())
        if best is None or score > best[0]:
            best = (score, lag)
    # An onset frame covers ONSET_FFT samples; its attack sits mid-frame.
    return best[1] * ONSET_HOP + ONSET_FFT // 2


def best_jump(x: np.ndarray, start: int, length: int) -> tuple[int, float]:
    """Point t in [start, start+length) where x[t+length:] best continues
    like x[t:]; the match is level-sensitive (a fade does not match)."""
    w = SEAM_WINDOW
    lo = max(start, w // 2)
    hi = min(start + length, len(x) - length - w)
    if hi <= lo:
        raise ValueError('recording too short to hold a second pass')
    a = x[lo - w // 2:hi + w // 2].astype(np.float64)
    b = x[lo + length - w // 2:hi + length + w // 2].astype(np.float64)

    def box(v):
        c = np.concatenate([[0.0], np.cumsum(v)])
        return c[w:] - c[:-w]
    diff = box(((a - b) ** 2).sum(axis=1))
    energy = box((a ** 2).sum(axis=1) + (b ** 2).sum(axis=1))
    loud = energy > energy.max() * 0.05       # silence matches trivially
    match = np.where(loud, 1 - diff / (energy + 1e-12), -1)
    i = int(match.argmax())
    return lo + i, float(match[i])


def clock(frames: int, rate: int) -> str:
    seconds = frames / rate
    return f'{int(seconds // 60)}:{seconds % 60:06.3f}'


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--rom', required=True)
    parser.add_argument('--music', required=True,
                        help='folder of "<disc>-<NN> - <title>.flac" files')
    parser.add_argument('--game', choices=sorted(GAMES), default='tbs')
    parser.add_argument('--table', help="song header -> album track table "
                        "(default: the game's own)")
    parser.add_argument('--ffmpeg', default=shutil.which('ffmpeg') or 'ffmpeg')
    args = parser.parse_args()
    ffprobe = os.path.join(os.path.dirname(args.ffmpeg), 'ffprobe')
    sha1, game_name, table = GAMES[args.game]
    rom = open(args.rom, 'rb').read()
    if hashlib.sha1(rom).hexdigest() != sha1:
        print(f'ROM SHA-1 does not match {game_name} (USA/Europe).')
        return 1
    files = {}
    for name in os.listdir(args.music):
        m = re.match(r'\d+-(\d+) .*\.(flac|wav)$', name, re.IGNORECASE)
        if m:
            files[int(m.group(1))] = name
    lines = ['# Loop jumps for the local music player (tools/music_loops.py).',
             '# <track> <jump_from> <jump_to> in file sample frames, or',
             '# <track> once. Comments: the game loop in file time, and how',
             '# closely the next pass matches at the jump (100% = identical).']
    done = set()
    for header, track, title in recordings(args.table or table):
        if track in done:     # another song playing the same recording
            continue
        done.add(track)
        if track not in files:
            print(f'{track:2}  missing')
            continue
        path = os.path.join(args.music, files[track])
        song = parse_song(rom, header)
        spt = 60.0 / (song['bpm'] * 24.0)
        x = decode(args.ffmpeg, path)
        rate = file_rate(ffprobe, path)
        scale = rate / RATE
        if song['loop'] is None:
            lines.append(f'{track} once  # {title}: the game plays it once')
            print(f'{track:2}  {title}: once')
            continue
        loop_start_tick, loop_end_tick = song['loop']
        zero = place_tick_zero(x.mean(axis=1), song, spt, loop_end_tick)
        start = max(0, zero + int(round(loop_start_tick * spt * RATE)))
        length = int(round((loop_end_tick - loop_start_tick) * spt * RATE))
        jump_to, match = best_jump(x, start, length)
        f = lambda v: int(round(v * scale))
        lines.append(
            f'{track} {f(jump_to + length)} {f(jump_to)}  # {title}: loop '
            f'{clock(start, RATE)} -> {clock(start + length, RATE)}, jump '
            f'{clock(jump_to + length, RATE)} -> {clock(jump_to, RATE)}, '
            f'match {match:.1%}')
        print(lines[-1])
    out = os.path.join(args.music, 'loops.txt')
    with open(out, 'w', encoding='utf-8', newline='\n') as handle:
        handle.write('\n'.join(lines) + '\n')
    print(f'wrote {out}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
