#include "sleep_service.h"
#include "sleep_policy.h"
#include "wallpaper.h"
#include "application.h"
#include "board.h"
#include "hal/hal.h"
#include "config.h"
#include "IOExpander.hpp"
#include "usb_virtual_disk.h"
#include "display/raw_display.h"
#include "dashboard/dashboard_service.h"
#include "xiaozhi/xiaozhi_client.h"
#include "xiaozhi/xiaozhi_audio.h"
#include "chat/history_service.h"
#include "network/wifi_setup.h"
#include "notes/note_writer.h"
#include "reader/reader_service.h"
#include "reminders/reminder_service.h"
#include "settings.h"
#include <wifi_station.h>
#include <driver/gpio.h>
#include <driver/usb_serial_jtag.h>
#include <esp_sleep.h>
#include <iot_button.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <ctime>
#include <cstdio>
namespace power {
SleepService& SleepService::Instance(){static SleepService service;return service;}
void SleepService::Start(){
    if(started_)return;
    Settings settings("sleep",false);int value=settings.GetInt("idle_sec",300);
    idle_seconds_=value==0?0:static_cast<unsigned>(std::clamp(value,60,1800));
    Touch(esp_timer_get_time()/1000);started_=true;
}
void SleepService::Toggle(){
    if(esp_timer_get_time()/1000<suppress_click_until_.load())return;
    toggle_.store(true);Application::GetInstance().RequestStatusUpdate();
}
void SleepService::PowerKeyDown(){
    if(Locked()){key_woke_.store(true);Wake();}
    else if(esp_timer_get_time()/1000>=suppress_click_until_.load())key_woke_.store(false);
}
void SleepService::PowerKeyClick(){if(key_woke_.exchange(false))return;Toggle();}
void SleepService::Wake(){wake_.store(true);Application::GetInstance().RequestStatusUpdate();}
bool SleepService::Busy()const{
    auto& audio=xiaozhi::AudioSession::GetInstance();const auto stats=audio.Stats();
    return xiaozhi::Client::GetInstance().BusyForSleep() || stats.capturing || stats.playback_open ||
        stats.decoder_open || stats.reminder_tone || audio.RecorderState().mode!=audio::RecorderMode::Idle ||
        reminders::Service::Instance().IsActive() || network::Busy(network::WifiSetup::Instance().Snapshot().state) ||
        notes::Writer::Instance().Snapshot().busy || reader::Service::Instance().Get().busy ||
        UsbVirtualDisk::GetInstance().IsGadgetActive() || UsbVirtualDisk::GetInstance().IsBusy();
}
bool SleepService::StorageIdle()const{
    const auto history=chat::History::Instance().Snapshot();
    return !history.busy && history.pending==0 && !notes::Writer::Instance().Snapshot().busy &&
        !reader::Service::Instance().Get().busy;
}
bool SleepService::RestorePeripherals(){
    if(buttons_paused_ && iot_button_resume()==ESP_OK)buttons_paused_=false;
    if(audio_suspended_ && GetHAL().SuspendAudioForSleep(false))audio_suspended_=false;
    // A failed DMA resume must not leave an enabled amplifier or unguarded I/O.
    if(pa_changed_ && !audio_suspended_ && IOExpander::getInstance().setLevel(IOExpander::Pin::PA,pa_was_high_)==ESP_OK)pa_changed_=false;
    if(radio_suspended_ && WifiStation::GetInstance().ResumeFromSleep())radio_suspended_=false;
    const bool ok=!audio_suspended_&&!radio_suspended_&&!pa_changed_&&!buttons_paused_;
    if(!ok){++failures_;retry_restore_ms_=esp_timer_get_time()/1000+5000;}
    return ok;
}
void SleepService::Unlock(const char* message){
    Gate::Instance().Thaw();
    const bool restored=RestorePeripherals();
    IOExpander::getInstance().SetInputPollingPaused(false);
    prepared_=false;blocked_=false;Gate::Instance().locked.store(false);Touch(esp_timer_get_time()/1000);
    if(!restored)message="外设恢复未完成，稍后自动重试";
    if(auto* raw=RawDisplay::Instance()){raw->SetLockScreen(false,{});if(message)raw->ShowNotification(message,3500);}
    dashboard::DashboardService::GetInstance().RefreshNow();
    ESP_LOGI("Sleep","awake failures=%lu",(unsigned long)failures_.load());
}
void SleepService::Tick(){
    if(!started_)return;
    const int64_t now_ms=esp_timer_get_time()/1000;
    const bool explicit_toggle=toggle_.exchange(false);
    const bool requested_wake=wake_.exchange(false);
    auto* raw=RawDisplay::Instance();
    if(Locked()&&(requested_wake||explicit_toggle||reminders::Service::Instance().IsActive())){Unlock();return;}
    if(!Locked()){
        if(audio_suspended_||radio_suspended_||pa_changed_||buttons_paused_){
            if(now_ms>=retry_restore_ms_)RestorePeripherals();
            Touch(now_ms);return;
        }
        if(requested_wake){Touch(now_ms);return;}
        const bool auto_due=AutoLockDue(now_ms,Gate::Instance().last_input_ms.load(),idle_seconds_,raw&&raw->HasUnsavedInput());
        if(!explicit_toggle&&!auto_due)return;
        if(!Gate::Instance().Freeze()){if(explicit_toggle)toggle_.store(true);return;}
        if(!raw||Busy()){
            Gate::Instance().Thaw();Touch(now_ms);if(explicit_toggle&&raw)raw->ShowNotification("请先结束对话、录音或保存，再休眠",2500);return;
        }
        Gate::Instance().locked.store(true);Gate::Instance().Thaw();preparing_since_=now_ms;blocked_=false;
        std::vector<uint8_t> wallpaper;
        if(GetHAL().IsSdMounted())(void)LoadWallpaper("/sdcard/wallpaper/lock.pbm",wallpaper);
        raw->SetLockScreen(true,wallpaper);
        ESP_LOGI("Sleep","lock requested; awaiting network and storage");
        return;
    }
    if(blocked_)return; // explicit unlock remains available; no retry loop on hardware error
    const bool paused=xiaozhi::Client::GetInstance().SleepReady() && dashboard::DashboardService::GetInstance().SleepReady();
    if(!paused||!StorageIdle()){
        if(now_ms-preparing_since_>20000)Unlock("未完成保存或网络操作，本次休眠已取消");
        return;
    }
    if(!prepared_){
        if(!Gate::Instance().Freeze())return;
        // 4G modem sleep is board/firmware-specific: lock only, never fake a
        // shutdown AT command or touch the USB/main/SD power rails.
        if(GetHAL().IsWifiMode() && WifiStation::GetInstance().IsStarted()){
            if(!WifiStation::GetInstance().SuspendForSleep()){Gate::Instance().Thaw();Unlock("Wi-Fi 暂停失败，未进入休眠");return;}
            radio_suspended_=true;
        }
        if(!GetHAL().SuspendAudioForSleep(true)){Gate::Instance().Thaw();Unlock("音频尚未空闲，未进入休眠");return;}
        audio_suspended_=true;
        auto& io=IOExpander::getInstance();uint8_t level=0;
        if(io.isInitialized() && io.getLevel(IOExpander::Pin::PA,&level)==ESP_OK){
            pa_was_high_=level!=0;pa_changed_=io.setLevel(IOExpander::Pin::PA,false)==ESP_OK;
        }
        io.SetInputPollingPaused(true);prepared_=true;Gate::Instance().Thaw();
    }
    // Native BLE is an experimental, separately budgeted mode: lock only.
#if CONFIG_WHITEAI_EXPERIMENTAL_BLE_DISCOVERY
    return;
#endif
    const bool host=usb_serial_jtag_is_connected();
    const bool key=gpio_get_level(POWER_BUTTON_GPIO)==0;
    if(!CanEnterSleep(paused,StorageIdle(),host,key,reminders::Service::Instance().IsActive(),GetHAL().IsWifiMode()))return;
    const int64_t now=time(nullptr);int64_t next=0;
    for(const auto& item:reminders::Service::Instance().List()){
        const int64_t at=item.snoozed_until ? item.snoozed_until : item.enabled ? item.at : 0;
        if(at>0 && (!next||at<next))next=at;
    }
    const auto duration=SleepWindowUs(now,next);if(duration==0)return;
    if(!Gate::Instance().Freeze())return;
    // Check again behind the barrier; a queued alarm may have become active.
    if(reminders::Service::Instance().IsActive()||gpio_get_level(POWER_BUTTON_GPIO)==0){Gate::Instance().Thaw();return;}
    // A 5 ms button timer must not replay thousands of missed callbacks.
    // Pause only across actual CPU sleep; GPIO level remains our wake source.
    esp_err_t err=iot_button_stop();
    if(err!=ESP_OK){Gate::Instance().Thaw();Unlock("按键暂停失败，未进入休眠");return;}
    buttons_paused_=true;
    err=gpio_wakeup_enable(POWER_BUTTON_GPIO,GPIO_INTR_LOW_LEVEL);
    if(err==ESP_OK)err=esp_sleep_enable_gpio_wakeup();
    if(err==ESP_OK)err=esp_sleep_enable_timer_wakeup(duration);
    const int64_t before=esp_timer_get_time();
    if(err==ESP_OK)err=esp_light_sleep_start();
    const auto cause=esp_sleep_get_wakeup_cause();
    if(cause==ESP_SLEEP_WAKEUP_GPIO){key_woke_.store(true);suppress_click_until_.store(esp_timer_get_time()/1000+1000);}
    const auto button_error=iot_button_resume();
    if(button_error==ESP_OK)buttons_paused_=false;
    else if(err==ESP_OK)err=button_error;
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
    (void)gpio_wakeup_disable(POWER_BUTTON_GPIO);
    Gate::Instance().Thaw();
    if(err!=ESP_OK){++failures_;blocked_=true;ESP_LOGW("Sleep","light sleep failed code=%d",int(err));Unlock("休眠未成功，设备保持唤醒");return;}
    ++sleeps_;slept_us_.fetch_add(esp_timer_get_time()-before);
    if(cause==ESP_SLEEP_WAKEUP_GPIO){key_woke_.store(true);suppress_click_until_.store(esp_timer_get_time()/1000+1000);Unlock();}
    // Timer wakes do not redraw the wallpaper or reconnect Wi-Fi. The next
    // event tick yields to reminders and sleeps again only if no alarm is due.
}
std::string SleepService::Status()const{
    char out[192];std::snprintf(out,sizeof(out),"locked=%d sleeps=%lu slept_ms=%lld failures=%lu idle_sec=%u usb_guard=1",
        Locked(),(unsigned long)sleeps_.load(),(long long)(slept_us_.load()/1000),(unsigned long)failures_.load(),idle_seconds_.load());return out;
}
}
