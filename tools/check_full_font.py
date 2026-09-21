#!/usr/bin/env python3
"""Validate complete fontpack bounds/coverage/hash without font source files."""
import hashlib
import json
from pathlib import Path
import struct
ROOT = Path(__file__).resolve().parents[1]
pack = ROOT / 'assets/fonts/harmony_full.fontpack'
blob = pack.read_bytes()
report = json.loads(pack.with_suffix('.json').read_text())
assert len(blob) <= 0x500000 and len(blob) == report['bytes']
assert hashlib.sha256(blob).hexdigest() == report['sha256']
magic, version, flags, count, index, data, reserved = struct.unpack_from('<4sHHIQQI', blob)
assert magic == b'EFNT' and version == 1 and count == report['glyphs']
assert index == 32 and data == index + count * 16
previous = 0
codepoints = set()
for i in range(count):
    key, offset, size, _ = struct.unpack_from('<QIHH', blob, index + i * 16)
    assert key > previous
    previous = key
    cp, px, bpp = key >> 32, (key >> 16) & 65535, key & 65535
    assert px == 26 and bpp == 1
    assert data <= offset < len(blob) and offset + size <= len(blob)
    w, h, x, y, advance = struct.unpack_from('<HHhhH', blob, offset)
    assert ((w + 7)//8)*h + 10 == size
    assert 0 <= x <= 52 and 0 <= y <= 32 and y+h <= 32 and advance > 0
    codepoints.add(cp)
assert all(cp in codepoints for cp in range(0x4e00,0xa000))
assert all(cp in codepoints for cp in range(0x3400,0x4db6))
assert all(ord(c) in codepoints for c in '闪念胶囊繁體麒麟龘𠮷，。！？Hello')
print(f'Full font OK: {count} characters, {len(blob)} bytes, complete CJK basic + 6582 Extension A characters, SHA-256 verified')
