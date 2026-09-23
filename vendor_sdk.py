#!/usr/bin/env python3
"""Install the pinned official SDK bundle for offline/self-hosted use.
No npm, extraction of arbitrary archive paths, or firmware access.
"""
from __future__ import annotations
import argparse
import hashlib
import io
import os
from pathlib import Path
import tarfile
import urllib.request

VERSION = '0.7.0'
URL = f'https://github.com/espressif/esptool-js/releases/download/v{VERSION}/esptool-js-{VERSION}.tgz'
# Official GitHub release asset digest, verified from release metadata 2026-09-21.
ARCHIVE_SHA256 = '9c7797cfbcac3a989ab85c52c9daaa65ffc0d538c273a055757b98bb846fdf1f'
MAX_ARCHIVE = 2 * 1024 * 1024

def install_archive(raw: bytes, destination: Path, expected_digest: str = ARCHIVE_SHA256) -> None:
    if len(raw) > MAX_ARCHIVE or hashlib.sha256(raw).hexdigest() != expected_digest:
        raise ValueError('SDK archive checksum/size mismatch; nothing was installed')
    wanted = {'package/bundle.js': f'esptool-js-{VERSION}.bundle.js', 'package/LICENSE': 'esptool-js-LICENSE.txt'}
    contents = {}
    with tarfile.open(fileobj=io.BytesIO(raw), mode='r:gz') as archive:
        for source, output in wanted.items():
            member = archive.getmember(source)
            if not member.isfile() or member.size <= 0 or member.size > 4 * 1024 * 1024:
                raise ValueError('Invalid SDK archive member')
            stream = archive.extractfile(member)
            if stream is None:
                raise ValueError('Missing SDK file')
            content = stream.read(member.size + 1)
            if len(content) != member.size:
                raise ValueError('Truncated SDK file')
            content.decode('utf-8')
            contents[output] = content
    destination.mkdir(parents=True, exist_ok=True)
    for name, content in contents.items():
        path = destination / name
        temporary = path.with_name(path.name + '.tmp')
        with temporary.open('wb') as output:
            output.write(content); output.flush(); os.fsync(output.fileno())
        os.replace(temporary, path)

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, help='Use an already downloaded official .tgz')
    args = parser.parse_args()
    if args.archive:
        if args.archive.stat().st_size > MAX_ARCHIVE:
            raise SystemExit('Archive exceeds 2 MiB limit')
        raw = args.archive.read_bytes()
    else:
        with urllib.request.urlopen(URL, timeout=30) as response:
            raw = response.read(MAX_ARCHIVE + 1)
    target = Path(__file__).resolve().parent / 'vendor'
    install_archive(raw, target)
    print(f'Installed esptool-js {VERSION} and its Apache-2.0 license in {target}')
    print('The web page will now use this local copy instead of the CDN.')

if __name__ == '__main__':
    main()
