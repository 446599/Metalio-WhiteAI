#ifndef PCF8563_H
#define PCF8563_H

#include <cstddef>
#include <ctime>
#include <cstdint>
#include <driver/i2c_master.h>

// ---------------------------------------------------------------------------
// Pcf8563 — NXP PCF8563 / BM8563 RTC 单例（I2C 0x51）
//
// 板级 Begin(bus) 后：
//   - ApplyRtcToSystem()：芯片时间 → 系统时间（离线开机）
//   - SyncSystemToRtc() ：系统时间 → 芯片（联网对时后回写）
// ---------------------------------------------------------------------------
class Pcf8563 {
public:
    static constexpr uint8_t kDefaultAddr = 0x51;

    static Pcf8563& GetInstance() {
        static Pcf8563 instance;
        return instance;
    }

    // probe 失败返回 false，不崩溃；后续接口会直接失败。
    bool Begin(i2c_master_bus_handle_t bus, uint8_t addr = kDefaultAddr);
    bool IsReady() const { return dev_ != nullptr; }

    // 读写日历。valid=false 表示电源失效位 VL 置位（时间可能不可信）。
    bool GetTime(struct tm& out, bool* valid = nullptr);
    bool SetTime(const struct tm& in);

    // 芯片 → settimeofday；成功返回 true。
    bool ApplyRtcToSystem();
    // localtime(now) → 芯片；成功返回 true。
    bool SyncSystemToRtc();

private:
    Pcf8563() = default;
    Pcf8563(const Pcf8563&) = delete;
    Pcf8563& operator=(const Pcf8563&) = delete;

    bool WriteRegs(uint8_t reg, const uint8_t* data, size_t len);
    bool ReadRegs(uint8_t reg, uint8_t* data, size_t len);

    static uint8_t DecToBcd(uint8_t v);
    static uint8_t BcdToDec(uint8_t v);

    i2c_master_bus_handle_t bus_  = nullptr;
    i2c_master_dev_handle_t dev_  = nullptr;
    uint8_t                 addr_ = kDefaultAddr;
};

#endif  // PCF8563_H
