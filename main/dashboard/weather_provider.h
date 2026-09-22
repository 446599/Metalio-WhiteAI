#pragma once
#include "dashboard_data.h"
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace dashboard {
struct WeatherPlace {
    uint32_t id=0;
    std::string name, region;
    double latitude=0, longitude=0;
};
const char* WeatherCondition(int code);
bool ValidWeatherPlace(const WeatherPlace& place);
std::string WeatherSearchUrl(const std::string& query);
std::string WeatherForecastUrl(const WeatherPlace& place);
bool ParseWeatherPlaces(const std::string& json, std::vector<WeatherPlace>& places);
bool ParseWeatherForecast(const std::string& json, const std::string& location, Weather& weather);
std::string EncodeWeatherPlace(const WeatherPlace& place);
bool DecodeWeatherPlace(const std::string& json, WeatherPlace& place);

std::string EncodeWeatherCache(const std::string& source, const Weather& weather);
bool DecodeWeatherCache(const std::string& json, const std::string& source, Weather& weather);

enum class WeatherSetupState { Idle, Searching, Results, Saving, Error };
struct WeatherSetupSnapshot {
    uint32_t revision=0, generation=0;
    WeatherSetupState state=WeatherSetupState::Idle;
    WeatherPlace selected;
    std::vector<WeatherPlace> results;
    std::string message;
};
struct WeatherJob {uint32_t generation=0;bool search=true;std::string query;WeatherPlace place;};
// Bounded mailbox, not a second network task. DashboardService is the sole I/O
// worker. UI operations only enqueue and copy snapshots under short locks.
class WeatherSetup {
public:
    static WeatherSetup& Instance();
    WeatherSetupSnapshot Snapshot()const;
    uint32_t Revision()const;
    void Restore(const WeatherPlace& place);
    bool Search(const std::string& query);
    bool Select(size_t index);
    bool Cancel();
    bool Take(WeatherJob& job);
    bool Current(uint32_t generation)const;
    void Searched(uint32_t generation, std::vector<WeatherPlace> places, const char* error);
    void Saved(uint32_t generation, bool ok);
private:
    mutable std::mutex mutex_;
    WeatherSetupSnapshot state_;
    WeatherJob job_;
    bool pending_=false;
};
}
