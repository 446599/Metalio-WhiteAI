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
#include "ui_work_queue.h"
#include <atomic>

#define MAIN_EVENT_SCHEDULE (1 << 0)
#define MAIN_EVENT_CLOCK_TICK (1 << 6)
#define MAIN_EVENT_UI (1 << 7)
#define MAIN_EVENT_AI_FOCUS (1 << 8)

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
    bool ScheduleUi(std::function<void()> callback);
    void RequestStatusUpdate(bool force = false);
    void RequestAiFocus();
    uint32_t DroppedUiInputs() const { return ui_tasks_.Dropped(); }
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
    UiWorkQueue ui_tasks_;
    std::atomic_bool force_status_update_{false};
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    volatile DeviceState device_state_ = kDeviceStateUnknown;
    TaskHandle_t main_event_loop_task_handle_ = nullptr;
};

#endif  // _APPLICATION_H_
