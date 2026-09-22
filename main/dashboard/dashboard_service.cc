#include "power/activity.h"
#include "dashboard_service.h"

#include "dashboard_data.h"
#include "dashboard_json.h"
#include "weather_provider.h"
#include "xiaozhi/conversation.h"
#include <nvs.h>
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
constexpr size_t kMaxResponseBytes = 16U * 1024U;
constexpr size_t kMaxUrlBytes = 512;
constexpr size_t kMaxLocationBytes = 90;
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

bool LoadWeatherPlace(WeatherPlace& place) {
    nvs_handle_t handle;
    if(nvs_open("dashboard",NVS_READONLY,&handle)!=ESP_OK)return false;
    size_t size=0;bool ok=nvs_get_blob(handle,"wx_place",nullptr,&size)==ESP_OK && size>0 && size<=1024;
    std::string json;
    if(ok){json.resize(size);ok=nvs_get_blob(handle,"wx_place",json.data(),&size)==ESP_OK;}
    nvs_close(handle);return ok && DecodeWeatherPlace(json,place);
}
bool SaveWeatherPlace(const WeatherPlace& place) {
    const auto json=EncodeWeatherPlace(place);if(json.empty())return false;
    nvs_handle_t handle;if(nvs_open("dashboard",NVS_READWRITE,&handle)!=ESP_OK)return false;
    const bool ok=nvs_set_blob(handle,"wx_place",json.data(),json.size())==ESP_OK && nvs_commit(handle)==ESP_OK;
    nvs_close(handle);return ok;
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
    // Preserve an explicitly provisioned endpoint, but do not pretend Beijing
    // is the user's location. On-device city selection takes precedence.
    weather_url_ = settings.GetString("weather_url", "");
    weather_location_ = settings.GetString("weather_loc", "");
    WeatherPlace place;
    if(LoadWeatherPlace(place)) {
        WeatherSetup::Instance().Restore(place);configured_place_=true;
        weather_url_=WeatherForecastUrl(place);weather_location_=place.name;
    }
    if(weather_url_.rfind("https://",0)!=0)weather_url_.clear();
    quota_url_ = settings.GetString("quota_url", "");
    quota_token_ = settings.GetString("quota_token", "");
    if (weather_url_.size() > kMaxUrlBytes) weather_url_.clear();
    if (quota_url_.size() > kMaxUrlBytes) quota_url_.clear();
    if (weather_location_.size() > kMaxLocationBytes) weather_location_.clear();
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
    // One committed record binds the city, source and all readings together.
    // Ignore old multi-key weather snapshots: torn caches must not look current.
    nvs_handle_t handle;
    if(!weather_url_.empty() && nvs_open("dashboard",NVS_READONLY,&handle)==ESP_OK) {
        size_t size=0;
        if(nvs_get_blob(handle,"wx_cache",nullptr,&size)==ESP_OK && size>0 && size<=2048) {
            std::string json(size,'\0');Weather weather;
            if(nvs_get_blob(handle,"wx_cache",json.data(),&size)==ESP_OK &&
               DecodeWeatherCache(json,weather_url_,weather))data.SetWeather(weather);
        }
        nvs_close(handle);
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
    const auto json=EncodeWeatherCache(weather_url_,weather);
    nvs_handle_t handle;
    if(json.empty() || nvs_open("dashboard",NVS_READWRITE,&handle)!=ESP_OK)return;
    const bool ok=nvs_set_blob(handle,"wx_cache",json.data(),json.size())==ESP_OK && nvs_commit(handle)==ESP_OK;
    nvs_close(handle);
    if(!ok)ESP_LOGW(kTag,"weather cache not saved");
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

bool DashboardService::FetchJson(const std::string& url,std::string& body) {
    if(url.empty() || url.size()>kMaxUrlBytes || url.rfind("https://",0)!=0)return false;
    auto* network=Board::GetInstance().GetNetwork();if(!network)return false;
    auto http=network->CreateHttp();if(!http)return false;
    http->SetTimeout(10000);http->SetHeader("Accept","application/json");http->SetKeepAlive(false);
    if(!http->Open("GET",url)){http->Close();return false;}
    const int status=http->GetStatusCode();
    const bool ok=status==200 && ReadBounded(http.get(),kMaxResponseBytes,body);
    http->Close();return ok;
}

void DashboardService::ServiceWeatherSetup() {
    WeatherJob job;if(!WeatherSetup::Instance().Take(job))return;
    auto& setup=WeatherSetup::Instance();
    if(job.search) {
        if(!EnsureNetwork()){setup.Searched(job.generation,{},"未联网");return;}
        std::string json;std::vector<WeatherPlace> results;
        const bool ok=FetchJson(WeatherSearchUrl(job.query),json) && ParseWeatherPlaces(json,results);
        setup.Searched(job.generation,std::move(results),ok?nullptr:"搜索失败，请重试");
    } else {
        // Selection cannot be cancelled once Saving is published. One committed
        // blob contains both coordinates and label, never a torn pair.
        const bool ok=SaveWeatherPlace(job.place);
        if(ok){configured_place_=true;weather_location_=job.place.name;weather_url_=WeatherForecastUrl(job.place);
            Weather empty;CopyText(empty.location,sizeof(empty.location),weather_location_.c_str());
            DashboardData::GetInstance().SetWeather(empty);last_refresh_ms_=0;refresh_requested_.store(true);}
        setup.Saved(job.generation,ok);
    }
}

bool DashboardService::FetchWeather() {
    std::string body;if(!FetchJson(weather_url_,body))return false;
    Weather weather;
    if(configured_place_) {
        if(!ParseWeatherForecast(body,weather_location_,weather))return false;
    } else {
        // Legacy configured provider compatibility; never forward quota tokens
        // to the weather service. New Open-Meteo path requires explicit units.
        cJSON* root=cJSON_ParseWithLengthOpts(body.c_str(),body.size()+1,nullptr,true);
        if(!root)return false;
        const cJSON* current=ObjectItem(root,"current");if(!current)current=root;
        double temperature,feels,humidity,wind,code;
        const bool ok=NumberItem(current,"temperature_2m",temperature) && NumberItem(current,"apparent_temperature",feels) &&
            NumberItem(current,"relative_humidity_2m",humidity) && NumberItem(current,"wind_speed_10m",wind) &&
            NumberItem(current,"weather_code",code) && temperature>=-99 && temperature<=99 && feels>=-99 && feels<=99 &&
            humidity>=0 && humidity<=100 && wind>=0 && wind<=999 && code>=0 && code<=99 && std::trunc(code)==code && WeatherCondition(static_cast<int>(code));
        if(ok){weather.valid=true;weather.request_state=RequestState::Succeeded;
            CopyText(weather.location,sizeof(weather.location),weather_location_.empty()?"自定义":weather_location_.c_str());
            CopyText(weather.condition,sizeof(weather.condition),WeatherCondition(static_cast<int>(code)));weather.temperature_c=std::lround(temperature);
            weather.feels_like_c=std::lround(feels);weather.humidity=std::lround(humidity);weather.wind_kmh=std::lround(wind);weather.updated_epoch=TimestampNow();}
        cJSON_Delete(root);if(!ok)return false;
    }
    DashboardData::GetInstance().SetWeather(weather);SaveWeatherSnapshot(weather);
    ESP_LOGI(kTag,"weather snapshot updated");return true;
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
        // Defer optional HTTP work while a foreground voice turn is active.
        // Reuse the provider task; do not allocate another internal task stack.
        const auto turn=xiaozhi::Conversation::GetInstance().State();
        if(turn==xiaozhi::TurnState::Connecting || turn==xiaozhi::TurnState::Listening ||
           turn==xiaozhi::TurnState::Transcribing || turn==xiaozhi::TurnState::Thinking ||
           turn==xiaozhi::TurnState::Speaking) {
            activity.Release();vTaskDelay(pdMS_TO_TICKS(1000));continue;
        }
        ServiceWeatherSetup();
        const int64_t now_ms = esp_timer_get_time() / 1000;
        auto& data = DashboardData::GetInstance();
        data.UpdateFreshness(TimestampNow());
        const bool requested = refresh_requested_.exchange(false) &&
            (last_refresh_ms_==0 || now_ms-last_refresh_ms_>=60000);
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
