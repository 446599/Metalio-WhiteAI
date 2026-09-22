from pathlib import Path
import hashlib, json, re, subprocess, zipfile
patch=b''.join(Path(f'.ci/audio.patch.{i}').read_bytes() for i in range(4))
old=b"     subprocess.run([str(p/'test')],check=True)\ndiff --git a/tools/preview_mono_support.py"
new=b"     subprocess.run([str(p/'test'),str(p/'books')],check=True)\ndiff --git a/tools/preview_mono_support.py"
assert patch.count(old)==1
patch=patch.replace(old,new)
assert hashlib.sha256(patch).hexdigest()=='0939703d126cc4f3cf34e4cb22e02b24d3835a8949c1988137e700396f83283a'
blocks=re.split(rb'(?=^diff --git )',patch,flags=re.M)
patch=b''.join(Path('.ci/audio-kconfig.patch').read_bytes() if b.startswith(b'diff --git a/main/Kconfig.projbuild ') else b for b in blocks)
assert hashlib.sha256(patch).hexdigest()=='65d3417547d2e26fb699b0c5e191ac5720a1960d7f4e9dd8f964419737c9dfaa'
Path('.ci/combined.patch').write_bytes(patch)
entries=[]
for block in re.split(rb'(?=^diff --git )',patch,flags=re.M):
    if not block: continue
    path=re.search(rb'^diff --git a/(\S+) b/(\S+)\n',block).group(2).decode()
    assert not path.startswith('/') and '..' not in Path(path).parts
    old,new=re.search(rb'^index ([0-9a-f]{40})\.\.([0-9a-f]{40})',block,re.M).groups()
    def blob(data): return hashlib.sha1(f'blob {len(data)}\0'.encode()+data).hexdigest()
    p=Path(path)
    if old!=b'0'*40: assert p.is_file() and blob(p.read_bytes())==old.decode(),('baseline mismatch',path)
    else: assert not p.exists(),('unexpected existing path',path)
    entries.append((path,new.decode()))
assert len(entries)==23
subprocess.run(['git','apply','--check','.ci/combined.patch'],check=True)
subprocess.run(['git','apply','.ci/combined.patch'],check=True)
for path,new in entries:
    assert blob(Path(path).read_bytes())==new,('output mismatch',path,blob(Path(path).read_bytes()),new)
with zipfile.ZipFile('audio-overlay.zip','w',zipfile.ZIP_DEFLATED) as z:
    for path,_ in entries:z.write(path,path)
Path('audio-paths.txt').write_text('\n'.join(path for path,_ in entries)+'\n')
Path('audio-hashes.json').write_text(json.dumps(dict(entries),indent=2)+'\n')
print('Source patch verified:',len(entries),'files')
