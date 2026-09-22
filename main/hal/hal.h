#pragma once

#include "display.h"
#include "dual_network_board.h"

#include <functional>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct HalWifiAp {
    std::string ssid;
    int8_t rssi = 0;
};

struct HalBatteryInfo {
    bool ok = false;
    int level = 0;
    bool charging = false;
    bool discharging = false;
    uint16_t voltage_mv = 0;
    int16_t current_ma = 0;
    const char* chrg_stat = "N/A";  // CX25601N 状态文案
};

enum class HalButtonId { Boot, Power, VolUp, VolDown };

struct HalBtDevice {
    std::string name;
    std::string addr;
    int rssi = 0;
};

// 硬件测试固件 HAL：
// - 本文件为统一入口（GetHAL）
// - 板级驱动在 hal/common、hal/metalio-e-ink-4
class Hal {
public:
    static Hal& Get();

    void Init();

    Display* GetDisplay();
    void SetMotor(bool on);

    // ---- 音频（BT I2S codec）----
    bool EnsureAudioStarted();
    void SetVolume(int volume);
    int GetVolume();
    int AudioInputSampleRate() const;
    int AudioOutputSampleRate() const;
    // 读/写一帧 PCM；samples 为样本数（int16）。返回实际样本数，失败 <0。
    int ReadMic(int16_t* dest, int samples);
    int WriteSpk(const int16_t* data, int samples);

    // ---- SD ----
    bool IsSdMounted() const;
    bool RemountSd();
    const char* GetSdMountPoint() const;

    // ---- 网络类型（DualNetworkBoard NVS 单实例）----
    NetworkType GetNetworkType() const;
    bool IsWifiMode() const;
    bool IsCellMode() const;
    // 写 NVS 并重启；已是目标类型则返回 false。
    bool RequestSwitchNetwork(NetworkType type);

    // ---- WiFi（仅 WiFi 模式）----
    bool WifiStartSta();
    bool WifiScan(std::vector<HalWifiAp>& out, uint32_t timeout_ms = 8000);
    bool WifiConnectSaved(uint32_t timeout_ms = 15000);
    // 写入凭据并扫连指定 SSID；须在后台任务调用。
    bool WifiConnect(const std::string& ssid, const std::string& password,
                     uint32_t timeout_ms = 15000);
    bool WifiIsConnected();
    std::string WifiSsid();
    std::string WifiIp();
    int8_t WifiRssi();

    // ---- 4G（仅 4G 模式）----
    // 阻塞：起模组并等待注册（可数十秒），须在后台任务调用。
    bool CellStart();
    bool CellProbeAt(std::string& response);
    int CellCsq();
    std::string CellRegistrationJson();
    // 阻塞 Ping（ECPING），须在后台任务调用。
    bool CellPing(std::string& detail);

    // ---- 电池 ----
    bool GetBatteryInfo(HalBatteryInfo& out);

    // ---- 物理按键（低有效；按下=true）----
    bool IsButtonPressed(HalButtonId id);

    // ---- 外置 BT 音频模组（UART）----
    bool IsBtUartReady() const;
    bool IsBtModeReady() const;
    // 发 AT 并收集应答；须在后台任务调用。
    bool BtSendCollect(const char* cmd, std::string& out, uint32_t timeout_ms = 2000);
    // ESP NimBLE 扫描周边 BLE；须在后台任务调用。结束后 deinit 释放。
    bool BleScan(std::vector<HalBtDevice>& out, std::string& detail, uint32_t timeout_ms = 8000,
                 const std::function<bool()>& cancelled = {});

private:
    Hal() = default;
    DualNetworkBoard* Dual();
    bool audio_started_ = false;
    bool inited_ = false;
};

inline Hal& GetHAL() { return Hal::Get(); }
