#include "app_button_test.h"

#include "hal/hal.h"
#include "screen_common.h"
#include "vk_key_handler.h"

#include <esp_log.h>
#include "fontpack_lvgl.h"

#include <cstdio>

namespace {

constexpr const char* TAG = "AppButtonTest";
constexpr const char* kScreenId = "app_button_test";
constexpr uint32_t kPollMs = 100;
constexpr int kButtonCount = 4;

struct ButtonRow {
    HalButtonId id;
    const char* name;
    lv_obj_t* label = nullptr;
    bool pressed = false;
    bool seen = false;
};

lv_obj_t* s_scr = nullptr;
lv_timer_t* s_timer = nullptr;
ButtonRow s_rows[kButtonCount] = {
    {HalButtonId::Boot, "BOOT (GPIO0)", nullptr, false, false},
    {HalButtonId::Power, "POWER (GPIO3)", nullptr, false, false},
    {HalButtonId::VolUp, "音量+ (P1.0)", nullptr, false, false},
    {HalButtonId::VolDown, "音量- (P0.7)", nullptr, false, false},
};

void UpdateRowLabel(ButtonRow& row) {
    if (row.label == nullptr || !lv_obj_is_valid(row.label)) {
        return;
    }
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s\n%s  %s", row.name, row.pressed ? "按下" : "松开",
                  row.seen ? "[已测到]" : "[未测到]");
    lv_label_set_text(row.label, buf);
}

void PollButtons() {
    for (int i = 0; i < kButtonCount; ++i) {
        ButtonRow& row = s_rows[i];
        const bool now = GetHAL().IsButtonPressed(row.id);
        if (now && !row.pressed) {
            row.seen = true;
        }
        row.pressed = now;
        UpdateRowLabel(row);
    }
}

void OnTimer(lv_timer_t* /*t*/) {
    PollButtons();
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
    for (int i = 0; i < kButtonCount; ++i) {
        s_rows[i].label = nullptr;
        s_rows[i].pressed = false;
        s_rows[i].seen = false;
    }
}

}  // namespace

lv_obj_t* AppButtonTest::Create() {
    ESP_LOGI(TAG, "create button test");
    ScreenSetIsHome(false);

    for (int i = 0; i < kButtonCount; ++i) {
        s_rows[i].pressed = false;
        s_rows[i].seen = false;
        s_rows[i].label = nullptr;
    }

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

    lv_obj_t* hint = lv_label_create(body);
    lv_label_set_text(hint, "依次按下四个物理键\nPOWER 短按验证即可，长按会关机");
    lv_obj_set_width(hint, lv_pct(100));
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_bottom(hint, 14, 0);

    for (int i = 0; i < kButtonCount; ++i) {
        lv_obj_t* box = lv_obj_create(body);
        lv_obj_remove_style_all(box);
        lv_obj_set_width(box, lv_pct(100));
        lv_obj_set_height(box, 72);
        lv_obj_set_style_bg_color(box, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(box, lv_color_black(), 0);
        lv_obj_set_style_border_width(box, 2, 0);
        lv_obj_set_style_radius(box, 8, 0);
        lv_obj_set_style_pad_all(box, 8, 0);
        lv_obj_set_style_margin_bottom(box, 10, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

        s_rows[i].label = lv_label_create(box);
        lv_obj_set_width(s_rows[i].label, lv_pct(100));
        lv_obj_set_style_text_align(s_rows[i].label, LV_TEXT_ALIGN_LEFT, 0);
        UpdateRowLabel(s_rows[i]);
    }

    PollButtons();
    s_timer = lv_timer_create(OnTimer, kPollMs, nullptr);

    VkKey_AttachScreen(scr, kScreenId, VkKeyScreenDesc{AppButtonTest::Create});
    return scr;
}
