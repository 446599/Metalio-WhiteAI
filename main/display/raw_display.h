#pragma once

#include "display.h"
#include "font/ai_ui_assets.h"
#include "dashboard/dashboard_data.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_touch.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>

class RawDisplay final : public Display {
public:
    RawDisplay(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io,
               esp_lcd_touch_handle_t touch, int width, int height);
    ~RawDisplay() override;

    void UpdateStatusBar(bool update_all = false) override;
    void ShowNotification(const char* notification, int duration_ms = 3000) override;
    void SetStatus(const char* status) override;
    void SetPowerSaveMode(bool on) override;

    void RegisterTouchVirtualKeys(const void*, size_t, void (*)(const char*, int, void*), void* = nullptr) {}
    void ShowScreenTestPattern();
    void ShowGray4TestPattern();
    void ShowWipeTestPattern();
    bool StartAnimationTest(bool incremental_du = true);
    bool StartWavefrontTest();
    bool StartPaperMonoTest();
    bool StartPaperMonoTextTest();
    bool StartPaperMonoPageTest();
    void ShowHomeScreen();
    void ShowProductHomeScreen();
    void ShowPoweredOffScreen();
    static RawDisplay* Instance() { return instance_; }

protected:
    bool Lock(int timeout_ms = 0) override;
    void Unlock() override;

private:
    enum class ProductPage : uint8_t {
        Home,
        AiResult,
        AiSteps,
        QuickNote,
        Reader,
        TodayList,
        CardBox,
        CardDetail,
        Keep,
        Apps,
        Workbench,
        Settings,
        Confirmation,
        More,
    };

    void DrawHomeScreenLocked();
    void DrawLegacyDashboardScreenLocked();
    void DrawTestConsoleLocked();
    void DrawProductScreenLocked();
    void DrawProductStatusBarLocked(const char* section);
    void DrawProductFooterLocked(const char* first, const char* second,
                                 const char* third, const char* fourth);
    void DrawProductButtonLocked(int x, int y, int width, int height,
                                 const char* label, bool filled = false);
    void DrawProductHomeLocked();
    void DrawProductAppsLocked();
    void DrawProductAiLocked(bool details);
    void DrawProductQuickNoteLocked();
    void DrawProductReaderLocked();
    void DrawProductTodayListLocked();
    void DrawProductCardBoxLocked();
    void DrawProductCardDetailLocked();
    void DrawProductKeepLocked();
    void DrawProductWorkbenchLocked();
    void DrawProductSettingsLocked();
    void DrawProductConfirmationLocked();
    void DrawProductMoreLocked();
    void DrawTestPatternLocked();
    void DrawTestPatternVariantLocked(uint32_t step);
    void FlushLocked();
    void FlushPartialLocked(int x, int y, int w, int h, bool incremental_du = false);
    void FlushGray4Locked(const uint8_t* lsb, const uint8_t* msb,
                          bool invert_planes, bool swap_planes,
                          const uint8_t* lut, size_t lut_size);
    void FlushWipeTestLocked(int strip_width);
    bool RecoverPanelForBinaryLocked();
    void UpdateGlassBinaryLocked(int x, int y, int w, int h);
    void RunAnimationTestLocked(bool incremental_du);
    void RunAnimationTest(bool incremental_du);
    void RunAnimationFrameLocked(bool incremental_du, int frame);
    void RunWavefrontTestLocked();
    void RunPaperMonoTestLocked();
    void RunPaperMonoTextTestLocked();
    void RunPaperMonoPageTestLocked();
    static void AnimationTaskEntry(void* arg);
    void AnimationTask();
    void SendAnimationResultToSerial() const;
    void SetPixel(int x, int y, bool black);
    void FillRect(int x, int y, int w, int h, bool black);
    void StrokeRect(int x, int y, int w, int h, int thickness);
    void FillRoundRect(int x, int y, int w, int h, int radius, bool black);
    void StrokeRoundRect(int x, int y, int w, int h, int radius, int thickness);
    void FillCircle(int cx, int cy, int radius, bool black);
    void StrokeCircle(int cx, int cy, int radius, int thickness);
    void DrawText(int x, int y, const char* text, const ui_font_t& font);
    void DrawTextInk(int x, int y, const char* text, const ui_font_t& font, bool black);
    void SetPaperMonoClassPixelLocked(int x, int y, uint8_t level);
    void FillPaperMonoClassRectLocked(int x, int y, int w, int h, uint8_t level);
    void DrawPaperMonoTextLocked(int x, int y, const char* text,
                                 const ui_font_t& font, uint8_t ink_level);
    void DrawPaperMonoPageLocked(bool page_b, int origin_x);
    int TextWidth(const char* text, const ui_font_t& font) const;
    void FitText(const char* text, const ui_font_t& font, int max_width,
                 char* out, size_t out_size) const;
    void FitTextLines(const char* text, const ui_font_t& font, int max_width,
                      char* line1, size_t line1_size, char* line2,
                      size_t line2_size) const;
    void DrawTextCentered(int x, int y, int width, int height, const char* text,
                          const ui_font_t& font);
    void DrawTextLinesCentered(int x, int y, int width, int height,
                               const char* line1, const char* line2,
                               const ui_font_t& font);
    void DrawCardIcon(int card, int x, int y);
    void DrawBattery(int x, int y, int percent, bool charging);
    static void TouchTaskEntry(void* arg);
    void TouchTask();
    void HandleHomeTap(int x, int y);
    static void FrameDumpTaskEntry(void* arg);
    void FrameDumpTask();
    void DumpFrameToSerial(int fd, bool panel_frame);
    void DrawDigit(int x, int y, int scale, int digit);
    void DrawPercent(int x, int y, int scale, int value);

