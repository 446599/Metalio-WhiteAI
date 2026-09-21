#include "app_motor_test.h"

#include "hal/hal.h"
#include "screen_common.h"
#include "vk_key_handler.h"

#include <esp_log.h>
#include "fontpack_lvgl.h"

namespace {

constexpr const char* TAG = "AppMotorTest";
constexpr const char* kScreenId = "app_motor_test";
constexpr lv_coord_t kBtnH = 72;
constexpr lv_coord_t kBorderW = 2;

lv_obj_t* s_scr = nullptr;
lv_obj_t* s_btn_lbl = nullptr;
lv_obj_t* s_desc = nullptr;
bool s_running = false;

void UpdateUi() {
    if (s_btn_lbl != nullptr) {
        lv_label_set_text(s_btn_lbl, s_running ? "停止震动" : "启动震动");
    }
    if (s_desc != nullptr) {
        lv_label_set_text(s_desc,
                          s_running ? "马达持续震动中\n再点一次停止"
                                    : "点击下方按钮在停止 / 持续震动间切换");
    }
}

void OnToggleClicked(lv_event_t* /*e*/) {
    s_running = !s_running;
    ESP_LOGI(TAG, "motor %s", s_running ? "on" : "off");
    GetHAL().SetMotor(s_running);
    UpdateUi();
}

void OnDeleted(lv_event_t* e) {
    if (lv_event_get_target(e) != s_scr) {
        return;
    }
    GetHAL().SetMotor(false);
    s_running = false;
    s_scr = nullptr;
    s_btn_lbl = nullptr;
    s_desc = nullptr;
}

}  // namespace

lv_obj_t* AppMotorTest::Create() {
    ESP_LOGI(TAG, "create motor test");
    ScreenSetIsHome(false);
    s_running = false;

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_scr = scr;
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, OnDeleted, LV_EVENT_DELETE, nullptr);

    EpdStatusBar status = ScreenCreateStatusBar(scr);
    if (status.status_label != nullptr) {
    }

    lv_obj_t* body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, LV_VER_RES - status.height);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, status.height);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(body, 24, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* desc = lv_label_create(body);
    s_desc = desc;
    lv_obj_set_style_text_align(desc, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(desc, lv_pct(100));
    lv_obj_set_style_margin_bottom(desc, 32, 0);

    lv_obj_t* btn = lv_obj_create(body);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, LV_HOR_RES - 64, kBtnH);
    lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, kBorderW, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    // 不挂 HapticAttachClick，避免与测试持续震动叠震；仅手动 SetMotor。
    lv_obj_add_event_cb(btn, OnToggleClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* lbl = lv_label_create(btn);
    s_btn_lbl = lbl;
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, fontpack_lv_font_ui(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    UpdateUi();

    VkKey_AttachScreen(scr, kScreenId, VkKeyScreenDesc{AppMotorTest::Create});
    return scr;
}
