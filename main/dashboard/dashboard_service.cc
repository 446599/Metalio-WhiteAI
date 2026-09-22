#include "power/activity.h"
#include "dashboard_service.h"

#include "dashboard_data.h"
#include "dashboard_json.h"
#include "board.h"
#include "hal/hal.h"
#include "settings.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/task.h>
#include <http.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cstring>

namespace dashboard {
namespace {

constexpr const char* kTag = "DashboardSvc";
constexpr const char* kDefaultWeatherUrl =
    "https://api.open-meteo.com/v1/forecast?latitude=39.9042&longitude=116.4074&current=temperature_2m,relative_humidity_2m,apparent_temperature,wind_speed_10m,weather_code&timezone=Asia%2FShanghai";
constexpr const char* kDefaultWeatherLocation = "北京";
constexpr size_t kMaxResponseBytes = 16U * 1024U;
constexpr size_t kMaxUrlBytes = 512;
constexpr size_t kMaxLocationBytes = 64;
constexpr size_t kMaxTokenBytes = 512;
constexpr int64_t kNetworkCheckIntervalMs = 30 * 1000;
constexpr int32_t kQuotaCacheSchema = 1;

uint32_t TimestampNow() {
    const time_t now = time(nullptr);
    // Cached timestamps use the existing signed NVS integer field.
    return now >= kEarliestTrustedEpoch && now <= INT32_MAX ? static_cast<uint32_t>(now) : 0;
}

const cJSON* ObjectItem(const cJSON* object, const char* key) {
    return object != nullptr && cJSON_IsObject(object) ? cJSON_GetObjectItemCaseSensitive(object, key)
                                                        : nullptr;
}

bool NumberItem(const cJSON* object, const char* key, double& value) {
    const cJSON* item = ObjectItem(object, key);
    if (item == nullptr || !cJSON_IsNumber(item)) return false;
    value = item->valuedouble;
    return std::isfinite(value);
}

const char* WeatherCodeText(int code) {
    if (code == 0) return "晴朗";
    if (code <= 3) return "多云";
    if (code == 45 || code == 48) return "雾";
    if (code >= 51 && code <= 57) return "毛毛雨";
    if (code >= 61 && code <= 67) return "雨";
    if (code >= 71 && code <= 77) return "雪";
    if (code >= 80 && code <= 82) return "阵雨";
    if (code >= 85 && code <= 86) return "阵雪";
    if (code >= 95) return "雷雨";
    return "天气变化";
}

// Http::ReadAll() materializes an entire remote response before the caller can
// check its size.  Providers are untrusted, so read in bounded chunks instead.
bool ReadBounded(Http* http, size_t max_bytes, std::string& body) {
    body.clear();
    if (http == nullptr || max_bytes == 0) return false;
    const size_t expected = http->GetBodyLength();
    if (expected > max_bytes) return false;
    const bool length_known = expected != 0;
    if (length_known) body.reserve(expected);

    char buffer[1024];
    size_t total = 0;
    while (total < max_bytes) {
        const int count = http->Read(buffer, sizeof(buffer));
        if (count < 0) return false;
        if (count == 0) break;
        const size_t bytes = static_cast<size_t>(count);
        if (bytes > max_bytes - total) return false;
        if (length_known && bytes > expected - total) return false;
        body.append(buffer, bytes);
        total += bytes;
        if (length_known && total >= expected) break;
    }
    if (length_known && total != expected) return false;
    // For an unknown/chunked response, reaching the cap is ambiguous: there
    // may be more bytes waiting. Reject it rather than parsing a prefix.
    if (!length_known && total >= max_bytes) return false;
    return !body.empty();
}

}  // namespace

DashboardService& DashboardService::GetInstance() {
    static DashboardService instance;
    return instance;
}

void DashboardService::Start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) return;
    LoadConfig();
    DashboardData::GetInstance().SetNetwork("离线");
    ESP_LOGI(kTag, "started: refresh=%us quota=%s", refresh_interval_ms_ / 1000U,
             quota_url_.empty() ? "off" : "on");
    if (xTaskCreatePinnedToCore(TaskEntry, "dashboard", 8192, this, 2, &task_handle_, 0) != pdPASS) {
        ESP_LOGE(kTag, "failed to create dashboard task");
        started_.store(false);
        task_handle_ = nullptr;
    }
}

