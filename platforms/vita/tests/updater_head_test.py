#!/usr/bin/env python3
"""Focused checks for the Vita package-head generator."""

from __future__ import annotations

import hashlib
import importlib.util
import struct
import sys
from pathlib import Path


def load_generator(path: Path):
    spec = importlib.util.spec_from_file_location("make_head_bin", path)
    if spec is None or spec.loader is None:
        raise RuntimeError("could not load make-head-bin.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def verify_hmacs(module, generated: bytes) -> None:
    header_length = struct.unpack_from(">I", generated, 0xD0)[0]
    assert (
        generated[header_length : header_length + 16]
        == module.fpkg_hmac(generated[:header_length])
    )
    info_offset = struct.unpack_from(">I", generated, 0x08)[0]
    info_length = struct.unpack_from(">I", generated, 0x10)[0]
    info_output = struct.unpack_from(">I", generated, 0xD4)[0]
    assert (
        generated[info_output : info_output + 16]
        == module.fpkg_hmac(
            generated[info_offset : info_offset + info_length - 64]
        )
    )
    full_length = struct.unpack_from(">I", generated, 0xE8)[0]
    assert (
        generated[full_length : full_length + 16]
        == module.fpkg_hmac(generated[:full_length])
    )


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: updater_head_test.py GENERATOR TEMPLATE")
    generator_path = Path(sys.argv[1]).resolve()
    template = Path(sys.argv[2]).read_bytes()
    module = load_generator(generator_path)

    main_head = module.build_head(template, "MLVITA002")
    helper_head = module.build_head(template, "MLVUPD001")
    assert len(main_head) == 1072
    assert main_head[0x30 : 0x30 + 48].rstrip(b"\0") == (
        b"EP9000-MLVITA002_00-0000000000000000"
    )
    assert helper_head[0x30 : 0x30 + 48].rstrip(b"\0") == (
        b"EP9000-MLVUPD001_00-0000000000000000"
    )
    assert hashlib.sha256(main_head).digest() != hashlib.sha256(helper_head).digest()
    verify_hmacs(module, main_head)
    verify_hmacs(module, helper_head)
    print("PASS: Vita package head generation")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
