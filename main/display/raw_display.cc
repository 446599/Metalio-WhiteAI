#include "raw_display.h"

#include "board.h"
#include "esp_lcd_ssd1677_commands.h"
#include "esp_lcd_panel_ssd1677.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace {
constexpr const char* TAG = "RawDisplay";
constexpr int kPanelW = 800;
constexpr int kPanelH = 480;
constexpr int kPortraitW = 480;
constexpr int kPortraitH = 800;
constexpr uint8_t kWhite = 0xff;
// Match EegoRead's proven cadence: DU for normal frames, with an occasional
// GC pass to re-establish charge/history and clear accumulated ghosting.
constexpr uint32_t kFullRefreshEvery = 8;

// Compact seven-segment digits keep the raw UI allocation-free.
constexpr uint8_t kSegments[10] = {0x3f, 0x06, 0x5b, 0x4f, 0x66,
                                   0x6d, 0x7d, 0x07, 0x7f, 0x6f};
}

RawDisplay* RawDisplay::instance_ = nullptr;

RawDisplay::RawDisplay(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io,
                       esp_lcd_touch_handle_t touch, int width, int height)
    : panel_(panel), panel_io_(panel_io), touch_(touch) {
    width_ = width;
    height_ = height;
    portrait_size_ = static_cast<size_t>(kPortraitW / 8) * kPortraitH;
    panel_size_ = static_cast<size_t>(kPanelW / 8) * kPanelH;
    portrait_fb_ = static_cast<uint8_t*>(heap_caps_malloc(portrait_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    panel_fb_ = static_cast<uint8_t*>(heap_caps_malloc(panel_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    panel_prev_fb_ = static_cast<uint8_t*>(heap_caps_malloc(panel_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    panel_region_fb_ = static_cast<uint8_t*>(heap_caps_malloc(panel_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    mutex_ = xSemaphoreCreateMutex();
    instance_ = this;
    if (portrait_fb_ == nullptr || panel_fb_ == nullptr || panel_prev_fb_ == nullptr ||
        panel_region_fb_ == nullptr || mutex_ == nullptr) {
        ESP_LOGE(TAG, "raw framebuffer allocation failed");
    }
    if (panel_prev_fb_) std::memset(panel_prev_fb_, kWhite, panel_size_);
}

RawDisplay::~RawDisplay() {
    if (instance_ == this) instance_ = nullptr;
    if (portrait_fb_) heap_caps_free(portrait_fb_);
    if (panel_fb_) heap_caps_free(panel_fb_);
    if (panel_prev_fb_) heap_caps_free(panel_prev_fb_);
    if (panel_region_fb_) heap_caps_free(panel_region_fb_);
    if (mutex_) vSemaphoreDelete(mutex_);
}

bool RawDisplay::Lock(int timeout_ms) {
    return mutex_ != nullptr && xSemaphoreTake(mutex_, timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void RawDisplay::Unlock() { if (mutex_) xSemaphoreGive(mutex_); }

void RawDisplay::SetPixel(int x, int y, bool black) {
    if (x < 0 || x >= kPortraitW || y < 0 || y >= kPortraitH) return;
    uint8_t& b = portrait_fb_[static_cast<size_t>(y) * (kPortraitW / 8) + (x >> 3)];
    const uint8_t mask = static_cast<uint8_t>(0x80u >> (x & 7));
    if (black) b &= static_cast<uint8_t>(~mask); else b |= mask;
}

void RawDisplay::DrawDigit(int x, int y, int scale, int digit) {
    if (digit < 0 || digit > 9) return;
    const uint8_t s = kSegments[digit];
    auto bar = [&](int bx, int by, int bw, int bh) {
        for (int yy = 0; yy < bh * scale; ++yy)
            for (int xx = 0; xx < bw * scale; ++xx) SetPixel(x + (bx + xx / scale) * scale, y + (by + yy / scale) * scale, true);
    };
    if (s & 1) bar(2, 0, 5, 1);
    if (s & 2) bar(7, 1, 1, 5);
    if (s & 4) bar(7, 7, 1, 5);
    if (s & 8) bar(2, 12, 5, 1);
    if (s & 16) bar(1, 7, 1, 5);
    if (s & 32) bar(1, 1, 1, 5);
    if (s & 64) bar(2, 6, 5, 1);
}

void RawDisplay::DrawPercent(int x, int y, int scale, int value) {
    char buf[5]; std::snprintf(buf, sizeof(buf), "%d", std::clamp(value, 0, 100));
    int cursor = x;
    for (const char* p = buf; *p; ++p) { DrawDigit(cursor, y, scale, *p - '0'); cursor += 10 * scale; }
    for (int yy = 0; yy < scale; ++yy) for (int xx = 0; xx < scale; ++xx) SetPixel(cursor + xx, y + yy, true);
}

void RawDisplay::DrawTestPatternLocked() {
    std::memset(portrait_fb_, kWhite, portrait_size_);

    auto hline = [&](int y, int x0, int x1, int thickness = 1) {
        for (int yy = y; yy < y + thickness; ++yy)
            for (int x = x0; x <= x1; ++x) SetPixel(x, yy, true);
    };
    auto vline = [&](int x, int y0, int y1, int thickness = 1) {
        for (int xx = x; xx < x + thickness; ++xx)
            for (int y = y0; y <= y1; ++y) SetPixel(xx, y, true);
    };

    // Border and 40-pixel calibration grid expose clipping, rotation and
    // row/column addressing errors without relying on any UI framework.
    hline(0, 0, kPortraitW - 1, 4);
    hline(kPortraitH - 4, 0, kPortraitW - 1, 4);
    vline(0, 0, kPortraitH - 1, 4);
    vline(kPortraitW - 4, 0, kPortraitH - 1, 4);
    for (int x = 40; x < kPortraitW - 1; x += 40) vline(x, 4, kPortraitH - 5);
    for (int y = 40; y < kPortraitH - 1; y += 40) hline(y, 4, kPortraitW - 5);

    // Center cross and nested boxes check aspect ratio and byte alignment.
    const int cx = kPortraitW / 2;
    const int cy = kPortraitH / 2;
    hline(cy - 1, 4, kPortraitW - 5, 3);
    vline(cx - 1, 4, kPortraitH - 5, 3);
    hline(cy - 48, cx - 48, cx + 48, 2);
    hline(cy + 46, cx - 48, cx + 48, 2);
    vline(cx - 48, cy - 48, cy + 48, 2);
    vline(cx + 46, cy - 48, cy + 48, 2);

    // Dither blocks provide a simple black/white transition test.
    for (int y = 100; y < 220; ++y) {
        for (int x = 40; x < 160; ++x) {
            if (((x / 8) + (y / 8)) & 1) SetPixel(x, y, true);
        }
    }
    for (int y = 100; y < 220; ++y) {
        for (int x = kPortraitW - 160; x < kPortraitW - 40; ++x) {
            if ((x / 4) & 1) SetPixel(x, y, true);
        }
    }

    // Corner numbers make the panel orientation immediately apparent.
    DrawDigit(16, 16, 4, 1);
    DrawDigit(kPortraitW - 48, 16, 4, 2);
    DrawDigit(16, kPortraitH - 76, 4, 3);
    DrawDigit(kPortraitW - 48, kPortraitH - 76, 4, 4);
}

void RawDisplay::DrawTestPatternVariantLocked(uint32_t step) {
    DrawTestPatternLocked();
    // Move a solid 48x48 block through the center test window.  The block is
    // deliberately confined to the same region used by the partial refresh.
    const int x = 190 + static_cast<int>((step % 4) * 48);
    const int y = 350;
    for (int yy = y; yy < y + 48; ++yy)
        for (int xx = x; xx < x + 48; ++xx) SetPixel(xx, yy, true);
}

void RawDisplay::ShowScreenTestPattern() {
    DisplayLockGuard lock(this);
    if (!portrait_fb_) return;
    screen_test_mode_ = true;
    screen_test_tick_ = 0;
    DrawTestPatternLocked();
    FlushLocked();
}

void RawDisplay::ShowHomeScreen() {
    DisplayLockGuard lock(this);
    if (!portrait_fb_) return;
    screen_test_mode_ = false;
    DrawHomeScreenLocked();
    FlushLocked();
}

void RawDisplay::DrawHomeScreenLocked() {
    std::memset(portrait_fb_, kWhite, portrait_size_);
    auto box = [&](int x, int y, int w, int h, bool filled) {
        for (int yy = y; yy < y + h; ++yy)
            for (int xx = x; xx < x + w; ++xx)
                if (filled || yy < y + 3 || yy >= y + h - 3 || xx < x + 3 || xx >= x + w - 3)
                    SetPixel(xx, yy, true);
    };
    // Header: time and battery, followed by a calm AI dashboard.  The raw
    // framebuffer intentionally uses geometry only: no LVGL, no antialiasing.

    time_t now = time(nullptr); struct tm tmv{}; localtime_r(&now, &tmv);
    DrawDigit(24, 24, 5, tmv.tm_hour / 10); DrawDigit(74, 24, 5, tmv.tm_hour % 10);
    DrawDigit(134, 24, 5, tmv.tm_min / 10); DrawDigit(184, 24, 5, tmv.tm_min % 10);
    DrawPercent(300, 28, 3, battery_percent_);
    // Six cards: AI important items, schedule, weather, Codex quota,
    // reading, and one user-configurable slot.  Data providers can replace
    // these pictograms without changing the display transport.
    for (int i = 0; i < 6; ++i) {
        const int col = i % 2, row = i / 2;
        const int x = 24 + col * 216, y = 150 + row * 150;
        box(x, y, 192, 120, false);
        // Simple pictograms remain legible without anti-aliased text/fonts.
        const int cx = x + 96, cy = y + 48;
        if (i == 0 || i == 1) { box(cx - 24, cy - 30, 48, 60, false); }
        else if (i == 2) { for (int k = -2; k <= 2; ++k) box(cx + k * 16 - 5, cy - 8, 10, 28, true); }
        else if (i == 3) { box(cx - 28, cy - 20, 56, 40, false); box(cx - 8, cy - 8, 16, 16, true); }
        else if (i == 4) { box(cx - 28, cy - 28, 56, 56, false); for (int k = -1; k <= 1; ++k) { SetPixel(cx + k * 14, cy, true); SetPixel(cx, cy + k * 14, true); } }
        else if (i == 5) { box(cx - 28, cy - 24, 56, 48, false); box(cx - 4, cy - 4, 8, 8, true); }
        else if (i == 6) { box(cx - 28, cy - 28, 56, 56, false); for (int k = -1; k <= 1; ++k) { SetPixel(cx + k * 16, cy - 20, true); SetPixel(cx + k * 16, cy + 20, true); } }
        else { box(cx - 30, cy - 20, 60, 40, false); for (int k = 0; k < 3; ++k) box(cx - 18 + k * 18, cy - 5, 10, 10, true); }
    }
}

void RawDisplay::FlushPartialLocked(int x, int y, int w, int h) {
    if (panel_region_fb_ == nullptr || !panel_history_valid_ || w <= 0 || h <= 0 ||
        (x & 7) != 0 || (w & 7) != 0 || x < 0 || y < 0 || x + w > kPanelW || y + h > kPanelH) {
        FlushLocked();
        return;
    }
    if (epaper_panel_wait_busy(panel_) != ESP_OK) return;
    const int stride = kPanelW / 8;
    const int row_bytes = w / 8;
    for (int row = 0; row < h; ++row) {
        std::memcpy(panel_region_fb_ + row * row_bytes,
                    panel_prev_fb_ + (y + row) * stride + x / 8, row_bytes);
    }
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_PREVIOUS);
    if (esp_lcd_panel_draw_bitmap(panel_, x, y, x + w, y + h, panel_region_fb_) != ESP_OK) return;
    for (int row = 0; row < h; ++row) {
        std::memcpy(panel_region_fb_ + row * row_bytes,
                    panel_fb_ + (y + row) * stride + x / 8, row_bytes);
    }
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_CURRENT);
    if (esp_lcd_panel_draw_bitmap(panel_, x, y, x + w, y + h, panel_region_fb_) != ESP_OK) return;
    epaper_panel_set_refresh_mode(panel_, SSD1677_EPAPER_REFRESH_PARTIAL);
    if (epaper_panel_refresh_screen(panel_) != ESP_OK) return;
    if (epaper_panel_wait_busy(panel_) != ESP_OK) return;
    std::memcpy(panel_prev_fb_, panel_fb_, panel_size_);
}

void RawDisplay::FlushLocked() {
    if (portrait_fb_ == nullptr || panel_fb_ == nullptr || panel_prev_fb_ == nullptr) return;

    // EPD updates are asynchronous.  Drain the previous waveform before
    // touching either VRAM plane; otherwise draw_bitmap rejects the write
    // with ESP_ERR_NOT_FINISHED and stale data gets refreshed repeatedly.
    if (epaper_panel_wait_busy(panel_) != ESP_OK) {
        ESP_LOGW(TAG, "wait BUSY before refresh failed");
        return;
    }

    for (int py = 0; py < kPortraitH; ++py) for (int px = 0; px < kPortraitW; ++px) {
        const bool white = (portrait_fb_[static_cast<size_t>(py) * (kPortraitW / 8) + (px >> 3)] & (0x80 >> (px & 7))) != 0;
        const int sx = py, sy = kPortraitW - 1 - px;
        uint8_t& b = panel_fb_[static_cast<size_t>(sy) * (kPanelW / 8) + (sx >> 3)];
        if (white) b |= static_cast<uint8_t>(0x80 >> (sx & 7)); else b &= static_cast<uint8_t>(~(0x80 >> (sx & 7)));
    }

    // Match EegoRead's unchanged-frame guard.  A redundant waveform is still
    // visible on e-paper (and needlessly consumes panel lifetime), even when
    // the rendered status bar did not change.
    if (panel_history_valid_ && std::memcmp(panel_prev_fb_, panel_fb_, panel_size_) == 0) {
        return;
    }

    // Keep the SSD1677 ordering from the validated bring-up image: current
    // plane first, then the auxiliary plane.  Unlike EegoRead's UC8279C,
    // SSD1677's full waveform bypasses RED, so do not reinterpret that plane
    // as a DTM1 register.
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_CURRENT);
    if (esp_lcd_panel_draw_bitmap(panel_, 0, 0, kPanelW, kPanelH, panel_fb_) != ESP_OK) {
        ESP_LOGW(TAG, "write current framebuffer failed");
        return;
    }
    epaper_panel_set_bitmap_color(panel_, SSD1677_EPAPER_BITMAP_PREVIOUS);
    if (esp_lcd_panel_draw_bitmap(panel_, 0, 0, kPanelW, kPanelH, panel_prev_fb_) != ESP_OK) {
        ESP_LOGW(TAG, "write auxiliary framebuffer failed");
        return;
    }

    const bool gc = !panel_history_valid_ || fast_refresh_count_ >= kFullRefreshEvery;
    /*
     * SSD1677's OTP FULL_FAST (0xD7) is not a one-flash waveform: it runs
     * several white/black phases and is visibly worse than a full-screen
     * differential update.  Use the controller's PARTIAL waveform for the
     * normal fast path instead.  With PREVIOUS and CURRENT already loaded
     * for the whole frame, this is the same single-pulse strategy used by
     * the LVGL adapter.  Keep the occasional OTP FULL pass for ghost cleanup.
     */
    epaper_panel_set_refresh_mode(panel_, gc ? SSD1677_EPAPER_REFRESH_FULL
                                             : SSD1677_EPAPER_REFRESH_PARTIAL);
    if (epaper_panel_refresh_screen(panel_) != ESP_OK) {
        ESP_LOGW(TAG, "EPD refresh start failed");
        return;
    }
    // Keep the software and controller histories in lockstep, and do not
    // start a second waveform while BUSY is asserted.
    if (epaper_panel_wait_busy(panel_) != ESP_OK) {
        ESP_LOGW(TAG, "wait BUSY after refresh failed");
        return;
    }
    std::memcpy(panel_prev_fb_, panel_fb_, panel_size_);
    panel_history_valid_ = true;
    fast_refresh_count_ = gc ? 0 : fast_refresh_count_ + 1;
}

void RawDisplay::UpdateStatusBar(bool update_all) {
    DisplayLockGuard lock(this);
    // Keep the deterministic bring-up image on screen; the application clock
    // timer otherwise redraws the battery UI one second after boot.
    if (!portrait_fb_) return;
    if (screen_test_mode_) {
        ++screen_test_tick_;
        if ((screen_test_tick_ % 3) == 0) {
            DrawTestPatternVariantLocked(screen_test_tick_ / 3);
            // Portrait (x=190..382,y=350..398) maps to panel
            // x=350..398, y=97..289 with the fixed 270-degree rotation.
            if ((screen_test_tick_ / 3) & 1) {
                FlushPartialLocked(344, 96, 56, 200);
            } else {
                FlushLocked();
            }
        }
        return;
    }
    bool discharging = false;
    Board::GetInstance().GetBatteryLevel(battery_percent_, charging_, discharging);
    time_t now = time(nullptr); struct tm tmv{}; localtime_r(&now, &tmv);
    if (!update_all && tmv.tm_min == last_minute_ && battery_percent_ == last_drawn_battery_ &&
        charging_ == last_drawn_charging_) return;
    last_minute_ = tmv.tm_min;
    last_drawn_battery_ = battery_percent_;
    last_drawn_charging_ = charging_;
    // Keep the launcher cards visible while updating the dynamic header.
    DrawHomeScreenLocked();
    FlushLocked();
}

void RawDisplay::SetStatus(const char*) {}
void RawDisplay::ShowNotification(const char*, int) {}
void RawDisplay::SetPowerSaveMode(bool) {}

void RawDisplay::ShowPoweredOffScreen() {
    DisplayLockGuard lock(this);
    if (!portrait_fb_) return;
    std::memset(portrait_fb_, kWhite, portrait_size_);
    FlushLocked();
}
