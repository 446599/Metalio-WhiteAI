#include "wifi_station.h"
#include <cstring>
#include <algorithm>
#include <freertos/task.h>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <nvs.h>
#include "nvs_flash.h"
#include <esp_netif.h>
#include <esp_system.h>
#include "ssid_manager.h"

#define TAG "WifiStation"
#define WIFI_EVENT_CONNECTED BIT0
#define WIFI_EVENT_SCAN_LIST_DONE BIT1
#define WIFI_EVENT_SETUP_DISCONNECTED BIT2
#define WIFI_EVENT_SETUP_FAILED BIT3
#define MAX_RECONNECT_COUNT 5
// esp_timer 单位为微秒
static constexpr uint64_t kRescanIntervalUs = 10ULL * 1000ULL * 1000ULL;

WifiStation& WifiStation::GetInstance() {
    static WifiStation instance;
    return instance;
}

WifiStation::WifiStation() {
    // Create the event group
    event_group_ = xEventGroupCreate();

    // 读取配置
    nvs_handle_t nvs = 0;
    max_tx_power_ = 0;
    remember_bssid_ = 0;
    esp_err_t err = nvs_open("wifi", NVS_READONLY, &nvs);
    if (err != ESP_OK) return;
    err = nvs_get_i8(nvs, "max_tx_power", &max_tx_power_);
    if (err != ESP_OK) {
        max_tx_power_ = 0;
    }
    err = nvs_get_u8(nvs, "remember_bssid", &remember_bssid_);
    if (err != ESP_OK) {
        remember_bssid_ = 0;
    }
    nvs_close(nvs);
}

WifiStation::~WifiStation() {
    vEventGroupDelete(event_group_);
}

void WifiStation::AddAuth(const std::string &&ssid, const std::string &&password) {
    auto& ssid_manager = SsidManager::GetInstance();
    ssid_manager.AddSsid(ssid, password);
}

bool WifiStation::SuspendForSleep() {
    std::unique_lock<std::mutex> operation(operation_mutex_,std::try_to_lock);
    if(!operation || SetupBusy())return false;
    std::lock_guard<std::recursive_mutex> state(state_mutex_);
    if(!started_ || sleeping_)return true;
    sleeping_.store(true);if(timer_handle_)esp_timer_stop(timer_handle_);
    (void)esp_wifi_scan_stop();
    const auto error=esp_wifi_stop();
    if(error!=ESP_OK){sleeping_.store(false);return false;}
    xEventGroupClearBits(event_group_,WIFI_EVENT_CONNECTED | WIFI_EVENT_SCAN_LIST_DONE);
    connect_queue_.clear();ip_address_.clear();return true;
}
bool WifiStation::ResumeFromSleep() {
    std::lock_guard<std::recursive_mutex> lock(state_mutex_);
    if(!sleeping_)return true;
    sleeping_.store(false);
    const auto error=esp_wifi_start();
    if(error!=ESP_OK){sleeping_.store(true);return false;}
    return true; // STA_START handles reconnect; no user credentials changed.
}
void WifiStation::Stop() {
    if (timer_handle_ != nullptr) {
        esp_timer_stop(timer_handle_);
        esp_timer_delete(timer_handle_);
        timer_handle_ = nullptr;
    }

    esp_wifi_scan_stop();
    listing_scan_.store(false);
    
    // 取消注册事件处理程序
    if (instance_any_id_ != nullptr) {
        ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id_));
        instance_any_id_ = nullptr;
    }
    if (instance_got_ip_ != nullptr) {
        ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip_));
        instance_got_ip_ = nullptr;
    }

    // Reset the WiFi stack
    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(esp_wifi_deinit());

    if (station_netif_ != nullptr) {
        esp_netif_destroy(station_netif_);
        station_netif_ = nullptr;
    }

    started_ = false;sleeping_.store(false);
    // Clear event group bits to prevent WaitForConnected from returning prematurely on restart
    xEventGroupClearBits(event_group_, WIFI_EVENT_CONNECTED | WIFI_EVENT_SCAN_LIST_DONE);
}

void WifiStation::OnScanBegin(std::function<void()> on_scan_begin) {
    on_scan_begin_ = on_scan_begin;
}

void WifiStation::OnConnect(std::function<void(const std::string& ssid)> on_connect) {
    on_connect_ = on_connect;
}

