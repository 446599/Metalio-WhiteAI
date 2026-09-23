from pathlib import Path
import hashlib, subprocess
patch = Path('.ci/final-review.patch').read_bytes()
expected = 'e760d1f7f1da54ddfa26d5264468130cd2fbf1ea50d8e7ee86f1fa1fb8c1e55f'
if hashlib.sha256(patch).hexdigest() != expected:
    raise SystemExit('Source patch checksum mismatch')
subprocess.run(['git','apply','--check','-'], input=patch, check=True)
subprocess.run(['git','apply','-'], input=patch, check=True)
print('Verified incremental source patch:', expected)
