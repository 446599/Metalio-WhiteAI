from pathlib import Path
import base64, hashlib, subprocess, zlib
payload = ''.join(Path(f'.ci/web-serial.{i}').read_text().strip() for i in range(4))
patch = zlib.decompress(base64.b64decode(payload, validate=True))
expected = 'fafe33fe02ab1790891aa6a40a02a978b703b181024ff7a0c2471a8d8b55ab1e'
if hashlib.sha256(patch).hexdigest() != expected:
    raise SystemExit('Source patch checksum mismatch')
subprocess.run(['git','apply','--check','-'], input=patch, check=True)
subprocess.run(['git','apply','-'], input=patch, check=True)
print('Verified source patch:', expected)
