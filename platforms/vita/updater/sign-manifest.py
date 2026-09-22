#!/usr/bin/env python3
"""Create the canonical signed manifest consumed by the Vita updater."""

from __future__ import annotations

import argparse
import hashlib
import re
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

SEMVER = re.compile(
    r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"
    r"(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?"
    r"(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$"
)


def load_private_key(path: Path) -> Ed25519PrivateKey:
    key = serialization.load_pem_private_key(path.read_bytes(), password=None)
    if not isinstance(key, Ed25519PrivateKey):
        raise ValueError("private key is not Ed25519")
    return key


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--asset", required=True, type=Path)
    parser.add_argument("--private-key", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    if not SEMVER.fullmatch(args.version):
        raise ValueError("version must be strict SemVer without a leading v")
    if not args.asset.name or args.asset.name in {".", ".."} or any(
        value in args.asset.name for value in ("/", "\\")
    ):
        raise ValueError("asset must have a safe filename")

    data = args.asset.read_bytes()
    payload = (
        f"format=1\n"
        f"version={args.version}\n"
        f"asset={args.asset.name}\n"
        f"size={len(data)}\n"
        f"sha256={hashlib.sha256(data).hexdigest()}\n"
    ).encode("ascii")
    signature = load_private_key(args.private_key).sign(payload)
    args.output.write_bytes(
        payload + b"signature=" + signature.hex().encode("ascii") + b"\n"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
