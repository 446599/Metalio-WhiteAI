#include "screen_common.h"

#include "apps/app_launcher/app_launcher.h"
#include "assets/lang_config.h"
#include "lv_adapter_display.h"
#include "vk_key_handler.h"

#include <cstring>

#include <esp_log.h>
#include "board.h"
#include "fontpack_lvgl.h"
#include "eego_battery_icons.h"

LV_FONT_DECLARE(font_awesome_30_1);

namespace {

constexpr const char* TAG = "ScreenCommon";
constexpr const char* kHomeScreen = "home";
constexpr int kMaxBack = 8;
// 数字时分在 CJK 行盒里偏上，下移后与两侧图标视觉居中。
constexpr lv_coord_t kStatusTextNudgeY = 4;

bool s_on_home = true;
ScreenFactory s_back_stack[kMaxBack] = {};
int s_back_depth = 0;

LVAdapterDisplay* GetAdapterDisplay() {
    if (auto* self = LVAdapterDisplay::Instance()) {
        return self;
    }
    return static_cast<LVAdapterDisplay*>(Board::GetInstance().GetDisplay());
}

void ClearBackStack() {
    s_back_depth = 0;
    for (int i = 0; i < kMaxBack; ++i) {
        s_back_stack[i] = nullptr;
    }
}

void PushBackFactory(ScreenFactory factory) {
    if (factory == nullptr) {
        return;
    }
    if (s_back_depth < kMaxBack) {
        s_back_stack[s_back_depth++] = factory;
    } else {
        for (int i = 1; i < kMaxBack; ++i) {
            s_back_stack[i - 1] = s_back_stack[i];
        }
        s_back_stack[kMaxBack - 1] = factory;
        ESP_LOGW(TAG, "back stack full, drop oldest");
    }
}

ScreenFactory PopBackFactory() {
    if (s_back_depth <= 0) {
        return nullptr;
    }
    --s_back_depth;
    ScreenFactory f = s_back_stack[s_back_depth];
    s_back_stack[s_back_depth] = nullptr;
    return f;
}

void GoHomeAsync(void* /*user_data*/) {
    if (s_on_home) {
        ESP_LOGI(TAG, "home ignored (already home)");
        return;
    }
    ESP_LOGI(TAG, "-> home (clear back stack depth was %d)", s_back_depth);
    ScreenGoHome();
}

void NavigateBackAsync(void* /*user_data*/) {
    ScreenFactory prev = PopBackFactory();
    if (prev != nullptr) {
        ESP_LOGI(TAG, "navigate back -> recreate (stack left=%d)", s_back_depth);
        s_on_home = false;
        ScreenLoadReplace(prev());
        return;
    }
    ESP_LOGI(TAG, "navigate back -> home (empty stack)");
    ScreenGoHome();
}

}  // namespace

