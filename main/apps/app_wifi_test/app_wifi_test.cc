#include "app_wifi_test.h"

#include "hal/hal.h"
#include "haptic_feedback.h"
#include "screen_common.h"
#include "vk_key_handler.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "fontpack_lvgl.h"

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr const char* TAG = "AppWifiTest";
constexpr const char* kScreenId = "app_wifi_test";
constexpr const char* kPasswordScreenId = "app_wifi_password";
constexpr lv_coord_t kBtnH = 48;
constexpr lv_coord_t kBorderW = 2;
constexpr int kPageSize = 5;
constexpr size_t kPasswordMaxLen = 63;

lv_obj_t* s_scr = nullptr;
lv_obj_t* s_status = nullptr;
lv_obj_t* s_list = nullptr;
std::atomic<bool> s_busy{false};
std::atomic<bool> s_alive{false};
std::vector<HalWifiAp> s_aps;
int s_page = 0;
std::string s_selected_ssid;

// ---- password page ----
lv_obj_t* s_pwd_scr = nullptr;
lv_obj_t* s_pwd_status = nullptr;
lv_obj_t* s_pwd_label = nullptr;
std::string s_password;
bool s_shift = false;
std::atomic<bool> s_pwd_alive{false};
std::atomic<bool> s_pwd_busy{false};

void SetStatus(lv_obj_t* label, const char* text) {
    if (label != nullptr && lv_obj_is_valid(label) && text != nullptr) {
        lv_label_set_text(label, text);
    }
}

void StatusAsync(void* p) {
    auto* msg = static_cast<char*>(p);
    if (s_alive.load() && msg != nullptr) {
        SetStatus(s_status, msg);
    }
    free(msg);
}

void PwdStatusAsync(void* p) {
    auto* msg = static_cast<char*>(p);
    if (s_pwd_alive.load() && msg != nullptr) {
        SetStatus(s_pwd_status, msg);
    }
    free(msg);
}

void PostStatus(const char* text) {
    char* copy = text != nullptr ? strdup(text) : nullptr;
    if (copy != nullptr) {
        lv_async_call(StatusAsync, copy);
    }
}

void PostPwdStatus(const char* text) {
    char* copy = text != nullptr ? strdup(text) : nullptr;
    if (copy != nullptr) {
        lv_async_call(PwdStatusAsync, copy);
    }
}

