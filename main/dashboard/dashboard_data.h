#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace dashboard {

constexpr size_t kAiSummaryCount = 3;
constexpr size_t kScheduleCount = 3;
constexpr size_t kCustomCardCount = 2;

enum class RequestState : uint8_t { Idle, Disabled, Refreshing, Succeeded, Failed, Offline };
enum class Freshness : uint8_t { NoData, UnknownTime, Fresh, Stale };
constexpr uint32_t kWeatherFreshSeconds = 60U * 60U;
constexpr uint32_t kQuotaFreshSeconds = 20U * 60U;
constexpr uint32_t kEarliestTrustedEpoch = 1704067200U;  // 2024-01-01 UTC

struct ScheduleItem {
    char time[8]{};
    char title[48]{};
    char detail[64]{};
    bool done = false;
};

struct Weather {
    bool valid = false;
    bool from_cache = false;
    RequestState request_state = RequestState::Idle;
    Freshness freshness = Freshness::NoData;
    char location[32]{};
    char condition[32]{};
    int16_t temperature_c = 0;
    int16_t feels_like_c = 0;
    uint8_t humidity = 0;
    int16_t wind_kmh = 0;
    uint32_t updated_epoch = 0;
};

struct Quota {
    bool valid = false;
    bool from_cache = false;
    RequestState request_state = RequestState::Idle;
    Freshness freshness = Freshness::NoData;
    // All stored/display values are percentages in [0, 100], never ratios.
    int16_t five_hour_remaining = 0;
    int16_t weekly_remaining = 0;
    char source[24]{};
    uint32_t updated_epoch = 0;
};

struct CustomCard {
    bool enabled = false;
    char title[24]{};
    char value[24]{};
    char detail[48]{};
};

// Fixed-size snapshot copied by the display thread. Providers may update the
// model from network/audio callbacks without ever touching the framebuffer.
struct Snapshot {
    uint32_t revision = 0;
    char network[24]{};
    char ai_status[24]{};
    char ai_summary[kAiSummaryCount][80]{};
    uint8_t ai_count = 0;
    ScheduleItem schedule[kScheduleCount]{};
    uint8_t schedule_count = 0;
    Weather weather{};
    Quota quota{};
    CustomCard custom[kCustomCardCount]{};
};

class DashboardData final {
public:
    static DashboardData& GetInstance();

    DashboardData(const DashboardData&) = delete;
    DashboardData& operator=(const DashboardData&) = delete;

    Snapshot GetSnapshot() const;
    uint32_t Revision() const;

    void ResetDefaults();
    void SetNetwork(const char* text);
    void SetAiStatus(const char* text);
    void SetAiSummary(size_t index, const char* text);
    void ClearAiSummary();
    void SetSchedule(size_t index, const char* time, const char* title,
                     const char* detail, bool done = false);
    void SetScheduleCount(size_t count);
    void SetWeather(const Weather& weather);
    void SetQuota(const Quota& quota);
    void SetWeatherRequestState(RequestState state);
    void SetQuotaRequestState(RequestState state);
    void UpdateFreshness(uint32_t now_epoch);
    void SetCustomCard(size_t index, const CustomCard& card);

private:
    DashboardData();

    void MarkChangedLocked();
    void RecomputeAiCountLocked();

    mutable std::mutex mutex_;
    Snapshot snapshot_{};
    uint32_t now_epoch_ = 0;
};

// Bounded UTF-8 copy used by network providers. It never leaves a partial
// multi-byte sequence at the end of a destination buffer.
void CopyText(char* destination, size_t destination_size, const char* source);

Freshness EvaluateFreshness(bool valid, uint32_t updated_epoch, uint32_t now_epoch,
                           uint32_t fresh_seconds);
void FormatWeatherStatus(const Weather& weather, char* destination, size_t size);
void FormatQuotaStatus(const Quota& quota, char* destination, size_t size);

}  // namespace dashboard
