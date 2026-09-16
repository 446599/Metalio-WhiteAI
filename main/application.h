#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

#include "device_state_event.h"

#define MAIN_EVENT_SCHEDULE (1 << 0)
#define MAIN_EVENT_CLOCK_TICK (1 << 6)

// 硬件测试固件：保留板级 / 状态栏所需的最小 Application 表面。
class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void Start();
    void MainEventLoop();
    DeviceState GetDeviceState() const { return device_state_; }
    void SetDeviceState(DeviceState state);
    void Schedule(std::function<void()> callback);
    void Alert(const char* status, const char* message, const char* emotion = "",
               const std::string_view& sound = "");
    void DismissAlert() {}
    void Reboot();
    void PlaySound(const std::string_view& sound);
    bool IsVoiceDetected() const { return false; }

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    volatile DeviceState device_state_ = kDeviceStateUnknown;
    TaskHandle_t main_event_loop_task_handle_ = nullptr;
};

#endif  // _APPLICATION_H_
