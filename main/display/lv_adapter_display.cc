#include "lv_adapter_display.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#include <esp_check.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_log.h>
#include <esp_mmap_assets.h>
#include <font_awesome.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "application.h"
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "board.h"
#include "esp_lv_adapter.h"
#include "esp_lv_fs.h"
#include "haptic_feedback.h"
#include "apps/app_launcher/app_launcher.h"
#include "touch_missing_screen/touch_missing_screen.h"
#include "mmap_generate_resources.h"
#include "SdCardManager.hpp"

#include "esp_lcd_panel_ssd1677.h"
#include "esp_lcd_ssd1677_commands.h"
#include "fontpack_lvgl.h"
#include "eego_battery_icons.h"

#include <cerrno>
#include <sys/stat.h>

#ifndef A2UI_I1_MAGIC
#define A2UI_I1_MAGIC 0x31493241u /* 'A2I1' little-endian */
#endif

static const char* TAG = "LVAdapterDisplay";

LVAdapterDisplay* LVAdapterDisplay::instance_ = nullptr;

namespace {

#pragma pack(push, 1)
struct A2i1Header {
    uint32_t magic;
    uint16_t width;
    uint16_t height;
    uint16_t stride;
    uint16_t reserved;
};
#pragma pack(pop)

bool ParseA2i1(const uint8_t* buf, size_t len, uint16_t* w, uint16_t* h, uint16_t* stride,
               const uint8_t** payload) {
    if (buf == nullptr || len < sizeof(A2i1Header) + 8) {
        return false;
    }
    const auto* hdr = reinterpret_cast<const A2i1Header*>(buf);
    if (hdr->magic != A2UI_I1_MAGIC || hdr->width == 0 || hdr->height == 0 || hdr->stride == 0) {
        return false;
    }
    const size_t need =
        sizeof(A2i1Header) + 8u + static_cast<size_t>(hdr->stride) * static_cast<size_t>(hdr->height);
    if (len < need) {
        return false;
    }
    if (w) {
        *w = hdr->width;
    }
    if (h) {
        *h = hdr->height;
    }
    if (stride) {
        *stride = hdr->stride;
    }
    if (payload) {
        *payload = buf + sizeof(A2i1Header); /* palette (8) + bitmap */
    }
    return true;
}

/** 硬件测试固件关机图仅用内置 mmap；保留钩子便于日后接 SD。 */
bool TryLoadShutdownA2i1FromSd(uint8_t** out_buf, size_t* out_len) {
    if (out_buf == nullptr || out_len == nullptr) {
        return false;
    }
    *out_buf = nullptr;
    *out_len = 0;
    // 硬件测试固件：不依赖壁纸 NVS，关机画仅用内置 mmap。
    (void)SdCardManager::GetInstance();
    return false;
}

constexpr size_t kMaxTouchVirtualKeys = 8;
TouchVirtualKey s_vk_keys[kMaxTouchVirtualKeys];
size_t s_vk_count = 0;
TouchVirtualKeyEventCb s_vk_cb = nullptr;
void* s_vk_user = nullptr;

// long_press_ms==0：按下即 Click（旧行为），仅用 was_down 防抖。
bool s_vk_instant_was_down = false;

// long_press_ms>0：完整按住生命周期（无堆分配，无定时器句柄）。
struct TouchVkHoldState {
    const char* name = nullptr;
    uint16_t long_press_ms = 0;
    int64_t down_us = 0;
    bool long_emitted = false;
    bool long_consumed = false;
};
TouchVkHoldState s_vk_hold{};

const TouchVirtualKey* HitVirtualKey(int x, int y) {
    for (size_t i = 0; i < s_vk_count; ++i) {
        if (x == s_vk_keys[i].x && y == s_vk_keys[i].y) {
            return &s_vk_keys[i];
        }
    }
    return nullptr;
}

bool EmitVkEvent(const char* name, TouchVkEvent event) {
    if (s_vk_cb == nullptr || name == nullptr) {
        return false;
    }
    return s_vk_cb(name, event, s_vk_user);
}

void EndHoldPress(bool emit_click_if_unconsumed) {
    if (s_vk_hold.name == nullptr) {
        return;
    }
    const char* name = s_vk_hold.name;
    const bool emit_click = emit_click_if_unconsumed && !s_vk_hold.long_consumed;
    EmitVkEvent(name, TouchVkEvent::PressUp);
    if (emit_click) {
        EmitVkEvent(name, TouchVkEvent::Click);
    }
    s_vk_hold = {};
}

esp_err_t CustomTouchRead(esp_lcd_touch_handle_t tp, esp_lcd_touch_point_data_t* points, uint8_t* count,
                          uint8_t max_count, void* /*user_ctx*/) {
    if (tp == nullptr || points == nullptr || count == nullptr || max_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(esp_lcd_touch_read_data(tp), TAG, "touch read_data");
    ESP_RETURN_ON_ERROR(esp_lcd_touch_get_data(tp, points, count, max_count), TAG, "touch get_data");

    if (*count == 0) {
        s_vk_instant_was_down = false;
        EndHoldPress(true);
        return ESP_OK;
    }

    const int x = static_cast<int>(points[0].x);
    const int y = static_cast<int>(points[0].y);
    const TouchVirtualKey* hit = HitVirtualKey(x, y);
    if (hit == nullptr) {
        s_vk_instant_was_down = false;
        EndHoldPress(true);
        return ESP_OK;
    }

    // 虚拟键在屏外，不喂给 LVGL，避免误点 UI
    *count = 0;

    if (hit->long_press_ms == 0) {
        // 瞬时键与长按键互斥：若正按住长按键，先正常收尾
        EndHoldPress(true);
        if (!s_vk_instant_was_down) {
            s_vk_instant_was_down = true;
            ESP_LOGI(TAG, "virtual key [%s] click", hit->name);
            EmitVkEvent(hit->name, TouchVkEvent::Click);
        }
        return ESP_OK;
    }

    s_vk_instant_was_down = false;

    // 换键：结束旧 hold，开始新 hold
    if (s_vk_hold.name != nullptr && std::strcmp(s_vk_hold.name, hit->name) != 0) {
        EndHoldPress(true);
    }

    if (s_vk_hold.name == nullptr) {
        s_vk_hold.name = hit->name;
        s_vk_hold.long_press_ms = hit->long_press_ms;
        s_vk_hold.down_us = esp_timer_get_time();
        s_vk_hold.long_emitted = false;
        s_vk_hold.long_consumed = false;
        ESP_LOGI(TAG, "virtual key [%s] press-down (long=%ums)", hit->name,
                 static_cast<unsigned>(hit->long_press_ms));
        return ESP_OK;
    }

    if (!s_vk_hold.long_emitted) {
        const int64_t elapsed_ms = (esp_timer_get_time() - s_vk_hold.down_us) / 1000;
        if (elapsed_ms >= static_cast<int64_t>(s_vk_hold.long_press_ms)) {
            s_vk_hold.long_emitted = true;
            ESP_LOGI(TAG, "virtual key [%s] long-press %ums", s_vk_hold.name,
                     static_cast<unsigned>(s_vk_hold.long_press_ms));
            // true=页面消费长按 → 松手不再发 Click（避免 PTT 后误回首页）
            s_vk_hold.long_consumed = EmitVkEvent(s_vk_hold.name, TouchVkEvent::LongPress);
        }
    }
    return ESP_OK;
}

}  // namespace

/*
 * LVGL I1 / HTILED：1=白、0=黑，与面板 VRAM（0xFF=白）一致，写屏不取反。
 * 局刷：脏区 PREV=旧 CURR=新；刷完（Drain）再同步脏区 RED=BW=新。
 * 每 kPartialRefreshBeforeFullFast 次局刷 / 屏幕测试「全刷」/ 开机首帧 / 关机末帧：
 * 整屏局刷黑闪一次再恢复（0xFC，无 FULL_FAST/SHUTDOWN OTP 多段白黑闪）。
 * 描边残影：bpp4 AA 边缘易剩一圈；在同一帧差分里外扩「黑→白」并 scrub PREV，不另刷。
 *
 * 触摸响应：日常 PARTIAL 启动刷新后立刻 lv_display_flush_ready，不等 BUSY；
 * 下次写屏前 DrainRefreshIfNeeded 再等完成并 SyncDirty。这样虚拟键/控件点击可立即进 indev。
 */
static constexpr uint8_t kLvglWhite = 0xFF;
static constexpr uint8_t kLvglBlack = 0x00;
static constexpr int kPartialDirtyPadPx = 24;
static constexpr int kOutlineScrubRadius = 3;  // bpp4 AA 描边稍厚
/** 局刷累计 N 次后整屏局刷「黑闪→恢复」清残影（单次黑屏，无 FULL_FAST 多段闪）；0=关闭 */
static constexpr uint32_t kPartialRefreshBeforeFullFast = 30;

struct EpdFlushCtx {
    lv_display_t* disp = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;
    SemaphoreHandle_t done_sem = nullptr;
    uint8_t* last_fb = nullptr;
    uint8_t* work_fb = nullptr;
    uint8_t* tx_prev = nullptr;
    uint8_t* tx_curr = nullptr;
    size_t fb_size = 0;
    int panel_w = 0;
    int panel_h = 0;
    bool has_last = false;
    uint32_t partial_refresh_count = 0;
    /** 下次 flush 走整屏局刷黑闪（非 FULL_FAST）；消费一次后清零。 */
    bool force_full_refresh = false;
    volatile bool suppress_lvgl_notify = false;
    /** 关机画已落墨：后续 LVGL flush 一律丢弃，避免白屏盖掉关机图。 */
    volatile bool freeze_updates = false;
    /**
     * 局刷已启动、BUSY 尚未结束：flush 已提前 flush_ready，LVGL 可继续读触摸。
     * 下次写屏前 Drain 再 SyncDirty，避免等 BUSY 堵住点击。
     */
    bool refresh_inflight = false;
    bool pending_sync_dirty = false;
    int pending_sync_x = 0;
    int pending_sync_y = 0;
    int pending_sync_w = 0;
    int pending_sync_h = 0;
};

namespace {

void EpdAreaRounder(lv_area_t* area, void* user_data) {
    (void)user_data;
    // 扩 invalidate：让 LVGL 把旧字/AA 边缘也重绘成白，局刷 CURR 才有黑→白差分
    area->x1 -= kPartialDirtyPadPx;
    area->y1 -= kPartialDirtyPadPx / 2;
    area->x2 += kPartialDirtyPadPx;
    area->y2 += kPartialDirtyPadPx / 2;
    if (area->x1 < 0) {
        area->x1 = 0;
    }
    if (area->y1 < 0) {
        area->y1 = 0;
    }
    area->x1 = (area->x1 >> 3) << 3;
    area->x2 = ((area->x2 >> 3) << 3) + 7;
}

bool EpdRefreshDoneCb(const esp_lcd_panel_handle_t handle, const void* edata, void* user_data) {
    (void)handle;
    (void)edata;
    auto* ctx = static_cast<EpdFlushCtx*>(user_data);
    if (!ctx) {
        return false;
    }

    if (ctx->suppress_lvgl_notify) {
        BaseType_t hp_task = pdFALSE;
        if (ctx->done_sem) {
            xSemaphoreGiveFromISR(ctx->done_sem, &hp_task);
        }
        if (hp_task == pdTRUE) {
            portYIELD_FROM_ISR();
        }
        return false;
    }

    return esp_lv_adapter_display_notify_frame_done_from_isr(ctx->disp);
}

static void AlignArea(int x_start, int y_start, int x_end, int y_end, int* x_draw, int* y_draw,
                      int* w_draw, int* h_draw) {
    const int len_x = x_end - x_start;
    const int len_y = y_end - y_start;
    *x_draw = x_start - (x_start % 8);
    *y_draw = y_start;
    *w_draw = ((len_x + 7) / 8) * 8;
    *h_draw = len_y;
}

// 扩脏区：边距盖住 AA/旧字边缘；再按 last⊕work 把真正变过的字节并进去
static void ExpandPartialDirty(EpdFlushCtx* ctx, int* x_draw, int* y_draw, int* w_draw, int* h_draw) {
    int x0 = *x_draw - kPartialDirtyPadPx;
    int y0 = *y_draw - kPartialDirtyPadPx;
    int x1 = *x_draw + *w_draw + kPartialDirtyPadPx;
    int y1 = *y_draw + *h_draw + kPartialDirtyPadPx;
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > ctx->panel_w) {
        x1 = ctx->panel_w;
    }
    if (y1 > ctx->panel_h) {
        y1 = ctx->panel_h;
    }
    x0 = (x0 >> 3) << 3;
    x1 = ((x1 + 7) >> 3) << 3;
    if (x1 > ctx->panel_w) {
        x1 = ctx->panel_w;
    }

