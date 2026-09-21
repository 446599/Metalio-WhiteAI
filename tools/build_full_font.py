#!/usr/bin/env python3
"""Pack every mapped character from a supplied font (not a UI word subset).

Requires Pillow and fontTools. Output uses the existing EFNT v1 format,
row-aligned MSB-first 1bpp bitmaps. The 26px merged full face fits font_data (5 MiB).
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from PIL import Image, ImageDraw, ImageFont
from fontTools.ttLib import TTFont


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('font', type=Path)
    p.add_argument('--fallback', type=Path)
    p.add_argument('--size', type=int, default=26)
    p.add_argument('--out', type=Path, default=Path('assets/fonts/harmony_full.fontpack'))
    args = p.parse_args()
    ttf = TTFont(args.font)
    primary = ttf.getBestCmap()
    fallback_cmap = TTFont(args.fallback).getBestCmap() if args.fallback else {}
    cps = sorted(cp for cp in set(primary) | set(fallback_cmap) if cp >= 32)
    size = args.size
    font = ImageFont.truetype(str(args.font), size)
    font.set_variation_by_name('Medium')
    ascent, descent = font.getmetrics()
    fallback_font = ImageFont.truetype(str(args.fallback), size) if args.fallback else font
    try:
        fallback_font.set_variation_by_name('Medium')
    except OSError:
        pass  # Static OFL fallback face.
    data = bytearray()
    index = bytearray()
    offset = 32 + 16 * len(cps)
    for cp in cps:
        char = chr(cp)
        face = font if cp in primary else fallback_font
        advance = max(1, int(face.getlength(char) + .999999))
        image = Image.new('L', (max(advance + 8, size * 2), ascent + descent), 0)
        ImageDraw.Draw(image).text((0, ascent), char, font=face, anchor='ls', fill=255)
        image = image.point(lambda v: 255 if v >= 128 else 0, '1')
        bounds = image.getbbox()
        if bounds:
            x, y, right, bottom = bounds
            image = image.crop(bounds)
            bitmap = image.tobytes()
            width, height = image.size
        else:
            x = y = width = height = 0
            bitmap = b''
        record = struct.pack('<HHhhH', width, height, x, y, advance) + bitmap
        key = (cp << 32) | (size << 16) | 1
        index.extend(struct.pack('<QIHH', key, offset + len(data), len(record), 0))
        data.extend(record)
    header = struct.pack('<4sHHIQQI', b'EFNT', 1, 0, len(cps), 32, offset, 0)
    blob = header + index + data
    print(f"Packed {len(cps)} glyphs into {len(blob)} bytes", flush=True)
    if len(blob) > 0x500000:
        raise SystemExit(f'Font exceeds 5 MiB partition: {len(blob)}')
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(blob)
    report = dict(font=args.font.name, fallback=args.fallback.name if args.fallback else None,
                  fallback_glyphs=len(set(fallback_cmap)-set(primary)),
                  fallback_sha256=hashlib.sha256(args.fallback.read_bytes()).hexdigest() if args.fallback else None, source_sha256=hashlib.sha256(args.font.read_bytes()).hexdigest(),
                  sha256=hashlib.sha256(blob).hexdigest(), bytes=len(blob), glyphs=len(cps), size=size,
                  line_height=ascent+descent, baseline=ascent, bpp=1,
                  basic_cjk=sum(0x4e00 <= c <= 0x9fff for c in cps),
                  extension_a=sum(0x3400 <= c <= 0x4dbf for c in cps),
                  supplementary=sum(c > 0xffff for c in cps))
    args.out.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
