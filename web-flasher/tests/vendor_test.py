#!/usr/bin/env python3
"""Offline tests for checksum-first SDK installation; no network downloads."""
import hashlib
import importlib.util
import io
from pathlib import Path
import tarfile
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('vendor_sdk', Path(__file__).resolve().parents[1]/'vendor_sdk.py')
sdk = importlib.util.module_from_spec(spec); spec.loader.exec_module(sdk)

def archive(files):
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode='w:gz') as out:
        for name, value in files.items():
            member = tarfile.TarInfo(name); member.size = len(value)
            out.addfile(member, io.BytesIO(value))
    return data.getvalue()

class VendorTests(unittest.TestCase):
    def test_good_archive(self):
        raw = archive({'package/bundle.js': b'export class Transport {}', 'package/LICENSE': b'Apache-2.0'})
        with tempfile.TemporaryDirectory() as tmp:
            sdk.install_archive(raw, Path(tmp), hashlib.sha256(raw).hexdigest())
            self.assertEqual((Path(tmp)/'esptool-js-0.7.0.bundle.js').read_text(), 'export class Transport {}')
            self.assertTrue((Path(tmp)/'esptool-js-LICENSE.txt').exists())
    def test_bad_checksum_writes_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(ValueError): sdk.install_archive(b'bad', Path(tmp))
            self.assertEqual(list(Path(tmp).iterdir()), [])
    def test_missing_license_writes_nothing(self):
        raw = archive({'package/bundle.js': b'export {}'})
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(KeyError): sdk.install_archive(raw, Path(tmp), hashlib.sha256(raw).hexdigest())
            self.assertEqual(list(Path(tmp).iterdir()), [])
    def test_unrelated_archive_paths_are_not_extracted(self):
        raw = archive({'package/bundle.js': b'export {}', 'package/LICENSE': b'license', '../evil': b'bad'})
        with tempfile.TemporaryDirectory() as tmp:
            sdk.install_archive(raw, Path(tmp), hashlib.sha256(raw).hexdigest())
            self.assertEqual(sorted(p.name for p in Path(tmp).iterdir()), ['esptool-js-0.7.0.bundle.js', 'esptool-js-LICENSE.txt'])
    def test_invalid_utf8_writes_nothing(self):
        raw = archive({'package/bundle.js': b'\xff', 'package/LICENSE': b'license'})
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(UnicodeDecodeError): sdk.install_archive(raw, Path(tmp), hashlib.sha256(raw).hexdigest())
            self.assertEqual(list(Path(tmp).iterdir()), [])

if __name__ == '__main__': unittest.main()
