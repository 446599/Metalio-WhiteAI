#ifndef DISPLAY_H
#define DISPLAY_H

#include <esp_timer.h>
#include <esp_log.h>
#include <esp_pm.h>

#include <string>
#include <chrono>
#include <cstdint>

class Theme {
public:
    Theme(const std::string& name) : name_(name) {}
    virtual ~Theme() = default;

    inline std::string name() const { return name_; }
private:
    std::string name_;
};

/** Idle 时状态栏中部文案策略（由当前 Screen 在进/离页设置）。 */
enum class IdleStatusMode : uint8_t {
    kClock = 0,      // 周期刷新 HH:MM（首页等）
    kFixedText = 1,  // 固定文案，不刷时钟（会话页 Idle 常用「待命」）
};

class Display {
public:
    Display();
    virtual ~Display();

    virtual void SetStatus(const char* status);
    virtual void ShowNotification(const char* notification, int duration_ms = 3000);
    virtual void ShowNotification(const std::string &notification, int duration_ms = 3000);
    virtual void SetEmotion(const char* emotion);
    virtual void SetChatMessage(const char* role, const char* content);
    virtual void SetTheme(Theme* theme);
    virtual Theme* GetTheme() { return current_theme_; }
    virtual void UpdateStatusBar(bool update_all = false);
    virtual void SetPowerSaveMode(bool on);

    // Idle 状态栏策略。切页时由 Screen 设置；离页应恢复 kClock，避免策略泄漏到下一页。
    // fixed_text 仅 kFixedText 有效；nullptr/"" 时 ApplyIdle 侧回退为「待命」。
    void SetIdleStatusMode(IdleStatusMode mode, const char* fixed_text = nullptr);
    IdleStatusMode GetIdleStatusMode() const { return idle_status_mode_; }
    const char* GetIdleStatusFixedText() const { return idle_status_fixed_text_; }
    // UpdateStatusBar / ApplyIdle 共用：是否允许把中部状态刷成时钟。
    bool AllowsIdleStatusClock() const;

    inline int width() const { return width_; }
    inline int height() const { return height_; }

protected:
    int width_ = 0;
    int height_ = 0;

    Theme* current_theme_ = nullptr;
    IdleStatusMode idle_status_mode_ = IdleStatusMode::kClock;
    char idle_status_fixed_text_[32] = {};

    friend class DisplayLockGuard;
    virtual bool Lock(int timeout_ms = 0) = 0;
    virtual void Unlock() = 0;
};


class DisplayLockGuard {
public:
    DisplayLockGuard(Display *display) : display_(display) {
        // Display updates can legitimately take longer than 30 seconds while
        // an experimental waveform is running.  Waiting here keeps callers
        // from touching the framebuffer without ownership of the mutex.  A
        // guard that failed to acquire the lock must never release it later.
        locked_ = display_ != nullptr && display_->Lock(-1);
        if (!locked_) {
            ESP_LOGE("Display", "Failed to lock display");
        }
    }
    ~DisplayLockGuard() {
        if (locked_) display_->Unlock();
    }

private:
    Display *display_ = nullptr;
    bool locked_ = false;
};

class NoDisplay : public Display {
private:
    virtual bool Lock(int timeout_ms = 0) override {
        return true;
    }
    virtual void Unlock() override {}
};

#endif