lv_obj_t* MakeBtn(lv_obj_t* parent, const char* title, lv_event_cb_t cb, lv_coord_t width = 0) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    if (width > 0) {
        lv_obj_set_width(btn, width);
    } else {
        lv_obj_set_width(btn, lv_pct(100));
    }
    lv_obj_set_height(btn, kBtnH);
    lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, kBorderW, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_margin_bottom(btn, 8, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, fontpack_lv_font_ui(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

void UpdatePasswordLabel() {
    if (s_pwd_label == nullptr || !lv_obj_is_valid(s_pwd_label)) {
        return;
    }
    char buf[96];
    if (s_password.empty()) {
        std::snprintf(buf, sizeof(buf), "密码%s: （空）", s_shift ? "[A]" : "[a]");
    } else {
        std::snprintf(buf, sizeof(buf), "密码%s: %s", s_shift ? "[A]" : "[a]", s_password.c_str());
    }
    lv_label_set_text(s_pwd_label, buf);
}

void OnApClicked(lv_event_t* e) {
    auto* ssid = static_cast<char*>(lv_event_get_user_data(e));
    if (ssid == nullptr || ssid[0] == '\0') {
        return;
    }
    s_selected_ssid = ssid;
    ESP_LOGI(TAG, "select ssid=%s", s_selected_ssid.c_str());
    ScreenNavigateTo(AppWifiTest::CreatePassword);
}

void RefreshListUi() {
    if (s_list == nullptr || !lv_obj_is_valid(s_list)) {
        return;
    }
    lv_obj_clean(s_list);
    if (s_aps.empty()) {
        lv_obj_t* empty = lv_label_create(s_list);
        lv_label_set_text(empty, "暂无扫描结果\n扫描后点选网络输入密码连接");
        return;
    }
    const int pages = static_cast<int>((s_aps.size() + kPageSize - 1) / kPageSize);
    if (s_page >= pages) {
        s_page = pages - 1;
    }
    if (s_page < 0) {
        s_page = 0;
    }
    const int start = s_page * kPageSize;
    const int end = std::min(start + kPageSize, static_cast<int>(s_aps.size()));
    for (int i = start; i < end; ++i) {
        char line[96];
        const auto& ap = s_aps[static_cast<size_t>(i)];
        std::snprintf(line, sizeof(line), "%s  (%d dBm)", ap.ssid.c_str(),
                      static_cast<int>(ap.rssi));
        // user_data 指向 heap 拷贝的 ssid；随 list clean 用 DELETE 释放
        char* ssid_copy = strdup(ap.ssid.c_str());
        lv_obj_t* row = lv_obj_create(s_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, 40);
        lv_obj_set_style_bg_color(row, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(row, lv_color_black(), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_margin_bottom(row, 6, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        HapticAttachClick(row);
        lv_obj_add_event_cb(row, OnApClicked, LV_EVENT_CLICKED, ssid_copy);
        lv_obj_add_event_cb(
            row,
            [](lv_event_t* ev) {
                free(lv_event_get_user_data(ev));
            },
            LV_EVENT_DELETE, ssid_copy);
        lv_obj_t* lbl = lv_label_create(row);
        lv_label_set_text(lbl, line);
        lv_obj_set_style_text_font(lbl, fontpack_lv_font_ui(), 0);
        lv_obj_center(lbl);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    }
    char foot[64];
    std::snprintf(foot, sizeof(foot), "第 %d / %d 页（点选网络）", s_page + 1, pages);
    lv_obj_t* pg = lv_label_create(s_list);
    lv_label_set_text(pg, foot);
    lv_obj_set_style_text_align(pg, LV_TEXT_ALIGN_CENTER, 0);
}

void RefreshListAsync(void* /*p*/) {
    if (s_alive.load()) {
        RefreshListUi();
    }
}

void ScanTask(void* /*arg*/) {
    PostStatus("正在扫描…");
    std::vector<HalWifiAp> aps;
    const bool ok = GetHAL().WifiScan(aps);
    if (!s_alive.load()) {
        s_busy.store(false);
        vTaskDelete(nullptr);
        return;
    }
    s_aps = std::move(aps);
    s_page = 0;
    char buf[64];
    if (!ok) {
        std::snprintf(buf, sizeof(buf), "扫描失败");
    } else {
        std::snprintf(buf, sizeof(buf), "扫描完成：%d 个，请点选网络", static_cast<int>(s_aps.size()));
    }
    PostStatus(buf);
    lv_async_call(RefreshListAsync, nullptr);
    s_busy.store(false);
    vTaskDelete(nullptr);
}

void OnScan(lv_event_t* /*e*/) {
    if (s_busy.exchange(true)) {
        return;
    }
    if (xTaskCreatePinnedToCore(ScanTask, "wifi_scan", 8192, nullptr, 5, nullptr, 0) != pdPASS) {
        PostStatus("创建任务失败");
        s_busy.store(false);
    }
}

void OnPrevPage(lv_event_t* /*e*/) {
    if (s_page > 0) {
        --s_page;
        RefreshListUi();
    }
}

void OnNextPage(lv_event_t* /*e*/) {
    const int pages =
        s_aps.empty() ? 1 : static_cast<int>((s_aps.size() + kPageSize - 1) / kPageSize);
    if (s_page + 1 < pages) {
        ++s_page;
        RefreshListUi();
    }
}

void OnSwitchWifi(lv_event_t* /*e*/) {
    PostStatus("正在切换到 WiFi 并重启…");
    GetHAL().RequestSwitchNetwork(NetworkType::WIFI);
}

void OnDeleted(lv_event_t* e) {
    if (lv_event_get_target(e) != s_scr) {
        return;
    }
    s_alive.store(false);
    s_scr = nullptr;
    s_status = nullptr;
    s_list = nullptr;
}

void ConnectWithPasswordTask(void* /*arg*/) {
    const std::string ssid = s_selected_ssid;
    const std::string password = s_password;
    PostPwdStatus("正在连接…");
    const bool ok = GetHAL().WifiConnect(ssid, password, 20000);
    if (!s_pwd_alive.load()) {
        s_pwd_busy.store(false);
        vTaskDelete(nullptr);
        return;
    }
    char buf[192];
    if (ok || GetHAL().WifiIsConnected()) {
        std::snprintf(buf, sizeof(buf), "连接成功\n%s\nIP %s  RSSI %d", GetHAL().WifiSsid().c_str(),
                      GetHAL().WifiIp().c_str(), static_cast<int>(GetHAL().WifiRssi()));
    } else {
        std::snprintf(buf, sizeof(buf), "连接失败\n请检查密码后重试");
    }
    PostPwdStatus(buf);
    s_pwd_busy.store(false);
    vTaskDelete(nullptr);
}

void OnConnectPassword(lv_event_t* /*e*/) {
    if (s_selected_ssid.empty()) {
        SetStatus(s_pwd_status, "未选择网络");
        return;
    }
    if (s_pwd_busy.exchange(true)) {
        return;
    }
    if (xTaskCreatePinnedToCore(ConnectWithPasswordTask, "wifi_pwd", 8192, nullptr, 5, nullptr,
                                0) != pdPASS) {
        SetStatus(s_pwd_status, "创建任务失败");
        s_pwd_busy.store(false);
    }
}

void OnPasswordBack(lv_event_t* /*e*/) {
    ScreenNavigateBack();
}

void OnKeyChar(lv_event_t* e) {
    const intptr_t code = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    if (code == 0) {
        return;
    }
    if (s_password.size() >= kPasswordMaxLen) {
        return;
    }
    char c = static_cast<char>(code);
    if (s_shift && c >= 'a' && c <= 'z') {
        c = static_cast<char>(c - 'a' + 'A');
    }
    s_password.push_back(c);
    UpdatePasswordLabel();
}

void OnKeyBackspace(lv_event_t* /*e*/) {
    if (!s_password.empty()) {
        s_password.pop_back();
        UpdatePasswordLabel();
    }
}

void OnKeyShift(lv_event_t* /*e*/) {
    s_shift = !s_shift;
    UpdatePasswordLabel();
}

void OnKeyClear(lv_event_t* /*e*/) {
    s_password.clear();
    UpdatePasswordLabel();
}

void OnPwdDeleted(lv_event_t* e) {
    if (lv_event_get_target(e) != s_pwd_scr) {
        return;
    }
    s_pwd_alive.store(false);
    s_pwd_scr = nullptr;
    s_pwd_status = nullptr;
    s_pwd_label = nullptr;
}

void AddKeyBtn(lv_obj_t* row, const char* label, lv_event_cb_t cb, void* user_data,
               lv_coord_t w) {
    lv_obj_t* btn = lv_obj_create(row);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, 36);
    lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 4, 0);
    lv_obj_set_style_margin_right(btn, 4, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, fontpack_lv_font_ui(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
}

void AddCharRow(lv_obj_t* parent, const char* chars) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 40);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_margin_bottom(row, 4, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    const lv_coord_t key_w = 28;
    for (const char* p = chars; *p != '\0'; ++p) {
        char label[2] = {*p, '\0'};
        AddKeyBtn(row, label, OnKeyChar, reinterpret_cast<void*>(static_cast<intptr_t>(*p)), key_w);
    }
}

}  // namespace

lv_obj_t* AppWifiTest::Create() {
    ESP_LOGI(TAG, "create wifi test");
    ScreenSetIsHome(false);
    s_alive.store(true);
    s_busy.store(false);
    s_aps.clear();
    s_page = 0;

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_scr = scr;
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, OnDeleted, LV_EVENT_DELETE, nullptr);

    EpdStatusBar bar = ScreenCreateStatusBar(scr);
    if (bar.status_label != nullptr) {
    }

    lv_obj_t* body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, LV_VER_RES - bar.height);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, bar.height);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(body, 12, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    s_status = lv_label_create(body);
    lv_obj_set_width(s_status, lv_pct(100));
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_bottom(s_status, 8, 0);

    if (!GetHAL().IsWifiMode()) {
        lv_label_set_text(s_status, "当前是 4G 模式\n请切换到 WiFi 后重试");
        MakeBtn(body, "切换到 WiFi 并重启", OnSwitchWifi);
    } else {
        lv_label_set_text(s_status, "扫描后点选网络，输入密码连接");
        MakeBtn(body, "扫描附近 WiFi", OnScan);

        lv_obj_t* nav = lv_obj_create(body);
        lv_obj_remove_style_all(nav);
        lv_obj_set_width(nav, lv_pct(100));
        lv_obj_set_height(nav, kBtnH + 4);
        lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_margin_bottom(nav, 4, 0);
        const lv_coord_t half = (LV_HOR_RES - 40) / 2;
        MakeBtn(nav, "上一页", OnPrevPage, half);
        MakeBtn(nav, "下一页", OnNextPage, half);

        s_list = lv_obj_create(body);
        lv_obj_remove_style_all(s_list);
        lv_obj_set_width(s_list, lv_pct(100));
        lv_obj_set_flex_grow(s_list, 1);
        lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    }

    VkKey_AttachScreen(scr, kScreenId, VkKeyScreenDesc{AppWifiTest::Create});
    return scr;
}

lv_obj_t* AppWifiTest::CreatePassword() {
    ESP_LOGI(TAG, "create wifi password ssid=%s", s_selected_ssid.c_str());
    ScreenSetIsHome(false);
    s_pwd_alive.store(true);
    s_pwd_busy.store(false);
    s_password.clear();
    s_shift = false;

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_pwd_scr = scr;
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, OnPwdDeleted, LV_EVENT_DELETE, nullptr);

    EpdStatusBar bar = ScreenCreateStatusBar(scr);
    if (bar.status_label != nullptr) {
    }

    lv_obj_t* body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, LV_VER_RES - bar.height);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, bar.height);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(body, 10, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    char ssid_line[128];
    std::snprintf(ssid_line, sizeof(ssid_line), "网络: %s",
                  s_selected_ssid.empty() ? "(未选择)" : s_selected_ssid.c_str());
    lv_obj_t* ssid_lbl = lv_label_create(body);
    lv_label_set_text(ssid_lbl, ssid_line);
    lv_obj_set_width(ssid_lbl, lv_pct(100));
    lv_obj_set_style_text_align(ssid_lbl, LV_TEXT_ALIGN_CENTER, 0);

    s_pwd_label = lv_label_create(body);
    lv_obj_set_width(s_pwd_label, lv_pct(100));
    lv_obj_set_style_text_align(s_pwd_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_bottom(s_pwd_label, 4, 0);
    UpdatePasswordLabel();

    s_pwd_status = lv_label_create(body);
    lv_obj_set_width(s_pwd_status, lv_pct(100));
    lv_obj_set_style_text_align(s_pwd_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_bottom(s_pwd_status, 6, 0);
    lv_label_set_text(s_pwd_status, "输入密码后点连接");

    // 软键盘：数字 + 字母三行 + 功能键
    AddCharRow(body, "1234567890");
    AddCharRow(body, "qwertyuiop");
    AddCharRow(body, "asdfghjkl");
    AddCharRow(body, "zxcvbnm");

    lv_obj_t* fn = lv_obj_create(body);
    lv_obj_remove_style_all(fn);
    lv_obj_set_width(fn, lv_pct(100));
    lv_obj_set_height(fn, 40);
    lv_obj_set_flex_flow(fn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(fn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_margin_bottom(fn, 8, 0);
    AddKeyBtn(fn, "大小写", OnKeyShift, nullptr, 72);
    AddKeyBtn(fn, "退格", OnKeyBackspace, nullptr, 64);
    AddKeyBtn(fn, "清空", OnKeyClear, nullptr, 64);
    AddKeyBtn(fn, "空格", OnKeyChar, reinterpret_cast<void*>(static_cast<intptr_t>(' ')), 64);

    lv_obj_t* actions = lv_obj_create(body);
    lv_obj_remove_style_all(actions);
    lv_obj_set_width(actions, lv_pct(100));
    lv_obj_set_height(actions, kBtnH + 4);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    const lv_coord_t half = (LV_HOR_RES - 40) / 2;
    MakeBtn(actions, "上一页", OnPasswordBack, half);
    MakeBtn(actions, "连接", OnConnectPassword, half);

    VkKey_AttachScreen(scr, kPasswordScreenId, VkKeyScreenDesc{AppWifiTest::CreatePassword});
    return scr;
}
