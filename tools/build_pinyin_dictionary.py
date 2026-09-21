#!/usr/bin/env python3
"""Generate an offline GB2312 Pinyin dictionary from pinned pinyin-data.

Input must be a local checkout of mozillazg/pinyin-data at the recorded commit.
No networking, fonts, runtime Python or LVGL dependency is added to the firmware.
"""
import argparse, hashlib, re, unicodedata
from pathlib import Path
REVISION='923b108dc5d45dee061324c011b478fb649f8b73'
PRIORITY='的一是在不了有和人这中大为上个国我以要他时来用们生到作地于出就分对成会可主发年动同工也能下过子说产种面而方后多定行学法所民得经之进着等部度家电力里如水化高自二理起小物现使心从本日因只想实去性好应开它合还都机当关重业由记你我他她它是时事试市室十实师诗识十设置网络密码笔记项目工作学习计划提醒连接保存完成取消搜索磁铁测试你好世界'
def normalize(s):
    s=unicodedata.normalize('NFD',s.lower()).replace('u\u0308','v')
    return ''.join(c for c in s if unicodedata.category(c)!='Mn')
def generate(source,output):
    raw=source.read_bytes();table={}
    for line in raw.decode('utf-8').splitlines():
        m=re.match(r'U\+([0-9A-F]+):\s*([^#]+)',line)
        if not m: continue
        ch=chr(int(m[1],16))
        if not '\u4e00'<=ch<='\u9fff':continue
        try: ch.encode('gb2312')
        except UnicodeEncodeError:continue
        for reading in m[2].strip().split(','):
            spelling=normalize(reading)
            if re.fullmatch('[a-z]{1,6}',spelling):table.setdefault(spelling,set()).add(ch)
    if len(table)<390 or len(set().union(*table.values()))<6700:raise ValueError('Incomplete source dictionary')
    order={c:i for i,c in reversed(list(enumerate(PRIORITY)))}
    lines=[f'// Generated from mozillazg/pinyin-data {REVISION}; MIT (see PINYIN_LICENSE).',
           f'// Source SHA256: {hashlib.sha256(raw).hexdigest()}', 'constexpr Syllable kPinyin[] = {']
    for spelling,chars in sorted(table.items()):
        chars=''.join(sorted(chars,key=lambda c:(order.get(c,len(PRIORITY)),c.encode('gb2312'))))
        lines.append(f'    {{"{spelling}","{chars}"}},')
    lines+=['};','']
    output.parent.mkdir(parents=True,exist_ok=True);output.write_text('\n'.join(lines),encoding='utf-8')
    print(f'Pinyin: {len(table)} syllables, {len(set().union(*table.values()))} unique GB2312 characters')
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('source',type=Path);p.add_argument('--out',type=Path,default=Path('main/input/pinyin_dictionary.inc'));a=p.parse_args();generate(a.source,a.out)