void WifiStation::OnConnected(std::function<void(const std::string& ssid)> on_connected) {
    on_connected_ = on_connected;
}

void WifiStation::Start() {
    std::lock_guard<std::recursive_mutex> state_lock(state_mutex_);
    if (started_ || sleeping_) {
        return;
    }

    // Initialize the TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());

    // Create the default event loop
    station_netif_ = esp_netif_create_default_wifi_sta();

    // Initialize the WiFi stack in station mode
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &WifiStation::WifiEventHandler,
                                                        this,
                                                        &instance_any_id_));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &WifiStation::IpEventHandler,
                                                        this,
                                                        &instance_got_ip_));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    if (max_tx_power_ != 0) {
        ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(max_tx_power_));
    }

    // Setup the timer to scan WiFi
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            auto* self = static_cast<WifiStation*>(arg);
            std::lock_guard<std::recursive_mutex> lock(self->state_mutex_);
            if (!self->sleeping_.load() && !self->SetupBusy()) esp_wifi_scan_start(nullptr, false);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "WiFiScanTimer",
        .skip_unhandled_events = true
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));
    started_ = true;
}

bool WifiStation::WaitForConnected(int timeout_ms) {
    auto bits = xEventGroupWaitBits(event_group_, WIFI_EVENT_CONNECTED, pdFALSE, pdFALSE, timeout_ms / portTICK_PERIOD_MS);
    return (bits & WIFI_EVENT_CONNECTED) != 0;
}

bool WifiStation::ScanForList(std::vector<WifiScanAp>& out, int timeout_ms) {
    out.clear();
    std::unique_lock<std::mutex> operation_lock(operation_mutex_, std::try_to_lock);
    if (!operation_lock || manual_setup_.load() || sleeping_.load()) return false;
    std::unique_lock<std::recursive_mutex> state_lock(state_mutex_);
    if (timer_handle_ != nullptr) {
        esp_timer_stop(timer_handle_);
    }

    list_scan_results_.clear();
    list_scan_ok_=false;
    xEventGroupClearBits(event_group_, WIFI_EVENT_SCAN_LIST_DONE);
    // 须在 Start() 之前置位，避免 STA_START 自动扫连抢结果
    listing_scan_.store(true);

    const bool first_start = !started_;
    if (first_start) {
        Start();
        // 首次启动：由 STA_START 处理器发起扫描
    } else {
        esp_wifi_scan_stop();
        esp_err_t err = esp_wifi_scan_start(nullptr, false);
        if (err != ESP_OK) {
            listing_scan_.store(false);
            ESP_LOGW(TAG, "ScanForList start failed: %s", esp_err_to_name(err));
            return false;
        }
    }

    state_lock.unlock();
    auto bits = xEventGroupWaitBits(event_group_, WIFI_EVENT_SCAN_LIST_DONE, pdTRUE, pdFALSE,
                                    pdMS_TO_TICKS(timeout_ms));
    state_lock.lock();
    listing_scan_.store(false);
    if ((bits & WIFI_EVENT_SCAN_LIST_DONE) == 0 || !list_scan_ok_) {
        ESP_LOGW(TAG, "ScanForList timeout");
        esp_wifi_scan_stop();
        return false;
    }

    out = std::move(list_scan_results_);
    list_scan_results_.clear();
    return true;
}

void WifiStation::StartAutoConnectScan() {
    std::lock_guard<std::recursive_mutex> state_lock(state_mutex_);
    if (SetupBusy() || sleeping_.load()) return;
    if (!started_) {
        Start();
        return;
    }
    if (timer_handle_ != nullptr) {
        esp_timer_stop(timer_handle_);
    }
    listing_scan_.store(false);
    connect_queue_.clear();
    esp_wifi_scan_stop();
    esp_wifi_scan_start(nullptr, false);
    if (on_scan_begin_) {
        on_scan_begin_();
    }
}

