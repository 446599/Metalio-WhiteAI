from pathlib import Path
import base64, hashlib, lzma, subprocess, sys, zipfile
raw=base64.b64decode(''.join(Path(f'.ci/chat-patch.{i}').read_text() for i in range(4)),validate=True)
assert hashlib.sha256(raw).hexdigest()=='3b65ce1f735f8017fc7e017cba45b1b6c20294a3e066bb8134b9a89376cfe130'
patch=lzma.decompress(raw)
subprocess.run(['git','apply','--check','-'],input=patch,check=True)
subprocess.run(['git','apply','-'],input=patch,check=True)
if Path('.ci/chat-fixes.py').exists(): subprocess.run([sys.executable,'.ci/chat-fixes.py'],check=True)
subprocess.run([sys.executable,'tools/build_ui_icons.py'],check=True)
subprocess.run(['git','add','main','tools','docs','assets'],check=True)
subprocess.run(['git','diff','--cached','--check'],check=True)
paths=subprocess.check_output(['git','diff','--cached','--name-only','-z']).decode().split('\0')
with zipfile.ZipFile('chat-overlay.zip','w',zipfile.ZIP_DEFLATED) as z:
 for name in paths:
  if not name: continue
  p=Path(name)
  assert p.parts[0] in ('main','tools','docs','assets') and p.is_file(),name
  assert p.suffix.lower() not in ('.ttf','.otf','.fontpack','.bin'),name
  z.write(p,name)
print('Prepared checked source overlay:',len([p for p in paths if p]))