void DashboardService::RefreshNow() {
    refresh_requested_.store(true);
}

void DashboardService::TaskEntry(void* arg) {
    auto* self = static_cast<DashboardService*>(arg);
    if (self != nullptr) self->Run();
    vTaskDelete(nullptr);
}

void DashboardService::LoadConfig() {
    Settings settings("dashboard", false);
    weather_url_ = settings.GetString("weather_url", kDefaultWeatherUrl);
    weather_location_ = settings.GetString("weather_loc", kDefaultWeatherLocation);
    quota_url_ = settings.GetString("quota_url", "");
    quota_token_ = settings.GetString("quota_token", "");
    if (weather_url_.size() > kMaxUrlBytes) weather_url_.clear();
    if (quota_url_.size() > kMaxUrlBytes) quota_url_.clear();
    if (weather_location_.size() > kMaxLocationBytes) weather_location_ = kDefaultWeatherLocation;
    if (quota_token_.size() > kMaxTokenBytes) quota_token_.clear();
    const int32_t refresh_minutes = settings.GetInt("refresh_minutes", 10);
    refresh_interval_ms_ = static_cast<uint32_t>(std::clamp<int32_t>(refresh_minutes, 1, 120)) * 60U * 1000U;

    // Optional card and schedule overrides are deliberately plain NVS values:
    // a companion provisioning page can write them without changing firmware.
    auto& data = DashboardData::GetInstance();
    for (size_t i = 0; i < kCustomCardCount; ++i) {
        char key[32];
        std::snprintf(key, sizeof(key), "custom%u_title", static_cast<unsigned>(i));
        const std::string title = settings.GetString(key, "");
        std::snprintf(key, sizeof(key), "custom%u_value", static_cast<unsigned>(i));
        const std::string value = settings.GetString(key, "");
        std::snprintf(key, sizeof(key), "custom%u_detail", static_cast<unsigned>(i));
        const std::string detail = settings.GetString(key, "");
        std::snprintf(key, sizeof(key), "custom%u_enabled", static_cast<unsigned>(i));
        const bool enabled = settings.GetBool(key, title.empty() ? false : true);
        if (!title.empty() || !value.empty() || !detail.empty()) {
            CustomCard card;
            card.enabled = enabled;
            CopyText(card.title, sizeof(card.title), title.c_str());
            CopyText(card.value, sizeof(card.value), value.c_str());
            CopyText(card.detail, sizeof(card.detail), detail.c_str());
            data.SetCustomCard(i, card);
        }
    }
    for (size_t i = 0; i < kScheduleCount; ++i) {
        char key[32];
        std::snprintf(key, sizeof(key), "schedule%u_time", static_cast<unsigned>(i));
        const std::string schedule_time = settings.GetString(key, "");
        std::snprintf(key, sizeof(key), "schedule%u_title", static_cast<unsigned>(i));
        const std::string schedule_title = settings.GetString(key, "");
        std::snprintf(key, sizeof(key), "sched%u_detail", static_cast<unsigned>(i));
        const std::string schedule_detail = settings.GetString(key, "");
        if (!schedule_time.empty() || !schedule_title.empty() || !schedule_detail.empty()) {
            data.SetSchedule(i, schedule_time.c_str(), schedule_title.c_str(), schedule_detail.c_str(), false);
        }
    }
    for (size_t i = 0; i < kAiSummaryCount; ++i) {
        char key[32];
        std::snprintf(key, sizeof(key), "summary%u", static_cast<unsigned>(i));
        const std::string summary = settings.GetString(key, "");
        if (!summary.empty()) data.SetAiSummary(i, summary.c_str());
    }
    LoadCachedSnapshots();
    data.SetWeatherRequestState(weather_url_.empty() ? RequestState::Disabled : RequestState::Idle);
    data.SetQuotaRequestState(quota_url_.empty() ? RequestState::Disabled : RequestState::Idle);
    data.UpdateFreshness(TimestampNow());
}

