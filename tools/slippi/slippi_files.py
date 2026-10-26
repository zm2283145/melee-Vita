# SPDX-License-Identifier: GPL-3.0-or-later
"""Make Slippi's menu files from your own Melee disc, for the Vita.

Slippi Dolphin patches a few of Melee's menu archives when it runs:
- the "Online Play" entry and the Online submenu labels (MnMaAll);
- their descriptions (SdMenu);
- the stage select screen (MnSlMap);
- character select text (SdSlChr);
- name entry (MnExtAll).
The patches (tools/slippi/gamefiles/*.usd.diff) are VCDIFF deltas copied
from project-slippi/dolphin, Data/Sys/GameFiles/GALE01 (commit 41a7a3a,
GPL-2.0+), by way of the 3DS Slippi port (melee-3ds slippi-beta-v0.1.0,
native-3ds/tools/slippi/slippi_files.py).  This reads the original files
from your NTSC 1.02 disc image, applies the patches and writes the results
to OUT.  Copy them to ux0:data/melee/slippi/files/ on the Vita, which loads
them in place of the disc's copies (platforms/vita/game/dvd.c).

    python3 tools/slippi/slippi_files.py GALE01.iso OUT

A folder of files extracted from the disc works in place of the image.
"""
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from vcdiff import decode  # noqa: E402

NAMES = ('MnMaAll.usd', 'SdMenu.usd', 'MnSlMap.usd', 'SdSlChr.usd', 'MnExtAll.usd')


def read_from_iso(iso_path, wanted):
    """The files named in wanted, by name, from a GameCube disc image."""
    found = {}
    with open(iso_path, 'rb') as iso:
        header = iso.read(0x440)
        if header[:6] != b'GALE01':
            raise SystemExit(f'{iso_path} is not an NTSC Melee disc (game id {header[:6]!r})')
        fst_offset, fst_size = struct.unpack('>II', header[0x424:0x42C])
        iso.seek(fst_offset)
        fst = iso.read(fst_size)
        # The root entry's third word is the number of entries.  The names
        # come right after the entries.
        count = struct.unpack('>I', fst[8:12])[0]
        names_start = count * 12
        for index in range(1, count):
            entry = fst[index * 12:index * 12 + 12]
            is_dir = entry[0] != 0
            name_offset = int.from_bytes(entry[1:4], 'big')
            if is_dir:
                continue
            end = fst.index(b'\0', names_start + name_offset)
            name = fst[names_start + name_offset:end].decode('ascii')
            if name not in wanted:
                continue
            offset, length = struct.unpack('>II', entry[4:12])
            iso.seek(offset)
            found[name] = iso.read(length)
    return found


def read_from_folder(folder, wanted):
    found = {}
    for name in wanted:
        path = folder / name
        if not path.exists():
            path = next(folder.rglob(name), None)
        if path is not None:
            found[name] = path.read_bytes()
    return found


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    source, out = Path(sys.argv[1]), Path(sys.argv[2])
    if source.is_dir():
        originals = read_from_folder(source, NAMES)
    else:
        originals = read_from_iso(source, NAMES)
    out.mkdir(parents=True, exist_ok=True)
    made = 0
    for name in NAMES:
        if name not in originals:
            print(f'{name}: not found in {source}; skipped')
            continue
        delta = (HERE / 'gamefiles' / (name + '.diff')).read_bytes()
        try:
            data = decode(originals[name], delta)
        except ValueError as error:
            print(f'{name}: {error}; skipped (is this an NTSC 1.02 disc?)')
            continue
        # An HSD archive starts with its own size.
        if len(data) < 4 or int.from_bytes(data[:4], 'big') != len(data):
            print(f'{name}: the result is not a valid archive; skipped')
            continue
        (out / name).write_bytes(data)
        made += 1
        print(f'{name}: {len(data)} bytes')
    print(f'{made} of {len(NAMES)} patched files written to {out}')
    print('Copy them to ux0:data/melee/slippi/files/ on the Vita.')
    if made != len(NAMES):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
