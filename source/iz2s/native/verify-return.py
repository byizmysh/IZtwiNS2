#!/usr/bin/env python3
"""Verify the optional Switch 2 launcher entry before embedding it."""
import struct
import sys
from pathlib import Path

data = Path(sys.argv[1]).read_bytes()
if not 4096 <= len(data) <= 128 * 1024 * 1024 or data[16:20] != b"NRO0":
    sys.exit("Expected the IZ2S Launcher NRO for Switch 2.")
mod, = struct.unpack_from("<I", data, 4)
image, = struct.unpack_from("<I", data, 24)
marker = b"OUN0\x02\0\0\0\x01\0\0\0"
if not 128 <= mod <= 4096 - 28 or data[mod:mod + 4] != b"MOD0" or not any(
    data[at:at + len(marker)] == marker
    for at in range(mod + 28, min(4096, mod + 128) - len(marker) + 1, 4)
):
    sys.exit("The return entry must be an Ounce NRO for Switch 2.")
if not 4096 <= image <= len(data) - 56 or data[image:image + 4] != b"ASET":
    sys.exit("The return entry does not have launcher metadata.")
offset, size = struct.unpack_from("<QQ", data, image + 24)
if offset < 56 or size < 0x4000 or image + offset + size > len(data):
    sys.exit("The return entry has invalid launcher metadata.")
if data[image + offset:image + offset + 512].split(b"\0", 1)[0] != b"IZ2S Launcher":
    sys.exit("Use the IZ2S Launcher homebrew entry from the matching IZ2S release.")
