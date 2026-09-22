#!/usr/bin/env python3
"""Exercise the C manifest boundary with a real Ed25519 implementation."""

from __future__ import annotations

import ctypes
import sys

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey,
    Ed25519PublicKey,
)


VERIFY_CALLBACK = ctypes.CFUNCTYPE(
    ctypes.c_int,
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
)


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: updater_crypto_test.py UPDATE_CORE_LIBRARY")

    library = ctypes.CDLL(sys.argv[1])
    verify_manifest = library.melee_update_verify_manifest_c
    verify_manifest.argtypes = [
        ctypes.c_char_p,
        ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_uint8),
        VERIFY_CALLBACK,
        ctypes.c_char_p,
        ctypes.c_size_t,
    ]
    verify_manifest.restype = ctypes.c_int

    private_key = Ed25519PrivateKey.generate()
    public_key = private_key.public_key()
    public_bytes = public_key.public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw
    )
    payload = (
        b"format=1\n"
        b"version=0.6.1\n"
        b"asset=SmashMeleevita.vpk\n"
        b"size=1048576\n"
        b"sha256=ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n"
    )
    signature = private_key.sign(payload)
    manifest = payload + b"signature=" + signature.hex().encode("ascii") + b"\n"
    key_buffer = (ctypes.c_uint8 * 32).from_buffer_copy(public_bytes)

    @VERIFY_CALLBACK
    def verifier(key_ptr, signature_ptr, message_ptr, message_size):
        key = ctypes.string_at(key_ptr, 32)
        candidate_signature = ctypes.string_at(signature_ptr, 64)
        message = ctypes.string_at(message_ptr, message_size)
        try:
            Ed25519PublicKey.from_public_bytes(key).verify(candidate_signature, message)
        except Exception:
            return 0
        return 1

    error = ctypes.create_string_buffer(256)
    assert (
        verify_manifest(
            manifest, len(manifest), key_buffer, verifier, error, len(error)
        )
        == 1
    ), error.value.decode()

    tampered = manifest.replace(b"size=1048576", b"size=1048577")
    assert (
        verify_manifest(
            tampered, len(tampered), key_buffer, verifier, error, len(error)
        )
        == 0
    )
    assert b"signature verification failed" in error.value

    malformed = manifest.replace(b"asset=", b"path=", 1)
    assert (
        verify_manifest(
            malformed, len(malformed), key_buffer, verifier, error, len(error)
        )
        == 0
    )
    assert b"fields are missing" in error.value

    print("PASS: Ed25519 manifest verification boundary")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
