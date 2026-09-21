#include "app_battery_test.h"

#include "hal/hal.h"
#include "haptic_feedback.h"
#include "screen_common.h"
#include "vk_key_handler.h"

#include <esp_log.h>
#include "fontpack_lvgl.h"

#include <cstdio>

namespace {

constexpr const char* TAG = "AppBatteryTest";
constexpr const char* kScreenId = "app_battery_test";
constexpr lv_coord_t kBtnH = 56;
constexpr lv_coord_t kBorderW = 2;
constexpr uint32_t kRefreshMs = 1000;

lv_obj_t* s_scr = nullptr;
lv_obj_t* s_status = nullptr;
lv_timer_t* s_timer = nullptr;

void RefreshInfo() {
    if (s_status == nullptr || !lv_obj_is_valid(s_status)) {
        return;
    }
    HalBatteryInfo info;
    if (!GetHAL().GetBatteryInfo(info)) {
        lv_label_set_text(s_status, "电池读取失败\n（电量计可能未就绪）");
        return;
    }
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "电量: %d%%\n"
                  "电压: %u mV\n"
                  "电流: %d mA\n"
                  "充电: %s\n"
                  "放电: %s\n"
                  "充电IC: %s",
                  info.level, static_cast<unsigned>(info.voltage_mv),
                  static_cast<int>(info.current_ma), info.charging ? "是" : "否",
                  info.discharging ? "是" : "否",
                  info.chrg_stat != nullptr ? info.chrg_stat : "N/A");
    lv_label_set_text(s_status, buf);
}

void OnTimer(lv_timer_t* /*t*/) {
    RefreshInfo();
}

void OnRefresh(lv_event_t* /*e*/) {
    RefreshInfo();
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

void OnDeleted(lv_event_t* e) {
    if (lv_event_get_target(e) != s_scr) {
        return;
    }
    if (s_timer != nullptr) {
        lv_timer_delete(s_timer);
        s_timer = nullptr;
    }
    s_scr = nullptr;
    s_status = nullptr;
}

}  // namespace

lv_obj_t* AppBatteryTest::Create() {
    ESP_LOGI(TAG, "create battery test");
    ScreenSetIsHome(false);

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
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    s_status = lv_label_create(body);
    lv_obj_set_width(s_status, lv_pct(100));
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_margin_bottom(s_status, 16, 0);
    lv_label_set_text(s_status, "读取中…");

    MakeBtn(body, "立即刷新", OnRefresh);

    RefreshInfo();
    s_timer = lv_timer_create(OnTimer, kRefreshMs, nullptr);

    VkKey_AttachScreen(scr, kScreenId, VkKeyScreenDesc{AppBatteryTest::Create});
    return scr;
}
