#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_event.h>
#include <cstdlib>
#include <ctime>

#include "application.h"
#include "system/boot_diag.h"

#define TAG "main"

extern "C" void app_main(void)
{
    // Start the early diagnostic channel before anything else can fail, so a
    // board that never reaches the product UI can still be interrogated.
    boot_diag::Mark(boot_diag::Stage::kAppMain);
    boot_diag::Init();

    // System/Unix time stays UTC; RTC and UI use the device's local timezone.
    setenv("TZ", "CST-8", 1);
    tzset();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    boot_diag::Mark(boot_diag::Stage::kNvsReady);

    Application::GetInstance().Start();
}
