import base64, hashlib, lzma, pathlib, subprocess, sys, zipfile
root=pathlib.Path('.')
data=lzma.decompress(base64.b64decode(''.join((root/f'.ci/sleep-patch.{i}').read_text() for i in range(10)),validate=True))
assert hashlib.sha256(data).hexdigest()=='9842de7280be6125884e9771701178d97bfbbe300b520de2dbf6905ea68b4e74'
p=root/'sleep.patch';p.write_bytes(data)
subprocess.run(['git','apply','--check',str(p)],check=True)
subprocess.run(['git','apply',str(p)],check=True)
if (root/'.ci/sleep-fixes.py').exists():
 subprocess.run([sys.executable,'.ci/sleep-fixes.py'],check=True)
subprocess.run(['git','add','-N','main','components','tools'],check=True)
subprocess.run(['git','diff','--check'],check=True)
paths=subprocess.check_output(['git','diff','--name-only','--','main','components','tools'],text=True).splitlines()
assert paths
with zipfile.ZipFile('sleep-overlay.zip','w',zipfile.ZIP_DEFLATED) as z:
 for name in paths:
  q=pathlib.PurePosixPath(name)
  assert q.parts[0] in ('main','components','tools') and '..' not in q.parts
  assert not q.is_absolute() and (root/q).is_file()
  z.write(root/q,name)
print('Verified source overlay:',len(paths),'files')