void WifiStation::HandleScanResult() {
    std::lock_guard<std::recursive_mutex> state_lock(state_mutex_);
    uint16_t ap_num = 0;
    if (manual_setup_.load()) { esp_wifi_clear_ap_list(); return; }
    if(esp_wifi_scan_get_ap_num(&ap_num)!=ESP_OK) {
        esp_wifi_clear_ap_list();
        if(listing_scan_.load()) xEventGroupSetBits(event_group_,WIFI_EVENT_SCAN_LIST_DONE);
        return;
    }
    ap_num = std::min<uint16_t>(ap_num, 64);
    wifi_ap_record_t *ap_records = (wifi_ap_record_t *)malloc(ap_num * sizeof(wifi_ap_record_t));
    if (ap_records == nullptr && ap_num > 0) {
        esp_wifi_clear_ap_list();
        ESP_LOGE(TAG, "HandleScanResult OOM for %u APs", ap_num);
        if (listing_scan_.load()) {
            list_scan_results_.clear();
            xEventGroupSetBits(event_group_, WIFI_EVENT_SCAN_LIST_DONE);
        }
        return;
    }
    if (ap_num > 0) {
        if(esp_wifi_scan_get_ap_records(&ap_num, ap_records)!=ESP_OK) {
            free(ap_records);esp_wifi_clear_ap_list();
            if(listing_scan_.load()) xEventGroupSetBits(event_group_,WIFI_EVENT_SCAN_LIST_DONE);
            return;
        }
        std::sort(ap_records, ap_records + ap_num, [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) {
            return a.rssi > b.rssi;
        });
    }

    if (listing_scan_.load()) {
        list_scan_results_.clear();
        list_scan_results_.reserve(ap_num);
        for (uint16_t i = 0; i < ap_num; i++) {
            const char* ssid = reinterpret_cast<const char*>(ap_records[i].ssid);
            if (ssid == nullptr || ssid[0] == '\0') {
                continue;
            }
            WifiScanAp ap;
            ap.ssid.assign(ssid, strnlen(ssid, 32));
            ap.rssi = ap_records[i].rssi;
            ap.authmode = ap_records[i].authmode;
            list_scan_results_.push_back(std::move(ap));
        }
        free(ap_records);
        list_scan_ok_=true;
        ESP_LOGI(TAG, "ScanForList done: %d APs", static_cast<int>(list_scan_results_.size()));
        xEventGroupSetBits(event_group_, WIFI_EVENT_SCAN_LIST_DONE);
        return;
    }

    auto& ssid_manager = SsidManager::GetInstance();
    auto ssid_list = ssid_manager.GetSsidList();
    for (int i = 0; i < ap_num; i++) {
        auto ap_record = ap_records[i];
        auto it = std::find_if(ssid_list.begin(), ssid_list.end(), [ap_record](const SsidItem& item) {
            return strcmp((char *)ap_record.ssid, item.ssid.c_str()) == 0;
        });
        if (it != ssid_list.end()) {
            ESP_LOGI(TAG, "Found AP: %s, BSSID: %02x:%02x:%02x:%02x:%02x:%02x, RSSI: %d, Channel: %d, Authmode: %d",
                (char *)ap_record.ssid, 
                ap_record.bssid[0], ap_record.bssid[1], ap_record.bssid[2],
                ap_record.bssid[3], ap_record.bssid[4], ap_record.bssid[5],
                ap_record.rssi, ap_record.primary, ap_record.authmode);
            WifiApRecord record = {
                .ssid = it->ssid,
                .password = it->password,
                .channel = ap_record.primary,
                .authmode = ap_record.authmode
            };
            memcpy(record.bssid, ap_record.bssid, 6);
            connect_queue_.push_back(record);
        }
    }
    free(ap_records);

    if (connect_queue_.empty()) {
        ESP_LOGI(TAG, "Wait for next scan");
        esp_timer_start_once(timer_handle_, kRescanIntervalUs);
        return;
    }

    StartConnect();
}