    esp_lcd_panel_handle_t panel_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_touch_handle_t touch_ = nullptr;
    uint8_t* portrait_fb_ = nullptr;
    uint8_t* panel_fb_ = nullptr;
    uint8_t* panel_prev_fb_ = nullptr;
    uint8_t* panel_region_fb_ = nullptr;
    uint8_t* frame_dump_fb_ = nullptr;
    // Paper Mono style software model. A set bit means the glass is currently
    // non-white / black respectively; it is rebuilt after every committed
    // waveform and is deliberately independent from the controller RAM roles.
    uint8_t* glass_nonwhite_fb_ = nullptr;
    uint8_t* glass_black_fb_ = nullptr;
    bool panel_history_valid_ = false;
    bool window_baseline_valid_ = false;
    // A custom gray/Paper Mono activation changes the controller LUT and RAM
    // roles.  Reinitialize once before the next binary page/animation so a
    // resident warm waveform cannot strand BUSY high or poison the diff base.
    bool panel_custom_waveform_active_ = false;
    uint32_t fast_refresh_count_ = 0;
    bool screen_test_mode_ = false;
    bool test_console_mode_ = true;
    uint8_t test_variant_ = 0;
    uint32_t animation_frames_ = 0;
    int64_t animation_total_us_ = 0;
    int64_t animation_frame_us_[12]{};
    std::atomic_bool animation_mode_du_{true};
    std::atomic_bool animation_wavefront_{false};
    std::atomic_bool animation_papermono_{false};
    std::atomic_bool animation_paper_text_{false};
    std::atomic_bool animation_paper_page_{false};
    bool paper_page_glass_uncertain_ = false;
    // Non-negative only while rendering a clipped, 400-pixel page for the
    // translated three-level page-turn test.
    int paper_page_draw_origin_x_ = -1;
    uint32_t screen_test_tick_ = 0;
    size_t portrait_size_ = 0;
    size_t panel_size_ = 0;
    SemaphoreHandle_t mutex_ = nullptr;
    int battery_percent_ = 0;
    bool charging_ = false;
    bool power_save_ = false;
    int last_minute_ = -1;
    int last_drawn_battery_ = -1;
    bool last_drawn_charging_ = false;
    uint32_t last_dashboard_revision_ = 0;
    char status_text_[48]{};
    char notification_text_[96]{};
    int64_t notification_deadline_ms_ = 0;
    TaskHandle_t touch_task_ = nullptr;
    TaskHandle_t frame_dump_task_ = nullptr;
    TaskHandle_t animation_task_ = nullptr;
    volatile bool frame_dump_stop_ = false;
    std::atomic_bool animation_running_{false};
    uint32_t frame_dump_sequence_ = 0;
    bool touch_down_ = false;
    int touch_start_x_ = 0;
    int touch_start_y_ = 0;
    int touch_last_x_ = 0;
    int touch_last_y_ = 0;
    int64_t touch_start_ms_ = 0;
    bool ai_listening_ = false;
    ProductPage product_page_ = ProductPage::Home;
    uint8_t quick_note_state_ = 0;
    uint8_t reader_page_ = 0;
    static RawDisplay* instance_;
};
