#include "hal.h"

#include "audio_codec.h"
#include "board.h"
#include "bq27220_gauge.h"
#include "config.h"
#include "cx25601n.h"
#include "IOExpander.hpp"
#include "nt26_board.h"
#include "SdCardManager.hpp"
#include "SimpleUart.hpp"
#include "wifi_board.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <wifi_station.h>

#include <esp_wifi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace {
constexpr const char* TAG = "Hal";
constexpr int kPingCount = 4;
constexpr int kPingSize = 32;
constexpr int kPingDelayMs = 1000;
constexpr uint32_t kEcpingTimeoutMs = 30000;

std::atomic<bool> s_net_switch_busy{false};

std::mutex s_bt_rx_mu;
std::string s_bt_rx_buf;
std::atomic<bool> s_bt_collector_installed{false};

void BtRxAppend(const std::vector<uint8_t>& data) {
    std::string line;
    line.reserve(data.size());
    for (uint8_t b : data) {
        if (b == '\r' || b == '\n') {
            line.push_back('\n');
        } else if (b >= 0x20 && b < 0x7F) {
            line.push_back(static_cast<char>(b));
        } else {
            line.push_back('.');
        }
    }
    ESP_LOGI(TAG, "BT RX (%u): %s", static_cast<unsigned>(data.size()), line.c_str());
    std::lock_guard<std::mutex> lock(s_bt_rx_mu);
    if (s_bt_rx_buf.size() > 8192) {
        s_bt_rx_buf.erase(0, s_bt_rx_buf.size() / 2);
    }
    s_bt_rx_buf.append(line);
}

void BtEnsureCollector() {
    if (s_bt_collector_installed.exchange(true)) {
        return;
    }
    SimpleUart::getInstance().registerCallback(BtRxAppend);
}

void BtClearRx() {
    std::lock_guard<std::mutex> lock(s_bt_rx_mu);
    s_bt_rx_buf.clear();
}

std::string BtSnapshotRx() {
    std::lock_guard<std::mutex> lock(s_bt_rx_mu);
    return s_bt_rx_buf;
}

// 外置 BT 模组应答：SET MODE 等，通常无经典 OK。
bool BtResponseSettled(const std::string& s) {
    return s.find("OK") != std::string::npos || s.find("ERROR") != std::string::npos ||
           s.find("SET MODE") != std::string::npos;
}

struct NetSwitchJob {
    DualNetworkBoard* dual = nullptr;
    NetworkType type = NetworkType::ML307;
};

void NetSwitchTask(void* arg) {
    auto* job = static_cast<NetSwitchJob*>(arg);
    if (job != nullptr && job->dual != nullptr) {
        ESP_LOGI(TAG, "net_switch task: target=%s",
                 job->type == NetworkType::WIFI ? "WiFi" : "4G");
        // 写 NVS + 重启；勿在 LVGL/PSRAM 栈上下文调用
        job->dual->SwitchToNetworkType(job->type);
    }
    delete job;
    s_net_switch_busy.store(false);
    vTaskDelete(nullptr);
}
}  // namespace

Hal& Hal::Get() {
    static Hal instance;
    return instance;
}

void Hal::Init() {
    if (inited_) {
        return;
    }
    ESP_LOGI(TAG, "init drivers under hal/");
    (void)Board::GetInstance();
    inited_ = true;
}

Display* Hal::GetDisplay() {
    return Board::GetInstance().GetDisplay();
}

void Hal::SetMotor(bool on) {
    Board::GetInstance().SetVibration(on);
}

DualNetworkBoard* Hal::Dual() {
    return dynamic_cast<DualNetworkBoard*>(&Board::GetInstance());
}

bool Hal::SuspendAudioForSleep(bool suspend) {
    if(!audio_started_)return true;
    auto* codec=Board::GetInstance().GetAudioCodec();
    return codec && codec->SuspendForSleep(suspend);
}
bool Hal::EnsureAudioStarted() {
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr || codec->IsSleepSuspended()) {
        return false;
    }
    // 等待外置 BT 模块进入模式 1（最多约 2s），否则 I2S Slave 无时钟
    constexpr int kWaitMs = 2000;
    constexpr int kStepMs = 50;
    int waited = 0;
    while (!Board::GetInstance().IsBtAudioModeReady()) {
        if (waited >= kWaitMs) {
            ESP_LOGW(TAG, "EnsureAudioStarted: BT audio mode not ready after %dms", kWaitMs);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(kStepMs));
        waited += kStepMs;
    }
    if (!audio_started_) {
        codec->Start();
        audio_started_ = true;
        ESP_LOGI(TAG, "EnsureAudioStarted: codec started");
    }
    return true;
}

