#include "xiaozhi/xiaozhi_activation.h"

#include "board.h"
#include "dashboard/dashboard_data.h"
#include "settings.h"
#include "system_info.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <sys/time.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace xiaozhi {
namespace {

constexpr const char* kTag = "XiaozhiActivation";
constexpr const char* kDefaultOtaUrl = "https://api.tenclass.net/xiaozhi/ota/";
constexpr int64_t kPollIntervalMs = 10 * 1000;
constexpr int kHttpTimeoutMs = 20000;

const char* StringItem(const cJSON* object, const char* key) {
    if (object == nullptr || !cJSON_IsObject(object)) return nullptr;
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return item != nullptr && cJSON_IsString(item) ? item->valuestring : nullptr;
}

int IntItem(const cJSON* object, const char* key, int fallback) {
    if (object == nullptr || !cJSON_IsObject(object)) return fallback;
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return item != nullptr && cJSON_IsNumber(item) ? item->valueint : fallback;
}

std::string JoinUrl(const std::string& base, const char* path) {
    if (base.empty()) return {};
    std::string url = base;
    if (url.back() != '/') {
        url += '/';
    }
    url += path;
    return url;
}

void CopyBounded(char* destination, size_t size, const char* source) {
    if (destination == nullptr || size == 0) return;
    if (source == nullptr) {
        destination[0] = '\0';
        return;
    }
    const size_t length = std::min(std::strlen(source), size - 1);
    std::memcpy(destination, source, length);
    destination[length] = '\0';
}

}  // namespace

Activation& Activation::GetInstance() {
    static Activation instance;
    return instance;
}

std::string Activation::OtaUrl() const {
    Settings settings("xiaozhi", false);
    std::string url = settings.GetString("ota_url");
    if (url.empty()) {
        url = kDefaultOtaUrl;
    }
    if (url.size() > 512) {
        url = kDefaultOtaUrl;
    }
    return url;
}

bool Activation::StoreWebsocketConfig(const char* url, const char* token) {
    if (url == nullptr || url[0] == '\0') {
        return false;
    }
    // Persisted under the same keys the transport already reads, so a server
    // assigned endpoint wins over the compiled-in default after one fetch.
    Settings settings("xiaozhi", true);
    if (settings.GetString("url") != url) {
        settings.SetString("url", url);
    }
    if (token != nullptr && token[0] != '\0' && settings.GetString("token") != token) {
        settings.SetString("token", token);
    }
    ESP_LOGI(kTag, "websocket endpoint updated from the activation server");
    return true;
}

bool Activation::FetchConfig() {
    auto* network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return false;
    }
    const std::string url = OtaUrl();
    auto http = network->CreateHttp(0);
    if (!http) {
        return false;
    }
    http->SetTimeout(kHttpTimeoutMs);
    http->SetKeepAlive(false);
    http->SetHeader("Activation-Version", "1");
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid());
    http->SetHeader("Accept-Language", "zh-CN");
    http->SetHeader("Content-Type", "application/json");
    http->SetContent(Board::GetInstance().GetSystemInfoJson());

    if (!http->Open("POST", url)) {
        ESP_LOGW(kTag, "activation endpoint unreachable");
        http->Close();
        return false;
    }
    const int status = http->GetStatusCode();
    if (status != 200) {
        ESP_LOGW(kTag, "activation endpoint status %d", status);
        http->Close();
        return false;
    }
    const std::string body = http->ReadAll();
    http->Close();

    cJSON* root = cJSON_ParseWithLength(body.c_str(), body.size());
    if (root == nullptr) {
        ESP_LOGW(kTag, "activation response was not JSON");
        return false;
    }

    State state;
    const cJSON* activation = cJSON_GetObjectItemCaseSensitive(root, "activation");
    const char* code = StringItem(activation, "code");
    const char* message = StringItem(activation, "message");
    const char* challenge = StringItem(activation, "challenge");
    state.has_code = code != nullptr && code[0] != '\0';
    state.has_challenge = challenge != nullptr && challenge[0] != '\0';
    state.timeout_ms = IntItem(activation, "timeout_ms", 30000);
    CopyBounded(state.code, sizeof(state.code), code);
    CopyBounded(state.message, sizeof(state.message), message);

    const cJSON* websocket = cJSON_GetObjectItemCaseSensitive(root, "websocket");
    if (websocket != nullptr) {
        StoreWebsocketConfig(StringItem(websocket, "url"), StringItem(websocket, "token"));
    }

    // Keep Unix time UTC. localtime() applies Asia/Shanghai exactly once;
    // adding timezone_offset here shifts all absolute reminders by eight hours.
    const cJSON* server_time = cJSON_GetObjectItemCaseSensitive(root, "server_time");
    const cJSON* timestamp = server_time != nullptr
                                 ? cJSON_GetObjectItemCaseSensitive(server_time, "timestamp")
                                 : nullptr;
    if (timestamp != nullptr && cJSON_IsNumber(timestamp) && std::isfinite(timestamp->valuedouble) &&
        timestamp->valuedouble >= 1704067200000.0 && timestamp->valuedouble < 4102444800000.0) {
        double ms = timestamp->valuedouble;
        struct timeval tv = {};
        tv.tv_sec = static_cast<time_t>(ms / 1000.0);
        tv.tv_usec = static_cast<suseconds_t>(static_cast<long long>(ms) % 1000) * 1000;
        if (settimeofday(&tv, nullptr) == 0) {
            state.has_server_time = true;
            Board::GetInstance().OnNetworkTimeSynced();
        }
    }

    const cJSON* firmware = cJSON_GetObjectItemCaseSensitive(root, "firmware");
    const char* latest = StringItem(firmware, "version");
    if (latest != nullptr) {
        ESP_LOGI(kTag, "server firmware version: %s", latest);
    }
    cJSON_Delete(root);

    state.checked = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Keep a bound device bound; only replace what the server just told us.
        state_.checked = true;
        state_.has_code = state.has_code;
        state_.has_challenge = state.has_challenge;
        state_.timeout_ms = state.timeout_ms;
        state_.has_server_time = state.has_server_time;
        CopyBounded(state_.code, sizeof(state_.code), state.code);
        CopyBounded(state_.message, sizeof(state_.message), state.message);
        challenge_ = state.has_challenge ? challenge : std::string();
        if (!state.has_code) {
            state_.bound = true;
        }
    }
    return true;
}

