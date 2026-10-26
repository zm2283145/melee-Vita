# SPDX-License-Identifier: GPL-3.0-or-later
"""Pack Slippi .slp files into replay files for the Vita's replay test.

The Vita plays the replay's recorded inputs through the game and compares
every fighter's state with what Slippi recorded, frame by frame, on the
Vita itself (src/melee/slippi/slippi_replay.c).  The result goes to
ux0:data/melee/slippi/slippi.log, so nothing needs copying back.

This is the 3DS Slippi port's SLR1 format (tools/slippi/replay_pack.py,
copied from melee-3ds) with the expected values added to every entry.

Layout, all big-endian:
  0x000 'VSR2'
  0x004 u8 UCF version the replay ran (84 = 0.84, 80 = 0.8, 0 = none),
        u8 Slippi major, minor, build
  0x008 game info block (0x138 bytes)
  0x140 u32 seed  0x144 s32 first frame  0x148 u32 frame count
  0x14C the .slp file name, 60 bytes, NUL padded
  0x188 u32 flags (1 = Final Destination's background is frozen)
  0x18C frames: u32 frame-start seed, u8 entry count, 3 pad, then per entry
        88 bytes:
   0  u8 port, u8 follower, s8 raw stick x/y, s8 raw c-stick x/y, 2 pad
   8  f32 stick x/y, c-stick x/y, trigger, u32 buttons     (the inputs)
  32  u8 flags (1 = pre-frame values, 2 = post-frame values, 4 = pre
      percent), u8 stocks, u16 pre action, u32 pre seed, f32 pre x, pre y
  48  u16 post action, 2 pad, f32 post x, y, facing, percent, shield,
      animation frame
  76  f32 pre facing, pre percent, 4 pad
Floats keep the exact bits Slippi recorded.

One replay:
    python3 tools/slippi/vita_replay_pack.py Game.slp replay1.bin
A batch (writes replay1.bin, replay2.bin, ... into the folder):
    python3 tools/slippi/vita_replay_pack.py --batch out_folder A.slp B.slp ...
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from slp import parse  # noqa: E402

HEADER_SIZE = 0x18C
ENTRY_SIZE = 88
NAME_SIZE = 60

# Header flags.
FLAG_FROZEN_FD = 1


def gecko_codes(gecko):
    """A replay's code list: the C2 (inject) addresses, and the 04 (write
    a word) codes as {address: word}."""
    injections = set()
    writes = {}
    i = 0
    while i + 8 <= len(gecko):
        word0, word1 = struct.unpack('>II', gecko[i:i + 8])
        if word0 == 0xFF000000:
            break
        kind = (word0 >> 24) & 0xFE
        address = (word0 & 0x01FFFFFF) | 0x80000000
        if kind == 0xC2:
            injections.add(address)
            i += 8 + word1 * 8
        elif kind == 0x06:
            i += 8 + ((word1 + 7) // 8) * 8
        else:
            if kind == 0x04:
                writes[address] = word1
            i += 8
    return injections, writes


def gecko_injections(gecko):
    return gecko_codes(gecko)[0]


def header_flags(replay):
    """Codes the Vita has to copy to match the replay.

    Slippi 3.14 writes a nop over Final Destination's background
    transformation call (04 8021aae4 60000000); newer Slippi injects
    DesyncProofBGTransformations (C2) there instead."""
    _, writes = gecko_codes(replay.gecko)
    flags = 0
    if writes.get(0x8021AAE4) == 0x60000000:
        flags |= FLAG_FROZEN_FD
    return flags


def ucf_version(replay):
    """Which UCF the match ran, from the codes Slippi recorded.

    UCF 0.84 adds "Pad Buffer + 1.0 Cardinals" at 8006b460.  UCF 0.8 has
    only dashback (800c9a44), shield drop and tumble.  Slippi 3.14 ran
    0.8, and newer Slippi runs 0.84."""
    addresses = gecko_injections(replay.gecko)
    if 0x8006B460 in addresses:
        return 84
    if 0x800C9A44 in addresses:
        return 80
    return 0


def pack_entry(port, follower, pre, post):
    entry = bytearray()
    entry += struct.pack('>BB4B2x', port, follower, *pre['raw'])
    entry += pre['lstick'][0] + pre['lstick'][1] + pre['cstick'][0] + pre['cstick'][1]
    entry += pre['trigger'] + pre['buttons']
    assert len(entry) == 32

    flags = 1
    stocks = 0
    if post is not None:
        flags |= 2
        stocks = post['stocks']
    pre_percent = 0
    if pre['percent'] is not None:
        flags |= 4
        pre_percent = pre['percent']
    entry += struct.pack('>BBHIII', flags, stocks, pre['action'], pre['seed'], pre['x'], pre['y'])
    assert len(entry) == 48

    if post is None:
        entry += bytes(76 - 48)
    else:
        entry += struct.pack('>H2xIIIIII', post['action'], post['x'], post['y'], post['facing'],
                             post['percent'], post['shield'], post['anim'])
    assert len(entry) == 76

    entry += struct.pack('>II4x', pre['facing'], pre_percent)
    assert len(entry) == ENTRY_SIZE
    return bytes(entry)


def pack(replay, name):
    r = replay
    out = bytearray(b'VSR2')
    out += struct.pack('>BBBB', ucf_version(r), r.version[0], r.version[1], r.version[2])
    out += r.game_info
    assert len(out) == 0x140
    frames = list(range(r.first, r.last + 1))
    out += struct.pack('>IiI', r.seed, r.first, len(frames))
    out += name.encode('utf-8')[:NAME_SIZE - 1].ljust(NAME_SIZE, b'\0')
    out += struct.pack('>I', header_flags(r))
    assert len(out) == HEADER_SIZE

    # Group the recorded fighters by frame.
    by_frame = {}
    for (frame, port, follower), pre in r.pre.items():
        by_frame.setdefault(frame, []).append((port, follower, pre))

    for frame in frames:
        entries = sorted(by_frame.get(frame, []), key=lambda e: (e[0], e[1]))
        out += struct.pack('>IB3x', r.frame_seed.get(frame, 0), len(entries))
        for port, follower, pre in entries:
            post = r.post.get((frame, port, follower))
            out += pack_entry(port, follower, pre, post)
    return bytes(out)


def pack_file(source, destination):
    replay = parse(source)
    name = Path(source).name
    data = pack(replay, name)
    Path(destination).write_bytes(data)
    stage = struct.unpack('>H', replay.game_info[0xE:0x10])[0]
    print(f'{destination}: {name}, Slippi {".".join(map(str, replay.version))}, '
          f'UCF {ucf_version(replay)}, flags {header_flags(replay)}, frames {replay.first}..{replay.last}, stage {stage}, '
          f'{len(data)} bytes')


def main():
    if len(sys.argv) >= 4 and sys.argv[1] == '--batch':
        folder = Path(sys.argv[2])
        folder.mkdir(parents=True, exist_ok=True)
        for number, source in enumerate(sys.argv[3:], start=1):
            pack_file(source, folder / f'replay{number}.bin')
        return 0
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    pack_file(sys.argv[1], sys.argv[2])
    return 0


if __name__ == '__main__':
    sys.exit(main())