    const int fb_stride = ctx->panel_w / 8;
    int ch_x0 = x1;
    int ch_x1 = x0;
    int ch_y0 = y1;
    int ch_y1 = y0;
    bool any = false;
    for (int y = y0; y < y1; y++) {
        const uint8_t* a = ctx->last_fb + y * fb_stride + (x0 / 8);
        const uint8_t* b = ctx->work_fb + y * fb_stride + (x0 / 8);
        const int nbytes = (x1 - x0) / 8;
        for (int i = 0; i < nbytes; i++) {
            if (a[i] == b[i]) {
                continue;
            }
            any = true;
            const int bx0 = x0 + i * 8;
            const int bx1 = bx0 + 8;
            if (bx0 < ch_x0) {
                ch_x0 = bx0;
            }
            if (bx1 > ch_x1) {
                ch_x1 = bx1;
            }
            if (y < ch_y0) {
                ch_y0 = y;
            }
            if (y + 1 > ch_y1) {
                ch_y1 = y + 1;
            }
        }
    }

    if (!any) {
        *w_draw = 0;
        *h_draw = 0;
        return;
    }

    // 变化核再扩一圈，避免字缘灰度抖到脏区外
    ch_x0 -= kPartialDirtyPadPx;
    ch_y0 -= kPartialDirtyPadPx / 2;
    ch_x1 += kPartialDirtyPadPx;
    ch_y1 += kPartialDirtyPadPx / 2;
    if (ch_x0 < 0) {
        ch_x0 = 0;
    }
    if (ch_y0 < 0) {
        ch_y0 = 0;
    }
    if (ch_x1 > ctx->panel_w) {
        ch_x1 = ctx->panel_w;
    }
    if (ch_y1 > ctx->panel_h) {
        ch_y1 = ctx->panel_h;
    }
    ch_x0 = (ch_x0 >> 3) << 3;
    ch_x1 = ((ch_x1 + 7) >> 3) << 3;
    if (ch_x1 > ctx->panel_w) {
        ch_x1 = ctx->panel_w;
    }

