#include "app_launcher.h"

#include "app_audio_test/app_audio_test.h"
#include "app_battery_test/app_battery_test.h"
#include "app_bluetooth_test/app_bluetooth_test.h"
#include "app_button_test/app_button_test.h"
#include "app_cell_test/app_cell_test.h"
#include "app_motor_test/app_motor_test.h"
#include "app_reader_txt/app_reader_txt.h"
#include "app_screen_test/app_screen_test.h"
#include "app_sd_test/app_sd_test.h"
#include "app_touch_test/app_touch_test.h"
#include "app_wifi_test/app_wifi_test.h"
#include "fontpack_lvgl.h"
#include "font_awesome.h"
#include "haptic_feedback.h"
#include "lv_adapter_display.h"
#include "screen_common.h"
#include "vk_key_handler.h"

#include <esp_log.h>
#include <cstdio>
#include <ctime>

LV_FONT_DECLARE(font_awesome_30_1);

namespace {
constexpr const char* TAG = "AppLauncher";
constexpr lv_coord_t kInset = 28;
constexpr lv_coord_t kGap = 10;
constexpr lv_coord_t kCardRadius = 10;

const lv_font_t* UiFont() { const lv_font_t* f = fontpack_lv_font_ui(); return f ? f : LV_FONT_DEFAULT; }
const lv_font_t* SmallFont() { const lv_font_t* f = fontpack_lv_font_get(18, 2); return f ? f : UiFont(); }
const lv_font_t* TitleFont() { const lv_font_t* f = fontpack_lv_font_get(30, 2); return f ? f : UiFont(); }

lv_obj_t* Label(lv_obj_t* parent, const char* text, const lv_font_t* font,
                lv_color_t color = lv_color_black()) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_style_text_font(label, font ? font : UiFont(), 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
}

lv_obj_t* Icon(lv_obj_t* parent, const char* glyph) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, glyph ? glyph : "");
    lv_obj_set_style_text_font(label, &font_awesome_30_1, 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
}

