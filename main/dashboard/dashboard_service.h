#pragma once

#include "dashboard_data.h"

#include <atomic>
#include <cstdint>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace dashboard {

// Background provider for the home dashboard.  It owns network polling and
// only publishes bounded values through DashboardData, so the display task
// never parses JSON or blocks on a modem request.
class DashboardService final {
public:
    static DashboardService& GetInstance();

    DashboardService(const DashboardService&) = delete;
    DashboardService& operator=(const DashboardService&) = delete;

    void Start();
    void RefreshNow();

private:
    DashboardService() = default;

    static void TaskEntry(void* arg);
    void Run();
    void LoadConfig();
    void LoadCachedSnapshots();
    void SaveWeatherSnapshot(const Weather& weather);
    void SaveQuotaSnapshot(const Quota& quota);
    bool EnsureNetwork();
    bool FetchWeather();
    bool FetchQuota();
    void PublishNetwork(const char* text);

    std::atomic<bool> started_{false};
    std::atomic<bool> refresh_requested_{false};
    TaskHandle_t task_handle_ = nullptr;
    std::string weather_url_;
    std::string weather_location_;
    std::string quota_url_;
    std::string quota_token_;
    uint32_t refresh_interval_ms_ = 10U * 60U * 1000U;
    bool network_attempted_ = false;
    bool network_ready_ = false;
    int64_t last_network_check_ms_ = 0;
    int64_t last_refresh_ms_ = 0;
};

}  // namespace dashboard