    *x_draw = ch_x0;
    *y_draw = ch_y0;
    *w_draw = ch_x1 - ch_x0;
    *h_draw = ch_y1 - ch_y0;
}

static void OverlayDirty(uint8_t* dst, const uint8_t* color_map, int panel_w, int x_draw,
                         int y_draw, int w_draw, int h_draw) {
    const int fb_stride = panel_w / 8;
    const int row_bytes = w_draw / 8;
    for (int row = 0; row < h_draw; row++) {
        const int y = y_draw + row;
        memcpy(dst + y * fb_stride + x_draw / 8, color_map + y * fb_stride + x_draw / 8, row_bytes);
    }
}

static void PackRegion(uint8_t* dst, const uint8_t* fb, int panel_w, int x_draw, int y_draw,
                       int w_draw, int h_draw) {
    const int fb_stride = panel_w / 8;
    const int row_bytes = w_draw / 8;
    for (int row = 0; row < h_draw; row++) {
        memcpy(dst + row * row_bytes, fb + (y_draw + row) * fb_stride + x_draw / 8, row_bytes);
    }
}

static inline bool MonoBitWhite(const uint8_t* fb, int stride, int x, int y) {
    return (fb[y * stride + (x >> 3)] & (0x80 >> (x & 7))) != 0;
}

static inline void MonoBitSetBlack(uint8_t* fb, int stride, int x, int y) {
    fb[y * stride + (x >> 3)] &= static_cast<uint8_t>(~(0x80 >> (x & 7)));
}

// 描边残影：只 scrub 已是白色的像素的 PREV（临时置黑），绝不改 work 里的黑墨，
// 否则会把新旧字重叠处打成白点（字断）。
static void ScrubGlyphOutline(EpdFlushCtx* ctx, int x0, int y0, int w, int h) {
    if (w <= 0 || h <= 0) {
        return;
    }
    const int stride = ctx->panel_w / 8;
    const int row_bytes = w / 8;
    const size_t region_bytes = static_cast<size_t>(row_bytes) * static_cast<size_t>(h);
    memset(ctx->tx_prev, 0, region_bytes);
    memset(ctx->tx_curr, 0, region_bytes);

    auto mask_set = [&](uint8_t* mask, int x, int y) {
        const int lx = x - x0;
        const int ly = y - y0;
        mask[ly * row_bytes + (lx >> 3)] |= static_cast<uint8_t>(0x80 >> (lx & 7));
    };
    auto mask_get = [&](const uint8_t* mask, int x, int y) -> bool {
        const int lx = x - x0;
        const int ly = y - y0;
        return (mask[ly * row_bytes + (lx >> 3)] & (0x80 >> (lx & 7))) != 0;
    };

    for (int y = y0; y < y0 + h; y++) {
        for (int x = x0; x < x0 + w; x++) {
            if (!MonoBitWhite(ctx->last_fb, stride, x, y) &&
                MonoBitWhite(ctx->work_fb, stride, x, y)) {
                mask_set(ctx->tx_prev, x, y);
            }
        }
    }

    const int R = kOutlineScrubRadius;
    for (int y = y0; y < y0 + h; y++) {
        for (int x = x0; x < x0 + w; x++) {
            if (!mask_get(ctx->tx_prev, x, y)) {
                continue;
            }
            for (int dy = -R; dy <= R; dy++) {
                for (int dx = -R; dx <= R; dx++) {
                    const int nx = x + dx;
                    const int ny = y + dy;
                    if (nx < x0 || ny < y0 || nx >= x0 + w || ny >= y0 + h) {
                        continue;
                    }
                    mask_set(ctx->tx_curr, nx, ny);
                }
            }
        }
    }

    for (int y = y0; y < y0 + h; y++) {
        for (int x = x0; x < x0 + w; x++) {
            if (!mask_get(ctx->tx_curr, x, y)) {
                continue;
            }
            // 只动「本帧应为白」的像素；黑笔画一律保留
            if (!MonoBitWhite(ctx->work_fb, stride, x, y)) {
                continue;
            }
            MonoBitSetBlack(ctx->last_fb, stride, x, y);
        }
    }
}

static esp_err_t WaitRefreshDone(EpdFlushCtx* ctx, const char* tag) {
    if (xSemaphoreTake(ctx->done_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "%s timeout", tag);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t DrawFull(EpdFlushCtx* ctx, esp_lcd_panel_handle_t panel, const uint8_t* fb,
                          esp_lcd_ssd1677_refresh_mode_t mode) {
    memcpy(ctx->tx_prev, fb, ctx->fb_size);
    memcpy(ctx->tx_curr, fb, ctx->fb_size);
    epaper_panel_set_bitmap_color(panel, SSD1677_EPAPER_BITMAP_PREVIOUS);
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_draw_bitmap(panel, 0, 0, ctx->panel_w, ctx->panel_h, ctx->tx_prev), TAG,
        "full prev");
    epaper_panel_set_bitmap_color(panel, SSD1677_EPAPER_BITMAP_CURRENT);
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_draw_bitmap(panel, 0, 0, ctx->panel_w, ctx->panel_h, ctx->tx_curr), TAG,
        "full curr");
    epaper_panel_set_refresh_mode(panel, mode);
    return epaper_panel_refresh_screen(panel);
}

static esp_err_t DrawPartialDirty(EpdFlushCtx* ctx, esp_lcd_panel_handle_t panel, const uint8_t* prev_fb,
                                  const uint8_t* curr_fb, int x_draw, int y_draw, int w_draw,
                                  int h_draw) {
    PackRegion(ctx->tx_prev, prev_fb, ctx->panel_w, x_draw, y_draw, w_draw, h_draw);
    PackRegion(ctx->tx_curr, curr_fb, ctx->panel_w, x_draw, y_draw, w_draw, h_draw);

    epaper_panel_set_bitmap_color(panel, SSD1677_EPAPER_BITMAP_PREVIOUS);
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_draw_bitmap(panel, x_draw, y_draw, x_draw + w_draw, y_draw + h_draw,
                                  ctx->tx_prev),
        TAG, "dirty prev");
    epaper_panel_set_bitmap_color(panel, SSD1677_EPAPER_BITMAP_CURRENT);
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_draw_bitmap(panel, x_draw, y_draw, x_draw + w_draw, y_draw + h_draw,
                                  ctx->tx_curr),
        TAG, "dirty curr");
    epaper_panel_set_refresh_mode(panel, SSD1677_EPAPER_REFRESH_PARTIAL);
    return epaper_panel_refresh_screen(panel);
}