void CardStyle(lv_obj_t* obj, lv_color_t fill = lv_color_white()) {
    lv_obj_set_style_bg_color(obj, fill, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, lv_color_black(), 0);
    lv_obj_set_style_border_width(obj, 2, 0);
    lv_obj_set_style_radius(obj, kCardRadius, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_black(), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(obj, lv_color_black(), LV_STATE_PRESSED);
}

lv_obj_t* SectionLabel(lv_obj_t* parent, const char* text) {
    lv_obj_t* label = Label(parent, text, SmallFont());
    lv_obj_set_style_text_letter_space(label, 2, 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    return label;
}

void Refresh(lv_obj_t* screen) {
    lv_obj_invalidate(screen);
    if (auto* display = LVAdapterDisplay::Instance()) display->RequestNextFullRefresh();
}

void NavigateAsync(void* data) {
    auto factory = reinterpret_cast<ScreenFactory>(data);
    if (factory != nullptr) ScreenNavigateTo(factory);
}
void BackClicked(lv_event_t*) { ScreenNavigateBack(); }

#define OPEN_REAL(name, type) \
    void name(lv_event_t*) { lv_async_call(NavigateAsync, reinterpret_cast<void*>(type::Create)); }
OPEN_REAL(OpenScreen, AppScreenTest)
OPEN_REAL(OpenMotor, AppMotorTest)
OPEN_REAL(OpenTouch, AppTouchTest)
OPEN_REAL(OpenButton, AppButtonTest)
OPEN_REAL(OpenBattery, AppBatteryTest)
OPEN_REAL(OpenAudio, AppAudioTest)
OPEN_REAL(OpenBluetooth, AppBluetoothTest)
OPEN_REAL(OpenWifi, AppWifiTest)
OPEN_REAL(OpenCell, AppCellTest)
OPEN_REAL(OpenSd, AppSdTest)
#undef OPEN_REAL

struct Route { const char* id; const char* title; const char* glyph; };

lv_obj_t* Placeholder(const Route& route) {
    ScreenSetIsHome(false);
    lv_obj_t* screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);
    EpdStatusBar bar = ScreenCreateStatusBar(screen);
    lv_obj_t* body = lv_obj_create(screen);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES - kInset * 2, LV_VER_RES - bar.height - 24);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, bar.height + 12);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* overline = SectionLabel(body, "FEATURE MODULE");
    lv_obj_align(overline, LV_ALIGN_TOP_LEFT, 0, 4);
    lv_obj_t* hero = lv_obj_create(body);
    lv_obj_remove_style_all(hero);
    lv_obj_set_size(hero, lv_pct(100), 184);
    lv_obj_align(hero, LV_ALIGN_TOP_LEFT, 0, 38);
    CardStyle(hero, lv_color_black());
    lv_obj_t* glyph = Icon(hero, route.glyph);
    lv_obj_set_style_text_color(glyph, lv_color_white(), 0);
    lv_obj_align(glyph, LV_ALIGN_TOP_LEFT, 20, 20);
    lv_obj_t* title = Label(hero, route.title, TitleFont(), lv_color_white());
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 20, 74);
    lv_obj_t* state = Label(hero, "功能准备中", SmallFont(), lv_color_white());
    lv_obj_align(state, LV_ALIGN_BOTTOM_LEFT, 20, -18);
    lv_obj_t* note = Label(body, "数据源接入后将在这里显示内容", SmallFont());
    lv_obj_align(note, LV_ALIGN_TOP_LEFT, 0, 244);
    lv_obj_t* back = Label(body, "‹  返回上一页", UiFont());
    lv_obj_set_size(back, 196, 54);
    lv_obj_set_style_border_color(back, lv_color_black(), 0);
    lv_obj_set_style_border_width(back, 2, 0);
    lv_obj_set_style_radius(back, kCardRadius, 0);
    lv_obj_set_style_text_align(back, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(back, 10, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(back);
    lv_obj_add_event_cb(back, BackClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_align(back, LV_ALIGN_BOTTOM_LEFT, 0, -8);
    VkKey_AttachScreen(screen, route.id);
    Refresh(screen);
    return screen;
}

lv_obj_t* CreateAi() { return Placeholder({"ai", "AI 助手", FONT_AWESOME_MICROCHIP_AI}); }
lv_obj_t* CreateWeather() { return Placeholder({"weather", "天气", FONT_AWESOME_CLOUD_SUN}); }
lv_obj_t* CreatePomodoro() { return Placeholder({"pomodoro", "番茄钟", FONT_AWESOME_CLOCK}); }
lv_obj_t* CreateAlarm() { return Placeholder({"alarm", "闹钟", FONT_AWESOME_ALARM_CLOCK}); }
lv_obj_t* CreateCalendar() { return Placeholder({"calendar", "日历", FONT_AWESOME_CALENDAR}); }
lv_obj_t* CreateCalculator() { return Placeholder({"calculator", "计算器", FONT_AWESOME_CALCULATOR}); }
lv_obj_t* CreateRecorder() { return Placeholder({"recorder", "录音机", FONT_AWESOME_MICROPHONE}); }
lv_obj_t* CreateFiles() { return Placeholder({"files", "文件", FONT_AWESOME_SD_CARD}); }
lv_obj_t* CreateMusic() { return Placeholder({"music", "音乐", FONT_AWESOME_MUSIC}); }
lv_obj_t* CreateNotes() { return Placeholder({"notes", "备忘录", FONT_AWESOME_PEN_TO_SQUARE}); }
lv_obj_t* CreateSettings() { return Placeholder({"settings", "设置", FONT_AWESOME_GEAR}); }
lv_obj_t* CreateApps();

void OpenReader(lv_event_t*) { lv_async_call(NavigateAsync, reinterpret_cast<void*>(AppReaderTxt::Create)); }
void OpenNotes(lv_event_t*) { lv_async_call(NavigateAsync, reinterpret_cast<void*>(CreateNotes)); }
void OpenSettings(lv_event_t*) { lv_async_call(NavigateAsync, reinterpret_cast<void*>(CreateSettings)); }
void OpenAi(lv_event_t*) { lv_async_call(NavigateAsync, reinterpret_cast<void*>(CreateAi)); }
void OpenWeather(lv_event_t*) { lv_async_call(NavigateAsync, reinterpret_cast<void*>(CreateWeather)); }
void OpenPomodoro(lv_event_t*) { lv_async_call(NavigateAsync, reinterpret_cast<void*>(CreatePomodoro)); }
void OpenAlarm(lv_event_t*) { lv_async_call(NavigateAsync, reinterpret_cast<void*>(CreateAlarm)); }
void OpenCalendar(lv_event_t*) { lv_async_call(NavigateAsync, reinterpret_cast<void*>(CreateCalendar)); }
void OpenCalculator(lv_event_t*) { lv_async_call(NavigateAsync, reinterpret_cast<void*>(CreateCalculator)); }
void OpenRecorder(lv_event_t*) { lv_async_call(NavigateAsync, reinterpret_cast<void*>(CreateRecorder)); }
void OpenFiles(lv_event_t*) { lv_async_call(NavigateAsync, reinterpret_cast<void*>(CreateFiles)); }
void OpenMusic(lv_event_t*) { lv_async_call(NavigateAsync, reinterpret_cast<void*>(CreateMusic)); }

struct AppEntry { const char* name; const char* glyph; lv_event_cb_t cb; };
const AppEntry kApps[] = {
    {"阅读", FONT_AWESOME_GLASSES, OpenReader}, {"AI 助手", FONT_AWESOME_MICROCHIP_AI, OpenAi},
    {"备忘录", FONT_AWESOME_PEN_TO_SQUARE, OpenNotes}, {"天气", FONT_AWESOME_CLOUD_SUN, OpenWeather},
    {"番茄钟", FONT_AWESOME_CLOCK, OpenPomodoro}, {"闹钟", FONT_AWESOME_ALARM_CLOCK, OpenAlarm},
    {"日历", FONT_AWESOME_CALENDAR, OpenCalendar}, {"计算器", FONT_AWESOME_CALCULATOR, OpenCalculator},
    {"录音机", FONT_AWESOME_MICROPHONE, OpenRecorder}, {"文件", FONT_AWESOME_SD_CARD, OpenFiles},
    {"音乐", FONT_AWESOME_MUSIC, OpenMusic}, {"设置", FONT_AWESOME_GEAR, OpenSettings},
};

lv_obj_t* AppItem(lv_obj_t* parent, const AppEntry& app) {
    lv_obj_t* item = lv_obj_create(parent);
    lv_obj_remove_style_all(item);
    const lv_coord_t grid_w = LV_HOR_RES - kInset * 2;
    lv_obj_set_width(item, (grid_w - kGap * 2) / 3);
    lv_obj_set_height(item, 104);
    CardStyle(item);
    lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
    HapticAttachClick(item);
    lv_obj_add_event_cb(item, app.cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* icon = Icon(item, app.glyph);
    lv_obj_set_style_text_color(icon, lv_color_black(), LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(icon, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 14);
    lv_obj_t* name = Label(item, app.name, SmallFont());
    lv_obj_set_width(name, lv_pct(100));
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(name, lv_color_black(), LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(name, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -14);
    return item;
}

lv_obj_t* HomeItem(lv_obj_t* parent, const AppEntry& app) {
    lv_obj_t* item = lv_obj_create(parent);
    lv_obj_remove_style_all(item);
    const lv_coord_t grid_w = LV_HOR_RES - kInset * 2;
    lv_obj_set_width(item, (grid_w - kGap) / 2);
    lv_obj_set_height(item, 142);
    CardStyle(item);
    lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
    HapticAttachClick(item);
    lv_obj_add_event_cb(item, app.cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* icon = Icon(item, app.glyph);
    lv_obj_set_style_text_color(icon, lv_color_black(), LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(icon, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_t* name = Label(item, app.name, UiFont());
    lv_obj_set_width(name, lv_pct(100));
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(name, lv_color_black(), LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(name, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -24);
    return item;
}

lv_obj_t* CreateApps() {
    ScreenSetIsHome(false);
    lv_obj_t* screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    EpdStatusBar bar = ScreenCreateStatusBar(screen);
    lv_obj_t* body = lv_obj_create(screen);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES - kInset * 2, LV_VER_RES - bar.height - 12);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, bar.height + 6);
    lv_obj_set_style_pad_row(body, 0, 0);
    lv_obj_set_style_pad_bottom(body, 24, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t* heading = Label(body, "所有应用", TitleFont());
    lv_obj_set_style_margin_bottom(heading, 6, 0);
    lv_obj_t* subheading = Label(body, "常用工具与阅读入口", SmallFont());
    lv_obj_set_style_text_color(subheading, lv_color_black(), 0);
    lv_obj_set_style_margin_bottom(subheading, 18, 0);
    // AI 桌面摘要：先显示本地快照，网络适配器接入后通过事件刷新。
    lv_obj_t* summary = lv_obj_create(body);
    lv_obj_remove_style_all(summary);
    lv_obj_set_size(summary, lv_pct(100), 92);
    lv_obj_align(summary, LV_ALIGN_TOP_MID, 0, 0);
    CardStyle(summary, lv_color_black());
    lv_obj_t* summary_title = Label(summary, "AI 今日摘要", SmallFont(), lv_color_white());
    lv_obj_align(summary_title, LV_ALIGN_TOP_LEFT, 18, 12);
    lv_obj_t* summary_text = Label(summary, "准备好迎接今天 · 暂无重要事项", SmallFont(), lv_color_white());
    lv_obj_align(summary_text, LV_ALIGN_BOTTOM_LEFT, 18, -14);

    lv_obj_t* grid = lv_obj_create(body);
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, lv_pct(100));
    lv_obj_set_height(grid, 4 * 104);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, kGap, 0);
    lv_obj_set_style_pad_column(grid, kGap, 0);
    for (const auto& app : kApps) AppItem(grid, app);
    lv_obj_t* system = Label(body, "设备诊断", UiFont());
    lv_obj_set_style_margin_top(system, 28, 0);
    lv_obj_set_style_margin_bottom(system, 10, 0);
    const AppEntry tests[] = {
        {"屏幕", FONT_AWESOME_IMAGE, OpenScreen}, {"马达", FONT_AWESOME_ARROWS_REPEAT, OpenMotor},
        {"触摸", FONT_AWESOME_COMPASS, OpenTouch}, {"按键", FONT_AWESOME_KEY, OpenButton},
        {"电池", FONT_AWESOME_BATTERY_FULL, OpenBattery}, {"音频", FONT_AWESOME_HEADPHONES, OpenAudio},
        {"蓝牙", FONT_AWESOME_BLUETOOTH, OpenBluetooth}, {"WiFi", FONT_AWESOME_WIFI, OpenWifi},
        {"4G", FONT_AWESOME_SIGNAL, OpenCell}, {"SD 卡", FONT_AWESOME_SD_CARD, OpenSd},
    };
    lv_obj_t* diag = lv_obj_create(body);
    lv_obj_remove_style_all(diag);
    lv_obj_set_width(diag, lv_pct(100));
    lv_obj_set_height(diag, 4 * 104);
    lv_obj_set_flex_flow(diag, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(diag, kGap, 0);
    lv_obj_set_style_pad_column(diag, kGap, 0);
    for (const auto& test : tests) AppItem(diag, test);
    VkKey_AttachScreen(screen, "apps", VkKeyScreenDesc{CreateApps});
    Refresh(screen);
    return screen;
}

}  // namespace

lv_obj_t* AppLauncher::Create() {
    ESP_LOGI(TAG, "create borderless home launcher");
    ScreenSetIsHome(true);
    lv_obj_t* screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);
    EpdStatusBar bar = ScreenCreateStatusBar(screen);
    // 旧版 EegoRead 首页顶栏左侧显示时钟，不显示“主页”标题。
    lv_obj_t* body = lv_obj_create(screen);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES - kInset * 2, LV_VER_RES - bar.height - 12);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, bar.height + 8);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    // AI 桌面：核心信息优先，诊断工具仍从“所有应用”进入。
    const AppEntry shortcuts[] = {
        {"AI 今日摘要", FONT_AWESOME_MICROCHIP_AI, OpenAi},
        {"今日日程", FONT_AWESOME_CALENDAR, OpenCalendar},
        {"当前天气", FONT_AWESOME_CLOUD_SUN, OpenWeather},
        {"Codex 用量", FONT_AWESOME_SIGNAL, OpenAi},
        {"继续阅读", FONT_AWESOME_GLASSES, OpenReader},
        {"设置卡片", FONT_AWESOME_GEAR, OpenSettings},
    };
    // AI 桌面摘要：先显示本地快照，网络适配器接入后通过事件刷新。
    lv_obj_t* summary = lv_obj_create(body);
    lv_obj_remove_style_all(summary);
    lv_obj_set_size(summary, lv_pct(100), 92);
    lv_obj_align(summary, LV_ALIGN_TOP_MID, 0, 0);
    CardStyle(summary, lv_color_black());
    lv_obj_t* summary_title = Label(summary, "AI 今日摘要", SmallFont(), lv_color_white());
    lv_obj_align(summary_title, LV_ALIGN_TOP_LEFT, 18, 12);
    lv_obj_t* summary_text = Label(summary, "准备好迎接今天 · 暂无重要事项", SmallFont(), lv_color_white());
    lv_obj_align(summary_text, LV_ALIGN_BOTTOM_LEFT, 18, -14);

    lv_obj_t* grid = lv_obj_create(body);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, lv_pct(100), 3 * 142 + 2 * kGap);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, 106);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, kGap, 0);
    lv_obj_set_style_pad_column(grid, kGap, 0);
    for (const auto& app : shortcuts) HomeItem(grid, app);
    lv_obj_t* dots = lv_obj_create(body);
    lv_obj_remove_style_all(dots);
    lv_obj_set_size(dots, lv_pct(100), 22);
    lv_obj_align(dots, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_t* dot = lv_obj_create(dots);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 12, 12);
    lv_obj_set_style_bg_color(dot, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_center(dot);
    VkKey_AttachScreen(screen, "home", VkKeyScreenDesc{AppLauncher::Create});
    Refresh(screen);
    return screen;
}