esp_err_t Activation::Poll() { return Activate(); }

esp_err_t Activation::Activate() {
    std::string challenge;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        challenge = challenge_;
    }
    auto* network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return ESP_FAIL;
    }
    auto http = network->CreateHttp(0);
    if (!http) {
        return ESP_FAIL;
    }
    http->SetTimeout(kHttpTimeoutMs);
    http->SetKeepAlive(false);
    http->SetHeader("Activation-Version", "1");
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid());
    http->SetHeader("Content-Type", "application/json");
    // Without a burned serial number there is no HMAC to send, and the server
    // accepts an empty payload for the polling form of the handshake.
    http->SetContent(challenge.empty() ? std::string("{}") : BuildChallengeJson(challenge));

    const std::string url = JoinUrl(OtaUrl(), "activate");
    if (!http->Open("POST", url)) {
        http->Close();
        return ESP_FAIL;
    }
    const int status = http->GetStatusCode();
    if (status == 202) {
        http->Close();
        return ESP_ERR_TIMEOUT;
    }
    if (status != 200) {
        if (status == 400 || status == 404 || status == 405) {
            // This deployment has no serial number to attest with and rejects
            // the polling form; fall back to config re-fetch for binding.
            if (activate_supported_) {
                ESP_LOGW(kTag, "activate endpoint rejected the poll (status %d); "
                              "binding will be detected by re-checking the config", status);
            }
            activate_supported_ = false;
        } else {
            ESP_LOGW(kTag, "activate status %d", status);
        }
        http->Close();
        return ESP_FAIL;
    }
    http->Close();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.bound = true;
    }
    ESP_LOGI(kTag, "device activation confirmed by the server");
    return ESP_OK;
}

bool Activation::ActivateSupported() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return activate_supported_;
}

std::string Activation::BuildChallengeJson(const std::string& challenge) const {
    cJSON* payload = cJSON_CreateObject();
    if (payload == nullptr) {
        return "{}";
    }
    cJSON_AddStringToObject(payload, "algorithm", "hmac-sha256");
    cJSON_AddStringToObject(payload, "challenge", challenge.c_str());
    char* text = cJSON_PrintUnformatted(payload);
    std::string json = text != nullptr ? text : "{}";
    if (text != nullptr) {
        cJSON_free(text);
    }
    cJSON_Delete(payload);
    return json;
}

bool Activation::PollDue() const {
    const int64_t now_ms = esp_timer_get_time() / 1000;
    std::lock_guard<std::mutex> lock(mutex_);
    return !first_poll_done_ || now_ms - last_poll_ms_ >= kPollIntervalMs;
}

void Activation::MarkPolled() {
    std::lock_guard<std::mutex> lock(mutex_);
    first_poll_done_ = true;
    last_poll_ms_ = esp_timer_get_time() / 1000;
}

Activation::State Activation::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void Activation::ClearBound() {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.bound = false;
    state_.checked = false;
    first_poll_done_ = false;
}

void PublishActivationState(const Activation::State& state) {
    auto& data = dashboard::DashboardData::GetInstance();
    if (state.bound) {
        data.SetAiStatus("小智已绑定");
        return;
    }
    if (!state.has_code) {
        return;
    }
    data.SetAiStatus("请绑定设备");
    char summary[80];
    std::snprintf(summary, sizeof(summary), "绑定码 %s", state.code);
    data.SetAiSummary(0, summary);
}

}  // namespace xiaozhi