/** 整屏局刷：PREV/CURR 差分，0xFC 波形无 FULL/FULL_FAST 的白黑多段闪（GxEPD2 refresh(true) 同款）。 */
static esp_err_t DrawFullScreenPartial(EpdFlushCtx* ctx, esp_lcd_panel_handle_t panel,
                                       const uint8_t* prev_fb, const uint8_t* curr_fb) {
    memcpy(ctx->tx_prev, prev_fb, ctx->fb_size);
    memcpy(ctx->tx_curr, curr_fb, ctx->fb_size);
    epaper_panel_set_bitmap_color(panel, SSD1677_EPAPER_BITMAP_PREVIOUS);
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_draw_bitmap(panel, 0, 0, ctx->panel_w, ctx->panel_h, ctx->tx_prev), TAG,
        "fsp prev");
    epaper_panel_set_bitmap_color(panel, SSD1677_EPAPER_BITMAP_CURRENT);
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_draw_bitmap(panel, 0, 0, ctx->panel_w, ctx->panel_h, ctx->tx_curr), TAG,
        "fsp curr");
    epaper_panel_set_refresh_mode(panel, SSD1677_EPAPER_REFRESH_PARTIAL);
    return epaper_panel_refresh_screen(panel);
}

static esp_err_t SyncFullBothNew(EpdFlushCtx* ctx, esp_lcd_panel_handle_t panel,
                                 const uint8_t* curr_fb) {
    memcpy(ctx->tx_prev, curr_fb, ctx->fb_size);
    memcpy(ctx->tx_curr, curr_fb, ctx->fb_size);
    epaper_panel_set_bitmap_color(panel, SSD1677_EPAPER_BITMAP_PREVIOUS);
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_draw_bitmap(panel, 0, 0, ctx->panel_w, ctx->panel_h, ctx->tx_prev), TAG,
        "sync full prev");
    epaper_panel_set_bitmap_color(panel, SSD1677_EPAPER_BITMAP_CURRENT);
    return esp_lcd_panel_draw_bitmap(panel, 0, 0, ctx->panel_w, ctx->panel_h, ctx->tx_curr);
}

/**
 * 定期清残影：两次整屏局刷（旧→全黑→新），肉眼仅一次快黑闪。
 * 不用 FULL_FAST(0xD7)：OTP 快全刷 LUT 会白黑来回多段闪（Waveshare/GxEPD2 实测）。
 */
static esp_err_t PeriodicBlackPulseClear(EpdFlushCtx* ctx, esp_lcd_panel_handle_t panel) {
    memset(ctx->tx_curr, kLvglBlack, ctx->fb_size);
    ESP_RETURN_ON_ERROR(DrawFullScreenPartial(ctx, panel, ctx->last_fb, ctx->tx_curr), TAG,
                        "pulse black");
    (void)WaitRefreshDone(ctx, "pulse_black");
    ESP_RETURN_ON_ERROR(SyncFullBothNew(ctx, panel, ctx->tx_curr), TAG, "sync black");

    ESP_RETURN_ON_ERROR(DrawFullScreenPartial(ctx, panel, ctx->tx_curr, ctx->work_fb), TAG,
                        "pulse restore");
    (void)WaitRefreshDone(ctx, "pulse_restore");
    return SyncFullBothNew(ctx, panel, ctx->work_fb);
}

// 局刷后脏区 RED/BW 都写成新图，避免 RED!=BW 残留
static esp_err_t SyncDirtyBothNew(EpdFlushCtx* ctx, esp_lcd_panel_handle_t panel,
                                  const uint8_t* curr_fb, int x_draw, int y_draw, int w_draw,
                                  int h_draw) {
    PackRegion(ctx->tx_prev, curr_fb, ctx->panel_w, x_draw, y_draw, w_draw, h_draw);
    PackRegion(ctx->tx_curr, curr_fb, ctx->panel_w, x_draw, y_draw, w_draw, h_draw);
    epaper_panel_set_bitmap_color(panel, SSD1677_EPAPER_BITMAP_PREVIOUS);
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_draw_bitmap(panel, x_draw, y_draw, x_draw + w_draw, y_draw + h_draw,
                                  ctx->tx_prev),
        TAG, "sync dirty prev");
    epaper_panel_set_bitmap_color(panel, SSD1677_EPAPER_BITMAP_CURRENT);
    return esp_lcd_panel_draw_bitmap(panel, x_draw, y_draw, x_draw + w_draw, y_draw + h_draw,
                                     ctx->tx_curr);
}

/** 排空上一次异步局刷：等 BUSY → 脏区 RED/BW 对齐。须在再次写 VRAM / 刷新前调用。 */
static void DrainRefreshIfNeeded(EpdFlushCtx* ctx) {
    if (ctx == nullptr || !ctx->refresh_inflight) {
        return;
    }
    (void)WaitRefreshDone(ctx, "drain");
    if (ctx->pending_sync_dirty && ctx->panel != nullptr) {
        const esp_err_t err =
            SyncDirtyBothNew(ctx, ctx->panel, ctx->last_fb, ctx->pending_sync_x, ctx->pending_sync_y,
                             ctx->pending_sync_w, ctx->pending_sync_h);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "drain dirty sync: %s", esp_err_to_name(err));
        }
    }
    ctx->pending_sync_dirty = false;
    ctx->refresh_inflight = false;
    ctx->suppress_lvgl_notify = false;
}

/** A2I1 逻辑竖屏 → 面板 HTILED（与 adapter ROTATE_270：px=ly, py=ph-1-lx）。 */
void BlitA2i1ToPanelFb(uint8_t* dst_fb, int panel_w, int panel_h, const uint8_t* a2i1_payload,
                       uint16_t log_w, uint16_t log_h, uint16_t src_stride) {
    const uint8_t* src = a2i1_payload + 8; /* skip LVGL palette */
    const int dst_stride = panel_w / 8;
    memset(dst_fb, kLvglWhite, static_cast<size_t>(dst_stride) * static_cast<size_t>(panel_h));
    for (int ly = 0; ly < static_cast<int>(log_h); ly++) {
        for (int lx = 0; lx < static_cast<int>(log_w); lx++) {
            const bool white =
                (src[ly * src_stride + (lx >> 3)] & static_cast<uint8_t>(0x80 >> (lx & 7))) != 0;
            const int px = ly;
            const int py = panel_h - 1 - lx;
            if (px < 0 || py < 0 || px >= panel_w || py >= panel_h) {
                continue;
            }
            uint8_t* byte = &dst_fb[py * dst_stride + (px >> 3)];
            const uint8_t mask = static_cast<uint8_t>(0x80 >> (px & 7));
            if (white) {
                *byte |= mask;
            } else {
                *byte = static_cast<uint8_t>(*byte & ~mask);
            }
        }
    }
}

/**
 * 关机末帧：与屏幕测试相同的整屏局刷黑闪 → VRAM 对齐 → park(0x83→0x10)。
 * 不用 OTP FULL/FULL_FAST，避免多段白黑闪。断电前仍须 park，否则 VCOM 无序放电发暗。
 * 日常 UI 局刷逻辑不变。
 */
