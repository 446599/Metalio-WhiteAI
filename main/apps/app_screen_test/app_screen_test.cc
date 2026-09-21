#include "app_screen_test.h"

#include "haptic_feedback.h"
#include "lv_adapter_display.h"
#include "screen_common.h"
#include "vk_key_handler.h"

#include <esp_log.h>
#include "fontpack_lvgl.h"

namespace {

constexpr const char* TAG = "AppScreenTest";
constexpr const char* kScreenId = "app_screen_test";
constexpr int kStripeH = 40;
constexpr lv_coord_t kBtnH = 56;
constexpr lv_coord_t kBorderW = 2;

enum class Pattern : int {
    Black = 0,
    White = 1,
    Stripes = 2,
    Count = 3,
};

lv_obj_t* s_scr = nullptr;
lv_obj_t* s_hint = nullptr;
lv_obj_t* s_stripe_layer = nullptr;
lv_obj_t* s_btn_layer = nullptr;
Pattern s_pattern = Pattern::Black;

void ClearStatusBindings() {
    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->BindStatusWidgets(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    }
}

const char* PatternName(Pattern p) {
    switch (p) {
        case Pattern::Black:
            return "当前：全黑";
        case Pattern::White:
            return "当前：全白";
        case Pattern::Stripes:
            return "当前：黑白条纹";
        default:
            return "";
    }
}

void ClearStripes() {
    if (s_stripe_layer != nullptr && lv_obj_is_valid(s_stripe_layer)) {
        lv_obj_delete(s_stripe_layer);
    }
    s_stripe_layer = nullptr;
}

void BuildStripes() {
    ClearStripes();
    s_stripe_layer = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_stripe_layer);
    lv_obj_set_size(s_stripe_layer, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(s_stripe_layer, 0, 0);
    lv_obj_set_style_bg_opa(s_stripe_layer, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_stripe_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_stripe_layer, LV_OBJ_FLAG_SCROLLABLE);

    for (lv_coord_t y = 0; y < LV_VER_RES; y += kStripeH) {
        const bool black = ((y / kStripeH) & 1) == 0;
        lv_obj_t* row = lv_obj_create(s_stripe_layer);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_HOR_RES, kStripeH);
        lv_obj_set_pos(row, 0, y);
        lv_obj_set_style_bg_color(row, black ? lv_color_black() : lv_color_white(), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_move_background(s_stripe_layer);
}

void ApplyPattern() {
    if (s_scr == nullptr || !lv_obj_is_valid(s_scr)) {
        return;
    }
    ClearStripes();

    switch (s_pattern) {
        case Pattern::Black:
            lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
            break;
        case Pattern::White:
            lv_obj_set_style_bg_color(s_scr, lv_color_white(), 0);
            break;
        case Pattern::Stripes:
            lv_obj_set_style_bg_color(s_scr, lv_color_white(), 0);
            BuildStripes();
            break;
        default:
            break;
    }

    if (s_hint != nullptr && lv_obj_is_valid(s_hint)) {
        lv_label_set_text(s_hint, PatternName(s_pattern));
        lv_obj_set_style_text_color(s_hint, lv_color_white(), 0);
    }
    if (s_btn_layer != nullptr && lv_obj_is_valid(s_btn_layer)) {
        lv_obj_move_foreground(s_btn_layer);
    }
    ESP_LOGI(TAG, "pattern=%s", PatternName(s_pattern));
}

void AdvancePattern() {
    s_pattern = static_cast<Pattern>((static_cast<int>(s_pattern) + 1) %
                                     static_cast<int>(Pattern::Count));
    ApplyPattern();
    if (s_scr != nullptr && lv_obj_is_valid(s_scr)) {
        lv_obj_invalidate(s_scr);
    }
}

void OnFullRefresh(lv_event_t* /*e*/) {
    HapticPulseIfEnabled();
    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->RequestNextFullRefresh();
    }
    AdvancePattern();
    ESP_LOGI(TAG, "next pattern via FULL refresh");
}

void OnPartialRefresh(lv_event_t* /*e*/) {
    HapticPulseIfEnabled();
    AdvancePattern();
    ESP_LOGI(TAG, "next pattern via PARTIAL refresh");
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
    s_scr = nullptr;
    s_hint = nullptr;
    s_stripe_layer = nullptr;
    s_btn_layer = nullptr;
    s_pattern = Pattern::Black;
}

}  // namespace

lv_obj_t* AppScreenTest::Create() {
    ESP_LOGI(TAG, "create screen test");
    ScreenSetIsHome(false);
    ClearStatusBindings();

    s_pattern = Pattern::Black;
    s_stripe_layer = nullptr;
    s_btn_layer = nullptr;

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_scr = scr;
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, fontpack_lv_font_ui(), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, OnDeleted, LV_EVENT_DELETE, nullptr);

    s_btn_layer = lv_obj_create(scr);
    lv_obj_remove_style_all(s_btn_layer);
    lv_obj_set_size(s_btn_layer, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_align(s_btn_layer, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_btn_layer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_btn_layer, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_btn_layer, 16, 0);
    lv_obj_set_flex_flow(s_btn_layer, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_btn_layer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* tip = lv_label_create(s_btn_layer);
    lv_label_set_text(tip, "全刷=整屏黑闪清残影；局刷较快");
    lv_obj_set_width(tip, lv_pct(100));
    lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(tip, lv_color_white(), 0);
    lv_obj_set_style_margin_bottom(tip, 8, 0);

    s_hint = lv_label_create(s_btn_layer);
    lv_obj_set_width(s_hint, lv_pct(100));
    lv_obj_set_style_text_font(s_hint, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_white(), 0);
    lv_obj_set_style_margin_bottom(s_hint, 10, 0);
    lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_CLICKABLE);

    MakeBtn(s_btn_layer, "下一图案·全刷", OnFullRefresh);
    MakeBtn(s_btn_layer, "下一图案·局刷", OnPartialRefresh);

    ApplyPattern();

    VkKey_AttachScreen(scr, kScreenId, VkKeyScreenDesc{AppScreenTest::Create});
    return scr;
}
