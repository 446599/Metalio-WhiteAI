#!/usr/bin/env python3
"""Prepare a local photo as a strict 480x800 black/white PBM; never uploads it."""
from pathlib import Path
import argparse
from PIL import Image, ImageOps

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image',type=Path)
    parser.add_argument('--out',type=Path,default=Path('lock.pbm'))
    parser.add_argument('--threshold',type=int,default=160)
    args=parser.parse_args()
    if not 0<=args.threshold<=255: parser.error('threshold must be 0..255')
    if args.image.resolve()==args.out.resolve(): parser.error('output must not overwrite the source')
    with Image.open(args.image) as source:
        source=ImageOps.exif_transpose(source).convert('RGBA')
        canvas=Image.new('RGBA',source.size,'white');canvas.alpha_composite(source)
        fitted=ImageOps.fit(canvas.convert('L'),(480,800),method=Image.Resampling.LANCZOS)
        mono=fitted.point(lambda p:255 if p>=args.threshold else 0,mode='1')
    # Pillow mode 1 has 1=white, P4 requires 1=black.
    payload=bytes(b ^ 255 for b in mono.tobytes())
    args.out.parent.mkdir(parents=True,exist_ok=True)
    args.out.write_bytes(b'P4\n480 800\n'+payload)
    print(f'Wrote {len(payload)} monochrome bytes. Copy to SD /wallpaper/lock.pbm; footer area y>=664 is reserved.')
if __name__=='__main__': main()