void WifiStation::StartConnect() {
    std::lock_guard<std::recursive_mutex> state_lock(state_mutex_);
    auto ap_record = connect_queue_.front();
    connect_queue_.erase(connect_queue_.begin());
    ssid_ = ap_record.ssid;
    password_ = ap_record.password;

    if (on_connect_) {
        on_connect_(ssid_);
    }

    wifi_config_t wifi_config;
    bzero(&wifi_config, sizeof(wifi_config));
    if (ap_record.ssid.empty() || ap_record.ssid.size() > sizeof(wifi_config.sta.ssid) ||
        ap_record.password.size() > sizeof(wifi_config.sta.password)) return;
    memcpy(wifi_config.sta.ssid, ap_record.ssid.data(), ap_record.ssid.size());
    memcpy(wifi_config.sta.password, ap_record.password.data(), ap_record.password.size());
    if (remember_bssid_) {
        wifi_config.sta.channel = ap_record.channel;
        memcpy(wifi_config.sta.bssid, ap_record.bssid, 6);
        wifi_config.sta.bssid_set = true;
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    reconnect_count_ = 0;
    ESP_ERROR_CHECK(esp_wifi_connect());
}

int8_t WifiStation::GetRssi() {
    // Get station info
    wifi_ap_record_t ap_info{};
    return esp_wifi_sta_get_ap_info(&ap_info)==ESP_OK ? ap_info.rssi : -127;
}

uint8_t WifiStation::GetChannel() {
    // Get station info
    wifi_ap_record_t ap_info{};
    return esp_wifi_sta_get_ap_info(&ap_info)==ESP_OK ? ap_info.primary : 0;
}

bool WifiStation::IsConnected() {
    return xEventGroupGetBits(event_group_) & WIFI_EVENT_CONNECTED;
}

void WifiStation::SetPowerSaveMode(bool enabled) {
    if(started_ && !sleeping_) (void)esp_wifi_set_ps(enabled ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
}

// Static event handler functions
void WifiStation::WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* this_ = static_cast<WifiStation*>(arg);
    std::lock_guard<std::recursive_mutex> state_lock(this_->state_mutex_);
    if(this_->sleeping_.load()){
        if(event_id==WIFI_EVENT_STA_DISCONNECTED)xEventGroupClearBits(this_->event_group_,WIFI_EVENT_CONNECTED);
        if(event_id==WIFI_EVENT_SCAN_DONE)esp_wifi_clear_ap_list();
        return;
    }
    if (event_id == WIFI_EVENT_STA_START) {
        if (this_->manual_setup_.load()) return;
        // listing_scan_：仍发起扫描，但 HandleScanResult 走列表路径而非自动连网
        esp_wifi_scan_start(nullptr, false);
        if (!this_->listing_scan_.load() && this_->on_scan_begin_) {
            this_->on_scan_begin_();
        }
    } else if (event_id == WIFI_EVENT_SCAN_DONE) {
        const auto* scan=static_cast<wifi_event_sta_scan_done_t*>(event_data);
        if(scan && scan->status!=0) {
            esp_wifi_clear_ap_list();
            if(this_->listing_scan_.load()) xEventGroupSetBits(this_->event_group_,WIFI_EVENT_SCAN_LIST_DONE);
            return;
        }
        this_->HandleScanResult();
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(this_->event_group_, WIFI_EVENT_CONNECTED);
        if (this_->manual_setup_.load()) {
            xEventGroupSetBits(this_->event_group_, WIFI_EVENT_SETUP_DISCONNECTED);
            if (this_->manual_attempting_.load()) xEventGroupSetBits(this_->event_group_, WIFI_EVENT_SETUP_FAILED);
            return;
        }
        if (this_->listing_scan_.load()) {
            return;
        }
        if (this_->reconnect_count_ < MAX_RECONNECT_COUNT) {
            esp_wifi_connect();
            this_->reconnect_count_++;
            ESP_LOGI(TAG, "Reconnecting %s (attempt %d / %d)", this_->ssid_.c_str(), this_->reconnect_count_, MAX_RECONNECT_COUNT);
            return;
        }

        if (!this_->connect_queue_.empty()) {
            this_->StartConnect();
            return;
        }
        
        ESP_LOGI(TAG, "No more AP to connect, wait for next scan");
        esp_timer_start_once(this_->timer_handle_, kRescanIntervalUs);
    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
    }
}

void WifiStation::IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* this_ = static_cast<WifiStation*>(arg);
    auto* event = static_cast<ip_event_got_ip_t*>(event_data);
    std::lock_guard<std::recursive_mutex> state_lock(this_->state_mutex_);
    if(this_->sleeping_.load())return;
    wifi_ap_record_t ap{};
    if(esp_wifi_sta_get_ap_info(&ap)!=ESP_OK || event->ip_info.ip.addr==0 ||
       this_->ssid_!=std::string(reinterpret_cast<char*>(ap.ssid),strnlen(reinterpret_cast<char*>(ap.ssid),32))) return;
    if(this_->manual_setup_.load() && !this_->manual_attempting_.load()) return;
    char ip_address[16];
    esp_ip4addr_ntoa(&event->ip_info.ip, ip_address, sizeof(ip_address));
    this_->ip_address_ = ip_address;
    ESP_LOGI(TAG, "Got IP: %s", this_->ip_address_.c_str());
    
    xEventGroupSetBits(this_->event_group_, WIFI_EVENT_CONNECTED);
    if (this_->on_connected_) {
        this_->on_connected_(this_->ssid_);
    }
    this_->connect_queue_.clear();
    this_->reconnect_count_ = 0;
}