void DashboardService::LoadCachedSnapshots() {
    Settings settings("dashboard", false);
    auto& data = DashboardData::GetInstance();
    if (settings.GetBool("w_valid", false)) {
        Weather weather;
        weather.valid = true;
        weather.from_cache = true;
        CopyText(weather.location, sizeof(weather.location),
                 settings.GetString("w_loc", weather_location_).c_str());
        CopyText(weather.condition, sizeof(weather.condition),
                 settings.GetString("w_cond", "天气待更新").c_str());
        weather.temperature_c = static_cast<int16_t>(std::clamp<int32_t>(
            settings.GetInt("w_temp", 0), -99, 99));
        weather.feels_like_c = static_cast<int16_t>(std::clamp<int32_t>(
            settings.GetInt("w_feels", 0), -99, 99));
        weather.humidity = static_cast<uint8_t>(std::clamp<int32_t>(
            settings.GetInt("w_hum", 0), 0, 100));
        weather.wind_kmh = static_cast<int16_t>(std::clamp<int32_t>(
            settings.GetInt("w_wind", 0), 0, 999));
        weather.updated_epoch = static_cast<uint32_t>(std::max<int32_t>(settings.GetInt("w_epoch", 0), 0));
        data.SetWeather(weather);
    }
    // Earlier firmware cached percentages after the ambiguous 0..1 conversion.
    // Do not present a possibly incorrect pre-fix quota as a valid reading.
    if (settings.GetBool("q_valid", false) && settings.GetInt("q_schema", 0) == kQuotaCacheSchema) {
        Quota quota;
        const int32_t five = settings.GetInt("q_five", -1);
        const int32_t weekly = settings.GetInt("q_week", -1);
        quota.valid = five >= 0 && five <= 100 && weekly >= 0 && weekly <= 100;
        quota.from_cache = quota.valid;
        quota.five_hour_remaining = quota.valid ? static_cast<int16_t>(five) : 0;
        quota.weekly_remaining = quota.valid ? static_cast<int16_t>(weekly) : 0;
        CopyText(quota.source, sizeof(quota.source),
                 settings.GetString("q_src", "缓存").c_str());
        quota.updated_epoch = static_cast<uint32_t>(std::max<int32_t>(settings.GetInt("q_epoch", 0), 0));
        data.SetQuota(quota);
    }
}

void DashboardService::SaveWeatherSnapshot(const Weather& weather) {
    Settings settings("dashboard", true);
    settings.SetBool("w_valid", weather.valid);
    settings.SetString("w_loc", weather.location);
    settings.SetString("w_cond", weather.condition);
    settings.SetInt("w_temp", weather.temperature_c);
    settings.SetInt("w_feels", weather.feels_like_c);
    settings.SetInt("w_hum", weather.humidity);
    settings.SetInt("w_wind", weather.wind_kmh);
    settings.SetInt("w_epoch", static_cast<int32_t>(weather.updated_epoch));
}

void DashboardService::SaveQuotaSnapshot(const Quota& quota) {
    Settings settings("dashboard", true);
    settings.SetBool("q_valid", quota.valid);
    settings.SetInt("q_five", quota.five_hour_remaining);
    settings.SetInt("q_week", quota.weekly_remaining);
    settings.SetString("q_src", quota.source);
    settings.SetInt("q_epoch", static_cast<int32_t>(quota.updated_epoch));
    settings.SetInt("q_schema", kQuotaCacheSchema);
}