void Hal::SetVolume(int volume) {
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        return;
    }
    volume = std::clamp(volume, 0, 100);
    codec->SetOutputVolume(volume);
}

int Hal::GetVolume() {
    auto* codec = Board::GetInstance().GetAudioCodec();
    return codec != nullptr ? codec->output_volume() : 0;
}

int Hal::AudioInputSampleRate() const {
    auto* codec = Board::GetInstance().GetAudioCodec();
    return codec != nullptr ? codec->input_sample_rate() : 16000;
}

int Hal::AudioOutputSampleRate() const {
    auto* codec = Board::GetInstance().GetAudioCodec();
    return codec != nullptr ? codec->output_sample_rate() : 16000;
}

int Hal::ReadMic(int16_t* dest, int samples) {
    if (dest == nullptr || samples <= 0 || !EnsureAudioStarted()) {
        return -1;
    }
    auto* codec = Board::GetInstance().GetAudioCodec();
    // codec Read 已按单声道帧返回；InputData 会 resize 为实际长度
    std::vector<int16_t> buf(static_cast<size_t>(samples));
    if (!codec->InputData(buf)) {
        return -1;
    }
    const int got = static_cast<int>(buf.size());
    std::memcpy(dest, buf.data(), static_cast<size_t>(got) * sizeof(int16_t));
    return got;
}

int Hal::WriteSpk(const int16_t* data, int samples) {
    if (data == nullptr || samples <= 0 || !EnsureAudioStarted()) {
        return -1;
    }
    auto* codec = Board::GetInstance().GetAudioCodec();
    std::vector<int16_t> buf(data, data + samples);
    codec->OutputData(buf);
    return samples;
}

bool Hal::IsSdMounted() const {
    return SdCardManager::GetInstance().IsMounted();
}

bool Hal::RemountSd() {
    auto& sd = SdCardManager::GetInstance();
    if (sd.IsMounted()) {
        sd.Unmount();
    }
    return sd.Mount();
}

const char* Hal::GetSdMountPoint() const {
    return SdCardManager::GetInstance().GetMountPoint();
}

NetworkType Hal::GetNetworkType() const {
    auto* dual = const_cast<Hal*>(this)->Dual();
    if (dual == nullptr) {
        return NetworkType::ML307;
    }
    return dual->GetNetworkType();
}

bool Hal::IsWifiMode() const {
    return GetNetworkType() == NetworkType::WIFI;
}

bool Hal::IsCellMode() const {
    return GetNetworkType() == NetworkType::ML307;
}

bool Hal::RequestSwitchNetwork(NetworkType type) {
    auto* dual = Dual();
    if (dual == nullptr) {
        return false;
    }
    if (dual->GetNetworkType() == type) {
        return false;
    }
    if (s_net_switch_busy.exchange(true)) {
        ESP_LOGW(TAG, "RequestSwitchNetwork: already switching");
        return false;
    }

    auto* job = new (std::nothrow) NetSwitchJob{dual, type};
    if (job == nullptr) {
        ESP_LOGE(TAG, "RequestSwitchNetwork: OOM");
        s_net_switch_busy.store(false);
        return false;
    }
    // 后台任务栈在内部 RAM，避免 LVGL(PSRAM 栈) 上 nvs_commit 触发 cache assert
    if (xTaskCreatePinnedToCore(NetSwitchTask, "net_switch", 4096, job, 5, nullptr, 0) !=
        pdPASS) {
        ESP_LOGE(TAG, "RequestSwitchNetwork: xTaskCreate failed");
        delete job;
        s_net_switch_busy.store(false);
        return false;
    }
    return true;
}

bool Hal::WifiStartSta() {
    if (!IsWifiMode()) {
        ESP_LOGW(TAG, "WifiStartSta: not in WiFi mode");
        return false;
    }
    WifiStation::GetInstance().Start();
    return true;
}

