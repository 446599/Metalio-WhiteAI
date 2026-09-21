#include "haptic_feedback.h"

#include "board.h"
#include "settings.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {

constexpr const char* TAG = "Haptic";
constexpr const char* kNvsNs = "display";
constexpr const char* kNvsKey = "haptic";
constexpr bool kDefaultEnabled = true;

bool s_enabled = kDefaultEnabled;
bool s_ready = false;

void LoadFromNvs() {
    Settings settings(kNvsNs, false);
    s_enabled = settings.GetBool(kNvsKey, kDefaultEnabled);
    s_ready = true;
    ESP_LOGI(TAG, "nvs load haptic=%d", s_enabled ? 1 : 0);
}

void PersistTask(void* arg) {
    const bool enabled = (arg != nullptr);
    Settings settings(kNvsNs, true);
    settings.SetBool(kNvsKey, enabled);
    ESP_LOGI(TAG, "nvs save haptic=%d", enabled ? 1 : 0);
    vTaskDelete(nullptr);
}

}  // namespace

bool HapticIsEnabled(void) {
    if (!s_ready) {
        LoadFromNvs();
    }
    return s_enabled;
}

void HapticSetEnabled(bool enabled) {
    s_enabled = enabled;
    s_ready = true;
    // 在独立任务中落盘，避免阻塞按键处理。
    if (xTaskCreate(PersistTask, "haptic_nvs", 4096, enabled ? reinterpret_cast<void*>(1) : nullptr, 5,
                    nullptr) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(haptic_nvs) failed");
    }
}

void HapticPulseIfEnabled(void) {
    if (!HapticIsEnabled()) {
        return;
    }
    Board::GetInstance().PulseVibration();
}
