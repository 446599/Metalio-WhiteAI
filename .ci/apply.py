from pathlib import Path
import hashlib
import json
import subprocess
import zipfile

root = Path('.')
chunks = sorted((root / '.ci').glob('patch.[0-9][0-9]'))
assert len(chunks) == 19, 'incomplete source transfer'
patch = b''.join(p.read_bytes() for p in chunks)
expected = '9ca7f33ea0549a2142accbd12556037c8e19fd8ec471f54fe7868b0309bf6b8b'
assert hashlib.sha256(patch).hexdigest() == expected, 'source checksum mismatch'
subprocess.run(['git', 'apply', '--check', '-'], input=patch, check=True)
subprocess.run(['git', 'apply', '-'], input=patch, check=True)
fixes = root / '.ci/fixes.py'
if fixes.exists():
    subprocess.run(['python3', str(fixes)], check=True)
subprocess.run(['git', 'diff', '--check'], check=True)
changed = subprocess.check_output(['git', 'diff', '--name-only'], text=True).splitlines()
changed += subprocess.check_output(['git', 'ls-files', '--others', '--exclude-standard'], text=True).splitlines()
paths = sorted({p for p in changed if p.split('/')[0] in ('main', 'components', 'tools', 'docs')})
assert paths and all((root / p).is_file() for p in paths), 'unexpected source deletion'
manifest = {'patch_sha256': expected, 'files': {p: hashlib.sha256((root / p).read_bytes()).hexdigest() for p in paths}}
with zipfile.ZipFile('turn-weather-overlay.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
    for p in paths:
        archive.write(root / p, p)
    archive.writestr('validation-source.json', json.dumps(manifest, indent=2) + '\n')
print('Validated source overlay:', len(paths), 'files')
