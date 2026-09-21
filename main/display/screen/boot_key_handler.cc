#include "boot_key_handler.h"

#include <atomic>

#include <esp_log.h>

#include "vk_key_handler.h"

namespace {

constexpr const char* TAG = "BootKey";
std::atomic<bool> s_boot_held{false};
std::atomic<bool> s_long_press_fired{false};

bool DispatchBootAction(BootKeyAction action, const char* kind, const char* screen) {
    if (action == nullptr) {
        ESP_LOGI(TAG, "%s no-op (screen=%s has no handler)", kind, screen);
        return false;
    }
    if (!action()) {
        ESP_LOGI(TAG, "%s ignored by screen=%s", kind, screen);
        return false;
    }
    ESP_LOGI(TAG, "%s handled by screen=%s", kind, screen);
    return true;
}

}  // namespace

void BootKey_OnPressDown() {
    s_boot_held.store(true, std::memory_order_release);
    s_long_press_fired.store(false, std::memory_order_release);
    const char* screen = VkKey_ActiveScreen();
    ESP_LOGI(TAG, "press-down on screen=%s", screen);
    DispatchBootAction(VkKey_GetBootPressDown(screen), "press-down", screen);
}

void BootKey_OnPressUp() {
    s_boot_held.store(false, std::memory_order_release);
    const char* screen = VkKey_ActiveScreen();
    ESP_LOGI(TAG, "press-up on screen=%s", screen);
    DispatchBootAction(VkKey_GetBootPressUp(screen), "press-up", screen);
}

void BootKey_OnClick() {
    // 与长按开听互斥：本轮已 long-press 则不再派发短按（打断）
    if (s_long_press_fired.load(std::memory_order_acquire)) {
        ESP_LOGI(TAG, "short-press suppressed (long-press already fired this press)");
        return;
    }
    const char* screen = VkKey_ActiveScreen();
    ESP_LOGI(TAG, "short-press on screen=%s", screen);
    DispatchBootAction(VkKey_GetBootClick(screen), "short-press", screen);
}

void BootKey_OnLongPress() {
    s_long_press_fired.store(true, std::memory_order_release);
    const char* screen = VkKey_ActiveScreen();
    ESP_LOGI(TAG, "long-press on screen=%s", screen);
    DispatchBootAction(VkKey_GetBootLongPress(screen), "long-press", screen);
}

bool BootKey_IsHeld() {
    return s_boot_held.load(std::memory_order_acquire);
}

bool BootKey_DidLongPress() {
    return s_long_press_fired.load(std::memory_order_acquire);
}
