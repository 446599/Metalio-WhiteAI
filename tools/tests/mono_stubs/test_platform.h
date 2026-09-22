#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <cstring>
#include <utility>
#include <algorithm>
#include <cassert>
using esp_err_t=int;using nvs_handle_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_NVS_NOT_FOUND=1,NVS_READONLY=0,NVS_READWRITE=1,pdPASS=1;
inline bool test_commit_ok=true,test_task_ok=true;
inline std::map<std::string,std::vector<uint8_t>> durable,pending;
inline std::map<int,std::string> namespaces;
inline int test_handle=0,test_updates=0;
inline std::vector<std::pair<void(*)(void*),void*>> tasks;
inline int nvs_open(const char* name,int,nvs_handle_t* h){*h=++test_handle;namespaces[*h]=name;return ESP_OK;}
inline void nvs_close(nvs_handle_t h){namespaces.erase(h);}
inline int nvs_get_blob(nvs_handle_t h,const char* key,void* data,size_t* size){
 auto i=durable.find(namespaces[h]+"/"+key);if(i==durable.end())return ESP_ERR_NVS_NOT_FOUND;
 if(!data){*size=i->second.size();return ESP_OK;}
 if(*size<i->second.size())return ESP_FAIL;
 *size=i->second.size();std::memcpy(data,i->second.data(),*size);return ESP_OK;
}
inline int nvs_set_blob(nvs_handle_t h,const char* key,const void* data,size_t size){const auto* p=static_cast<const uint8_t*>(data);pending[namespaces[h]+"/"+key]={p,p+size};return ESP_OK;}
inline int nvs_get_u8(nvs_handle_t h,const char* key,uint8_t* v){size_t size=1;return nvs_get_blob(h,key,v,&size);}
inline int nvs_set_u8(nvs_handle_t h,const char* key,uint8_t v){return nvs_set_blob(h,key,&v,1);}
inline int nvs_commit(nvs_handle_t){if(!test_commit_ok){pending.clear();return ESP_FAIL;}for(auto& p:pending)durable[p.first]=p.second;pending.clear();return ESP_OK;}
inline int xTaskCreate(void(*f)(void*),const char*,uint32_t,void* arg,unsigned,void*){if(!test_task_ok)return 0;tasks.emplace_back(f,arg);return pdPASS;}
inline void vTaskDelete(void*){}
inline void RunTasks(){while(!tasks.empty()){auto t=tasks.front();tasks.erase(tasks.begin());t.first(t.second);}}
inline int64_t esp_timer_get_time(){return 123000000;}
constexpr int MALLOC_CAP_INTERNAL=1;
inline size_t heap_caps_get_free_size(int){return 65536;}
inline size_t heap_caps_get_largest_free_block(int){return 32768;}
struct esp_app_desc_t{const char* version="host-test";};
inline const esp_app_desc_t* esp_app_get_description(){static esp_app_desc_t d;return &d;}
class Application{public:static Application& GetInstance(){static Application a;return a;}void RequestStatusUpdate(bool){++test_updates;}};
struct HalBtDevice{std::string name,addr;int rssi;};
class Hal{public:
 mutable std::function<void()> sd_probe;
 bool sd=true,wifi=true,connected=true,ble_ok=true;int volume=40;unsigned scan_calls=0;
 bool IsSdMounted()const{if(sd_probe)sd_probe();return sd;}bool IsWifiMode()const{return wifi;}
 bool WifiIsConnected()const{return connected;}std::string WifiSsid()const{return "Test AP";}
 int GetVolume()const{return volume;}void SetVolume(int v){volume=v;}
 bool BleScan(std::vector<HalBtDevice>& out,std::string&,uint32_t,const std::function<bool()>& cancelled){++scan_calls;if(cancelled())return false;out={{"Fixture BLE","AA:BB:CC:DD:EE:FF",-40}};return ble_ok;}
};
inline Hal& GetHAL(){static Hal h;return h;}
namespace audio{enum class RecorderMode{Idle,Recording};}
namespace xiaozhi{
class AudioSession{public:
 struct Stat{bool capturing=false,playback_open=false,decoder_open=false;}stats;
 struct Recorder{audio::RecorderMode mode=audio::RecorderMode::Idle;}recorder;
 static AudioSession& GetInstance(){static AudioSession a;return a;}
 Stat Stats()const{return stats;}Recorder RecorderState()const{return recorder;}
};
}
namespace reminders{class Service{public:bool active=false;static Service& Instance(){static Service s;return s;}bool IsActive()const{return active;}};}

inline int pdMS_TO_TICKS(int n){return n;}
inline void vTaskDelay(int){}
