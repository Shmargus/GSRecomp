"""The Lost Age's supported ROM revisions for the Windows/Linux translation
pipeline, read from the one list, src/tla_rom_revisions.inc (row 0 is
English and uses config/tla and local/tla; every later row its own code)."""
import hashlib
import re
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
_ROW=re.compile(r'^GSR_ROM_REVISION\("([a-z]{2})", "([^"]+)", "([0-9a-f]{40})", '
                r'(0x[0-9A-Fa-f]+)u, (0x[0-9A-Fa-f]+)u, "([a-z]{2})", (?:true|false)\)', re.M)


def _revisions():
    rows=_ROW.findall((ROOT/"src"/"tla_rom_revisions.inc").read_text(encoding="utf-8"))
    out={}
    for index,(code,name,sha1,builder,allocator,_language) in enumerate(rows):
        config="tla" if index==0 else code
        out[sha1]=dict(config=config,corpus="tla" if index==0 else f"{code}/tla",name=name,
                       allocator=int(allocator,16),builder=int(builder,16))
    return out


REVISIONS=_revisions()
ENGLISH=next(iter(REVISIONS))


def identify(rom):
    digest=hashlib.sha1(rom).hexdigest()
    if not 0x01000000<=len(rom)<=0x02000000 or digest not in REVISIONS:
        names=", ".join(r["name"] for r in REVISIONS.values())
        raise ValueError(f"Expected a verified The Lost Age ROM ({names})")
    return digest,REVISIONS[digest]
