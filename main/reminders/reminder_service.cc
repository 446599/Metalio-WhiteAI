#include "reminder_service.h"
#include "system/quick_controls.h"
#include "application.h"
#include "board.h"
#include "dashboard/dashboard_data.h"
#include "display/raw_display.h"
#include "xiaozhi/xiaozhi_audio.h"
#include "notes/note_service.h"
#include "system/device_control.h"
#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <nvs.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

namespace reminders {
namespace {
constexpr const char* TAG = "Reminders";
bool SaveNvs(const std::string& json) {
    nvs_handle_t handle;
    if (nvs_open("reminders",NVS_READWRITE,&handle) != ESP_OK) return false;
    const bool ok = nvs_set_blob(handle,"items",json.c_str(),json.size()+1) == ESP_OK && nvs_commit(handle) == ESP_OK;
    nvs_close(handle); return ok;
}
bool LoadNvs(std::string& json) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("reminders",NVS_READONLY,&handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return true;
    if (err != ESP_OK) return false;
    size_t size = 0; err = nvs_get_blob(handle,"items",nullptr,&size);
    if (err == ESP_ERR_NVS_NOT_FOUND) { nvs_close(handle); return true; }
    if (err != ESP_OK || size < 2 || size > 8193) { nvs_close(handle); return false; }
    std::vector<char> data(size);
    err = nvs_get_blob(handle,"items",data.data(),&size); nvs_close(handle);
    if (err != ESP_OK || data.back() != '\0') return false;
    json.assign(data.data(),size-1); return json.find('\0') == std::string::npos;
}
void Notify(const std::string& text, int duration) {
    (void)Application::GetInstance().ScheduleUi([text,duration]() {
        if (auto* display = Board::GetInstance().GetDisplay()) {
            display->SetPowerSaveMode(false);
            display->ShowNotification(text.c_str(),duration);
        }
    });
}
}  // namespace
Service::Service() : store_(SaveNvs),
    system_tools_(store_,notes::DeviceStore(),[](const auto& command){return device::Control::Instance().Execute(command);}),
    remote_(store_,[this]{ return Dismiss(); },&system_tools_),
    diagnostic_(store_,[this]{ return Dismiss(); },&system_tools_) {}
Service& Service::Instance() { static Service service; return service; }
void Service::Start() {
    if (task_) return;
    std::string json;
    if (!LoadNvs(json) || !store_.Restore(json)) ESP_LOGE(TAG,"reminder storage unavailable; preserving saved data");
    Publish();
    if (xTaskCreatePinnedToCore(Task,"reminders",8192,this,2,&task_,0) != pdPASS) {
        task_ = nullptr; ESP_LOGE(TAG,"reminder task creation failed");
    }
}
bool Service::Dismiss(uint32_t token) {
    std::lock_guard<std::mutex> lock(alert_mutex_);
    if (!alert_.active || (token && token != alert_.token)) return false;
    alert_action_ = Action::Stop;
    return true;
}
bool Service::Snooze(uint32_t token) {
    std::lock_guard<std::mutex> lock(alert_mutex_);
    if (!alert_.active || token != alert_.token || alert_action_ != Action::None) return false;
    alert_action_ = Action::Snooze;
    return true;
}
void Service::PublishAlert() {
    AlertSnapshot snapshot;
    { std::lock_guard<std::mutex> lock(alert_mutex_); snapshot = alert_; }
    alert_dirty_ = !Application::GetInstance().ScheduleUi([snapshot]() {
        if (auto* display = RawDisplay::Instance()) display->ShowReminderAlert(snapshot);
    });
}
void Service::Task(void* arg) { static_cast<Service*>(arg)->Run(); }
bool Service::SetEnabled(uint32_t id, bool enabled, std::string& error) {
    std::lock_guard<std::mutex> lock(rpc_mutex_);
    for (auto item : store_.List()) {
        if (item.id != id) continue;
        item.enabled = enabled;
        item.snoozed_until = 0;
        if (enabled && item.weekdays && item.at <= time(nullptr)) item.at = NextOccurrence(item, time(nullptr));
        Item saved;
        return store_.Put(item, time(nullptr), saved, error);
    }
    error = "提醒已不存在";
    return false;
}
std::string Service::HandleMcp(const std::string& payload, uint32_t epoch, bool diagnostic) {
    std::lock_guard<std::mutex> lock(rpc_mutex_);
    if (!task_) return R"({"jsonrpc":"2.0","id":null,"error":{"code":-32603,"message":"Reminder service unavailable"}})";
    if (!diagnostic && epoch != rpc_epoch_) { remote_.Reset(); rpc_epoch_ = epoch; }
    cJSON* root = cJSON_Parse(payload.c_str());
    const auto* method = root ? cJSON_GetObjectItemCaseSensitive(root,"method") : nullptr;
    if (!diagnostic && cJSON_IsString(method)) {
        ++requests_;
        if (!std::strcmp(method->valuestring,"initialize")) ++initialized_;
        else if (!std::strcmp(method->valuestring,"tools/list")) ++discoveries_;
        else if (!std::strcmp(method->valuestring,"tools/call")) ++calls_;
        ESP_LOGI(TAG,"MCP method=%s",method->valuestring);
    }
    cJSON_Delete(root);
    return (diagnostic ? diagnostic_ : remote_).Handle(payload,time(nullptr));
}
std::string Service::Status() const {
    char result[416];
    const auto audio = xiaozhi::AudioSession::GetInstance().Stats();
    const auto now = time(nullptr); const auto local = LocalTime(now);
    std::snprintf(result,sizeof(result),
        "ready=%d clock_valid=%d items=%u active=%lu requests=%lu initialize=%lu tools_list=%lu calls=%lu tone=%d tone_frames=%lu tone_err=%lu stack=%lu local=%s timezone=Asia/Shanghai",
        task_ && store_.Ready(),ValidClock(now),static_cast<unsigned>(store_.List().size()),
        static_cast<unsigned long>(active_id_.load()),static_cast<unsigned long>(requests_.load()),
        static_cast<unsigned long>(initialized_.load()),static_cast<unsigned long>(discoveries_.load()),
        static_cast<unsigned long>(calls_.load()),audio.reminder_tone,
        static_cast<unsigned long>(audio.reminder_frames),static_cast<unsigned long>(audio.reminder_errors),
        static_cast<unsigned long>(task_ ? uxTaskGetStackHighWaterMark(task_) : 0),local.c_str());
    return result;
}
void Service::Publish() {
    const auto revision = store_.Revision(); const auto minute = time(nullptr)/60;
    if (revision == published_revision_ && minute == published_minute_) return;
    std::array<dashboard::ScheduleItem,dashboard::kScheduleCount> schedule{};
    size_t count = 0;
    for (const auto& i : store_.List()) {
        if ((!i.enabled && !i.snoozed_until) || count == schedule.size()) continue;
        auto& row = schedule[count++];
        const auto local = LocalTime(i.snoozed_until ? i.snoozed_until : i.at);
        dashboard::CopyText(row.time,sizeof(row.time),local.substr(11,5).c_str());
        dashboard::CopyText(row.title,sizeof(row.title),i.title.c_str());
        char detail[64];
        std::snprintf(detail,sizeof(detail),"%s / %s%s",local.substr(5,5).c_str(),
                      i.kind == "alarm" ? "闹钟" : "日程",i.snoozed_until ? " / 稍后" : i.weekdays ? " / 重复" : "");
        dashboard::CopyText(row.detail,sizeof(row.detail),detail);
    }
    auto& dashboard = dashboard::DashboardData::GetInstance();
    dashboard.SetScheduleCount(count);
    for (size_t i = 0; i < count; ++i) {
        const auto& row = schedule[i];
        dashboard.SetSchedule(i, row.time, row.title, row.detail, row.done);
    }
    published_revision_ = revision; published_minute_ = minute;
}
void Service::Run() {
    int64_t deadline_ms = 0, next_pulse_ms = 0, next_tone_attempt_ms=0;
    bool playing_tone=false;
    auto& preferences=device::QuickControls::Instance();
    std::vector<uint32_t> active_ids;
    auto& audio = xiaozhi::AudioSession::GetInstance();
    while (true) {
        Action action;
        { std::lock_guard<std::mutex> lock(alert_mutex_); action = alert_action_; alert_action_ = Action::None; }
        if (action != Action::None) {
            std::string error;
            const bool done = action == Action::Stop || store_.Snooze(active_ids,time(nullptr),error);
            if (done) {
                audio.SetReminderTone(false);playing_tone=false;
                active_id_.store(0);
                active_ids.clear();
                { std::lock_guard<std::mutex> lock(alert_mutex_); alert_.active = false; alert_.audible = false; }
                Notify(action == Action::Snooze ? "已延后 5 分钟" : "闹钟已停止",1800);
                ESP_LOGI(TAG,"alert %s",action == Action::Snooze ? "snoozed" : "stopped");
            } else {
                std::lock_guard<std::mutex> lock(alert_mutex_);
                alert_.message = error;
            }
            alert_dirty_ = true;
        }
        std::vector<Item> due;
        if (ValidClock(time(nullptr)) && !store_.TakeDue(time(nullptr),due) && store_.Ready()) {
            ESP_LOGW(TAG,"due scan deferred: persistence unavailable");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        const int64_t now_ms = esp_timer_get_time()/1000;
        if (!due.empty()) {
            preferences.CancelBluetooth();
            for (const auto& item : due) {
                if (std::find(active_ids.begin(),active_ids.end(),item.id) == active_ids.end()) active_ids.push_back(item.id);
                ESP_LOGI(TAG,"due id=%lu kind=%s",static_cast<unsigned long>(item.id),item.kind.c_str());
            }
            active_id_.store(due.front().id);
            deadline_ms = now_ms + 180000;
            next_pulse_ms = now_ms;
            const bool tone_requested=preferences.RingEnabled();
            const bool tone_started=!tone_requested || audio.SetReminderTone(true);
            playing_tone=tone_requested && tone_started;
            next_tone_attempt_ms=now_ms+5000;
            {
                std::lock_guard<std::mutex> lock(alert_mutex_);
                ++alert_.token;
                alert_.active = true;
                alert_.audible = true;
                alert_.title = due.front().title;
                alert_.at = due.front().at;
                alert_.count = active_ids.size();
                alert_.message = !tone_started ? "铃声暂不可用，请查看提醒" :
                    !tone_requested && !preferences.VibrationEnabled() ? "静默提醒" : "";
                alert_action_ = Action::None;
            }
            alert_dirty_ = true;
        }
        bool audible;
        { std::lock_guard<std::mutex> lock(alert_mutex_); audible = alert_.active && alert_.audible; }
        if (audible && now_ms >= deadline_ms) {
            audio.SetReminderTone(false);playing_tone=false;
            {
                std::lock_guard<std::mutex> lock(alert_mutex_);
                alert_.audible = false;
                alert_.message = "已自动静音，请处理提醒";
            }
            audible = false;
            alert_dirty_ = true;
        }
        const bool want_tone=audible && preferences.RingEnabled();
        if(want_tone!=playing_tone && (!want_tone || now_ms>=next_tone_attempt_ms)) {
            const bool applied=audio.SetReminderTone(want_tone);
            playing_tone=want_tone && applied;
            next_tone_attempt_ms=now_ms+5000;
        }
        if (audible && now_ms >= next_pulse_ms) {
            if(preferences.VibrationEnabled()) Board::GetInstance().PulseVibration();
            next_pulse_ms = now_ms + 2000;
        }
        if (alert_dirty_) PublishAlert();
        Publish();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
}  // namespace reminders
