#include "hal.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

namespace {

constexpr const char* kTag = "HalBle";

std::mutex s_ble_api_mu;
std::mutex s_ble_dev_mu;
std::vector<HalBtDevice> s_ble_devs;
SemaphoreHandle_t s_sync_sem = nullptr;
SemaphoreHandle_t s_disc_done_sem = nullptr;
std::atomic<bool> s_synced{false};
std::atomic<bool> s_host_running{false};

void EnsureSems() {
    if (s_sync_sem == nullptr) {
        s_sync_sem = xSemaphoreCreateBinary();
    }
    if (s_disc_done_sem == nullptr) {
        s_disc_done_sem = xSemaphoreCreateBinary();
    }
}

std::string FormatBleAddr(const ble_addr_t& addr) {
    char buf[18];
    std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", addr.val[5], addr.val[4],
                  addr.val[3], addr.val[2], addr.val[1], addr.val[0]);
    return buf;
}

void UpsertDevice(const std::string& addr, const std::string& name, int8_t rssi) {
    std::lock_guard<std::mutex> lock(s_ble_dev_mu);
    for (auto& d : s_ble_devs) {
        if (d.addr == addr) {
            if (rssi > d.rssi) {
                d.rssi = rssi;
            }
            if (!name.empty() && (d.name.empty() || d.name == d.addr)) {
                d.name = name;
            }
            return;
        }
    }
    HalBtDevice dev;
    dev.addr = addr;
    dev.name = name.empty() ? addr : name;
    dev.rssi = rssi;
    s_ble_devs.push_back(std::move(dev));
}

int OnGapEvent(struct ble_gap_event* event, void* /*arg*/) {
    switch (event->type) {
        case BLE_GAP_EVENT_DISC: {
            struct ble_hs_adv_fields fields;
            std::memset(&fields, 0, sizeof(fields));
            std::string name;
            if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) == 0) {
                if (fields.name != nullptr && fields.name_len > 0) {
                    name.assign(reinterpret_cast<const char*>(fields.name), fields.name_len);
                }
            }
            UpsertDevice(FormatBleAddr(event->disc.addr), name, event->disc.rssi);
            return 0;
        }
        case BLE_GAP_EVENT_DISC_COMPLETE:
            ESP_LOGI(kTag, "DISC_COMPLETE reason=%d", event->disc_complete.reason);
            if (s_disc_done_sem != nullptr) {
                xSemaphoreGive(s_disc_done_sem);
            }
            return 0;
        default:
            return 0;
    }
}

void OnReset(int reason) {
    ESP_LOGW(kTag, "nimble reset reason=%d", reason);
    s_synced.store(false);
}

void OnSync() {
    ESP_LOGI(kTag, "nimble synced");
    s_synced.store(true);
    if (s_sync_sem != nullptr) {
        xSemaphoreGive(s_sync_sem);
    }
}

void HostTask(void* /*param*/) {
    ESP_LOGI(kTag, "host task start");
    s_host_running.store(true);
    nimble_port_run();
    s_host_running.store(false);
    nimble_port_freertos_deinit();
    ESP_LOGI(kTag, "host task exit");
}

bool StartStack(std::string& detail) {
    EnsureSems();
    s_synced.store(false);
    xSemaphoreTake(s_sync_sem, 0);

    ble_hs_cfg.reset_cb = OnReset;
    ble_hs_cfg.sync_cb = OnSync;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "nimble_port_init 失败: %s", esp_err_to_name(err));
        detail = buf;
        return false;
    }
    nimble_port_freertos_init(HostTask);

    if (xSemaphoreTake(s_sync_sem, pdMS_TO_TICKS(5000)) != pdTRUE || !s_synced.load()) {
        detail = "NimBLE sync 超时";
        return false;
    }

    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "ensure_addr 失败 rc=%d", rc);
        detail = buf;
        return false;
    }
    return true;
}

void StopStack() {
    ble_gap_disc_cancel();
    int rc = nimble_port_stop();
    if (rc == 0) {
        nimble_port_deinit();
    } else {
        ESP_LOGW(kTag, "nimble_port_stop rc=%d", rc);
        nimble_port_deinit();
    }
    s_synced.store(false);
    // 等 host task 退出
    for (int i = 0; i < 50 && s_host_running.load(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

}  // namespace

bool Hal::BleScan(std::vector<HalBtDevice>& out, std::string& detail, uint32_t timeout_ms) {
    out.clear();
    detail.clear();
    std::lock_guard<std::mutex> api_lock(s_ble_api_mu);

    {
        std::lock_guard<std::mutex> lock(s_ble_dev_mu);
        s_ble_devs.clear();
    }

    if (!StartStack(detail)) {
        StopStack();
        return false;
    }

    uint8_t own_addr_type = 0;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "infer_addr 失败 rc=%d", rc);
        detail = buf;
        StopStack();
        return false;
    }

    ble_gap_disc_params params{};
    params.filter_duplicates = 1;
    params.passive = 0;  // active：更容易拿到设备名
    params.itvl = 0;
    params.window = 0;
    params.filter_policy = 0;
    params.limited = 0;

    xSemaphoreTake(s_disc_done_sem, 0);
    const int32_t duration_ms = static_cast<int32_t>(timeout_ms > 0 ? timeout_ms : 8000);
    ESP_LOGI(kTag, "BLE scan start duration=%dms", static_cast<int>(duration_ms));
    rc = ble_gap_disc(own_addr_type, duration_ms, &params, OnGapEvent, nullptr);
    if (rc != 0) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "ble_gap_disc 失败 rc=%d", rc);
        detail = buf;
        StopStack();
        return false;
    }

    const TickType_t wait_ticks = pdMS_TO_TICKS(static_cast<uint32_t>(duration_ms) + 1500);
    if (xSemaphoreTake(s_disc_done_sem, wait_ticks) != pdTRUE) {
        ESP_LOGW(kTag, "DISC_COMPLETE wait timeout, cancel");
        ble_gap_disc_cancel();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    {
        std::lock_guard<std::mutex> lock(s_ble_dev_mu);
        out = s_ble_devs;
    }
    std::sort(out.begin(), out.end(),
              [](const HalBtDevice& a, const HalBtDevice& b) { return a.rssi > b.rssi; });

    StopStack();

    char buf[64];
    std::snprintf(buf, sizeof(buf), "BLE 扫描完成，共 %u 个", static_cast<unsigned>(out.size()));
    detail = buf;
    ESP_LOGI(kTag, "%s", detail.c_str());
    return true;
}
