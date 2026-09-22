#pragma once

#include "display.h"
#include "font/ai_ui_assets.h"
#include "dashboard/dashboard_data.h"
#include "reminders/alert_state.h"
#include "icons/lucide_icons.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_touch.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include "input/text_input.h"
#include "network/setup_model.h"
#include "notes/note_store.h"
#include "notes_layout.h"

class RawDisplay final : public Display {
public:
    enum class HardwareKey : uint8_t {
        Previous,
        Next,
        Select,
        Back,
        Home,
    };

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
    // Route the board's physical keys into the product UI.  The return value
    // lets the board keep its legacy volume behavior outside the product
    // pages (for example while a diagnostic console is active).
    bool HandleHardwareKey(HardwareKey key);
    // Non-blocking with respect to display/TLS, callable from button callbacks.
    bool HandleAiKey(bool down);
    void ShowAiConversation();
    void ShowReminderAlert(const reminders::AlertSnapshot& alert);
    struct DeviceSnapshot { std::string app; int battery; bool charging, sleeping; };
    DeviceSnapshot SystemSnapshot();
    // Called only by the UI task, never from the network callback.
    bool OpenSystemApp(const std::string& app, const std::string& date = "");
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
        Alarm,
        Recorder,
        Notes,
        NoteDetail,
        WifiList,
        WifiCredentials,
        TextEntry,
        NoteCompose,
        ChatList,
        ChatDetail,
    };

    enum class EditTarget { None, WifiSsid, WifiPassword, NoteTitle, NoteProject, NoteBody, NoteSearch, ChatMessage };
    void DrawProductHistoryLocked(bool detail);
    bool HandleHistoryTap(int x,int y);
    bool HandleHistoryKey(HardwareKey key);
    bool HandleReaderTap(int x,int y);
    bool HandleReaderKey(HardwareKey key);
    void DrawProductQuickControlsLocked();
    void SetQuickControls(bool open);
    bool HandleQuickPull(int x0,int y0,int x1,int y1,int held_ms);
    bool HandleQuickTap(int x,int y);
    bool HandleQuickKey(HardwareKey key);
    void DrawProductWifiLocked(bool credentials);
    void DrawProductTextEntryLocked();
    void DrawProductNoteComposeLocked();
    void DrawProductDiscardLocked();
    bool HandleSetupTap(int x, int y);
    bool HandleSetupKey(HardwareKey key);
    void OpenEditorLocked(EditTarget target);
    void OpenNoteEditorLocked(bool existing);
    void OpenConversationNote();
    void FinishEditorLocked(bool accept);
    void LeaveFormLocked(ProductPage destination);
    void ClearFormLocked();
    void AdvanceFormsLocked();
    void DrawHomeScreenLocked();
    void DrawLegacyDashboardScreenLocked();
    void DrawTestConsoleLocked();
    void DrawProductScreenLocked();
    void DrawReminderAlertLocked();
    bool HandleReminderTap(int x, int y);
    void DrawProductStatusBarLocked();
    void DrawProductControlRailLocked(const char* context);
    void DrawProductLabelLocked(int x, int y, int width, const char* text, const ui_font_t& font);
    void DrawProductChevronLocked(int x, int y);
    void DrawProductHeadingLocked(const char* title, const char* index);
    void DrawProductClockLocked(int x, int y, const char* text);
    void DrawProductHomeLocked();
    void DrawProductAppIconLocked(int icon, int x, int y);
    void DrawProductIconLocked(lucide::Id id, int x, int y, int size, bool black);
    void DrawProductIconRowLocked(int y, lucide::Id icon, const char* title, const char* detail, bool selected);
    void DrawProductNotesLocked(bool detail);
    void DrawProductAlarmLocked();
    void DrawProductRecorderLocked();
    void DrawProductAppsLocked();
    void DrawProductAiLocked(bool details);
    void DrawProductQuickNoteLocked();
    void DrawProductReaderLocked();
    void DrawProductTodayListLocked();
    void DrawProductCardBoxLocked();
    bool SelectCardSnapshotLocked(int selected);
    void DrawProductCardDetailLocked();
    void DrawProductKeepLocked();
    void DrawProductWorkbenchLocked();
    void DrawProductSettingsLocked();
    void DrawProductConfirmationLocked();
    void DrawProductMoreLocked();
    void DrawTestPatternLocked();
    void DrawTestPatternVariantLocked(uint32_t step);
    void FlushLocked();
    bool FlushBlackPulseLocked();
    bool FlushPartialLocked(int x, int y, int w, int h, bool incremental_du = false);
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

    std::atomic_bool quick_controls_open_{false};
    bool quick_bluetooth_=false;
    int quick_ble_page_=0;
    uint32_t last_quick_revision_=0,last_reader_revision_=0;
    int book_list_page_=0;
    uint32_t last_history_revision_=0;
    int history_list_page_=0,history_text_page_=0,history_text_pages_=1;
    bool history_delete_confirm_=false;
    std::string chat_draft_;

    std::string selected_card_title_,selected_card_text_;
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
    size_t refresh_changed_bytes_ = 0;
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
    reminders::AlertSnapshot reminder_alert_;
    int64_t reminder_visible_since_ms_ = 0;
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
    ProductPage product_page_ = ProductPage::Home;
    ProductPage app_parent_ = ProductPage::Home;
    int navigation_index_ = 0;
    std::atomic_bool ai_key_down_{false};
    int ai_text_page_ = 0;
    int ai_page_count_ = 1;
    bool ai_show_transcript_ = false;
    bool voice_note_mode_ = false;
    uint32_t ai_drawn_turn_ = 0;
    uint32_t last_conversation_revision_ = 0;
    uint8_t quick_note_state_ = 0;

    int calendar_month_ = 0;
    int calendar_day_ = 0;
    int calendar_events_page_ = 0;
    int alarm_page_ = 0;
    int alarm_pages_ = 1;
    uint32_t alarm_ids_[4]{};
    bool alarm_enabled_[4]{};
    uint32_t last_recorder_revision_ = 0;
    uint32_t last_notes_revision_ = 0;
    int notes_page_ = 0, notes_pages_ = 1, note_text_page_ = 0, note_text_pages_ = 1;
    uint32_t note_id_ = 0, note_ids_[6]{};
    input::Editor editor_;
    EditTarget edit_target_ = EditTarget::None;
    ProductPage editor_parent_ = ProductPage::WifiCredentials, discard_destination_ = ProductPage::Home;
    std::atomic_bool form_active_{false};
    bool discard_pending_ = false, draft_dirty_ = false, password_reveal_ = false, symbols_second_ = false;
    network::AccessPoint selected_ap_;
    std::string wifi_password_, form_message_, notes_query_;
    notes::Note draft_note_;
    int wifi_page_ = 0;
    bool wifi_manual_ = false, wifi_switch_confirm_ = false;
    uint32_t last_wifi_revision_ = 0, last_writer_revision_ = 0, note_save_operation_ = 0;
    static RawDisplay* instance_;
};