esp_err_t EpdShutdownFullFb(EpdFlushCtx* ctx, const uint8_t* curr_fb) {
    if (ctx == nullptr || ctx->panel == nullptr || ctx->last_fb == nullptr || curr_fb == nullptr ||
        ctx->work_fb == nullptr || ctx->tx_prev == nullptr || ctx->tx_curr == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    DrainRefreshIfNeeded(ctx);
    if (!ctx->has_last) {
        memset(ctx->last_fb, kLvglWhite, ctx->fb_size);
    }
    if (curr_fb != ctx->work_fb) {
        memcpy(ctx->work_fb, curr_fb, ctx->fb_size);
    }
    ctx->suppress_lvgl_notify = true;
    xSemaphoreTake(ctx->done_sem, 0);
    ESP_LOGI(TAG, "BLACK_PULSE (shutdown)");
    esp_err_t err = PeriodicBlackPulseClear(ctx, ctx->panel);
    if (err == ESP_OK) {
        // 双保险：控制器 RAM 与帧缓冲一致后再 park
        err = SyncFullBothNew(ctx, ctx->panel, curr_fb);
    }
    if (err == ESP_OK) {
        err = epaper_panel_park_for_shutdown(ctx->panel);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "shutdown epd park: %s", esp_err_to_name(err));
        }
    }
    ctx->suppress_lvgl_notify = false;
    if (err == ESP_OK) {
        memcpy(ctx->last_fb, curr_fb, ctx->fb_size);
        ctx->has_last = true;
    }
    return err;
}

esp_err_t EpdLvglDrawBitmap(lv_display_t* disp, esp_lcd_panel_handle_t panel, int x_start,
                            int y_start, int x_end, int y_end, const void* color_map,
                            void* user_ctx) {
    (void)disp;
    auto* ctx = static_cast<EpdFlushCtx*>(user_ctx);
    if (!ctx || !color_map || !ctx->last_fb || !ctx->work_fb || !ctx->tx_prev || !ctx->tx_curr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ctx->freeze_updates) {
        if (ctx->disp) {
            lv_display_flush_ready(ctx->disp);
        }
        return ESP_OK;
    }

    // 上一次异步局刷未结束则先排空，再写本帧（触摸已在上一帧 flush_ready 后继续处理）
    DrainRefreshIfNeeded(ctx);

    int x_draw = 0, y_draw = 0, w_draw = 0, h_draw = 0;
    AlignArea(x_start, y_start, x_end, y_end, &x_draw, &y_draw, &w_draw, &h_draw);
    if (w_draw <= 0 || h_draw <= 0 || x_draw < 0 || y_draw < 0 ||
        x_draw + w_draw > ctx->panel_w || y_draw + h_draw > ctx->panel_h) {
        ESP_LOGW(TAG, "bad area (%d,%d)-(%d,%d)", x_start, y_start, x_end, y_end);
        return ESP_ERR_INVALID_ARG;
    }

    if (ctx->has_last) {
        memcpy(ctx->work_fb, ctx->last_fb, ctx->fb_size);
        OverlayDirty(ctx->work_fb, static_cast<const uint8_t*>(color_map), ctx->panel_w, x_draw,
                     y_draw, w_draw, h_draw);
    } else {
        // color_map 在旋转 I1 下是整帧缓冲，未画到的区域是 0（黑）。先铺白再叠脏区，
        // 避免关机全刷把未 flush 的条带刷成黑屏。
        memset(ctx->work_fb, kLvglWhite, ctx->fb_size);
        OverlayDirty(ctx->work_fb, static_cast<const uint8_t*>(color_map), ctx->panel_w, x_draw,
                     y_draw, w_draw, h_draw);
    }

    // 首帧与请求「全刷」同走整屏局刷黑闪（避 FULL_FAST 0xD7 多段闪）
    if (!ctx->has_last) {
        ESP_LOGI(TAG, "BLACK_PULSE (first baseline)");
        memset(ctx->last_fb, kLvglWhite, ctx->fb_size);
        ctx->suppress_lvgl_notify = true;
        xSemaphoreTake(ctx->done_sem, 0);
        ESP_RETURN_ON_ERROR(PeriodicBlackPulseClear(ctx, panel), TAG, "black_pulse_first");
        ctx->suppress_lvgl_notify = false;
        memcpy(ctx->last_fb, ctx->work_fb, ctx->fb_size);
        ctx->has_last = true;
        ctx->partial_refresh_count = 0;
        lv_display_flush_ready(ctx->disp);
        return ESP_OK;
    }

    if (ctx->force_full_refresh) {
        ctx->force_full_refresh = false;
        ESP_LOGI(TAG, "BLACK_PULSE (requested full)");
        ctx->suppress_lvgl_notify = true;
        xSemaphoreTake(ctx->done_sem, 0);
        ESP_RETURN_ON_ERROR(PeriodicBlackPulseClear(ctx, panel), TAG, "black_pulse_req");
        ctx->suppress_lvgl_notify = false;
        memcpy(ctx->last_fb, ctx->work_fb, ctx->fb_size);
        ctx->partial_refresh_count = 0;
        lv_display_flush_ready(ctx->disp);
        return ESP_OK;
    }

    ExpandPartialDirty(ctx, &x_draw, &y_draw, &w_draw, &h_draw);
    if (w_draw <= 0 || h_draw <= 0) {
        lv_display_flush_ready(ctx->disp);
        return ESP_OK;
    }

    const bool periodic_black_pulse =
        kPartialRefreshBeforeFullFast > 0 &&
        ctx->partial_refresh_count >= kPartialRefreshBeforeFullFast;

    ctx->suppress_lvgl_notify = true;
    xSemaphoreTake(ctx->done_sem, 0);

    if (periodic_black_pulse) {
        ESP_LOGI(TAG, "BLACK_PULSE clear after %u partials", ctx->partial_refresh_count);
        ESP_RETURN_ON_ERROR(PeriodicBlackPulseClear(ctx, panel), TAG, "black_pulse");
        ctx->partial_refresh_count = 0;
        ctx->suppress_lvgl_notify = false;
        memcpy(ctx->last_fb, ctx->work_fb, ctx->fb_size);
        lv_display_flush_ready(ctx->disp);
        return ESP_OK;
    }

    // 同一帧内清描边：扩「黑→白」邻域；last 临时置黑仅影响本帧 PREV，刷完用 work 覆盖回 last
    ScrubGlyphOutline(ctx, x_draw, y_draw, w_draw, h_draw);

    // 启动局刷后立刻 flush_ready：不等 BUSY，触摸/虚拟键可马上再进 indev
    ESP_LOGD(TAG, "PARTIAL dirty %dx%d @(%d,%d) #%u (async)", w_draw, h_draw, x_draw, y_draw,
             ctx->partial_refresh_count + 1);
    {
        const esp_err_t err =
            DrawPartialDirty(ctx, panel, ctx->last_fb, ctx->work_fb, x_draw, y_draw, w_draw, h_draw);
        if (err != ESP_OK) {
            ctx->suppress_lvgl_notify = false;
            ESP_LOGE(TAG, "partial: %s", esp_err_to_name(err));
            return err;
        }
    }

    ctx->pending_sync_x = x_draw;
    ctx->pending_sync_y = y_draw;
    ctx->pending_sync_w = w_draw;
    ctx->pending_sync_h = h_draw;
    ctx->pending_sync_dirty = true;
    ctx->refresh_inflight = true;
    ctx->partial_refresh_count++;

    memcpy(ctx->last_fb, ctx->work_fb, ctx->fb_size);
    lv_display_flush_ready(ctx->disp);
    return ESP_OK;
}

}  // namespace

