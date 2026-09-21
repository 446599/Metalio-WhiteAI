#!/usr/bin/env python3
"""Generate compact raw-framebuffer 2bpp fonts for the AI dashboard.

The layout and packing intentionally match EegoRead's a4_ui_assets format:
MSB-first 2-bit coverage, four pixels per byte, and sorted codepoint/glyph
parallel arrays.  No LVGL headers or runtime are involved.
"""
from pathlib import Path
import re
import sys
from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
source = Path(sys.argv[1]).expanduser() if len(sys.argv) > 1 else Path('/Users/henry/Documents/eego阅读器/components/a4_ui/a4_ui_assets.c')
ttf = Path(sys.argv[2]).expanduser() if len(sys.argv) > 2 else Path('/Users/henry/Documents/eego阅读器/components/a4_ui/assets/fonts/HarmonyOS_Sans_SC.ttf')
out_h = HERE / 'ai_ui_assets.h'
out_c = HERE / 'ai_ui_assets.c'
old = source.read_text(encoding='utf-8')
m = re.search(r'static const uint16_t ui_font_body_codepoints\[\] = \{(.*?)\};', old, re.S)
base = {int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+|\d+', m.group(1))}
# Fixed UI vocabulary plus common punctuation used by provider data.  Keep the
# source charset deterministic so regenerated assets have stable diffs.
terms = (
    '聊些什么？此处选择或'
    'AI 今日摘要小智日程天气额度自定义提醒未连接离线同步重要事项当前天气每日日程'
    '体感湿度风速连接阅读卡片状态使用剩余本周小时网络数据更新时间待办概览助手'
    '计划完成优先级事件温度晴朗多云阴雨雪北京上海深圳杭州广州城市地区刷新中'
    '未设置配置成功失败在线重连准备听我说语音问候欢迎回来专注工作会议出发'
    '早上下午晚上明天今天星期一二三四五六日月年秒分度摄氏工作空间任务窗口'
    '五小时周限额已用可用自定义卡片点击查看小智主体云端摘要稍后更新暂无数据'
    '请先联网保存快照离线快照同步时间提醒事项'
    '与交你具再化变对思持绑给考聆节话顾'
    # Product UI strings.  Keep these in the generated charset so every
    # literal drawn by raw_display.cc has a real bitmap glyph instead of
    # silently disappearing when FindGlyph() misses its codepoint.
    '随手记留屏应用问今天先做重要的事今日重点暂无重点点击开始询问小智下一件'
    '暂无安排继续阅读打开书库暂无阅读记录应用目录阅读助手卡片盒设置更多'
    '从SD卡继续阅读提问总结与整理录音并保存原文浏览已保存内容布局网络与省电'
    'AI结果详情摘要等待小智返回结论下一步查看执行建议存卡片返回结果首页'
    '流式结果按完整段落更新屏幕开始录音整理确认任务待开始录音中原文已保存'
    '整理完成草稿待确认按左下按钮再次点击结束保存原文成功检查标题和内容确认后'
    '才会创建任务想法会先保存为原文标题完成后再交给小智安排下一步第一章从一件小事开始'
    '今天只做一件重要的事把目标写下来再把它拆成可以马上完成的小步完成一小步以后停下来'
    '看看下一步是否仍然清楚第二章保持连续稳定的节奏比偶尔的冲刺更容易留下真正的进展'
    '把注意力放回当前一页让工具安静地服务于阅读第三章记录结果在一天结束以前记下结果'
    '明天就不必从头寻找方向一张卡片足够承接一个想法今日清单今天还没有安排已完成待处理'
    '卡片详情整理来源内容创建时间刚刚快照时间进入留屏时设备将在唤醒后返回原页面'
    '工作台同步天气与额度刷新AI摘要显示测试页查看设备状态刷新数据关于本项目'
    '布局方向网络连接省电策略关于设备竖屏默认自动RAW灰度字体任务尚未创建'
    '确认草稿后才会加入今日清单保存原文和整理结果不会自动创建任务设备测试切换方向'
    '与小智连接断开开始聆听已发送结束聆听天气数据待更新正在刷新天气与额度'
)
charset = base | {ord(ch) for ch in terms} | set(range(32, 127)) | {ord(ch) for ch in '°·—…％：，。'}
charset = sorted(charset)

fonts = [
    ('ui_font_small', 'Medium', 20),
    ('ui_font_status', 'SemiBold', 24),
    ('ui_font_body', 'Medium', 28),
    ('ui_font_title', 'SemiBold', 36),
    ('ui_font_h1', 'SemiBold', 42),
]
clock_chars = sorted({ord(ch) for ch in '0123456789:'})
fonts.append(('ui_font_clock', 'SemiBold', 72))

def fmt_bytes(values, indent='    '):
    lines=[]
    for i in range(0,len(values),16):
        lines.append(indent + ', '.join(f'0x{x:02x}' for x in values[i:i+16]) + ',')
    return '\n'.join(lines)

def render(name, variation, px, chars):
    f = ImageFont.truetype(str(ttf), px)
    f.set_variation_by_name(variation)
    ascent, descent = f.getmetrics()
    height = ascent + descent
    bitmap = bytearray(); glyphs=[]
    for cp in chars:
        ch=chr(cp)
        width=max(1,int((f.getlength(ch)+0.999999)))
        img=Image.new('L',(width+8,height),0)
        d=ImageDraw.Draw(img); d.fontmode='L'; d.text((0,0),ch,font=f,fill=255)
        img=img.crop((0,0,width,height)); pix=img.load(); stride=(width+3)//4; off=len(bitmap)
        for y in range(height):
            row=bytearray(stride)
            for x in range(width):
                level=(pix[x,y]*3+127)//255
                row[x>>2] |= level << (6-2*(x&3))
            bitmap.extend(row)
        glyphs.append((off,width))
    return height,ascent,bitmap,glyphs,chars

h=[]
h.append('/* Generated from EegoRead a4_ui bitmap-font format; do not edit by hand. */')
h.append('#pragma once\n#include <stdint.h>\n\ntypedef struct { uint32_t offset; uint8_t width; } ui_glyph_t;')
h.append('typedef struct { const uint8_t* bitmap; const ui_glyph_t* glyphs; const uint16_t* codepoints; uint16_t glyph_count; uint8_t height; uint8_t baseline; uint8_t bits_per_pixel; } ui_font_t;\n')
for name,_,_ in fonts: h.append(f'extern const ui_font_t {name};')
out_h.write_text('\n'.join(h)+'\n',encoding='utf-8')

c=['/* Generated from EegoRead a4_ui bitmap-font format; do not edit by hand. */','#include "ai_ui_assets.h"\n']
for name,var,px in fonts:
    chars=clock_chars if name=='ui_font_clock' else charset
    height,baseline,bm,glyphs,cps=render(name,var,px,chars)
    c.append(f'static const uint8_t {name}_bitmap[] = {{\n{fmt_bytes(bm)}\n}};\n')
    c.append(f'static const ui_glyph_t {name}_glyphs[] = {{')
    for off,w in glyphs: c.append(f'    {{{off}u, {w}u}},')
    c.append('};\n')
    c.append(f'static const uint16_t {name}_codepoints[] = {{')
    for i in range(0,len(cps),16): c.append('    '+', '.join(f'0x{cp:04x}' for cp in cps[i:i+16])+',')
    c.append('};\n')
    c.append(f'const ui_font_t {name} = {{ {name}_bitmap, {name}_glyphs, {name}_codepoints, {len(cps)}u, {height}u, {baseline}u, 2u }};\n')
out_c.write_text('\n'.join(c),encoding='utf-8')
print('charset',len(charset),'bytes',out_c.stat().st_size)
for name,_,_ in fonts:
    # rough source output only
    print(name)