bool WifiStation::ConnectForSetup(const std::string& ssid, const std::string& password,
                                  bool open, int timeout_ms, const std::function<bool()>& cancelled) {
    std::unique_lock<std::mutex> operation_lock(operation_mutex_, std::try_to_lock);
    if (!operation_lock || ssid.empty() || ssid.size()>32 || password.size()>64) return false;
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex_);
        if (SetupBusy() || sleeping_.load()) return false;
        manual_setup_.store(true);
        manual_attempting_.store(false);
        Start();
        if (timer_handle_) esp_timer_stop(timer_handle_);
        esp_wifi_scan_stop();
        connect_queue_.clear();
        xEventGroupClearBits(event_group_, WIFI_EVENT_CONNECTED | WIFI_EVENT_SETUP_DISCONNECTED | WIFI_EVENT_SETUP_FAILED);
        esp_wifi_disconnect();
    }
    // Drain the disconnect from the previous association before arming this one.
    xEventGroupWaitBits(event_group_, WIFI_EVENT_SETUP_DISCONNECTED, pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
    if (cancelled && cancelled()) return false;
    wifi_config_t config{};
    memcpy(config.sta.ssid, ssid.data(), ssid.size());
    memcpy(config.sta.password, password.data(), password.size());
    config.sta.threshold.authmode = open ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_PSK;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex_);
        ssid_ = ssid;
        ip_address_.clear();
        xEventGroupClearBits(event_group_, WIFI_EVENT_CONNECTED | WIFI_EVENT_SETUP_FAILED);
        const auto configured = esp_wifi_set_config(WIFI_IF_STA, &config);
        volatile uint8_t* secret = config.sta.password;
        for (size_t i=0;i<sizeof(config.sta.password);++i) secret[i]=0;
        if (configured != ESP_OK) return false;
        manual_attempting_.store(true);
        if (esp_wifi_connect() != ESP_OK) return false;
    }
    const TickType_t start=xTaskGetTickCount(), duration=pdMS_TO_TICKS(std::max(100,timeout_ms));
    while (xTaskGetTickCount()-start<duration) {
        if (cancelled && cancelled()) return false;
        const auto bits=xEventGroupWaitBits(event_group_, WIFI_EVENT_CONNECTED | WIFI_EVENT_SETUP_FAILED,
                                            pdFALSE,pdFALSE,pdMS_TO_TICKS(100));
        if (bits & WIFI_EVENT_SETUP_FAILED) return false;
        if (bits & WIFI_EVENT_CONNECTED) {
            wifi_ap_record_t ap{};
            return esp_wifi_sta_get_ap_info(&ap)==ESP_OK &&
                ssid==std::string(reinterpret_cast<char*>(ap.ssid),strnlen(reinterpret_cast<char*>(ap.ssid),32));
        }
    }
    return false;
}

void WifiStation::FinishSetup(bool keep_connection) {
    if(!manual_setup_.load()) return;
    {
        std::lock_guard<std::recursive_mutex> lock(state_mutex_);
        manual_attempting_.store(false);
        if (!keep_connection) {
            esp_wifi_disconnect();
            xEventGroupClearBits(event_group_,WIFI_EVENT_CONNECTED);
            ssid_.clear();ip_address_.clear();
        }
        // A late disconnect from a cancelled attempt must not retry its
        // uncommitted password. Only the saved-profile scan may reconnect.
        reconnect_count_=keep_connection ? 0 : MAX_RECONNECT_COUNT;
        manual_setup_.store(false);
    }
    if (!keep_connection) StartAutoConnectScan();
}