LVAdapterDisplay::LVAdapterDisplay(const esp_lcd_panel_handle_t panel,
                                   const esp_lcd_panel_io_handle_t panel_io,
                                   const esp_lcd_touch_handle_t touch_handle, const int width,
                                   const int height) {
    instance_ = this;
    width_ = width;
    height_ = height;

    esp_timer_create_args_t notification_timer_args = {
        .callback =
            [](void* arg) {
                auto* display = static_cast<LVAdapterDisplay*>(arg);
                DisplayLockGuard lock(display);
                if (display->notification_label_) {
                    lv_obj_add_flag(display->notification_label_, LV_OBJ_FLAG_HIDDEN);
                }
                if (display->status_label_) {
                    lv_obj_remove_flag(display->status_label_, LV_OBJ_FLAG_HIDDEN);
                }
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "epd_notif_timer",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&notification_timer_args, &notification_timer_));

    epd_flush_ctx_ = new EpdFlushCtx();
    epd_flush_ctx_->panel = panel;
    epd_flush_ctx_->panel_w = SSD1677_PANEL_WIDTH;
    epd_flush_ctx_->panel_h = SSD1677_PANEL_HEIGHT;
    epd_flush_ctx_->fb_size = SSD1677_PANEL_BUFFER_SIZE;
    epd_flush_ctx_->done_sem = xSemaphoreCreateBinary();

    // last/work/tx_* 均在 PSRAM（仅 CPU 读写）。发屏由面板侧拷到内部 DMA bounce，
    // 避免 PSRAM 直 DMA 的 cache 不一致与鬼影。内部只常驻 1×48KB bounce。
    const uint32_t psram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    epd_flush_ctx_->last_fb =
        static_cast<uint8_t*>(heap_caps_malloc(epd_flush_ctx_->fb_size, psram));
    epd_flush_ctx_->work_fb =
        static_cast<uint8_t*>(heap_caps_malloc(epd_flush_ctx_->fb_size, psram));
    epd_flush_ctx_->tx_prev =
        static_cast<uint8_t*>(heap_caps_malloc(epd_flush_ctx_->fb_size, psram));
    epd_flush_ctx_->tx_curr =
        static_cast<uint8_t*>(heap_caps_malloc(epd_flush_ctx_->fb_size, psram));

    if (epd_flush_ctx_->last_fb) {
        memset(epd_flush_ctx_->last_fb, kLvglWhite, epd_flush_ctx_->fb_size);
    }

    if (!epd_flush_ctx_->done_sem || !epd_flush_ctx_->last_fb || !epd_flush_ctx_->work_fb ||
        !epd_flush_ctx_->tx_prev || !epd_flush_ctx_->tx_curr) {
        ESP_LOGE(TAG, "epd flush ctx alloc failed");
    }

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_stack_size = 16 * 1024;
    adapter_cfg.task_max_delay_ms = 1000;
    // PSRAM 栈上禁止调用 spi_flash/NVS（会触发 cache_utils assert）。
    // 业务里凡 EnsureAudioServiceRunning / Settings 等须放到 DRAM 栈任务（参见录音/翻译）。
    adapter_cfg.stack_in_psram = true;
    adapter_cfg.task_priority = 1;
    adapter_cfg.task_core_id = 1;

    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_cfg));
    ESP_ERROR_CHECK(esp_lv_adapter_set_default_display_idf_callback_registration_enabled(false));

    esp_lv_adapter_display_config_t disp_cfg = ESP_LV_ADAPTER_DISPLAY_SPI_MONO_DEFAULT_CONFIG(
        panel, panel_io, static_cast<uint16_t>(width), static_cast<uint16_t>(height),
        ESP_LV_ADAPTER_ROTATE_270, ESP_LV_ADAPTER_MONO_LAYOUT_HTILED);
    disp_cfg.profile.use_psram = true;

    lv_display_t* disp = esp_lv_adapter_register_display(&disp_cfg);
    ESP_ERROR_CHECK(disp ? ESP_OK : ESP_FAIL);
    epd_flush_ctx_->disp = disp;

    epaper_panel_callbacks_t epd_cbs = {
        .on_epaper_refresh_done = EpdRefreshDoneCb,
    };
    ESP_ERROR_CHECK(epaper_panel_register_event_callbacks(panel, &epd_cbs, epd_flush_ctx_));
    ESP_ERROR_CHECK(esp_lv_adapter_set_area_rounder_cb(disp, EpdAreaRounder, nullptr));

    esp_lv_adapter_draw_bitmap_callbacks_t draw_cbs = {
        .custom_draw_bitmap = EpdLvglDrawBitmap,
    };
    ESP_ERROR_CHECK(
        esp_lv_adapter_set_draw_bitmap_callbacks(disp, &draw_cbs, epd_flush_ctx_));

    if (touch_handle != nullptr) {
        esp_lv_adapter_touch_config_t touch_cfg =
            ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, touch_handle);
        touch_cfg.callbacks.custom_touch_read = CustomTouchRead;
        touch_indev_ = esp_lv_adapter_register_touch(&touch_cfg);
        ESP_ERROR_CHECK(touch_indev_ ? ESP_OK : ESP_FAIL);
    }

    ESP_ERROR_CHECK(esp_lv_adapter_start());

    const mmap_assets_config_t mmap_cfg = {
        .partition_label = "resources",
        .max_files = MMAP_RESOURCES_FILES,
        .checksum = MMAP_RESOURCES_CHECKSUM,
        .flags = {.mmap_enable = true},
    };
    ESP_ERROR_CHECK(mmap_assets_new(&mmap_cfg, &resources_assets_));

    esp_lv_fs_handle_t fs_handle = nullptr;
    const fs_cfg_t fs_cfg = {
        .fs_letter = 'A',
        .fs_nums = MMAP_RESOURCES_FILES,
        .fs_assets = resources_assets_,
    };
    ESP_ERROR_CHECK(esp_lv_adapter_fs_mount(&fs_cfg, &fs_handle));

    // 在内部 RAM 栈的初始化线程预读 NVS，避免首次进页时在 LVGL(PSRAM 栈)里碰 flash。
    (void)HapticIsEnabled();

    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        SetupUI();
        esp_lv_adapter_unlock();
    }
}

void LVAdapterDisplay::SetupUI() {
    if (touch_indev_ == nullptr) {
        ESP_LOGW(TAG, "SetupUI: no touch, load TouchMissingScreen");
        lv_screen_load(TouchMissingScreen::Create());
        return;
    }
    ESP_LOGI(TAG, "SetupUI: create AppLauncher");
    lv_screen_load(AppLauncher::Create());
    ESP_LOGI(TAG, "SetupUI: AppLauncher loaded");
}

