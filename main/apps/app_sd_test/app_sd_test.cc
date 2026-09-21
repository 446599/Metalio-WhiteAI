#include "app_sd_test.h"

#include "hal/hal.h"
#include "haptic_feedback.h"
#include "screen_common.h"
#include "vk_key_handler.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "fontpack_lvgl.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr const char* TAG = "AppSdTest";
constexpr const char* kScreenId = "app_sd_test";
constexpr lv_coord_t kBtnH = 56;
constexpr lv_coord_t kBorderW = 2;

lv_obj_t* s_scr = nullptr;
lv_obj_t* s_status = nullptr;
std::atomic<bool> s_busy{false};
std::atomic<bool> s_alive{false};

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

void RefreshMountStatus() {
    auto& hal = GetHAL();
    char buf[160];
    if (hal.IsSdMounted()) {
        std::snprintf(buf, sizeof(buf), "已挂载\n挂载点: %s", hal.GetSdMountPoint());
    } else {
        std::snprintf(buf, sizeof(buf), "未挂载\n挂载点: %s", hal.GetSdMountPoint());
    }
    SetStatus(buf);
}

void RemountTask(void* /*arg*/) {
    PostStatus("重新挂载中…");
    const bool ok = GetHAL().RemountSd();
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s", ok ? "重新挂载成功" : "重新挂载失败");
    PostStatus(buf);
    s_busy.store(false);
    vTaskDelete(nullptr);
}

void OnRemount(lv_event_t* /*e*/) {
    if (s_busy.exchange(true)) {
        return;
    }
    if (xTaskCreatePinnedToCore(RemountTask, "sd_remount", 6144, nullptr, 5, nullptr, 0) !=
        pdPASS) {
        PostStatus("创建任务失败");
        s_busy.store(false);
    }
}

void OnRefresh(lv_event_t* /*e*/) {
    RefreshMountStatus();
}

void OnDeleted(lv_event_t* e) {
    if (lv_event_get_target(e) != s_scr) {
        return;
    }
    s_alive.store(false);
    s_scr = nullptr;
    s_status = nullptr;
}

}  // namespace

lv_obj_t* AppSdTest::Create() {
    ESP_LOGI(TAG, "create sd test");
    ScreenSetIsHome(false);
    s_alive.store(true);
    s_busy.store(false);

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
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_bottom(s_status, 16, 0);
    RefreshMountStatus();

    MakeBtn(body, "刷新挂载状态", OnRefresh);
    MakeBtn(body, "重新挂载", OnRemount);

    VkKey_AttachScreen(scr, kScreenId, VkKeyScreenDesc{AppSdTest::Create});
    return scr;
}
