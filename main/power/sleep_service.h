#pragma once
#include "activity.h"
#include <atomic>
#include <cstdint>
#include <string>
namespace power {
class SleepService {
public:
    static SleepService& Instance();
    void Start();
    // Only atomics here: button/esp_timer callbacks must not do NVS or draw.
    void Toggle();
    void PowerKeyDown();
    void PowerKeyClick();
    void Wake();
    void Tick(); // application event task, internal stack
    std::string Status()const;
private:
    void Unlock(const char* message=nullptr);
    bool RestorePeripherals();
    bool Busy()const;
    bool StorageIdle()const;
    bool radio_suspended_=false,audio_suspended_=false,pa_was_high_=false,pa_changed_=false;
    bool blocked_=false,buttons_paused_=false;
    std::atomic<bool> prepared_{false},started_{false};
    std::atomic<bool> toggle_{false},wake_{false},key_woke_{false};
    std::atomic<int64_t> suppress_click_until_{0};
    std::atomic<uint32_t> sleeps_{0},failures_{0};
    std::atomic<int64_t> slept_us_{0};
    int64_t preparing_since_=0,retry_restore_ms_=0;
    std::atomic<unsigned> idle_seconds_{300};
};
}
