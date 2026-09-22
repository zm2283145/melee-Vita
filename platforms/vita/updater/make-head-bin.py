#!/usr/bin/env python3
"""Create a Vita fake-package head.bin for a specific title ID."""

# The package-header transform and template are derived from VitaShell,
# Copyright (C) 2015-2018 TheFloW, under GPL-3.0-or-later.

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path

VITASHELL_TEMPLATE_SHA256 = (
    "cbb88299319048e19115a1fc9c76b04e33745291749de0e08963b9b425623f45"
)


def fpkg_hmac(data: bytes) -> bytes:
    first = hashlib.sha1(data).digest()
    block = bytearray(64)
    block[0:8] = first[4:12]
    block[8:16] = first[4:12]
    block[16:20] = first[12:16]
    block[20:24] = bytes((first[16], first[1], first[2], first[3]))
    block[24:32] = block[16:24]
    return hashlib.sha1(block).digest()[:16]


def read_be32(data: bytearray, offset: int) -> int:
    if offset + 4 > len(data):
        raise ValueError("head.bin offset is out of range")
    return struct.unpack_from(">I", data, offset)[0]


def write_hmac(data: bytearray, source_offset: int, source_length: int, output: int) -> None:
    if (
        source_offset < 0
        or source_length < 0
        or source_offset + source_length > len(data)
        or output + 16 > len(data)
    ):
        raise ValueError("head.bin HMAC range is out of bounds")
    data[output : output + 16] = fpkg_hmac(
        bytes(data[source_offset : source_offset + source_length])
    )


def build_head(template: bytes, title_id: str) -> bytes:
    if len(title_id) != 9 or not title_id.isascii() or not title_id.isalnum():
        raise ValueError("title ID must contain nine ASCII alphanumeric characters")
    if title_id.upper() != title_id:
        raise ValueError("title ID must be uppercase")

    data = bytearray(template)
    if (
        len(data) != 1072
        or hashlib.sha256(template).hexdigest() != VITASHELL_TEMPLATE_SHA256
    ):
        raise ValueError("unexpected VitaShell head.bin template")

    content_id = f"EP9000-{title_id}_00-0000000000000000".encode("ascii")
    data[0x30 : 0x30 + 48] = content_id.ljust(48, b"\0")

    header_length = read_be32(data, 0xD0)
    write_hmac(data, 0, header_length, header_length)

    info_offset = read_be32(data, 0x08)
    info_length = read_be32(data, 0x10)
    info_output = read_be32(data, 0xD4)
    if info_length < 64:
        raise ValueError("head.bin package-info range is invalid")
    write_hmac(data, info_offset, info_length - 64, info_output)

    full_length = read_be32(data, 0xE8)
    write_hmac(data, 0, full_length, full_length)
    return bytes(data)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--template", required=True, type=Path)
    parser.add_argument("--title-id", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    output = build_head(args.template.read_bytes(), args.title_id)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