bool Hal::WifiScan(std::vector<HalWifiAp>& out, uint32_t timeout_ms) {
    out.clear();
    if (!IsWifiMode()) {
        return false;
    }

    std::vector<WifiScanAp> scanned;
    if (!WifiStation::GetInstance().ScanForList(scanned, static_cast<int>(timeout_ms))) {
        return false;
    }
    out.reserve(scanned.size());
    for (auto& item : scanned) {
        HalWifiAp ap;
        ap.ssid = std::move(item.ssid);
        ap.rssi = item.rssi;
        out.push_back(std::move(ap));
    }
    return true;
}

bool Hal::WifiConnectSaved(uint32_t timeout_ms) {
    if (!IsWifiMode()) {
        return false;
    }
    if (!WifiStartSta()) {
        return false;
    }
    auto& wifi = WifiStation::GetInstance();
    if (wifi.IsConnected()) {
        return true;
    }
    // ScanForList 会停掉自动重扫；连接前重新触发扫连
    wifi.StartAutoConnectScan();
    return wifi.WaitForConnected(static_cast<int>(timeout_ms));
}

bool Hal::WifiConnect(const std::string& ssid, const std::string& password, uint32_t timeout_ms) {
    if (!IsWifiMode()) {
        return false;
    }
    if (ssid.empty()) {
        ESP_LOGW(TAG, "WifiConnect: empty ssid");
        return false;
    }
    auto& wifi = WifiStation::GetInstance();
    wifi.AddAuth(std::string(ssid), std::string(password));
    if (!WifiStartSta()) {
        return false;
    }
    if (wifi.IsConnected() && wifi.GetSsid() == ssid) {
        return true;
    }
    // 已连其它网络时先断开，否则 WaitForConnected 会立刻因旧 bit 成功
    if (wifi.IsConnected()) {
        esp_wifi_disconnect();
        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
        while (wifi.IsConnected() && xTaskGetTickCount() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    wifi.StartAutoConnectScan();
    if (!wifi.WaitForConnected(static_cast<int>(timeout_ms))) {
        return false;
    }
    return wifi.GetSsid() == ssid;
}

bool Hal::WifiIsConnected() {
    if (!IsWifiMode()) {
        return false;
    }
    return WifiStation::GetInstance().IsConnected();
}

std::string Hal::WifiSsid() {
    return WifiStation::GetInstance().GetSsid();
}

std::string Hal::WifiIp() {
    return WifiStation::GetInstance().GetIpAddress();
}

int8_t Hal::WifiRssi() {
    return WifiStation::GetInstance().GetRssi();
}

bool Hal::CellStart() {
    if (!IsCellMode()) {
        ESP_LOGW(TAG, "CellStart: not in 4G mode");
        return false;
    }
    auto* dual = Dual();
    if (dual == nullptr) {
        return false;
    }
    auto* nt26 = dynamic_cast<Nt26Board*>(&dual->GetCurrentBoard());
    if (nt26 == nullptr) {
        return false;
    }
    // 起模组期间 TCA9555 易超时；暂停音量键轮询，避免每秒 I2C 满超时刷屏
    struct PauseGuard {
        PauseGuard() { IOExpander::getInstance().SetInputPollingPaused(true); }
        ~PauseGuard() { IOExpander::getInstance().SetInputPollingPaused(false); }
    } pause;
    nt26->StartNetwork();
    return true;
}

bool Hal::CellProbeAt(std::string& response) {
    response.clear();
    if (!IsCellMode()) {
        return false;
    }
    auto* dual = Dual();
    if (dual == nullptr) {
        return false;
    }
    auto* nt26 = dynamic_cast<Nt26Board*>(&dual->GetCurrentBoard());
    if (nt26 == nullptr) {
        return false;
    }
    esp_err_t err = nt26->SendAtCommand("AT", response, 1000, true);
    return err == ESP_OK;
}

int Hal::CellCsq() {
    if (!IsCellMode()) {
        return 99;
    }
    auto* dual = Dual();
    if (dual == nullptr) {
        return 99;
    }
    auto* nt26 = dynamic_cast<Nt26Board*>(&dual->GetCurrentBoard());
    if (nt26 == nullptr) {
        return 99;
    }
    return nt26->GetSignalStrength();
}

std::string Hal::CellRegistrationJson() {
    if (!IsCellMode()) {
        return "{}";
    }
    auto* dual = Dual();
    if (dual == nullptr) {
        return "{}";
    }
    auto* nt26 = dynamic_cast<Nt26Board*>(&dual->GetCurrentBoard());
    if (nt26 == nullptr) {
        return "{}";
    }
    return nt26->GetRegistrationState().ToString();
}

bool Hal::CellPing(std::string& detail) {
    detail.clear();
    if (!IsCellMode()) {
        detail = "当前不是 4G 模式";
        return false;
    }
    auto* dual = Dual();
    if (dual == nullptr) {
        detail = "无双网板";
        return false;
    }
    auto* nt26 = dynamic_cast<Nt26Board*>(&dual->GetCurrentBoard());
    if (nt26 == nullptr) {
        detail = "无 NT26 板";
        return false;
    }

    struct PauseGuard {
        PauseGuard() { IOExpander::getInstance().SetInputPollingPaused(true); }
        ~PauseGuard() { IOExpander::getInstance().SetInputPollingPaused(false); }
    } pause;

    char cmd[96];
    std::snprintf(cmd, sizeof(cmd), "AT+ECPING=\"www.baidu.com\",%d,%d,%d", kPingCount, kPingSize,
                  kPingDelayMs);
    std::string resp;
    esp_err_t err =
        nt26->SendAtCommandCollectUntil(cmd, resp, kEcpingTimeoutMs, "+ECPING: DONE", true);
    detail = resp.empty() ? esp_err_to_name(err) : resp;

    if (err != ESP_OK) {
        return false;
    }
    if (resp.find("+ECPING: FAIL") != std::string::npos ||
        resp.find("+ECPING: TIMEOUT") != std::string::npos) {
        return false;
    }
    if (resp.find("+ECPING: SUCC") != std::string::npos ||
        resp.find("+ECPING: DONE") != std::string::npos ||
        resp.find("received") != std::string::npos) {
        return true;
    }
    return err == ESP_OK;
}

bool Hal::GetBatteryInfo(HalBatteryInfo& out) {
    out = HalBatteryInfo{};
    out.chrg_stat = "N/A";

    int level = 0;
    bool charging = false;
    bool discharging = false;
    const bool level_ok = Board::GetInstance().GetBatteryLevel(level, charging, discharging);

    auto& gauge = Bq27220Gauge::GetInstance();
    uint16_t mv = 0;
    int16_t ma = 0;
    const bool mv_ok = gauge.ReadVoltageMv(mv);
    const bool ma_ok = gauge.ReadCurrentMa(ma);

    if (level_ok) {
        out.level = level;
        out.charging = charging;
        out.discharging = discharging;
    }
    if (mv_ok) {
        out.voltage_mv = mv;
    }
    if (ma_ok) {
        out.current_ma = ma;
    }

    if (cx25601n_is_ready()) {
        uint8_t stat = 0;
        if (cx25601n_get_chrg_stat(&stat) == ESP_OK) {
            out.chrg_stat = cx25601n_chrg_stat_str(stat);
        }
    }

    out.ok = level_ok || mv_ok || ma_ok;
    return out.ok;
}

bool Hal::IsButtonPressed(HalButtonId id) {
    switch (id) {
        case HalButtonId::Boot:
            return gpio_get_level(BOOT_BUTTON_GPIO) == 0;
        case HalButtonId::Power:
            return gpio_get_level(POWER_BUTTON_GPIO) == 0;
        case HalButtonId::VolUp: {
            uint8_t level = 1;
            if (IOExpander::getInstance().getLevel(IOExpander::Pin::VOLUME_UP, &level) != ESP_OK) {
                return false;
            }
            return level == 0;
        }
        case HalButtonId::VolDown: {
            uint8_t level = 1;
            if (IOExpander::getInstance().getLevel(IOExpander::Pin::VOLUME_DOWN, &level) != ESP_OK) {
                return false;
            }
            return level == 0;
        }
    }
    return false;
}

bool Hal::IsBtUartReady() const {
    return SimpleUart::getInstance().isInitialized();
}

bool Hal::IsBtModeReady() const {
    return Board::GetInstance().IsBtAudioModeReady();
}

bool Hal::BtSendCollect(const char* cmd, std::string& out, uint32_t timeout_ms) {
    out.clear();
    if (cmd == nullptr || !IsBtUartReady()) {
        out = "BT UART 未就绪";
        return false;
    }
    BtEnsureCollector();
    BtClearRx();
    if (!SimpleUart::getInstance().sendString(cmd)) {
        out = "发送失败";
        return false;
    }

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        out = BtSnapshotRx();
        if (BtResponseSettled(out)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    out = BtSnapshotRx();
    if (out.empty()) {
        out = "超时无应答";
        return false;
    }
    return out.find("ERROR") == std::string::npos;
}
