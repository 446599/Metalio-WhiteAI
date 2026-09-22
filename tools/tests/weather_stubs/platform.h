#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
namespace weather_test {
inline std::map<std::string,std::string> blobs,staged,settings;
inline bool set_fail=false,commit_fail=false,wifi=true;
inline std::string response,url;
inline int status=200,closes=0,opens=0;
inline size_t declared_size=0;
inline int64_t now_ms=100000;
}
using nvs_handle_t=int;
constexpr int ESP_OK=0,NVS_READONLY=0,NVS_READWRITE=1,pdPASS=1;
inline int nvs_open(const char*,int,nvs_handle_t* out){*out=1;return 0;}
inline int nvs_get_blob(nvs_handle_t,const char* key,void* out,size_t* size){
 auto it=weather_test::blobs.find(key);if(it==weather_test::blobs.end())return -1;
 if(!out){*size=it->second.size();return 0;}
 if(*size<it->second.size())return -1;
 std::memcpy(out,it->second.data(),it->second.size());*size=it->second.size();return 0;
}
inline int nvs_set_blob(nvs_handle_t,const char* key,const void* p,size_t n){
 if(weather_test::set_fail)return -1;
 weather_test::staged[key]=std::string(static_cast<const char*>(p),n);return 0;
}
inline int nvs_commit(nvs_handle_t){if(weather_test::commit_fail)return -1;for(const auto& e:weather_test::staged)weather_test::blobs[e.first]=e.second;return 0;}
inline void nvs_close(nvs_handle_t){weather_test::staged.clear();}
class Settings {
public:
 Settings(const char*,bool){}
 std::string GetString(const char* k,const std::string& d=""){auto it=weather_test::settings.find(k);return it==weather_test::settings.end()?d:it->second;}
 int32_t GetInt(const char* k,int32_t d=0){auto s=GetString(k);return s.empty()?d:std::stoi(s);}
 bool GetBool(const char* k,bool d=false){return GetInt(k,d)!=0;}
 void SetString(const char* k,const std::string& v){weather_test::settings[k]=v;}
 void SetInt(const char* k,int32_t n){SetString(k,std::to_string(n));}
 void SetBool(const char* k,bool b){SetInt(k,b);}
};
class Http {
 size_t offset_=0;
public:
 void SetTimeout(int){} void SetHeader(const char*,const std::string&){} void SetKeepAlive(bool){}
 bool Open(const char*,const std::string& url){weather_test::url=url;++weather_test::opens;return true;}
 int GetStatusCode(){return weather_test::status;}
 size_t GetBodyLength(){return weather_test::declared_size;}
 int Read(char* out,size_t cap){const auto n=std::min(cap,weather_test::response.size()-offset_);std::memcpy(out,weather_test::response.data()+offset_,n);offset_+=n;return static_cast<int>(n);}
 void Close(){++weather_test::closes;}
};
class Network {public:std::unique_ptr<Http> CreateHttp(){return std::make_unique<Http>();}};
class Board {public:static Board& GetInstance(){static Board b;return b;} Network* GetNetwork(){static Network n;return &n;}};
class TestHal {public:bool IsWifiMode(){return true;}bool WifiIsConnected(){return weather_test::wifi;}bool WifiConnectSaved(uint32_t){return weather_test::wifi;}bool CellStart(){return false;}std::string CellRegistrationJson(){return {};}};
inline TestHal& GetHAL(){static TestHal h;return h;}
using TaskHandle_t=void*;
inline int xTaskCreatePinnedToCore(void(*)(void*),const char*,int,void*,int,TaskHandle_t*,int){return pdPASS;}
inline void vTaskDelete(void*){}inline void vTaskDelay(int){}
inline int64_t esp_timer_get_time(){return weather_test::now_ms*1000;}
#define pdMS_TO_TICKS(n) (n)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
