#!/usr/bin/env python3
"""Generate loops.txt for The Lost Age using the shared loop finder."""

from pathlib import Path
from runpy import run_path
import sys


if __name__ == '__main__':
    collection = Path(__file__).resolve().parents[2]
    shared = run_path(str(collection / 'The Broken Seal' / 'tools' / 'music_loops.py'))
    sys.exit(shared['main'](default_game='tla'))
