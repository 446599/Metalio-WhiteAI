#!/usr/bin/env python3
"""Render the firmware's actual C++ product pages on the host (no ESP board).

Uses the checked-in bitmaps, drawing methods and DashboardData. Only the clock
and the hardware-owning RawDisplay shell are substituted. Every text draw is
checked for missing glyphs and overflow. PBM/PNG output is a software preview,
not optical evidence from the e-paper panel. Requires C++17 and Pillow.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from preview_input_support import HEADERS, FIELDS, METHODS, EXERCISE
import preview_mono_support as mono
HEADERS += mono.HEADERS
FIELDS += mono.FIELDS
METHODS += mono.METHODS
EXERCISE += mono.EXERCISE

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'main/display/raw_display.cc'


def function(source: str, name: str) -> str:
    match = re.search(r'^(?:constexpr )?(?:void|bool|int|uint32_t|const ui_glyph_t\*) ' + re.escape(name) + r'\(', source, re.M)
    if not match:
        raise ValueError(f'Missing render function: {name}')
    begin = source.index('{', match.start())
    # Ignore strings/comments when matching braces in C++ bodies.
    tokens = re.finditer(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/|[{}]', source[begin:], re.S)
    depth = 0
    for token in tokens:
        if token.group() == '{':
            depth += 1
        elif token.group() == '}':
            depth -= 1
            if depth == 0:
                return source[match.start():begin + token.end()]
    raise ValueError(f'Unbalanced function: {name}')


def host_source(source: str) -> str:
    product_names = re.findall(r'^void RawDisplay::(DrawProduct\w+)\(', source, re.M)
    methods = ['SetPixel', 'FillRect', 'StrokeRect', 'FillRoundRect', 'StrokeRoundRect',
               'FillCircle', 'StrokeCircle', 'DrawTextInk', 'DrawText', 'TextWidth',
               'FitText', 'FitTextLines', 'DrawTextCentered'] + product_names + METHODS
    bodies = [function(source, 'RawDisplay::' + name) for name in methods]
    bodies.append(function((ROOT/'main/display/reminder_view.cc').read_text(), 'RawDisplay::DrawReminderAlertLocked'))
    declarations = [body[:body.index('{')].replace('RawDisplay::', '').strip() + ';' for body in bodies]
    constants = '\n'.join(re.findall(r'^constexpr (?:int|uint8_t) k(?:Ui\w*|Ai\w*|PortraitW|PortraitH|White)[^;]*;', source, re.M))
    enums = re.search(r'enum class ProductPage[^}]+}', (ROOT / 'main/display/raw_display.h').read_text()).group() + ';'
    free_functions = '\n'.join(function(source, name) for name in ['Utf8Next', 'FindGlyph', 'CopyDisplayText', 'ProductRowAt'])
    text_check = '''
    if (text && *text) {
        if (x < 0 || x + TextWidth(text, font) > kPortraitW || y < 0 || y + font.height > kPortraitH) {
            std::fprintf(stderr, "TEXT OVERFLOW [%s] x=%d y=%d w=%d h=%d: %s\\n", preview_page, x, y, TextWidth(text, font), font.height, text);
            ++preview_errors;
        }
        const char* checked = text;
        while (*checked) {
            uint32_t cp = Utf8Next(&checked);
            if (raw_font::Lookup(font, cp).missing) {
                std::fprintf(stderr, "MISSING GLYPH [%s]: U+%04X\\n", preview_page, cp);
                ++preview_errors;
            }
        }
    }
'''
    for i, body in enumerate(bodies):
        if body.startswith('void RawDisplay::DrawTextInk('):
            pos = body.index('{') + 1
            bodies[i] = body[:pos] + text_check + body[pos:]
    return '''#include <algorithm>
#include <cassert>
#include <fstream>
#include <iterator>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include "display/font/ai_ui_assets.h"
#include "display/font/raw_font.h"
#include "display/font/text_layout.h"
#include "xiaozhi/conversation.h"
#include "reminders/alert_state.h"
#include "display/reminder_layout.h"
#include "reminders/presentation.h"
#include "audio/recorder_state.h"
#include "display/icons/lucide_icons.h"
#include "notes/note_store.h"
namespace notes { Store& DeviceStore() { static Store s([](const auto&){return true;}); return s; } }
namespace reminders {
class Service {
public:
    static Service& Instance() { static Service instance; return instance; }
    std::vector<Item> items;
    std::vector<Item> List() const { return items; }
};
}
namespace xiaozhi {
class AudioSession {
public:
    static AudioSession& GetInstance() { static AudioSession instance; return instance; }
    audio::RecorderSnapshot state;
    audio::RecorderSnapshot RecorderState() const { return state; }
};
}

#include "dashboard/dashboard_data.h"
const char* preview_page = "";
int preview_errors = 0;
time_t preview_epoch = 0;
time_t preview_time(time_t* out) { if (out) *out = preview_epoch; return preview_epoch; }
int64_t esp_timer_get_time() { return 1000000; }
#define time(out) preview_time(out)
''' + HEADERS + constants + '\n' + free_functions + '''
class RawDisplay {
public:
''' + enums + '\n' + FIELDS + '\n'.join(declarations) + '''
    uint8_t pixels[480 * 800 / 8]{};
    uint8_t* portrait_fb_ = pixels;
    size_t portrait_size_ = sizeof(pixels);
    int battery_percent_ = 82;
    bool charging_ = false;
    bool power_save_ = false;
    uint32_t last_dashboard_revision_ = 0;
    uint32_t last_conversation_revision_ = 0;
    int ai_text_page_ = 0;
    int ai_page_count_ = 1;
    bool ai_show_transcript_ = false;
    bool voice_note_mode_ = false;
    uint32_t ai_drawn_turn_ = 0;
    char status_text_[48]{};
    char notification_text_[96]{};
    int64_t notification_deadline_ms_ = 0;
    reminders::AlertSnapshot reminder_alert_;
    ProductPage product_page_ = ProductPage::Home;
    int navigation_index_ = 0;
    uint8_t quick_note_state_ = 0;
    uint8_t reader_page_ = 0;
    int calendar_month_ = 0, calendar_day_ = 0, calendar_events_page_ = 0;
    int alarm_page_ = 0, alarm_pages_ = 1;
    uint32_t alarm_ids_[4]{};
    bool alarm_enabled_[4]{};
    uint32_t last_recorder_revision_ = 0, last_notes_revision_ = 0;
    int notes_page_=0, notes_pages_=1, note_text_page_=0, note_text_pages_=1;
    uint32_t note_id_=0, note_ids_[6]{};
};
''' + '\n\n'.join(bodies) + '''
int main(int argc, char** argv) {
    if (argc != 4) return 2;
    std::ifstream font_file(argv[3], std::ios::binary);
    std::vector<uint8_t> font_data((std::istreambuf_iterator<char>(font_file)), {});
    assert(raw_font::InitMemory(font_data.data(), font_data.size()));
    assert(raw_font::Count() == 45248);
    struct tm fixed{};
    fixed.tm_year = 126; fixed.tm_mon = 8; fixed.tm_mday = 18;
    fixed.tm_hour = 9; fixed.tm_min = 41;
    preview_epoch = mktime(&fixed);
    auto& data = dashboard::DashboardData::GetInstance();
    const std::string scenario = argv[2];
    data.UpdateFreshness(static_cast<uint32_t>(preview_epoch));
    if (scenario != "offline" && scenario != "empty") {
        data.SetNetwork("Wi-Fi");
        data.SetAiStatus("小智已连接");
        data.SetAiSummary(0, "今天先完成最重要的一件事");
        dashboard::Weather weather{};
        weather.valid = true; weather.temperature_c = 24;
        weather.updated_epoch = static_cast<uint32_t>(preview_epoch);
        dashboard::CopyText(weather.condition, sizeof(weather.condition), "晴");
        data.SetWeather(weather);
        dashboard::Quota quota{};
        quota.valid = true; quota.updated_epoch = static_cast<uint32_t>(preview_epoch); quota.five_hour_remaining = 82; quota.weekly_remaining = 64;
        data.SetQuota(quota);
    }
    if (scenario == "empty") {
        data.ClearAiSummary(); data.SetScheduleCount(0);
        for (size_t i = 0; i < dashboard::kCustomCardCount; ++i) data.SetCustomCard(i, {});
    }
    if (scenario == "long") {
        data.SetNetwork("网络连接正在重新连接");
        data.SetAiStatus("设备正在等待连接成功");
        data.SetAiSummary(0, "今天先完成当前最重要的一件事完成后再交给小智安排下一步");
        data.SetAiSummary(1, "完成一小步以后停下来看看下一步是否仍然清楚明天就不必从头寻找方向");
        dashboard::Weather weather{};
        weather.valid = true; weather.temperature_c = -12;
        dashboard::CopyText(weather.condition, sizeof(weather.condition), "天气数据等待更新");
        data.SetWeather(weather);
        dashboard::Quota quota{};
        quota.valid = true; quota.five_hour_remaining = -10; quota.weekly_remaining = 200;
        data.SetQuota(quota);
        for (int i = 0; i < 3; ++i) data.SetSchedule(i, "23:59", "今天需要完成的最重要的一件工作事项", "", i == 0);
    }
    if (scenario == "stale") {
        auto stale = data.GetSnapshot();
        stale.weather.updated_epoch = static_cast<uint32_t>(preview_epoch) - 7200;
        stale.weather.from_cache = true;
        stale.weather.request_state = dashboard::RequestState::Offline;
        stale.quota.updated_epoch = static_cast<uint32_t>(preview_epoch) - 7200;
        stale.quota.from_cache = true;
        stale.quota.request_state = dashboard::RequestState::Failed;
        data.SetWeather(stale.weather); data.SetQuota(stale.quota);
        data.SetNetwork("离线");
    }
    // Exercise the real row hit helper at the visible edges and in gutters.
    for (int row = 0; row < 6; ++row) {
        const int y = kUiBodyY + row * kUiRowPitch;
        assert(ProductRowAt(kUiInset, y, 6, kUiRowPitch, kUiRowHeight) == row);
        assert(ProductRowAt(447, y + kUiRowHeight - 1, 6, kUiRowPitch, kUiRowHeight) == row);
        assert(ProductRowAt(448, y, 6, kUiRowPitch, kUiRowHeight) == -1);
        assert(ProductRowAt(32, y + kUiRowHeight, 6, kUiRowPitch, kUiRowHeight) == -1);
    }
    assert(ProductRowAt(32, kUiBodyY - 1, 6, kUiRowPitch, kUiRowHeight) == -1);
    assert(ProductRowAt(32, kUiBodyY, 0, kUiRowPitch, kUiRowHeight) == -1);
    assert(ProductRowAt(32, kUiRailY, 6, kUiRowPitch, kUiRowHeight) == -1);
    auto& conversation = xiaozhi::Conversation::GetInstance();
    if (scenario == "normal" || scenario == "long") {
        conversation.Begin();
        conversation.SetTranscript("明天下午把墨水屏的交互设计整理成笔记，顺便测试繁體、麒麟、龘、𠮷这些字符。");
        conversation.ReceiveAnswer("可以分成两步完成：\\n先整理按键与屏幕交互，明确按住说话、松开发送的体验。\\n再验证完整中文字库，包括繁體字、生僻字与英文。\\n待办只是草稿，不会自动执行。", true);
        if (scenario == "long") for (int i = 0; i < 20; ++i)
            conversation.ReceiveAnswer("让每一个闪过的想法，都能安静地留在屏幕上。", true);
        conversation.SetState(xiaozhi::TurnState::Done);
    }
    auto& reminders = reminders::Service::Instance().items;
    if (scenario != "empty") {
        for (int i=0;i<(scenario=="long" ? 16 : 4);++i) {
            reminders::Item item;
            item.id=i+1; item.kind=i%2 ? "event" : "alarm";
            item.title=scenario=="long" ? "完成今天最重要的项目进展整理并且按时休息喝水" : i%2 ? "整理项目计划" : "早起读书";
            item.at=preview_epoch+3600+i*1800; item.enabled=true;
            reminders.push_back(item);
        }
        reminders[0].weekdays=127;
    }
    auto& notes=notes::DeviceStore();assert(notes.Restore(""));
    if (scenario!="empty") for (int i=0;i<(scenario=="long" ? 8 : 2);++i) {
        notes::Note saved;std::string error;std::string body="购物清单\\n牛奶、面包和水果\\n周末整理工作笔记。";
        if (scenario=="long") for (int j=0;j<14;++j) body+="今天安排一个番茄钟，专注完成一件事。";
        assert(notes.Put({0,"购物清单与本周计划",body,preview_epoch},saved,error));
    }
    RawDisplay display;display.note_id_=1;
    char fitted[32];
    display.FitText("今天先完成当前最重要的一件事", ui_font_body, 160, fitted, sizeof(fitted));
    assert(display.TextWidth(fitted, ui_font_body) <= 160);
    assert(std::strstr(fitted, "…"));
    if (scenario == "long") { display.battery_percent_ = 100; display.charging_ = true; }
    const char* names[] = {"home", "ai", "steps", "note", "reader", "today", "cards", "detail", "keep", "apps", "tools", "settings", "confirm", "more", "alarm", "recorder", "notes", "note-detail"};
    auto save = [&](const char* name) {
        preview_page = name;
        display.DrawProductScreenLocked();
        std::string path = std::string(argv[1]) + "/" + name + ".pbm";
        FILE* out = std::fopen(path.c_str(), "wb");
        if (!out) std::exit(2);
        std::fprintf(out, "P4\\n480 800\\n");
        for (const auto pixel : display.pixels) std::fputc(pixel ^ 0xff, out);
        std::fclose(out);
    };
    for (int i = 0; i < 18; ++i) {
        display.product_page_ = static_cast<RawDisplay::ProductPage>(i);
        save(names[i]);
    }
    display.product_page_=RawDisplay::ProductPage::Notes;display.notes_page_=1;save("notes-page-2");
    display.product_page_=RawDisplay::ProductPage::NoteDetail;display.note_text_page_=1;save("note-detail-page-2");
    for (int battery : {0,25,50,100}) {
        display.product_page_=RawDisplay::ProductPage::Home;display.battery_percent_=battery;
        save(("battery-"+std::to_string(battery)).c_str());
    }
    for (int i = 1; i < 6; ++i) {
        display.product_page_ = RawDisplay::ProductPage::Home;
        display.navigation_index_ = i;
        const auto name = "home-selected-" + std::to_string(i);
        save(name.c_str());
    }
    for (int i = 1; i < 5; ++i) {
        display.product_page_ = RawDisplay::ProductPage::QuickNote;
        display.quick_note_state_ = i;
        const auto name = "note-" + std::to_string(i);
        save(name.c_str());
    }
    for (int i = 1; i < 3; ++i) {
        display.product_page_ = RawDisplay::ProductPage::Reader;
        display.reader_page_ = i;
        const auto name = "reader-" + std::to_string(i);
        save(name.c_str());
    }
    display.product_page_ = RawDisplay::ProductPage::Recorder;
    auto& recorder = xiaozhi::AudioSession::GetInstance().state;
    recorder.has_clip=true; recorder.seconds=30;
    save("recorder-clip");
    recorder.mode=audio::RecorderMode::Recording;
    save("recorder-recording");
    recorder.mode=audio::RecorderMode::Playing;
    save("recorder-playing");
    recorder.mode=audio::RecorderMode::Idle; recorder.failed=true;
    save("recorder-error");
    display.product_page_=RawDisplay::ProductPage::Alarm; display.alarm_page_=1;
    save("alarm-page-2");
    display.product_page_=RawDisplay::ProductPage::TodayList; display.calendar_events_page_=1;
    save("calendar-events-2");
    display.calendar_month_=1; display.calendar_day_=1;
    save("calendar-next-month");
    display.calendar_month_=17; display.calendar_day_=29;
    save("calendar-leap-february");
    display.product_page_ = RawDisplay::ProductPage::Home;
    dashboard::CopyText(display.notification_text_, sizeof(display.notification_text_), "正在刷新天气与额度");
    display.notification_deadline_ms_ = 3000;
    save("notification");
    display.product_page_ = RawDisplay::ProductPage::AiResult;
    for (int i = 0; i < 8; ++i) {
        conversation.SetState(static_cast<xiaozhi::TurnState>(i));
        const auto name = "ai-state-" + std::to_string(i);
        save(name.c_str());
    }
    display.ai_show_transcript_ = true;
    save("ai-transcript");
    display.ai_show_transcript_ = false;
    display.ai_text_page_ = 1;
    save("ai-page-2");
    display.voice_note_mode_ = true;
    display.ai_drawn_turn_ = UINT32_MAX;
    display.product_page_ = RawDisplay::ProductPage::AiResult;
    save("voice-note");
    assert(display.ai_show_transcript_);
    data.SetAiStatus("请绑定设备");
    data.SetAiSummary(0,"绑定码 123456");
    save("activation");
    data.SetAiStatus("小智已连接");
    display.product_page_ = RawDisplay::ProductPage::Home;
    display.power_save_ = true;
    save("sleep");
    display.power_save_ = false;
    display.reminder_alert_.active = true;
    display.reminder_alert_.audible = true;
    display.reminder_alert_.at = preview_epoch;
    display.reminder_alert_.title = scenario == "long" ? "提醒自己完成今天最重要的项目进展整理并且按时休息喝水" : "该起床啦";
    display.reminder_alert_.count = 1;
    save("alarm-ringing");
    display.reminder_alert_.audible = false;
    display.reminder_alert_.count = 3;
    display.reminder_alert_.message = "已自动静音，请处理提醒";
    save("alarm-silent");
''' + EXERCISE + '''
    return preview_errors ? 1 : 0;
}
'''


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=ROOT / 'build/ui-preview')
    parser.add_argument('--scenario', choices=['normal', 'offline', 'empty', 'long', 'stale', 'all'], default='all')
    args = parser.parse_args()
    from PIL import Image, ImageDraw
    compiler = shutil.which(os.environ.get('CXX', 'c++'))
    if not compiler:
        raise SystemExit('A C++17 compiler is required (set CXX).')
    scenarios = ['normal', 'offline', 'empty', 'long', 'stale'] if args.scenario == 'all' else [args.scenario]
    with tempfile.TemporaryDirectory(prefix='miaoink-ui-') as temporary:
        host = Path(temporary) / 'preview.cc'
        executable = Path(temporary) / 'preview'
        host.write_text(host_source(SOURCE.read_text()+'\n'+(ROOT/'main/display/system_view.cc').read_text()+'\n'+(ROOT/'main/display/input_view.cc').read_text()+'\n'+(ROOT/'main/display/quick_controls_view.cc').read_text()+'\n'+(ROOT/'main/display/reader_view.cc').read_text()))
        cjson = ROOT / "managed_components/espressif__cjson/cJSON"
        subprocess.run(["cc", "-c", str(cjson / "cJSON.c"), "-I", str(cjson), "-o", str(Path(temporary)/"cjson.o")], check=True)
        subprocess.run([compiler, '-std=c++17', '-O1', '-DFONTPACK_HOST_TEST', '-x', 'c++', '-I', str(ROOT / 'main'),
                        '-I', str(cjson), str(host), str(ROOT / 'main/display/font/ai_ui_assets.c'),
                        str(ROOT / 'main/dashboard/dashboard_data.cc'),
                        str(ROOT / 'main/display/font/raw_font.cc'),
                        str(ROOT / 'main/display/font/font_loader.c'),
                        str(ROOT / 'main/input/text_input.cc'), str(ROOT / 'main/network/setup_model.cc'), str(ROOT / 'main/xiaozhi/conversation.cc'), str(ROOT / 'main/notes/note_store.cc'), str(ROOT / 'main/reminders/reminder_store.cc'), '-x', 'none', str(Path(temporary)/'cjson.o'), '-o', str(executable)], check=True)
        failures = []
        for scenario in scenarios:
            out = args.out.resolve() / scenario
            out.mkdir(parents=True, exist_ok=True)
            run = subprocess.run([str(executable), str(out), scenario, str(ROOT / 'assets/fonts/harmony_full.fontpack')], check=False)
            if run.returncode:
                failures.append(scenario)
            for image in out.glob('*.pbm'):
                with Image.open(image) as frame:
                    frame.save(image.with_suffix('.png'))
            names = ['home', 'apps', 'alarm', 'today', 'recorder', 'ai', 'more', 'tools']
            sheet = Image.new('RGB', (4 * 264 + 24, 2 * 460 + 24), '#e8e8e5')
            draw = ImageDraw.Draw(sheet)
            for i, name in enumerate(names):
                x, y = 24 + i % 4 * 264, 24 + i // 4 * 460
                draw.text((x, y), f'{name.upper()} / {scenario}', fill='#222222')
                with Image.open(out / (name + '.png')) as frame:
                    sheet.paste(frame.resize((240, 400), Image.Resampling.LANCZOS), (x, y + 24))
            sheet.save(out / 'overview.png')
            print(f'{scenario}: {len(list(out.glob("*.pbm")))} firmware frames -> {out}')
        if failures:
            raise SystemExit(f'Render validation failed: {", ".join(failures)}')


if __name__ == '__main__':
    main()
