import base64, hashlib, lzma, pathlib, subprocess, sys, zipfile
root=pathlib.Path('.')
data=lzma.decompress(base64.b64decode(''.join((root/f'.ci/sleep-patch.{i}').read_text() for i in range(10)),validate=True))
assert hashlib.sha256(data).hexdigest()=='9842de7280be6125884e9771701178d97bfbbe300b520de2dbf6905ea68b4e74'
p=root/'sleep.patch';p.write_bytes(data)
subprocess.run(['git','apply','--check',str(p)],check=True)
subprocess.run(['git','apply',str(p)],check=True)
if (root/'.ci/sleep-fixes.py').exists():
 subprocess.run([sys.executable,'.ci/sleep-fixes.py'],check=True)
def replace(path,old,new):
 p=root/path;s=p.read_text();assert s.count(old)==1,(path,s.count(old));p.write_text(s.replace(old,new,1))
replace('main/display/raw_display.cc','void RawDisplay::DrawProductScreenLocked() {','void RawDisplay::DrawProductScreenLocked() {\n    if(lock_screen_.load())return; // no direct redraw may expose content behind the lock page')
replace('tools/preview_mono_support.py','    save("lock-wallpaper-default");','    const auto lock_pixels=display.pixels;\n    save("lock-wallpaper-default");assert(display.pixels==lock_pixels);\n    display.UpdateStatusBar(true);assert(display.pixels==lock_pixels);')
replace('tools/preview_mono_support.py','assert(display.portrait_fb_[6000]==0x7e);save("lock-wallpaper-custom-fixture");','assert(display.portrait_fb_[6000]==0x7e);save("lock-wallpaper-custom-fixture");assert(display.portrait_fb_[6000]==0x7e);')
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
