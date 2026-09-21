#include "boot_diag.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <strings.h>

#include <driver/usb_serial_jtag.h>
#include <driver/usb_serial_jtag_vfs.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace boot_diag {
namespace {

constexpr const char* TAG = "BootDiag";
constexpr size_t kLineBytes = 96;

std::atomic<uint32_t> g_stage{static_cast<uint32_t>(Stage::kAppMain)};
std::atomic<bool> g_display_reader_active{false};
std::atomic<bool> g_started{false};

const char* ResetReasonName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_UNKNOWN:    return "unknown";
        case ESP_RST_POWERON:    return "poweron";
        case ESP_RST_EXT:        return "ext";
        case ESP_RST_SW:         return "sw";
        case ESP_RST_PANIC:      return "panic";
        case ESP_RST_INT_WDT:    return "int_wdt";
        case ESP_RST_TASK_WDT:   return "task_wdt";
        case ESP_RST_WDT:        return "wdt";
        case ESP_RST_DEEPSLEEP:  return "deepsleep";
        case ESP_RST_BROWNOUT:   return "brownout";
        case ESP_RST_SDIO:       return "sdio";
        case ESP_RST_USB:        return "usb";
        case ESP_RST_JTAG:       return "jtag";
        case ESP_RST_EFUSE:      return "efuse";
        case ESP_RST_PWR_GLITCH: return "pwr_glitch";
        case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
        default:                 return "?";
    }
}

// Replies use the driver directly, matching the product protocol, so the early
// service does not depend on the /dev/secondary VFS node being openable.
bool WriteAll(const char* data, size_t size) {
    if (data == nullptr || size == 0 || !usb_serial_jtag_is_driver_installed()) {
        return false;
    }
    size_t offset = 0;
    const int64_t deadline = esp_timer_get_time() + 1000000;
    while (offset < size && esp_timer_get_time() < deadline) {
        const int written = usb_serial_jtag_write_bytes(
            data + offset, std::min(size - offset, size_t(128)), pdMS_TO_TICKS(10));
        if (written < 0) {
            return false;
        }
        if (written > 0) {
            offset += static_cast<size_t>(written);
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    return offset == size;
}

void Reply(const char* text) {
    if (text == nullptr) {
        return;
    }
    flockfile(stdout);
    (void)WriteAll(text, std::strlen(text));
    funlockfile(stdout);
}

void HandleLine(const char* line) {
    const unsigned free_bytes =
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    const unsigned min_bytes =
        static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    const unsigned largest =
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    char response[192];

    if (strcasecmp(line, "PING") == 0) {
        Reply("@@DIAG_PONG\n");
        return;
    }
    if (strcasecmp(line, "BOOT?") == 0) {
        const auto stage = Current();
        const int size = std::snprintf(
            response, sizeof(response),
            "@@DIAG_BOOT stage=%s(%u) reset=%s uptime_ms=%lld heap=%u min=%u largest=%u\n",
            StageName(stage), static_cast<unsigned>(stage), ResetReasonName(esp_reset_reason()),
            static_cast<long long>(esp_timer_get_time() / 1000), free_bytes, min_bytes, largest);
        if (size > 0) {
            Reply(response);
        }
        return;
    }
    if (strcasecmp(line, "RESET?") == 0) {
        const int size = std::snprintf(response, sizeof(response), "@@DIAG_RESET reason=%s\n",
                                       ResetReasonName(esp_reset_reason()));
        if (size > 0) {
            Reply(response);
        }
        return;
    }
    if (strcasecmp(line, "HEAP?") == 0) {
        const int size = std::snprintf(response, sizeof(response),
                                       "@@DIAG_HEAP free=%u min=%u largest=%u\n", free_bytes,
                                       min_bytes, largest);
        if (size > 0) {
            Reply(response);
        }
        return;
    }

    // Report unhandled input so a tester can tell the early service is alive and
    // reading. Echo is bounded and sanitized; raw input is never sent back.
    char safe[40];
    size_t length = 0;
    for (; line[length] != '\0' && length < sizeof(safe) - 1; ++length) {
        const char c = line[length];
        safe[length] = (c >= 0x20 && c < 0x7F) ? c : '.';
    }
    safe[length] = '\0';
    const int size = std::snprintf(response, sizeof(response), "@@DIAG_UNHANDLED cmd=%s\n", safe);
    if (size > 0) {
        Reply(response);
    }
}

void DiagTask(void*) {
    char command[kLineBytes] = {};
    size_t command_size = 0;
    bool overflow = false;

    for (;;) {
        if (g_display_reader_active.load()) {
            // The product reader owns the RX path now. Stay idle so a command is
            // never split between two readers.
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        char input[64];
        int received = 0;
        if (usb_serial_jtag_is_driver_installed()) {
            received = usb_serial_jtag_read_bytes(input, sizeof(input), 0);
        }
        if (received <= 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        for (int i = 0; i < received; ++i) {
            const char c = input[i];
            if (c == '\r' || c == '\n') {
                if (overflow) {
                    overflow = false;
                    command_size = 0;
                    continue;
                }
                if (command_size == 0) {
                    continue;
                }
                command[command_size] = '\0';
                HandleLine(command);
                command_size = 0;
                continue;
            }
            if (command_size + 1 >= sizeof(command)) {
                overflow = true;
                command_size = 0;
                continue;
            }
            command[command_size++] = c;
        }
    }
}

}  // namespace

const char* StageName(Stage stage) {
    switch (stage) {
        case Stage::kAppMain:          return "app_main";
        case Stage::kNvsReady:         return "nvs_ready";
        case Stage::kAppStart:         return "app_start";
        case Stage::kHalBegin:         return "hal_begin";
        case Stage::kBoardI2c:         return "board_i2c";
        case Stage::kBoardSd:          return "board_sd";
        case Stage::kBoardPanel:       return "board_panel";
        case Stage::kBoardDisplay:     return "board_display";
        case Stage::kBoardReady:       return "board_ready";
        case Stage::kHomeShown:        return "home_shown";
        case Stage::kProvidersStarted: return "providers_started";
        default:                       return "?";
    }
}

void Mark(Stage stage) {
    g_stage.store(static_cast<uint32_t>(stage));
}

Stage Current() {
    return static_cast<Stage>(g_stage.load());
}

void SetDisplayReaderActive() {
    g_display_reader_active.store(true);
}

bool DisplayReaderActive() {
    return g_display_reader_active.load();
}

void Init() {
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) {
        return;
    }

    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config{.tx_buffer_size = 1024, .rx_buffer_size = 1024};
        const esp_err_t result = usb_serial_jtag_driver_install(&config);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "early serial driver unavailable: %s", esp_err_to_name(result));
            return;
        }
    }
    usb_serial_jtag_vfs_use_driver();

    if (xTaskCreatePinnedToCore(DiagTask, "boot_diag", 4096, nullptr, 2, nullptr, 0) != pdPASS) {
        ESP_LOGE(TAG, "boot_diag task creation failed");
        return;
    }
    ESP_LOGI(TAG, "early diagnostic ready (PING; BOOT?; RESET?; HEAP?)");
}

}  // namespace boot_diag
