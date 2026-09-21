#pragma once

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_touch.h>
#include <esp_timer.h>

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "display.h"
#include "esp_lv_adapter.h"
#include "esp_mmap_assets.h"

struct EpdFlushCtx;

struct TouchVirtualKey {
    const char* name;
    int16_t x;
    int16_t y;
    /** 0=按下即 Click（旧行为）；>0=延迟到松手再 Click，并在阈值到达时发 LongPress。 */
    uint16_t long_press_ms = 0;
};

enum class TouchVkEvent : uint8_t {
    Click = 0,       // 短按确认
    LongPress = 1,   // 达到 long_press_ms（仅配置了长按的键）
    PressUp = 2,     // 松开（仅配置了长按的键）
};

using TouchVirtualKeyEventCb = bool (*)(const char* name, TouchVkEvent event, void* user_data);

class LVAdapterDisplay : public Display {
public:
    LVAdapterDisplay(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io,
                     esp_lcd_touch_handle_t touch_handle, int width, int height);
    ~LVAdapterDisplay() override;

    // 板级构造期间 Board::GetInstance() 会重入死锁，屏创建请用本接口。
    static LVAdapterDisplay* Instance() { return instance_; }

    // 盖板虚拟键：在 esp_lv_adapter 的 custom_touch_read 里识别，不另开 touch_feed 任务。
    // long_press_ms==0 的键保持「按下即 Click」；>0 的键走 Click/LongPress/PressUp 生命周期。
    void RegisterTouchVirtualKeys(const TouchVirtualKey* keys, size_t count,
                                  TouchVirtualKeyEventCb cb, void* user_data = nullptr);

    void SetEmotion(const char* emotion) override;
    void SetStatus(const char* status) override;
    void SetChatMessage(const char* role, const char* content) override;
    void SetTheme(Theme* theme) override;
    void ShowNotification(const char* notification, int duration_ms = 3000) override;
    void UpdateStatusBar(bool update_all = false) override;
    void SetPowerSaveMode(bool on) override;
    void SetPreviewImage(const void* image);

    // 由各 Screen 在 Create 时绑定当前页控件；切页时覆盖。
    void BindStatusWidgets(lv_obj_t* network, lv_obj_t* mute, lv_obj_t* battery_percent,
                           lv_obj_t* battery, lv_obj_t* status, lv_obj_t* notification,
                           lv_obj_t* low_battery_popup);

    // 状态栏标题前缀（如「数学老师」）；非空时 SetStatus 显示为「前缀·原状态」。
    // 传 nullptr / "" 清除。须在持锁外调用（内部自带锁）。
    void SetStatusTitlePrefix(const char* prefix);

    // 全屏 A2I1 关机画：直写 EPD 帧缓冲后全刷（失败则 LVGL「已关机」）。
    // 可在非 LVGL 线程调用（内部 DisplayLock；须 DRAM 栈，勿在 LVGL PSRAM 栈上跑）。
    void ShowPoweredOffScreen();

    // 下次 LVGL flush 走整屏局刷黑闪清残影（非 FULL_FAST）；消费一次后清零。
    void RequestNextFullRefresh();

private:
    bool Lock(int timeout_ms = 0) override;
    void Unlock() override;
    void SetupUI();
    // 已持 DisplayLock（或 LVGL 任务内）；按 prefix 规则写入 status_label_，并缓存正文。
    void ApplyStatusTextLocked(const char* status);
    // 已持锁/LVGL 上下文：把缓存的网络/静音/电池/状态灌进当前绑定的控件（首帧同屏画出）。
    void RestoreStatusWidgetsLocked();
    void UpdateBatteryWidgetsLocked(bool charging);

    lv_obj_t* network_label_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* notification_label_ = nullptr;
    lv_obj_t* mute_label_ = nullptr;
    lv_obj_t* battery_percent_label_ = nullptr;
    lv_obj_t* battery_label_ = nullptr;
    lv_obj_t* low_battery_popup_ = nullptr;

    char status_title_prefix_[48] = {};
    char status_body_cache_[32] = {};  // 不含前缀的状态正文（如「待命」/「12:34」）

    const char* battery_icon_ = nullptr;
    int battery_percent_ = -1;
    bool charging_ = false;
    const char* network_icon_ = nullptr;
    bool muted_ = false;
    std::chrono::system_clock::time_point last_status_update_time_;
    esp_timer_handle_t notification_timer_ = nullptr;

    EpdFlushCtx* epd_flush_ctx_ = nullptr;
    lv_indev_t* touch_indev_ = nullptr;
    mmap_assets_handle_t resources_assets_ = nullptr;

    static LVAdapterDisplay* instance_;
};
