#!/usr/bin/env python3
"""Self-check for image-container detection.  python tools/test_extract_resources.py"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_resources import load_hfs  # noqa: E402

SECTOR = 512


def cd_image(vol: bytes, start_blk: int = 30, n_parts: int = 2) -> bytes:
    """A Mac CD: 'ER' descriptor, a partition map, then the HFS volume."""
    img = bytearray(b"ER" + bytes(SECTOR - 2))
    for i in range(n_parts):
        name = b"Apple_HFS" if i == n_parts - 1 else b"Apple_Driver"
        e = bytearray(SECTOR)
        e[0:2] = b"PM"
        e[4:8] = struct.pack(">I", n_parts)
        e[8:16] = struct.pack(">II", start_blk if name == b"Apple_HFS" else 1,
                              len(vol) // SECTOR if name == b"Apple_HFS" else 1)
        e[48:48 + len(name)] = name
        img += e
    img += bytes(start_blk * SECTOR - len(img))
    return bytes(img) + vol


def test_partitioned_cd():
    # The volume is past the map, so returning offset 0 would hand back the map.
    vol = b"HFSVOLUME" + bytes(SECTOR * 4 - 9)
    assert load_hfs(cd_image(vol)) == vol
    # The Apple_HFS entry is not always first in the map.
    assert load_hfs(cd_image(vol, n_parts=4)) == vol


def test_dc42():
    raw = bytearray(84 + 819200)
    raw[64:68] = struct.pack(">I", 819200)
    raw[84:88] = b"DATA"
    assert load_hfs(bytes(raw))[:4] == b"DATA"


def test_raw_hfs_passthrough():
    # No 'ER' and no plausible DiskCopy size: hand the input straight back.
    raw = bytes(2048)
    assert load_hfs(raw) is raw


if __name__ == "__main__":
    test_partitioned_cd()
    test_dc42()
    test_raw_hfs_passthrough()
    print("extract_resources self-check OK")
