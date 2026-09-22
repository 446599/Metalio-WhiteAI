#include "quick_controls.h"
#include "application.h"
#include "hal/hal.h"
#include "xiaozhi/xiaozhi_audio.h"
#include "xiaozhi/conversation.h"
#include "reminders/reminder_service.h"
#include <algorithm>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace device {
QuickControls& QuickControls::Instance(){static QuickControls service;return service;}
QuickSnapshot QuickControls::Snapshot()const{std::lock_guard<std::mutex> lock(mutex_);return state_;}
uint32_t QuickControls::Revision()const{std::lock_guard<std::mutex> lock(mutex_);return state_.revision;}
void QuickControls::Start(){
    std::lock_guard<std::mutex> guard(settings_mutex_);
    if(started_) return;
    uint8_t mode=3;nvs_handle_t handle=0;
    const auto opened=nvs_open("quick_controls",NVS_READONLY,&handle);
    if(opened==ESP_OK){
        if(nvs_get_u8(handle,"alerts",&mode)!=ESP_OK || mode>3) mode=3;
        nvs_close(handle);
    }
    alerts_.store(mode);started_=true;
    std::lock_guard<std::mutex> lock(mutex_);
    state_.ring=mode&1;state_.vibration=mode&2;
    state_.version=esp_app_get_description()->version;++state_.revision;
}
void QuickControls::Refresh(){
    // Never call this while holding DisplayLockGuard.
    const int volume=GetHAL().GetVolume();
    const bool wifi=GetHAL().IsWifiMode(),connected=wifi && GetHAL().WifiIsConnected();
    const std::string network=connected ? GetHAL().WifiSsid() : wifi ? "Wi-Fi 未连接" : "4G 模式";
    std::lock_guard<std::mutex> lock(mutex_);
    if(state_.volume!=volume || state_.wifi_mode!=wifi || state_.wifi_connected!=connected || state_.network!=network) ++state_.revision;
    state_.volume=volume;state_.wifi_mode=wifi;state_.wifi_connected=connected;state_.network=network;
    state_.uptime_seconds=esp_timer_get_time()/1000000;
    state_.free_internal=heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    state_.largest_internal=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
}
bool QuickControls::SetVolume(int value){
    value=std::clamp(value,0,100);GetHAL().SetVolume(value);
    const int actual=GetHAL().GetVolume();
    std::lock_guard<std::mutex> lock(mutex_);
    state_.volume=actual;state_.message=actual==value ? "播放音量已调整" : "音量调整失败";++state_.revision;
    return actual==value;
}
bool QuickControls::SetAlerts(bool ring,bool vibration){
    std::lock_guard<std::mutex> guard(settings_mutex_);
    const uint8_t mode=(ring?1:0)|(vibration?2:0);
    nvs_handle_t handle=0;auto error=nvs_open("quick_controls",NVS_READWRITE,&handle);
    if(error==ESP_OK){error=nvs_set_u8(handle,"alerts",mode);if(error==ESP_OK) error=nvs_commit(handle);nvs_close(handle);}
    std::lock_guard<std::mutex> lock(mutex_);
    if(error==ESP_OK){alerts_.store(mode);state_.ring=ring;state_.vibration=vibration;state_.message=mode ? "提醒方式已保存" : "静默提醒：仍显示到期卡片";}
    else state_.message="保存失败，原提醒方式未改变";
    ++state_.revision;return error==ESP_OK;
}
bool QuickControls::ScanBluetooth(){
    const auto stats=xiaozhi::AudioSession::GetInstance().Stats();
    const auto turn=xiaozhi::Conversation::GetInstance().Snapshot().state;
    const bool speech=turn!=xiaozhi::TurnState::Idle && turn!=xiaozhi::TurnState::Done && turn!=xiaozhi::TurnState::Error;
    std::lock_guard<std::mutex> lock(mutex_);
    if(state_.ble_busy) return false;
    if(speech || stats.capturing || stats.playback_open || stats.decoder_open ||
       xiaozhi::AudioSession::GetInstance().RecorderState().mode!=audio::RecorderMode::Idle || reminders::Service::Instance().IsActive()) {
        state_.message="请在录音、回答和提醒结束后扫描蓝牙";++state_.revision;return false;
    }
    cancel_scan_.store(false);state_.ble_busy=true;state_.ble_cancelled=false;state_.ble_ok=false;
    state_.nearby.clear();state_.message="正在扫描 BLE，结束后释放蓝牙栈";++state_.revision;
    if(xTaskCreate(ScanTask,"quick_ble",4096,this,2,nullptr)!=pdPASS){
        state_.ble_busy=false;state_.message="内存不足，蓝牙扫描未启动";++state_.revision;return false;
    }
    return true;
}
void QuickControls::CancelBluetooth(){
    std::lock_guard<std::mutex> lock(mutex_);
    if(state_.ble_busy){cancel_scan_.store(true);state_.message="正在停止蓝牙扫描";++state_.revision;}
}
void QuickControls::ScanTask(void* arg){
    {
        auto& self=*static_cast<QuickControls*>(arg);
        std::vector<HalBtDevice> devices;std::string detail;
        const bool ok=GetHAL().BleScan(devices,detail,6000,[&self]{return self.cancel_scan_.load();});
        std::lock_guard<std::mutex> lock(self.mutex_);
        self.state_.ble_busy=false;self.state_.ble_cancelled=self.cancel_scan_.load();
        self.state_.ble_ok=ok && !self.state_.ble_cancelled;
        if(self.state_.ble_ok) for(const auto& item:devices){
            if(self.state_.nearby.size()==24) break;
            self.state_.nearby.push_back({item.name,item.addr,item.rssi});
        }
        self.state_.message=self.state_.ble_cancelled ? "蓝牙扫描已停止" : ok ? "BLE 扫描完成；不支持耳机配对" : "蓝牙扫描失败，未改变音频模式";
        ++self.state_.revision;
    }
    Application::GetInstance().RequestStatusUpdate(true);vTaskDelete(nullptr);
}
} // namespace device