void DashboardService::PublishNetwork(const char* text) {
    auto snapshot = DashboardData::GetInstance().GetSnapshot();
    if (std::strncmp(snapshot.network, text != nullptr ? text : "", sizeof(snapshot.network)) != 0) {
        DashboardData::GetInstance().SetNetwork(text);
    }
}

bool DashboardService::EnsureNetwork() {
    auto& hal = GetHAL();
    if (hal.IsWifiMode()) {
        if (hal.WifiIsConnected()) {
            PublishNetwork("WiFi 在线");
            return true;
        }
        PublishNetwork(network_attempted_ ? "离线" : "连接中");
        if (!network_attempted_) {
            network_attempted_ = true;
            (void)hal.WifiConnectSaved(15000);
        }
        const bool ready = hal.WifiIsConnected();
        PublishNetwork(ready ? "WiFi 在线" : "离线");
        return ready;
    }

    // The cellular board's StartNetwork() is intentionally attempted once;
    // it can take tens of seconds while the modem registers.  HTTP/WebSocket
    // retries remain independent, so a temporary SIM outage does not block the
    // display task.
    PublishNetwork(network_attempted_ ? "离线" : "连接中");
    if (!network_attempted_) {
        network_attempted_ = true;
        (void)hal.CellStart();
    }
    const std::string registration = hal.CellRegistrationJson();
    const bool ready = registration.find("\"stat\":1") != std::string::npos ||
                       registration.find("\"stat\":5") != std::string::npos;
    PublishNetwork(ready ? "4G 在线" : "离线");
    return ready;
}

bool DashboardService::FetchWeather() {
    auto* network = Board::GetInstance().GetNetwork();
    if (network == nullptr || weather_url_.empty()) return false;
    auto http = network->CreateHttp();
    if (!http) return false;
    http->SetTimeout(20000);
    http->SetHeader("Accept", "application/json");
    http->SetKeepAlive(false);
    if (!http->Open("GET", weather_url_)) {
        http->Close();
        return false;
    }
    const int status = http->GetStatusCode();
    if (status < 200 || status >= 300) {
        http->Close();
        return false;
    }
    std::string body;
    const bool read_ok = ReadBounded(http.get(), kMaxResponseBytes, body);
    http->Close();
    if (!read_ok) return false;

    cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
    if (root == nullptr) return false;
    const cJSON* current = ObjectItem(root, "current");
    if (current == nullptr) current = root;
    double temperature = 0, feels_like = 0, humidity = 0, wind = 0, code = 0;
    const bool ok = NumberItem(current, "temperature_2m", temperature) &&
                   NumberItem(current, "apparent_temperature", feels_like) &&
                   NumberItem(current, "relative_humidity_2m", humidity) &&
                   NumberItem(current, "wind_speed_10m", wind) &&
                   NumberItem(current, "weather_code", code) &&
                   temperature >= -99 && temperature <= 99 &&
                   feels_like >= -99 && feels_like <= 99 &&
                   humidity >= 0 && humidity <= 100 && wind >= 0 && wind <= 999 &&
                   code >= 0 && code <= 99;
    if (ok) {
        Weather weather;
        weather.valid = true;
        weather.request_state = RequestState::Succeeded;
        CopyText(weather.location, sizeof(weather.location), weather_location_.c_str());
        CopyText(weather.condition, sizeof(weather.condition), WeatherCodeText(static_cast<int>(std::lround(code))));
        weather.temperature_c = static_cast<int16_t>(std::clamp<int>(static_cast<int>(std::lround(temperature)), -99, 99));
        weather.feels_like_c = static_cast<int16_t>(std::clamp<int>(static_cast<int>(std::lround(feels_like)), -99, 99));
        weather.humidity = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(humidity)), 0, 100));
        weather.wind_kmh = static_cast<int16_t>(std::clamp<int>(static_cast<int>(std::lround(wind)), 0, 999));
        weather.updated_epoch = TimestampNow();
        DashboardData::GetInstance().SetWeather(weather);
        SaveWeatherSnapshot(weather);
        ESP_LOGI(kTag, "weather snapshot updated");
    }
    cJSON_Delete(root);
    return ok;
}