void LVAdapterDisplay::RegisterTouchVirtualKeys(const TouchVirtualKey* keys, size_t count,
                                                TouchVirtualKeyEventCb cb, void* user_data) {
    s_vk_count = 0;
    s_vk_cb = cb;
    s_vk_user = user_data;
    s_vk_instant_was_down = false;
    s_vk_hold = {};
    if (keys == nullptr || count == 0) {
        return;
    }
    if (count > kMaxTouchVirtualKeys) {
        count = kMaxTouchVirtualKeys;
    }
    for (size_t i = 0; i < count; ++i) {
        s_vk_keys[i] = keys[i];
    }
    s_vk_count = count;
    ESP_LOGI(TAG, "registered %u touch virtual keys", static_cast<unsigned>(s_vk_count));
}

void LVAdapterDisplay::RestoreStatusWidgetsLocked() {
    // 切页只换控件指针，图标/静音/时钟缓存跨页保留，首帧与页面内容同一次局刷画出。
    // 局刷已改为启动后立刻 flush_ready（BUSY 延后到下次 Drain），状态栏合并刷新仍有助于少刷。
    UpdateBatteryWidgetsLocked(charging_);
    if (battery_percent_label_ != nullptr) {
        char percent[12];
        if (battery_percent_ >= 0) {
            std::snprintf(percent, sizeof(percent), "%d%%", battery_percent_);
            lv_label_set_text(battery_percent_label_, percent);
        } else {
            lv_label_set_text(battery_percent_label_, "");
        }
    }
    if (status_label_ != nullptr) {
        lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    }
}

void LVAdapterDisplay::UpdateBatteryWidgetsLocked(bool charging) {
    if (battery_label_ != nullptr) {
        lv_image_set_src(battery_label_, charging ? &eego_battery_charging : &eego_battery);
    }
}

void LVAdapterDisplay::BindStatusWidgets(lv_obj_t* network, lv_obj_t* mute,
                                         lv_obj_t* battery_percent, lv_obj_t* battery,
                                         lv_obj_t* status, lv_obj_t* notification,
                                         lv_obj_t* low_battery_popup) {
    network_label_ = network;
    mute_label_ = mute;
    battery_percent_label_ = battery_percent;
    battery_label_ = battery;
    status_label_ = status;
    notification_label_ = notification;
    low_battery_popup_ = low_battery_popup;

    // 勿清 battery_icon_/network_icon_/muted_/status_body_cache_，也勿调 Board/UpdateStatusBar
    // （SetupUI 持锁重入会死锁）。有缓存则立刻灌进新控件，与本页首帧合并刷新。
    RestoreStatusWidgetsLocked();
}

LVAdapterDisplay::~LVAdapterDisplay() {
    if (instance_ == this) {
        instance_ = nullptr;
    }
    if (notification_timer_) {
        esp_timer_stop(notification_timer_);
        esp_timer_delete(notification_timer_);
        notification_timer_ = nullptr;
    }
    if (epd_flush_ctx_) {
        if (epd_flush_ctx_->last_fb) {
            heap_caps_free(epd_flush_ctx_->last_fb);
        }
        if (epd_flush_ctx_->work_fb) {
            heap_caps_free(epd_flush_ctx_->work_fb);
        }
        if (epd_flush_ctx_->tx_prev) {
            heap_caps_free(epd_flush_ctx_->tx_prev);
        }
        if (epd_flush_ctx_->tx_curr) {
            heap_caps_free(epd_flush_ctx_->tx_curr);
        }
        if (epd_flush_ctx_->done_sem) {
            vSemaphoreDelete(epd_flush_ctx_->done_sem);
        }
        delete epd_flush_ctx_;
        epd_flush_ctx_ = nullptr;
    }
}

void LVAdapterDisplay::SetEmotion(const char* /*emotion*/) {}

void LVAdapterDisplay::SetChatMessage(const char* /*role*/, const char* /*content*/) {}

void LVAdapterDisplay::SetStatusTitlePrefix(const char* prefix) {
    DisplayLockGuard lock(this);
    if (prefix == nullptr || prefix[0] == '\0') {
        status_title_prefix_[0] = '\0';
    } else {
        std::snprintf(status_title_prefix_, sizeof(status_title_prefix_), "%s", prefix);
    }
}

void LVAdapterDisplay::RequestNextFullRefresh() {
    if (epd_flush_ctx_ != nullptr) {
        epd_flush_ctx_->force_full_refresh = true;
        ESP_LOGI(TAG, "RequestNextFullRefresh armed");
    }
}

void LVAdapterDisplay::ShowPoweredOffScreen() {
    DisplayLockGuard lock(this);
    BindStatusWidgets(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);

    // 直接往 EPD 帧缓冲画 A2I1 再全刷：全屏 lv_image 在低内存下易画不出来只剩白底。
    // 加载链：NVS 源文件 →（失败/坏图）内置 mmap →（再失败）「已关机」文字
    bool have_img = false;
    uint8_t* sd_buf = nullptr;
    size_t sd_len = 0;
    if (epd_flush_ctx_ != nullptr && epd_flush_ctx_->work_fb != nullptr) {
        const uint8_t* mem = nullptr;
        size_t plen = 0;
        bool from_sd = TryLoadShutdownA2i1FromSd(&sd_buf, &sd_len);

        auto try_parse = [&](const uint8_t* raw, size_t raw_len, bool strip_mmap_prefix) -> bool {
            if (raw == nullptr || raw_len == 0) {
                return false;
            }
            const uint8_t* p = raw;
            size_t n = raw_len;
            if (strip_mmap_prefix && n >= 6 && p[0] == 0x5A && p[1] == 0x5A && p[2] == 'A' &&
                p[3] == '2' && p[4] == 'I' && p[5] == '1') {
                p += 2;
                n -= 2;
            }
            uint16_t w = 0, h = 0, stride = 0;
            const uint8_t* payload = nullptr;
            if (!ParseA2i1(p, n, &w, &h, &stride, &payload) || payload == nullptr) {
                return false;
            }
            BlitA2i1ToPanelFb(epd_flush_ctx_->work_fb, epd_flush_ctx_->panel_w,
                              epd_flush_ctx_->panel_h, payload, w, h, stride);
            const esp_err_t err = EpdShutdownFullFb(epd_flush_ctx_, epd_flush_ctx_->work_fb);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "shutdown full refresh failed: %s", esp_err_to_name(err));
                return false;
            }
            return true;
        };

        if (from_sd) {
            if (try_parse(sd_buf, sd_len, false)) {
                have_img = true;
            } else {
                ESP_LOGW(TAG, "shutdown img: SD A2I1 unusable → built-in");
                heap_caps_free(sd_buf);
                sd_buf = nullptr;
                sd_len = 0;
                from_sd = false;
            }
        }

        if (!have_img && resources_assets_ != nullptr) {
            mem = mmap_assets_get_mem(resources_assets_, MMAP_RESOURCES_BG_SHUTDOWN_A2I1);
            const int sz = mmap_assets_get_size(resources_assets_, MMAP_RESOURCES_BG_SHUTDOWN_A2I1);
            if (mem != nullptr && sz > 0) {
                plen = static_cast<size_t>(sz);
                ESP_LOGI(TAG, "shutdown img: use built-in bg_shutdown.a2i1 (%u bytes)",
                         static_cast<unsigned>(plen));
                have_img = try_parse(mem, plen, true);
                if (!have_img) {
                    ESP_LOGW(TAG, "shutdown img: built-in A2I1 parse/blit fail");
                }
            } else {
                ESP_LOGW(TAG, "shutdown img: built-in missing mem=%p sz=%d", mem, sz);
            }
        } else if (!have_img) {
            ESP_LOGW(TAG, "shutdown img: no resources_assets_");
        }
    }
    if (sd_buf != nullptr) {
        heap_caps_free(sd_buf);
        sd_buf = nullptr;
    }

    if (have_img) {
        // 冻结后续 flush，禁止 LVGL 再把白屏刷上去。
        if (epd_flush_ctx_ != nullptr) {
            epd_flush_ctx_->freeze_updates = true;
        }
    } else {
        lv_obj_t* scr = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(scr, 0, 0);
        lv_obj_set_style_border_width(scr, 0, 0);
        lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* lbl = lv_label_create(scr);
        lv_label_set_text(lbl, "已关机");
        const lv_font_t* font = fontpack_lv_font_get(30, 2);
        if (font == nullptr) {
            font = fontpack_lv_font_ui();
        }
        if (font != nullptr) {
            lv_obj_set_style_text_font(lbl, font, 0);
        }
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_center(lbl);
        lv_obj_t* old_scr = lv_screen_active();
        lv_screen_load(scr);
        if (old_scr != nullptr && old_scr != scr) {
            lv_obj_delete(old_scr);
        }
        lv_obj_invalidate(scr);
        lv_refr_now(nullptr);
        if (epd_flush_ctx_ != nullptr) {
            epd_flush_ctx_->freeze_updates = true;
        }
    }
    ESP_LOGI(TAG, "powered-off screen shown (direct full, a2i1=%d)", have_img ? 1 : 0);
}

