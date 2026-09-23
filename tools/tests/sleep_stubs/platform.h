#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#include <ctime>
#include "reminders/reminder_store.h"
namespace sim {
inline int64_t us=1000000,epoch=1770000000,last_sleep=0;
inline bool usb=false,key=false,wifi=true,wifi_started=true,radio_ok=true,resume_ok=true,codec_ok=true,paused=false,audio=false,pa=true,poll=false;
inline bool client_busy=false,client_ready=true,dashboard_ready=true,storage_busy=false,radio_busy=false,alarm=false,writer_busy=false,usb_disk=false,editing=false;
inline bool buttons=false; inline int button_stop_error=0,button_resume_error=0;
inline int pending=0,sleep_calls=0,sleep_error=0,wake_cause=1,config_error=0,draws=0,unlocks=0;inline int idle_sec=300;
inline std::string message;
inline std::vector<reminders::Item> reminders;
inline void Reset(){buttons=false;button_stop_error=button_resume_error=0;us+=1000000000;usb=key=false;wifi=wifi_started=radio_ok=resume_ok=codec_ok=pa=true;paused=audio=poll=false;client_busy=storage_busy=radio_busy=alarm=writer_busy=usb_disk=editing=false;client_ready=dashboard_ready=true;pending=sleep_error=config_error=0;wake_cause=1;message.clear();reminders.clear();}
}
inline int64_t esp_timer_get_time(){return sim::us;}
inline time_t fake_time(time_t* p){if(p)*p=sim::epoch;return sim::epoch;}
#define time fake_time
using esp_err_t=int;constexpr int ESP_OK=0,POWER_BUTTON_GPIO=3,GPIO_INTR_LOW_LEVEL=0,ESP_SLEEP_WAKEUP_TIMER=1,ESP_SLEEP_WAKEUP_GPIO=2;
template<class...T> void Quiet(const T&...){}
#define ESP_LOGI(...) Quiet(__VA_ARGS__)
#define ESP_LOGW(...) Quiet(__VA_ARGS__)
inline bool usb_serial_jtag_is_connected(){return sim::usb;}
inline int iot_button_stop(){if(sim::button_stop_error)return sim::button_stop_error;sim::buttons=true;return 0;}
inline int iot_button_resume(){if(sim::button_resume_error)return sim::button_resume_error;sim::buttons=false;return 0;}
inline int gpio_get_level(int){return !sim::key;}
inline int gpio_wakeup_enable(int pin,int){assert(pin==3);return sim::config_error;}
inline int gpio_wakeup_disable(int){return 0;}
inline int esp_sleep_enable_gpio_wakeup(){return 0;}
inline int esp_sleep_enable_timer_wakeup(uint64_t duration){sim::last_sleep=duration;return 0;}
inline int esp_sleep_disable_wakeup_source(int){return 0;}
inline int esp_sleep_get_wakeup_cause(){return sim::wake_cause;}
inline int esp_light_sleep_start(){assert(sim::buttons && sim::paused && sim::audio && !sim::pa && sim::poll && !sim::usb && !sim::key);++sim::sleep_calls;sim::us+=sim::last_sleep;return sim::sleep_error;}
class Application {public:static Application& GetInstance(){static Application a;return a;}void RequestStatusUpdate(bool=false){}};
class Settings {public:Settings(const char*,bool){}int GetInt(const char*,int){return sim::idle_sec;}};
class RawDisplay {public:static RawDisplay* Instance(){static RawDisplay r;return &r;}bool HasUnsavedInput()const{return sim::editing;}void SetLockScreen(bool on,const std::vector<uint8_t>&){if(on)++sim::draws;else ++sim::unlocks;}void ShowNotification(const char* m,int){sim::message=m;}};
struct FakeHal {bool IsWifiMode(){return sim::wifi;}bool IsSdMounted(){return false;}bool SuspendAudioForSleep(bool on){if(!sim::codec_ok)return false;sim::audio=on;return true;}};
inline FakeHal& GetHAL(){static FakeHal h;return h;}
class IOExpander {public:enum class Pin{PA};static IOExpander& getInstance(){static IOExpander i;return i;}bool isInitialized(){return true;}int getLevel(Pin,uint8_t* v){*v=sim::pa;return 0;}int setLevel(Pin,bool high){sim::pa=high;return 0;}void SetInputPollingPaused(bool p){sim::poll=p;}};
class UsbVirtualDisk{public:static UsbVirtualDisk& GetInstance(){static UsbVirtualDisk d;return d;}bool IsGadgetActive(){return sim::usb_disk;}bool IsBusy(){return false;}};
class WifiStation {public:static WifiStation& GetInstance(){static WifiStation s;return s;}bool IsStarted(){return sim::wifi_started;}bool SuspendForSleep(){if(!sim::radio_ok)return false;sim::paused=true;return true;}bool ResumeFromSleep(){if(!sim::resume_ok)return false;sim::paused=false;return true;}};
namespace xiaozhi {
class Client {public:static Client& GetInstance(){static Client c;return c;}bool SleepReady(){return sim::client_ready;}bool BusyForSleep(){return sim::client_busy;}};
}
namespace audio {enum class RecorderMode{Idle,Record};}
namespace xiaozhi {class AudioSession {public:struct Data{bool capturing=false,playback_open=false,decoder_open=false,reminder_tone=false;};struct Recorder{audio::RecorderMode mode=audio::RecorderMode::Idle;};static AudioSession& GetInstance(){static AudioSession a;return a;}Data Stats(){return {};}Recorder RecorderState(){return {};}};}
namespace dashboard {class DashboardService{public:static DashboardService& GetInstance(){static DashboardService s;return s;}bool SleepReady(){return sim::dashboard_ready;}void RefreshNow(){}};}
namespace chat {class History{public:struct View{bool busy;int pending;};static History& Instance(){static History h;return h;}View Snapshot(){return {sim::storage_busy,sim::pending};}};}
namespace network {inline bool Busy(bool value){return value;}class WifiSetup{public:struct View{bool state;};static WifiSetup& Instance(){static WifiSetup w;return w;}View Snapshot(){return {sim::radio_busy};}};}
namespace notes {class Writer{public:struct View{bool busy;};static Writer& Instance(){static Writer w;return w;}View Snapshot(){return {sim::writer_busy};}};}
namespace reminders {class Service{public:static Service& Instance(){static Service s;return s;}bool IsActive(){return sim::alarm;}std::vector<Item> List(){return sim::reminders;}};}
