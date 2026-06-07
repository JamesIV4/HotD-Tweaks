#!/usr/bin/env python3
"""Build the compact reversible crosshair patch resource.

The known-good no-crosshair assets only zero bytes in their decompressed zlib
payloads. This tool stores the original bytes and their offsets, plus hashes of
both states, instead of embedding complete level archives.
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import zlib
from pathlib import Path


PATCHES = (
    ("campaign", "l1_plantation_house_hud_permasector.pc"),
    ("labs", "l7_labs_world.pc"),
    ("civilians", "w_civilians.pc"),
    ("database", "w_database_world.pc"),
    ("shooting", "w_shootinggallery.pc"),
    ("survival", "w_survivalmode_hud_permasector.pc"),
)

STREAM_OFFSET = 0x190
MAGIC = b"HOTDCHP1"


def inflate_asset(path: Path) -> bytes:
    data = path.read_bytes()
    if len(data) < STREAM_OFFSET:
        raise ValueError(f"{path} is too small to be a HOTD asset")
    return zlib.decompress(data[STREAM_OFFSET:])


def changed_runs(original: bytes, patched: bytes) -> list[tuple[int, bytes]]:
    if len(original) != len(patched):
        raise ValueError("decompressed payload sizes differ")

    runs: list[tuple[int, bytes]] = []
    offset = 0
    while offset < len(original):
        if original[offset] == patched[offset]:
            offset += 1
            continue

        start = offset
        while offset < len(original) and original[offset] != patched[offset]:
            if patched[offset] != 0:
                raise ValueError(
                    f"patch changes byte 0x{offset:x} to a non-zero value"
                )
            offset += 1
        runs.append((start, original[start:offset]))
    return runs


def build_resource(original_dir: Path, patched_dir: Path) -> bytes:
    entries: list[bytes] = []
    for name, file_name in PATCHES:
        original = inflate_asset(original_dir / file_name)
        patched = inflate_asset(patched_dir / file_name)
        runs = changed_runs(original, patched)

        encoded_name = name.encode("ascii")
        entry = bytearray()
        entry += struct.pack("<I", len(encoded_name))
        entry += encoded_name
        entry += struct.pack("<I", len(original))
        entry += hashlib.sha256(original).digest()
        entry += hashlib.sha256(patched).digest()
        entry += struct.pack("<I", len(runs))
        for offset, original_bytes in runs:
            entry += struct.pack("<II", offset, len(original_bytes))
            entry += original_bytes
        entries.append(bytes(entry))

        changed = sum(len(data) for _, data in runs)
        print(
            f"{name:10} raw={len(original):9} runs={len(runs):5} "
            f"stored={changed:6}"
        )

    output = bytearray(MAGIC)
    output += struct.pack("<II", 1, len(entries))
    for entry in entries:
        output += entry
    return bytes(output)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--original-dir", type=Path, required=True)
    parser.add_argument("--patched-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    resource = build_resource(args.original_dir, args.patched_dir)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(resource)
    print(f"Wrote {len(resource)} bytes to {args.output}")


if __name__ == "__main__":
    main()
