#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace network {
enum class Security { Open, Personal, Unsupported };
struct AccessPoint { std::string ssid; int rssi = -127; Security security = Security::Personal; };
enum class SetupState { Idle, Scanning, Choosing, Connecting, Cancelling, Saving, Connected, Failed, Cancelled };
struct SetupSnapshot {
    uint32_t revision = 0, operation = 0;
    SetupState state = SetupState::Idle;
    std::vector<AccessPoint> access_points;
    std::string ssid, ip, message;
    bool saved = false;
};
bool ValidCredentials(const std::string& ssid, const std::string& password, Security security);
std::vector<AccessPoint> NormalizeScan(std::vector<AccessPoint> items);
bool Busy(SetupState state);

// Generation-fenced, credential-free snapshots. Cancelling cannot race a new
// operation; the existing worker must finish before the next request is accepted.
class SetupModel {
public:
    uint32_t Begin(bool scan, const std::string& ssid = "");
    bool Cancel();
    bool Cancelled(uint32_t operation) const;
    bool Scanned(uint32_t operation, bool ok, std::vector<AccessPoint> items);
    bool BeginSave(uint32_t operation);
    bool Finish(uint32_t operation, bool connected, bool saved, const std::string& ip = "");
    SetupSnapshot Snapshot() const;
    uint32_t Revision() const;
private:
    mutable std::mutex mutex_;
    SetupSnapshot state_;
};
}  // namespace network
