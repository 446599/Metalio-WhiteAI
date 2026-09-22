#pragma once
#include "build_features.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace device {
struct NearbyDevice { std::string name, address; int rssi=0; };
struct QuickSnapshot {
    uint32_t revision=0;
    int volume=0;
    bool ring=true, vibration=true, wifi_mode=true, wifi_connected=false;
    bool ble_busy=false, ble_cancelled=false, ble_ok=false;
    std::string network, message, version;
    uint32_t uptime_seconds=0, free_internal=0, largest_internal=0;
    std::vector<NearbyDevice> nearby;
};
// Short settings work executes outside the framebuffer lock on the event task.
// BLE discovery is omitted from the product build: even an idle linked
// controller costs IRAM. Only the explicitly experimental build has a worker.
class QuickControls {
public:
    static QuickControls& Instance();
    void Start();
    void Refresh();
    QuickSnapshot Snapshot() const;
    uint32_t Revision() const;
    bool SetVolume(int value);
    bool SetAlerts(bool ring,bool vibration);
    bool RingEnabled() const {return (alerts_.load()&1)!=0;}
    bool VibrationEnabled() const {return (alerts_.load()&2)!=0;}
    bool ScanBluetooth();
    void CancelBluetooth();
private:
    static void ScanTask(void*);
    mutable std::mutex mutex_;
    std::mutex settings_mutex_;
    QuickSnapshot state_;
    std::atomic<uint8_t> alerts_{3};
    std::atomic<bool> cancel_scan_{false};
    bool started_=false;
};
} // namespace device
