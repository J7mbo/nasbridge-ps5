#!/usr/bin/env python3
"""Packs an app folder for the release installer (`make release`).

Writes the same install stream tools/install.py sends to the installer over
the network ("NBI1", every file with its size and SHA-256, "END1"; see
tools/installer/installer.c), zlib-compressed. tools/release/blob.S links it
into the installer, which then installs the app from itself.

    python3 tools/release/pack.py out/app/PPSA62737 build/ps5/release/app.nbz

Files go in sorted order and zlib's output is deterministic, so the same
folder always packs to the same bytes.
"""

import os
import struct
import sys
import zlib

sys.dont_write_bytecode = True  # no __pycache__ in tools/
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from install import CHUNK, collect, sha256_of  # noqa: E402


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: pack.py <app folder> <output>")
    folder = os.path.normpath(sys.argv[1])
    out_path = sys.argv[2]
    name = os.path.basename(folder).encode()
    files = collect(folder)
    packer = zlib.compressobj(9)
    raw = 0
    with open(out_path, "wb") as out:

        def put(data):
            nonlocal raw
            raw += len(data)
            out.write(packer.compress(data))

        put(b"NBI1" + struct.pack("<BH", 0, len(name)) + name + struct.pack("<I", len(files)))
        for rel, full in files:
            rel_b = rel.encode()
            put(struct.pack("<H", len(rel_b)) + rel_b
                + struct.pack("<Q", os.path.getsize(full)) + sha256_of(full))
            with open(full, "rb") as f:
                for block in iter(lambda: f.read(CHUNK), b""):
                    put(block)
        put(b"END1")
        out.write(packer.flush())
    packed = os.path.getsize(out_path)
    print(f"Packed {len(files)} files: {raw / 1e6:.1f} MB -> {packed / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
