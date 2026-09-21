#include "device_control.h"
#include "application.h"
#include "display/raw_display.h"
#include "dashboard/dashboard_service.h"
#include "hal/hal.h"
#include "reminders/reminder_service.h"
#include "xiaozhi/xiaozhi_audio.h"
#include "xiaozhi/xiaozhi_client.h"
#include <cJSON.h>
#include <esp_timer.h>
#include <memory>

namespace device {
namespace {
using Kind=xiaozhi::SystemCommand::Kind;
using Json=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
int64_t Now() {return esp_timer_get_time()/1000;}
xiaozhi::ToolReply Reply(const cJSON* data,bool ok=true) {
    char* text=cJSON_PrintUnformatted(data);if (!text) return {false,"内存不足"};
    std::string value(text);cJSON_free(text);return {ok,value};
}
void RecorderFields(cJSON* data) {
    const auto state=xiaozhi::AudioSession::GetInstance().RecorderState();
    const char* names[]={"idle","recording","playing","loading"};
    cJSON_AddStringToObject(data,"state",names[static_cast<unsigned>(state.mode)]);
    cJSON_AddNumberToObject(data,"seconds",state.seconds);cJSON_AddBoolToObject(data,"has_clip",state.has_clip);
    cJSON_AddBoolToObject(data,"saved_to_sd",state.saved);cJSON_AddBoolToObject(data,"failed",state.failed);
    cJSON_AddNumberToObject(data,"maximum_seconds",30);
}
bool RecordingAction(Kind kind) {return kind==Kind::Record || kind==Kind::PlayRecording;}
}
Control& Control::Instance() {static Control instance;return instance;}
xiaozhi::ToolReply Control::Execute(const xiaozhi::SystemCommand& command) {
    Json data(cJSON_CreateObject(),cJSON_Delete);if (!data) return {false,"内存不足"};
    const auto kind=command.kind;
    if (kind==Kind::ActionStatus || kind==Kind::CancelAction) {
        if (kind==Kind::CancelAction && !actions_.Cancel(command.number)) return {false,"操作已执行、已结束或不存在"};
        const auto item=actions_.Get(command.number,Now());
        if (!item) return {false,"操作记录不存在或已过期清理，请查询设备实际状态"};
        cJSON_AddNumberToObject(data.get(),"id",item->id);
        cJSON_AddStringToObject(data.get(),"status",StateName(item->state));
        cJSON_AddStringToObject(data.get(),"detail",item->detail.c_str());return Reply(data.get());
    }
    if (kind==Kind::RecorderStatus) {RecorderFields(data.get());return Reply(data.get());}
    if (kind==Kind::Status) {
        if (auto* display=RawDisplay::Instance()) {
            const auto status=display->SystemSnapshot();
            cJSON_AddStringToObject(data.get(),"app",status.app.c_str());
            cJSON_AddNumberToObject(data.get(),"battery_percent",status.battery);
            cJSON_AddBoolToObject(data.get(),"charging",status.charging);
            cJSON_AddBoolToObject(data.get(),"sleeping",status.sleeping);
        }
        cJSON_AddNumberToObject(data.get(),"volume",GetHAL().GetVolume());
        const auto snapshot=dashboard::DashboardData::GetInstance().GetSnapshot();
        cJSON_AddStringToObject(data.get(),"network",snapshot.network);
        cJSON_AddBoolToObject(data.get(),"ai_connected",xiaozhi::Client::GetInstance().IsConnected());
        cJSON_AddBoolToObject(data.get(),"sd_mounted",GetHAL().IsSdMounted());
        cJSON_AddBoolToObject(data.get(),"alarm_active",reminders::Service::Instance().IsActive());
        auto* recorder=cJSON_AddObjectToObject(data.get(),"recorder");RecorderFields(recorder);
        return Reply(data.get());
    }
    if (kind==Kind::Dashboard) {
        const auto snapshot=dashboard::DashboardData::GetInstance().GetSnapshot();
        auto* weather=cJSON_AddObjectToObject(data.get(),"weather");
        cJSON_AddBoolToObject(weather,"valid",snapshot.weather.valid);
        cJSON_AddStringToObject(weather,"location",snapshot.weather.location);
        cJSON_AddStringToObject(weather,"condition",snapshot.weather.valid ? snapshot.weather.condition : "无数据");
        if (snapshot.weather.valid) cJSON_AddNumberToObject(weather,"temperature_c",snapshot.weather.temperature_c);
        cJSON_AddNumberToObject(weather,"updated_epoch",snapshot.weather.updated_epoch);
        cJSON_AddBoolToObject(weather,"from_cache",snapshot.weather.from_cache);
        char status[96];dashboard::FormatWeatherStatus(snapshot.weather,status,sizeof(status));
        cJSON_AddStringToObject(weather,"status",status);
        auto* quota=cJSON_AddObjectToObject(data.get(),"quota");
        cJSON_AddBoolToObject(quota,"valid",snapshot.quota.valid);
        if (snapshot.quota.valid) {
            cJSON_AddNumberToObject(quota,"five_hour_remaining",snapshot.quota.five_hour_remaining);
            cJSON_AddNumberToObject(quota,"weekly_remaining",snapshot.quota.weekly_remaining);
        }
        cJSON_AddNumberToObject(quota,"updated_epoch",snapshot.quota.updated_epoch);
        dashboard::FormatQuotaStatus(snapshot.quota,status,sizeof(status));cJSON_AddStringToObject(quota,"status",status);
        return Reply(data.get());
    }
    if (kind==Kind::Refresh) {
        dashboard::DashboardService::GetInstance().RefreshNow();
        cJSON_AddStringToObject(data.get(),"status","requested");return Reply(data.get());
    }
    if (kind==Kind::StopRecording) {
        std::lock_guard<std::mutex> lock(recorder_mutex_);
        actions_.CancelRecordings();xiaozhi::AudioSession::GetInstance().StopRecorder();
        cJSON_AddStringToObject(data.get(),"status","stop_requested");return Reply(data.get());
    }
    const auto id=actions_.Add(command,Now());
    if (!id) return {false,"系统操作队列已满，请稍后重试"};
    if (!RecordingAction(kind) && !Application::GetInstance().ScheduleUi([this,id,command](){Run(id,command);})) {
        actions_.Finish(id,false,"界面队列已满，未执行");return {false,"界面队列已满，未执行"};
    }
    cJSON_AddNumberToObject(data.get(),"id",id);cJSON_AddStringToObject(data.get(),"status","queued");
    cJSON_AddStringToObject(data.get(),"detail",RecordingAction(kind) ? "等待AI回答结束及音频空闲；30秒未执行则过期" : "等待界面任务执行");
    return Reply(data.get());
}
void Control::Run(uint32_t id,const xiaozhi::SystemCommand& command) {
    if (!actions_.Begin(id,Now())) return;
    auto* display=RawDisplay::Instance();bool ok=false;std::string detail;
    if (command.kind==Kind::Volume) {
        GetHAL().SetVolume(command.number);ok=GetHAL().GetVolume()==static_cast<int>(command.number);
        detail=ok ? "音量已设置" : "音量设置失败";
    } else if (command.kind==Kind::Open || command.kind==Kind::Calendar) {
        ok=display && display->OpenSystemApp(command.kind==Kind::Calendar ? "calendar" : command.value,
                                            command.kind==Kind::Calendar ? command.value : "");
        detail=ok ? "页面已打开" : "页面无法打开或闹钟正在响铃";
    } else if (RecordingAction(command.kind)) {
        auto& audio=xiaozhi::AudioSession::GetInstance();
        ok=display && display->OpenSystemApp("recorder");
        if (ok) {
            std::lock_guard<std::mutex> lock(recorder_mutex_);
            const auto action=actions_.Get(id,Now());
            ok=action && action->state==ActionState::Running && audio.StartRecorder(command.kind==Kind::PlayRecording);
        }
        detail=ok ? (command.kind==Kind::Record ? "已开始录音" : "已开始回放") : "音频忙碌、没有录音或闹钟正在响铃";
    }
    actions_.Finish(id,ok,detail);
    if (display) display->UpdateStatusBar(true);
}
void Control::Tick() {
    const auto now=Now();const auto pending=actions_.Pending(now);
    if (pending.empty()) {audio_idle_since_=0;return;}
    auto& audio=xiaozhi::AudioSession::GetInstance();
    const auto state=xiaozhi::Conversation::GetInstance().Snapshot().state;
    const auto stats=audio.Stats();
    const bool idle=(state==xiaozhi::TurnState::Idle || state==xiaozhi::TurnState::Done || state==xiaozhi::TurnState::Error) &&
        !stats.capturing && !stats.playback_open && !stats.decoder_open && !reminders::Service::Instance().IsActive();
    if (!idle) {audio_idle_since_=0;return;}
    if (!audio_idle_since_) {audio_idle_since_=now;return;}
    if (now-audio_idle_since_<2000) return;
    for (const auto& action:pending) {
        if (!RecordingAction(action.command.kind) || now-(action.deadline-30000)<2000) continue;
        if (audio.RecorderState().mode!=audio::RecorderMode::Idle) return;
        if (!audio.RecorderState().has_clip) {
            audio.RestoreRecorder();
            if (audio.RecorderState().mode==audio::RecorderMode::Loading) return;
        }
        Run(action.id,action.command);audio_idle_since_=0;break;
    }
}
}
