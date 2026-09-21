#include "app_cell_test.h"

#include "hal/hal.h"
#include "haptic_feedback.h"
#include "screen_common.h"
#include "vk_key_handler.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "fontpack_lvgl.h"

#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr const char* TAG = "AppCellTest";
constexpr const char* kScreenId = "app_cell_test";
constexpr lv_coord_t kBtnH = 56;
constexpr lv_coord_t kBorderW = 2;

lv_obj_t* s_scr = nullptr;
lv_obj_t* s_status = nullptr;
lv_obj_t* s_start_btn = nullptr;
lv_obj_t* s_ping_btn = nullptr;
std::atomic<bool> s_busy{false};
std::atomic<bool> s_alive{false};
std::atomic<bool> s_started{false};

void SetStatus(const char* text) {
    if (s_status != nullptr && lv_obj_is_valid(s_status) && text != nullptr) {
        lv_label_set_text(s_status, text);
    }
}

void StatusAsync(void* p) {
    auto* msg = static_cast<char*>(p);
    if (s_alive.load() && msg != nullptr) {
        SetStatus(msg);
    }
    free(msg);
}

void PostStatus(const char* text) {
    char* copy = text != nullptr ? strdup(text) : nullptr;
    if (copy != nullptr) {
        lv_async_call(StatusAsync, copy);
    }
}

void HideActionBtnsAsync(void* /*p*/) {
    if (!s_alive.load()) {
        return;
    }
    if (s_start_btn != nullptr && lv_obj_is_valid(s_start_btn)) {
        lv_obj_add_flag(s_start_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_ping_btn != nullptr && lv_obj_is_valid(s_ping_btn)) {
        lv_obj_add_flag(s_ping_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t* MakeBtn(lv_obj_t* parent, const char* title, lv_event_cb_t cb) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, kBtnH);
    lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, kBorderW, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_margin_bottom(btn, 10, 0);
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

void StartTask(void* /*arg*/) {
    PostStatus("启动模组中（可能需数十秒）…");
    const bool ok = GetHAL().CellStart();
    s_started.store(ok);
    if (!s_alive.load()) {
        s_busy.store(false);
        vTaskDelete(nullptr);
        return;
    }

    std::string at;
    const bool at_ok = GetHAL().CellProbeAt(at);
    const int csq = GetHAL().CellCsq();
    const std::string reg = GetHAL().CellRegistrationJson();

    char buf[384];
    std::snprintf(buf, sizeof(buf),
                  "%s\nAT:%s\nCSQ:%d\n注册:%s", ok ? "模组已启动" : "启动结束（请看下方）",
                  at_ok ? "OK" : "失败", csq, reg.c_str());
    PostStatus(buf);
    s_busy.store(false);
    vTaskDelete(nullptr);
}

void PingTask(void* /*arg*/) {
    PostStatus("Ping 中…");
    if (!s_started.load()) {
        // 允许未点启动时先起模组
        PostStatus("先启动模组…");
        (void)GetHAL().CellStart();
        s_started.store(true);
    }
    std::string detail;
    const bool ok = GetHAL().CellPing(detail);
    if (!s_alive.load()) {
        s_busy.store(false);
        vTaskDelete(nullptr);
        return;
    }
    // 截断过长 URC，避免撑爆标签
    if (detail.size() > 220) {
        detail.resize(220);
        detail += "…";
    }
    char buf[320];
    std::snprintf(buf, sizeof(buf), "%s\n%s", ok ? "Ping 成功" : "Ping 失败", detail.c_str());
    PostStatus(buf);
    if (ok) {
        lv_async_call(HideActionBtnsAsync, nullptr);
    }
    s_busy.store(false);
    vTaskDelete(nullptr);
}

void OnStart(lv_event_t* /*e*/) {
    if (s_busy.exchange(true)) {
        return;
    }
    if (xTaskCreatePinnedToCore(StartTask, "cell_start", 12288, nullptr, 5, nullptr, 0) !=
        pdPASS) {
        PostStatus("创建任务失败");
        s_busy.store(false);
    }
}

void OnPing(lv_event_t* /*e*/) {
    if (s_busy.exchange(true)) {
        return;
    }
    if (xTaskCreatePinnedToCore(PingTask, "cell_ping", 12288, nullptr, 5, nullptr, 0) != pdPASS) {
        PostStatus("创建任务失败");
        s_busy.store(false);
    }
}

void OnSwitchCell(lv_event_t* /*e*/) {
    PostStatus("正在切换到 4G 并重启…");
    GetHAL().RequestSwitchNetwork(NetworkType::ML307);
}

void OnDeleted(lv_event_t* e) {
    if (lv_event_get_target(e) != s_scr) {
        return;
    }
    s_alive.store(false);
    s_scr = nullptr;
    s_status = nullptr;
    s_start_btn = nullptr;
    s_ping_btn = nullptr;
}

}  // namespace

lv_obj_t* AppCellTest::Create() {
    ESP_LOGI(TAG, "create cell test");
    ScreenSetIsHome(false);
    s_alive.store(true);
    s_busy.store(false);
    s_started.store(false);
    s_start_btn = nullptr;
    s_ping_btn = nullptr;

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
    lv_obj_set_style_pad_all(body, 16, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    s_status = lv_label_create(body);
    lv_obj_set_width(s_status, lv_pct(100));
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_margin_bottom(s_status, 12, 0);

    if (!GetHAL().IsCellMode()) {
        lv_label_set_text(s_status, "当前是 WiFi 模式\n请切换到 4G 后重试");
        MakeBtn(body, "切换到 4G 并重启", OnSwitchCell);
    } else {
        lv_label_set_text(s_status, "待命");
        s_start_btn = MakeBtn(body, "启动模组", OnStart);
        s_ping_btn = MakeBtn(body, "Ping (baidu)", OnPing);
    }

    VkKey_AttachScreen(scr, kScreenId, VkKeyScreenDesc{AppCellTest::Create});
    return scr;
}