EpdStatusBar ScreenCreateStatusBar(lv_obj_t* scr) {
    EpdStatusBar out;

    const lv_font_t* ui_font = fontpack_lv_font_ui();
    const lv_font_t* clock_font = fontpack_lv_font_get(40, 2);
    if (clock_font == nullptr) clock_font = ui_font;
    const lv_coord_t status_line_h = clock_font != nullptr ? clock_font->line_height : 44;
    const lv_coord_t header_pad_v = 8;
    out.height = status_line_h + header_pad_v * 2;

    const bool is_home = ScreenIsHome();
    out.bar = lv_obj_create(scr);
    lv_obj_set_size(out.bar, LV_HOR_RES, out.height);
    lv_obj_set_style_radius(out.bar, 0, 0);
    lv_obj_set_style_bg_opa(out.bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(out.bar, 0, 0);
    lv_obj_set_style_pad_all(out.bar, 0, 0);
    lv_obj_set_style_pad_top(out.bar, header_pad_v, 0);
    lv_obj_set_style_pad_bottom(out.bar, header_pad_v, 0);
    lv_obj_set_style_pad_left(out.bar, 24, 0);
    lv_obj_set_style_pad_right(out.bar, 24, 0);
    lv_obj_set_flex_flow(out.bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(out.bar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(out.bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(out.bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(out.bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(out.bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* right_icons = lv_obj_create(out.bar);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(right_icons, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(right_icons, LV_OBJ_FLAG_SCROLLABLE);

    out.battery_percent_label = lv_label_create(right_icons);
    lv_label_set_text(out.battery_percent_label, "");
    lv_obj_set_style_text_font(out.battery_percent_label, clock_font, 0);
    lv_obj_set_style_text_color(out.battery_percent_label, lv_color_black(), 0);
    lv_obj_set_style_margin_left(out.battery_percent_label, 12, 0);
    lv_obj_clear_flag(out.battery_percent_label, LV_OBJ_FLAG_CLICKABLE);

    // EegoRead Tabler battery icon (the SVG is kept under main/assets/eego/tabler).
    // Use the embedded raster descriptor so the icon does not depend on a filesystem.
    out.battery_label = lv_image_create(right_icons);
    lv_image_set_src(out.battery_label, &eego_battery);
    lv_image_set_scale(out.battery_label, 384);
    lv_obj_set_style_margin_left(out.battery_label, 8, 0);
    lv_obj_clear_flag(out.battery_label, LV_OBJ_FLAG_CLICKABLE);

    out.overlay = lv_obj_create(scr);
    lv_obj_set_size(out.overlay, LV_HOR_RES, out.height);
    lv_obj_set_style_radius(out.overlay, 0, 0);
    lv_obj_set_style_bg_opa(out.overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(out.overlay, 0, 0);
    lv_obj_set_style_pad_all(out.overlay, 0, 0);
    lv_obj_set_style_pad_top(out.overlay, header_pad_v, 0);
    lv_obj_set_style_pad_bottom(out.overlay, header_pad_v, 0);
    lv_obj_set_scrollbar_mode(out.overlay, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(out.overlay, LV_LAYOUT_NONE, 0);
    lv_obj_align(out.overlay, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(out.overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(out.overlay, LV_OBJ_FLAG_SCROLLABLE);

    // 顶栏只保留时间和右侧电池；返回由硬件/虚拟键处理。
    out.notification_label = nullptr;

    out.status_label = lv_label_create(out.overlay);
    lv_obj_set_width(out.status_label, 220);
    lv_obj_set_style_text_align(out.status_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_font(out.status_label, clock_font, 0);
    lv_obj_set_style_text_color(out.status_label, lv_color_black(), 0);
    lv_label_set_text(out.status_label, "");
    lv_obj_align(out.status_label, LV_ALIGN_LEFT_MID, 24, 0);
    lv_obj_clear_flag(out.status_label, LV_OBJ_FLAG_CLICKABLE);

    out.low_battery_popup = lv_obj_create(scr);
    lv_obj_set_scrollbar_mode(out.low_battery_popup, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(out.low_battery_popup, LV_HOR_RES * 9 / 10, status_line_h * 2);
    lv_obj_align(out.low_battery_popup, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(out.low_battery_popup, lv_color_black(), 0);
    lv_obj_set_style_radius(out.low_battery_popup, 0, 0);
    lv_obj_t* low_battery_label = lv_label_create(out.low_battery_popup);
    lv_label_set_text(low_battery_label, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_font(low_battery_label, ui_font, 0);
    lv_obj_set_style_text_color(low_battery_label, lv_color_white(), 0);
    lv_obj_center(low_battery_label);
    lv_obj_add_flag(out.low_battery_popup, LV_OBJ_FLAG_HIDDEN);

    if (auto* disp = GetAdapterDisplay()) {
        disp->BindStatusWidgets(nullptr, nullptr, out.battery_percent_label,
                                out.battery_label, out.status_label, out.notification_label,
                                out.low_battery_popup);
    }
    return out;
}

void ScreenSetIsHome(bool is_home) {
    s_on_home = is_home;
}

bool ScreenIsHome() {
    return s_on_home;
}

void ScreenLoadReplace(lv_obj_t* new_scr) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_screen_load(new_scr);
    if (old_scr != nullptr && old_scr != new_scr) {
        lv_obj_delete_async(old_scr);
    }
    // 清掉跨页残留的 press/gesture，避免新页首击被当成旧拖拽吞掉
    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev != nullptr;
         indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_reset(indev, nullptr);
        }
    }
}

void ScreenNavigateTo(lv_obj_t* (*create)()) {
    if (create == nullptr) {
        return;
    }
    const char* cur = VkKey_ActiveScreen();
    if (cur != nullptr && std::strcmp(cur, kHomeScreen) != 0 &&
        std::strcmp(cur, "none") != 0) {
        if (ScreenFactory factory = VkKey_GetScreenFactory(cur)) {
            PushBackFactory(factory);
            ESP_LOGI(TAG, "navigate to: push back factory for %s (depth=%d)", cur, s_back_depth);
        }
    }
    s_on_home = false;
    ScreenLoadReplace(create());
}

void ScreenNavigateBack() {
    lv_async_call(NavigateBackAsync, nullptr);
}

void ScreenGoHome() {
    ClearBackStack();
    s_on_home = true;
    ScreenLoadReplace(AppLauncher::Create());
}

void ScreenRequestHome() {
    lv_async_call(GoHomeAsync, nullptr);
}

void ScreenRequestBack() {
    ScreenRequestHome();
}

lv_obj_t* ScreenCreatePlaceholder(const char* screen_id, const char* title) {
    s_on_home = false;

    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);

    EpdStatusBar status = ScreenCreateStatusBar(scr);
    if (status.status_label != nullptr) {
        lv_label_set_text(status.status_label, "");
        lv_obj_add_flag(status.status_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (status.notification_label != nullptr) {
        lv_obj_add_flag(status.notification_label, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t* body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, LV_VER_RES - status.height);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, status.height);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* label = lv_label_create(body);
    lv_label_set_text(label, title != nullptr ? title : "");
    lv_obj_set_style_text_font(label, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);

    if (screen_id != nullptr) {
        VkKey_AttachScreen(scr, screen_id);
    }
    return scr;
}

namespace {

// I1 无真灰阶：弹窗外遮罩用大颗粒网点（16x16 平铺，3x3 黑块）。
constexpr int kDotBackdropW = 16;
constexpr int kDotBackdropH = 16;
constexpr int kDotSize = 3;
constexpr int kDotPitch = 8;
uint8_t s_dot_backdrop_l8[kDotBackdropW * kDotBackdropH];
lv_image_dsc_t s_dot_backdrop_img;
bool s_dot_backdrop_ready = false;

const lv_image_dsc_t* DotBackdropImg() {
    if (!s_dot_backdrop_ready) {
        for (int i = 0; i < kDotBackdropW * kDotBackdropH; ++i) {
            s_dot_backdrop_l8[i] = 0xFF;
        }
        for (int gy = 0; gy < kDotBackdropH; gy += kDotPitch) {
            const int x0 = ((gy / kDotPitch) & 1) ? (kDotPitch / 2) : 2;
            for (int gx = x0; gx < kDotBackdropW; gx += kDotPitch) {
                for (int dy = 0; dy < kDotSize; ++dy) {
                    for (int dx = 0; dx < kDotSize; ++dx) {
                        const int x = (gx + dx) % kDotBackdropW;
                        const int y = (gy + 2 + dy) % kDotBackdropH;
                        s_dot_backdrop_l8[y * kDotBackdropW + x] = 0x00;
                    }
                }
            }
        }
        s_dot_backdrop_img.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_dot_backdrop_img.header.cf = LV_COLOR_FORMAT_L8;
        s_dot_backdrop_img.header.flags = 0;
        s_dot_backdrop_img.header.w = kDotBackdropW;
        s_dot_backdrop_img.header.h = kDotBackdropH;
        s_dot_backdrop_img.header.stride = kDotBackdropW;
        s_dot_backdrop_img.data_size = sizeof(s_dot_backdrop_l8);
        s_dot_backdrop_img.data = s_dot_backdrop_l8;
        s_dot_backdrop_ready = true;
    }
    return &s_dot_backdrop_img;
}

}  // namespace

void ScreenApplyDotBackdrop(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_image_src(obj, DotBackdropImg(), 0);
    lv_obj_set_style_bg_image_tiled(obj, true, 0);
}
