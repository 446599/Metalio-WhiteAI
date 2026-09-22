from pathlib import Path
import base64, hashlib, lzma, subprocess, zipfile
payload=''.join(Path(f'.ci/mono-patch.{i}').read_text() for i in range(4))
patch=lzma.decompress(base64.b64decode(payload,validate=True))
assert hashlib.sha256(patch).hexdigest()=='9e2e4488cf17ba2b8c33ba7b03543ace10b50b60a03401b2b89df5611a182f1b'
subprocess.run(['git','apply','--check','-'],input=patch,check=True)
subprocess.run(['git','apply','-'],input=patch,check=True)
if Path('.ci/mono-fixes.py').exists():
    subprocess.run(['python3','.ci/mono-fixes.py'],check=True)
# Build an explicit changed-source overlay, not a checkout/archive with assets.
subprocess.run(['git','add','--intent-to-add','main','tools','docs'],check=True)
paths=subprocess.check_output(['git','diff','--name-only','--','main','tools','docs'],text=True).splitlines()
assert paths and all(not p.endswith(('.pyc','.bin','.fontpack','.ttf','.otf')) for p in paths)
with zipfile.ZipFile('mono-overlay.zip','w',zipfile.ZIP_DEFLATED) as z:
    for name in paths:
        p=Path(name)
        assert p.is_file(),name
        z.write(p,name)
print('Prepared',len(paths),'changed source files')