bool DashboardService::FetchQuota() {
    if (quota_url_.empty()) return false;
    auto* network = Board::GetInstance().GetNetwork();
    if (network == nullptr) return false;
    auto http = network->CreateHttp();
    if (!http) return false;
    http->SetTimeout(20000);
    http->SetHeader("Accept", "application/json");
    if (!quota_token_.empty()) http->SetHeader("Authorization", std::string("Bearer ") + quota_token_);
    http->SetKeepAlive(false);
    if (!http->Open("GET", quota_url_)) {
        http->Close();
        return false;
    }
    const int status = http->GetStatusCode();
    if (status < 200 || status >= 300) {
        http->Close();
        return false;
    }
    std::string body;
    const bool read_ok = ReadBounded(http.get(), kMaxResponseBytes, body);
    http->Close();
    if (!read_ok) return false;
    cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
    if (root == nullptr) return false;

    Quota quota;
    const bool ok = ParseQuotaRemaining(root, quota.five_hour_remaining, quota.weekly_remaining);
    if (ok) {
        quota.valid = true;
        quota.request_state = RequestState::Succeeded;
        CopyText(quota.source, sizeof(quota.source), "远端接口");
        quota.updated_epoch = TimestampNow();
        DashboardData::GetInstance().SetQuota(quota);
        SaveQuotaSnapshot(quota);
        ESP_LOGI(kTag, "quota snapshot updated");
    }
    cJSON_Delete(root);
    return ok;
}

void DashboardService::Run() {
    while (true) {
        if(power::Locked()){sleep_ready_.store(true);vTaskDelay(pdMS_TO_TICKS(100));continue;}
        sleep_ready_.store(false);
        power::Activity activity;if(!activity){vTaskDelay(pdMS_TO_TICKS(20));continue;}
        const int64_t now_ms = esp_timer_get_time() / 1000;
        auto& data = DashboardData::GetInstance();
        data.UpdateFreshness(TimestampNow());
        const bool requested = refresh_requested_.exchange(false);
        const bool network_due = last_network_check_ms_ == 0 ||
                                 now_ms - last_network_check_ms_ >= kNetworkCheckIntervalMs ||
                                 requested;
        if (network_due) {
            const bool was_ready = network_ready_;
            network_ready_ = EnsureNetwork();
            last_network_check_ms_ = now_ms;
            if (network_ready_ && !was_ready) last_refresh_ms_ = 0;
            if (!network_ready_) {
                if (!weather_url_.empty()) data.SetWeatherRequestState(RequestState::Offline);
                if (!quota_url_.empty()) data.SetQuotaRequestState(RequestState::Offline);
            }
        }
        const bool refresh_due = last_refresh_ms_ == 0 ||
                                 now_ms - last_refresh_ms_ >= refresh_interval_ms_;
        if (refresh_due || requested) {
            if (!network_ready_) {
                network_ready_ = EnsureNetwork();
                last_network_check_ms_ = now_ms;
            }
            if (network_ready_) {
                if (!weather_url_.empty()) {
                    data.SetWeatherRequestState(RequestState::Refreshing);
                    if (!FetchWeather()) data.SetWeatherRequestState(RequestState::Failed);
                }
                if (!quota_url_.empty()) {
                    data.SetQuotaRequestState(RequestState::Refreshing);
                    if (!FetchQuota()) data.SetQuotaRequestState(RequestState::Failed);
                }
            } else {
                if (!weather_url_.empty()) data.SetWeatherRequestState(RequestState::Offline);
                if (!quota_url_.empty()) data.SetQuotaRequestState(RequestState::Offline);
            }
            data.UpdateFreshness(TimestampNow());
            last_refresh_ms_ = now_ms;
        }
        activity.Release();vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

}  // namespace dashboard
