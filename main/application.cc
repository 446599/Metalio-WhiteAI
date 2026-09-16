#include "application.h"

#include "board.h"
#include "display.h"
#include "display/raw_display.h"
#include "dashboard/dashboard_service.h"
#include "hal/hal.h"
#include "xiaozhi/xiaozhi_client.h"

#include <esp_log.h>
#include <esp_system.h>

#define TAG "Application"

namespace {

Application* s_app_for_timer = nullptr;

void MainEventLoopTask(void* arg) {
    auto* app = static_cast<Application*>(arg);
    app->MainEventLoop();
}

void OnClockTimer(void* /*arg*/) {
    if (s_app_for_timer == nullptr) {
        return;
    }
    s_app_for_timer->Schedule([]() {
        if (auto* display = Board::GetInstance().GetDisplay()) {
            display->UpdateStatusBar(false);
        }
    });
}

}  // namespace

Application::Application() {
    event_group_ = xEventGroupCreate();
    s_app_for_timer = this;
    device_state_ = kDeviceStateStarting;

    esp_timer_create_args_t clock_timer_args = {
        .callback = OnClockTimer,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&clock_timer_args, &clock_timer_handle_));
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
    s_app_for_timer = nullptr;
}

void Application::Start() {
    ESP_LOGI(TAG, "start raw AI dashboard firmware (no LVGL)");
    SetDeviceState(kDeviceStateStarting);

    GetHAL().Init();

    SetDeviceState(kDeviceStateIdle);

    // Home-screen text wrapping keeps a bounded glyph-unit table on the
    // caller's stack.  The 4 KiB event task overflowed on the first periodic
    // dashboard refresh; keep enough headroom for rendering and callbacks.
    const BaseType_t event_task_result =
        xTaskCreatePinnedToCore(MainEventLoopTask, "main_event", 8192, this, 3,
                                &main_event_loop_task_handle_, 0);
    if (event_task_result != pdPASS) {
        main_event_loop_task_handle_ = nullptr;
        ESP_LOGE(TAG, "main event task creation failed; keeping providers disabled");
    } else {
        ESP_ERROR_CHECK(esp_timer_start_periodic(clock_timer_handle_, 1000000ULL));
    }

    if (auto* raw = RawDisplay::Instance()) {
        raw->ShowProductHomeScreen();
    } else if (auto* display = Board::GetInstance().GetDisplay()) {
        display->UpdateStatusBar(true);
    }

    // Providers run on their own bounded tasks.  The display remains usable
    // offline while weather, quota and Xiaozhi refresh in the background.
    if (event_task_result == pdPASS) {
        dashboard::DashboardService::GetInstance().Start();
        xiaozhi::Client::GetInstance().Start();
    }
}

void Application::MainEventLoop() {
    while (true) {
        EventBits_t bits =
            xEventGroupWaitBits(event_group_, MAIN_EVENT_SCHEDULE, pdTRUE, pdFALSE, portMAX_DELAY);
        if (bits & MAIN_EVENT_SCHEDULE) {
            std::deque<std::function<void()>> tasks;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                tasks.swap(main_tasks_);
            }
            for (auto& task : tasks) {
                task();
            }
        }
    }
}

void Application::Schedule(std::function<void()> callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::SetDeviceState(DeviceState state) {
    if (device_state_ == state) {
        return;
    }
    DeviceState previous = device_state_;
    device_state_ = state;
    DeviceStateEventManager::GetInstance().PostStateChangeEvent(previous, state);
}

void Application::Alert(const char* status, const char* message, const char* /*emotion*/,
                        const std::string_view& /*sound*/) {
    ESP_LOGW(TAG, "Alert: %s | %s", status != nullptr ? status : "",
             message != nullptr ? message : "");
    if (auto* display = Board::GetInstance().GetDisplay()) {
        if (status != nullptr && status[0] != '\0') {
            display->SetStatus(status);
        }
        if (message != nullptr && message[0] != '\0') {
            display->ShowNotification(message, 5000);
        }
    }
}

void Application::Reboot() {
    ESP_LOGW(TAG, "Reboot");
    esp_restart();
}

void Application::PlaySound(const std::string_view& /*sound*/) {}
