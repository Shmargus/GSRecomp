"""Make the ROM copy the self-patching RAM routines are translated from.

Some routines copy a block of their own code onto zero-filled space every
time they run (config/tla/reloc/*.toml, "# post-setup-copy SRC:DST:WORDS",
offsets from the routine's ROM source; tools/tla_reloc_configs.py has the
evidence). Their body is translated from the bytes as they stand after that
copy. This writes --out/selfpatch.gba, the ROM with every such copy applied,
and --out/<name>.toml, each such config with the [identity] SHA-1 of that
copy. All output is ROM-derived and belongs under local/ (gitignored).

Usage: python tools/tla_selfpatch_rom.py --rom <rom> --configs config/tla/reloc --out local/tla/selfpatch
"""
import argparse
import hashlib
import os
import re

ROM_SHA1 = "b500663220cb9bf56b9f8e8c0c544f1d6fa3a824"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", required=True)
    ap.add_argument("--configs", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    rom = open(args.rom, "rb").read()
    if hashlib.sha1(rom).hexdigest() != ROM_SHA1:
        raise SystemExit("unexpected ROM")
    img = bytearray(rom)
    patched = {}
    for name in sorted(os.listdir(args.configs)):
        if not name.endswith(".toml"):
            continue
        text = open(os.path.join(args.configs, name), encoding="utf-8").read()
        copies = re.findall(r"(?m)^# post-setup-copy (0x[0-9A-F]+):(0x[0-9A-F]+):(0x[0-9A-F]+)", text)
        if not copies:
            continue
        source = int(re.search(r"source_start\s*=\s*(0x[0-9A-Fa-f]+)", text).group(1), 16)
        base = source - 0x08000000
        for src, dst, words in copies:
            src, dst, n = int(src, 16), int(dst, 16), 4 * int(words, 16)
            if any(img[base + dst:base + dst + n]):
                raise SystemExit("%s: copy target is not zero-filled" % name)
            img[base + dst:base + dst + n] = rom[base + src:base + src + n]
        patched[name] = text
    os.makedirs(args.out, exist_ok=True)
    open(os.path.join(args.out, "selfpatch.gba"), "wb").write(img)
    sha = hashlib.sha1(img).hexdigest()
    for name, text in patched.items():
        text = re.sub(r'(?m)^sha1 = "[0-9a-f]+"', 'sha1 = "%s"' % sha, text)
        open(os.path.join(args.out, name), "w", encoding="utf-8", newline="\n").write(text)
        print(name[:-5])


if __name__ == "__main__":
    main()
