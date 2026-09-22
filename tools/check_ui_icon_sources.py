#!/usr/bin/env python3
"""Verify pinned upstream geometry, license and the generated native bitmaps."""
import hashlib,json,pathlib,re
root=pathlib.Path(__file__).resolve().parents[1];icons=root/'assets/icons/lucide'
m=json.loads((icons/'source.json').read_text())
for name,digest in m['sha256'].items():
    assert hashlib.sha256((icons/name).read_bytes()).hexdigest()==digest,name
header=(root/'main/display/icons/lucide_icons.h').read_text()
arrays=re.findall(r'inline constexpr uint8_t (\w+)(24|28|32|40)\[\] = \{([^}]+)\}',header)
assert len(arrays)==len(list(icons.glob('*.svg')))*4
for name,size,data in arrays:
    values=[int(x) for x in data.split(',')];assert len(values)==int(size)**2//8
    assert all(0<=x<=255 for x in values) and any(values),name
source=(root/'main/display/input_view.cc').read_text()
assert 'lucide::Id::Delete' in source and 'lucide::Id::ArrowBigUp' in source
print(f'Icons PASS: {len(arrays)//4} pinned Lucide SVGs, license and {len(arrays)} binary native-size bitmaps')