void LVAdapterDisplay::ApplyStatusTextLocked(const char* status) {
    const char* body = (status != nullptr) ? status : "";
    if (body != status_body_cache_) {
        std::snprintf(status_body_cache_, sizeof(status_body_cache_), "%s", body);
    }
    // 顶栏固定为时钟；应用状态只保存在缓存，不覆盖左侧时间。
}

void LVAdapterDisplay::SetStatus(const char* status) {
    DisplayLockGuard lock(this);
    ApplyStatusTextLocked(status);
    last_status_update_time_ = std::chrono::system_clock::now();
}

void LVAdapterDisplay::ShowNotification(const char* notification, int duration_ms) {
    DisplayLockGuard lock(this);
    (void)notification;
    (void)duration_ms;
    if (notification_timer_) {
        esp_timer_stop(notification_timer_);
        ESP_ERROR_CHECK(esp_timer_start_once(notification_timer_, duration_ms * 1000ULL));
    }
}

void LVAdapterDisplay::UpdateStatusBar(bool update_all) {
    auto& app = Application::GetInstance();
    auto& board = Board::GetInstance();
    char time_str[16] = {};
    bool set_time = false;
    const bool want_clock = AllowsIdleStatusClock() && app.GetDeviceState() == kDeviceStateIdle;
    if (want_clock &&
        (update_all || last_status_update_time_ + std::chrono::seconds(10) <
                            std::chrono::system_clock::now())) {
        time_t now = time(nullptr);
        struct tm* tm = localtime(&now);
        if (tm && tm->tm_year >= 2025 - 1900) {
            strftime(time_str, sizeof(time_str), "%H:%M", tm);
            set_time = true;
        }
    }

    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    const char* new_battery_icon = nullptr;
    const bool battery_ok = board.GetBatteryLevel(battery_level, charging, discharging);
    if (battery_ok) {
        if (charging) {
            new_battery_icon = FONT_AWESOME_BATTERY_BOLT;
        } else {
            static const char* levels[] = {
                FONT_AWESOME_BATTERY_EMPTY, FONT_AWESOME_BATTERY_QUARTER, FONT_AWESOME_BATTERY_HALF,
                FONT_AWESOME_BATTERY_THREE_QUARTERS, FONT_AWESOME_BATTERY_FULL, FONT_AWESOME_BATTERY_FULL,
            };
            new_battery_icon = levels[battery_level / 20];
        }
    }

    const char* new_network_icon = nullptr;
    static int seconds_counter = 0;
    if (update_all || seconds_counter++ % 10 == 0) {
        const auto device_state = app.GetDeviceState();
        if (device_state == kDeviceStateIdle || device_state == kDeviceStateStarting) {
            new_network_icon = board.GetNetworkStateIcon();
        }
    }

    {
        DisplayLockGuard lock(this);
        if (status_label_ == nullptr && battery_percent_label_ == nullptr && battery_label_ == nullptr) {
            return;
        }

        if (set_time && status_label_ != nullptr) {
            lv_label_set_text(status_label_, time_str);
            lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
            last_status_update_time_ = std::chrono::system_clock::now();
        }

        if (battery_ok && new_battery_icon != nullptr) {
            const int clamped_percent = battery_level < 0 ? 0 : (battery_level > 100 ? 100 : battery_level);
            if (battery_percent_ != clamped_percent) {
                battery_percent_ = clamped_percent;
                if (battery_percent_label_ != nullptr) {
                    char percent[12];
                    std::snprintf(percent, sizeof(percent), "%d%%", battery_percent_);
                    lv_label_set_text(battery_percent_label_, percent);
                }
            }
            charging_ = charging;
            UpdateBatteryWidgetsLocked(charging_);
            if (battery_icon_ != new_battery_icon) {
                battery_icon_ = new_battery_icon;
            }
            if (low_battery_popup_ != nullptr) {
                if (std::strcmp(new_battery_icon, FONT_AWESOME_BATTERY_EMPTY) == 0 && discharging) {
                    if (lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) {
                        lv_obj_remove_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                    }
                } else if (!lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) {
                    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }

        if (new_network_icon != nullptr && network_icon_ != new_network_icon) {
            network_icon_ = new_network_icon;
            if (network_label_ != nullptr) {
                lv_label_set_text(network_label_, network_icon_);
            }
        }
    }
}

void LVAdapterDisplay::SetPowerSaveMode(bool on) {
    if (on) {
        SetChatMessage("system", "");
        SetEmotion("sleepy");
    } else {
        SetChatMessage("system", "");
        SetEmotion("neutral");
    }
}

void LVAdapterDisplay::SetPreviewImage(const void* image) {
    (void)image;
}

void LVAdapterDisplay::SetTheme(Theme* theme) {
    Display::SetTheme(theme);
}

bool LVAdapterDisplay::Lock(int timeout_ms) {
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

void LVAdapterDisplay::Unlock() {
    esp_lv_adapter_unlock();
}
