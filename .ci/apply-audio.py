from pathlib import Path
import hashlib, json, re, subprocess, zipfile
patch=b''.join(Path(f'.ci/audio.patch.{i}').read_bytes() for i in range(4))
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
