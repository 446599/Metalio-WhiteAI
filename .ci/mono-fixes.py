from pathlib import Path

def replace(name,old,new):
    p=Path(name);s=p.read_text();assert s.count(old)==1,(name,s.count(old));p.write_text(s.replace(old,new,1))

replace('main/display/raw_display.cc',
    'const bool notice = !form_active_.load() && notification_text_[0]',
    'const bool notice = !quick_controls_open_.load() && !form_active_.load() && product_page_!=ProductPage::Reader && notification_text_[0]')
replace('main/display/quick_controls_view.cc',
    'DrawProductLabelLocked(32,624,416,"输入草稿保留；闹钟始终优先",ui_font_small);',
    'DrawProductLabelLocked(32,624,416,form_active_.load() ? "编辑中：请先完成或取消，再设置网络" : "输入草稿保留；闹钟始终优先",ui_font_small);')
replace('main/display/font/raw_font.h',
    '// Full HarmonyOS Sans SC face in font_data; the small embedded UI face remains\n// a recovery fallback when no pack is installed. No LVGL or runtime TTF engine.',
    '// Native-size HarmonyOS Sans SC UI bitmaps are preferred; font_data supplies\n// the remaining characters. Binary pixels only; no LVGL or runtime TTF engine.')
replace('tools/preview_mono_support.py',
    'void QuickControls::Refresh(){assert(ui_lock_depth==0);preview_quick.version="test-build";}',
    'void QuickControls::Refresh(){assert(ui_lock_depth==0);preview_quick.version="test-build";preview_quick.network="家里的 Wi-Fi";}')
