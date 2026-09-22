#include "weather_provider.h"
#include "input/text_input.h"
#include <cJSON.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
namespace dashboard {
namespace {
using Json=std::unique_ptr<cJSON,decltype(&cJSON_Delete)>;
bool Text(const std::string& s,size_t limit,bool empty=false) {
    return (empty || !s.empty()) && s.size()<=limit && input::ValidUtf8(s) &&
        std::none_of(s.begin(),s.end(),[](unsigned char c){return c<32 || c==127;});
}
Json Parse(const std::string& json) {
    if(json.size()>16384 || json.find('\0')!=std::string::npos)return Json(nullptr,cJSON_Delete);
    // cJSON stores strings as C strings: never accept an encoded NUL prefix.
    for(size_t i=0;i<json.size();++i)if(json[i]=='\\' && i+1<json.size()) {
        if(json.compare(i+1,5,"u0000")==0)return Json(nullptr,cJSON_Delete);
        ++i;
    }
    return Json(cJSON_ParseWithLengthOpts(json.c_str(),json.size()+1,nullptr,true),cJSON_Delete);
}
const cJSON* Item(const cJSON* p,const char* key) {return cJSON_IsObject(p)?cJSON_GetObjectItemCaseSensitive(p,key):nullptr;}
bool Number(const cJSON* p,const char* key,double min,double max,double& out) {
    const auto* v=Item(p,key);if(!cJSON_IsNumber(v)||!std::isfinite(v->valuedouble)||v->valuedouble<min||v->valuedouble>max)return false;
    out=v->valuedouble;return true;
}
std::string String(const cJSON* p,const char* key) {const auto* v=Item(p,key);return cJSON_IsString(v)?v->valuestring:"";}
const char* Condition(int code) {
    switch(code){case 0:return "晴";case 1:return "晴间多云";case 2:return "多云";case 3:return "阴";
    case 45:case 48:return "雾";case 51:case 53:case 55:return "毛毛雨";case 56:case 57:return "冻毛毛雨";
    case 61:return "小雨";case 63:return "中雨";case 65:return "大雨";case 66:case 67:return "冻雨";
    case 71:return "小雪";case 73:return "中雪";case 75:return "大雪";case 77:return "雪粒";
    case 80:case 81:case 82:return "阵雨";case 85:case 86:return "阵雪";case 95:return "雷雨";
    case 96:case 99:return "雷雨冰雹";default:return nullptr;}
}
}
const char* WeatherCondition(int code){return Condition(code);}
bool ValidWeatherPlace(const WeatherPlace& p){return p.id && Text(p.name,90)&&Text(p.region,180,true)&&
    std::isfinite(p.latitude)&&p.latitude>=-90&&p.latitude<=90&&std::isfinite(p.longitude)&&p.longitude>=-180&&p.longitude<=180;}
std::string WeatherSearchUrl(const std::string& query) {
    if(!Text(query,90))return {};
    std::string encoded;constexpr char hex[]="0123456789ABCDEF";
    for(unsigned char c:query) {
        if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.')encoded+=char(c);
        else {encoded+='%';encoded+=hex[c>>4];encoded+=hex[c&15];}
    }
    return "https://geocoding-api.open-meteo.com/v1/search?name="+encoded+"&count=5&language=zh&format=json";
}
std::string WeatherForecastUrl(const WeatherPlace& p) {
    if(!ValidWeatherPlace(p))return {};
    char url[512];std::snprintf(url,sizeof(url),"https://api.open-meteo.com/v1/forecast?latitude=%.5f&longitude=%.5f&current=temperature_2m,relative_humidity_2m,apparent_temperature,wind_speed_10m,weather_code&temperature_unit=celsius&wind_speed_unit=kmh&timeformat=unixtime&forecast_days=1",p.latitude,p.longitude);
    return url;
}
bool ParseWeatherPlaces(const std::string& json,std::vector<WeatherPlace>& places) {
    const auto root=Parse(json);if(!cJSON_IsObject(root.get())||cJSON_IsTrue(Item(root.get(),"error")))return false;
    const auto* rows=Item(root.get(),"results");if(rows && !cJSON_IsArray(rows))return false;
    std::vector<WeatherPlace> loaded;std::set<uint32_t> ids;
    if(cJSON_GetArraySize(rows)>100)return false;
    for(int i=0;rows && i<cJSON_GetArraySize(rows) && loaded.size()<5;++i) {
        const auto* row=cJSON_GetArrayItem(rows,i);WeatherPlace p;double id;
        if(!Number(row,"id",1,UINT32_MAX,id)||std::trunc(id)!=id||!Number(row,"latitude",-90,90,p.latitude)||!Number(row,"longitude",-180,180,p.longitude))continue;
        p.id=static_cast<uint32_t>(id);p.name=String(row,"name");
        p.region=String(row,"admin1");const auto country=String(row,"country");
        if(!country.empty()){if(!p.region.empty())p.region+=" · ";p.region+=country;}
        if(ValidWeatherPlace(p) && ids.insert(p.id).second)loaded.push_back(std::move(p));
    }
    places=std::move(loaded);return true;
}
bool ParseWeatherForecast(const std::string& json,const std::string& location,Weather& out) {
    if(!Text(location,90))return false;
    auto root=Parse(json);if(!root || cJSON_IsTrue(Item(root.get(),"error")))return false;
    const auto* current=Item(root.get(),"current");const auto* units=Item(root.get(),"current_units");
    if(String(units,"temperature_2m")!="°C"||String(units,"apparent_temperature")!="°C"||String(units,"relative_humidity_2m")!="%"||String(units,"wind_speed_10m")!="km/h"||String(units,"time")!="unixtime")return false;
    double temperature,feels,humidity,wind,code,timestamp;
    if(!Number(current,"temperature_2m",-99,99,temperature)||!Number(current,"apparent_temperature",-99,99,feels)||
       !Number(current,"relative_humidity_2m",0,100,humidity)||!Number(current,"wind_speed_10m",0,999,wind)||
       !Number(current,"weather_code",0,99,code)||std::trunc(code)!=code||!Condition(static_cast<int>(code))||
       !Number(current,"time",kEarliestTrustedEpoch,INT32_MAX,timestamp)||std::trunc(timestamp)!=timestamp)return false;
    Weather w;w.valid=true;w.request_state=RequestState::Succeeded;CopyText(w.location,sizeof(w.location),location.c_str());
    CopyText(w.condition,sizeof(w.condition),Condition(static_cast<int>(code)));
    w.temperature_c=static_cast<int16_t>(std::lround(temperature));w.feels_like_c=static_cast<int16_t>(std::lround(feels));
    w.humidity=static_cast<uint8_t>(std::lround(humidity));w.wind_kmh=static_cast<int16_t>(std::lround(wind));
    w.updated_epoch=static_cast<uint32_t>(timestamp);out=w;return true;
}
std::string EncodeWeatherPlace(const WeatherPlace& p) {
    if(!ValidWeatherPlace(p))return {};
    Json root(cJSON_CreateObject(),cJSON_Delete);if(!root)return {};
    if(!cJSON_AddNumberToObject(root.get(),"schema",1)||!cJSON_AddNumberToObject(root.get(),"id",p.id)||
       !cJSON_AddStringToObject(root.get(),"name",p.name.c_str())||!cJSON_AddStringToObject(root.get(),"region",p.region.c_str())||
       !cJSON_AddNumberToObject(root.get(),"lat",p.latitude)||!cJSON_AddNumberToObject(root.get(),"lon",p.longitude))return {};
    char* data=cJSON_PrintUnformatted(root.get());if(!data)return {};std::string json(data);cJSON_free(data);return json;
}
bool DecodeWeatherPlace(const std::string& json,WeatherPlace& place) {
    auto root=Parse(json);WeatherPlace p;double schema,id;
    if(!root||!Number(root.get(),"schema",1,1,schema)||!Number(root.get(),"id",1,UINT32_MAX,id)||std::trunc(id)!=id||
       !Number(root.get(),"lat",-90,90,p.latitude)||!Number(root.get(),"lon",-180,180,p.longitude))return false;
    p.id=static_cast<uint32_t>(id);p.name=String(root.get(),"name");p.region=String(root.get(),"region");
    if(!ValidWeatherPlace(p))return false;
    place=std::move(p);return true;
}

std::string EncodeWeatherCache(const std::string& source,const Weather& w) {
    if(source.rfind("https://",0)!=0 || source.size()>512 || !w.valid ||
       w.updated_epoch<kEarliestTrustedEpoch || w.updated_epoch>INT32_MAX ||
       !Text(w.location,sizeof(w.location)-1) || !Text(w.condition,sizeof(w.condition)-1) ||
       w.temperature_c<-99 || w.temperature_c>99 || w.feels_like_c<-99 || w.feels_like_c>99 ||
       w.humidity>100 || w.wind_kmh<0 || w.wind_kmh>999)return {};
    Json root(cJSON_CreateObject(),cJSON_Delete);
    if(!root || !cJSON_AddNumberToObject(root.get(),"schema",1) ||
       !cJSON_AddStringToObject(root.get(),"source",source.c_str()) ||
       !cJSON_AddStringToObject(root.get(),"location",w.location) ||
       !cJSON_AddStringToObject(root.get(),"condition",w.condition) ||
       !cJSON_AddNumberToObject(root.get(),"temperature",w.temperature_c) ||
       !cJSON_AddNumberToObject(root.get(),"feels",w.feels_like_c) ||
       !cJSON_AddNumberToObject(root.get(),"humidity",w.humidity) ||
       !cJSON_AddNumberToObject(root.get(),"wind",w.wind_kmh) ||
       !cJSON_AddNumberToObject(root.get(),"updated",w.updated_epoch))return {};
    char* raw=cJSON_PrintUnformatted(root.get());if(!raw)return {};
    std::string json(raw);cJSON_free(raw);return json;
}
bool DecodeWeatherCache(const std::string& json,const std::string& source,Weather& w) {
    auto root=Parse(json);double schema,temperature,feels,humidity,wind,updated;
    if(!root || !Number(root.get(),"schema",1,1,schema) || String(root.get(),"source")!=source ||
       !Number(root.get(),"temperature",-99,99,temperature) || !Number(root.get(),"feels",-99,99,feels) ||
       !Number(root.get(),"humidity",0,100,humidity) || !Number(root.get(),"wind",0,999,wind) ||
       !Number(root.get(),"updated",kEarliestTrustedEpoch,INT32_MAX,updated))return false;
    for(double n:{temperature,feels,humidity,wind,updated})if(std::trunc(n)!=n)return false;
    const auto location=String(root.get(),"location"),condition=String(root.get(),"condition");
    if(!Text(location,sizeof(w.location)-1) || !Text(condition,sizeof(w.condition)-1))return false;
    Weather loaded;loaded.valid=loaded.from_cache=true;loaded.temperature_c=static_cast<int16_t>(temperature);
    loaded.feels_like_c=static_cast<int16_t>(feels);loaded.humidity=static_cast<uint8_t>(humidity);
    loaded.wind_kmh=static_cast<int16_t>(wind);loaded.updated_epoch=static_cast<uint32_t>(updated);
    CopyText(loaded.location,sizeof(loaded.location),location.c_str());
    CopyText(loaded.condition,sizeof(loaded.condition),condition.c_str());w=loaded;return true;
}
WeatherSetup& WeatherSetup::Instance(){static WeatherSetup instance;return instance;}
WeatherSetupSnapshot WeatherSetup::Snapshot()const{std::lock_guard<std::mutex> l(mutex_);return state_;}
uint32_t WeatherSetup::Revision()const{std::lock_guard<std::mutex> l(mutex_);return state_.revision;}
void WeatherSetup::Restore(const WeatherPlace& p){std::lock_guard<std::mutex> l(mutex_);if(ValidWeatherPlace(p)){state_.selected=p;++state_.revision;}}
bool WeatherSetup::Search(const std::string& q) {
    if(WeatherSearchUrl(q).empty())return false;
    std::lock_guard<std::mutex> l(mutex_);if(state_.state==WeatherSetupState::Saving||state_.state==WeatherSetupState::Searching)return false;
    ++state_.generation;++state_.revision;state_.state=WeatherSetupState::Searching;state_.message.clear();state_.results.clear();
    job_={state_.generation,true,q,{}};pending_=true;return true;
}
bool WeatherSetup::Select(size_t index) {
    std::lock_guard<std::mutex> l(mutex_);if(state_.state!=WeatherSetupState::Results||index>=state_.results.size())return false;
    ++state_.generation;++state_.revision;state_.state=WeatherSetupState::Saving;state_.message.clear();
    job_={state_.generation,false,{},state_.results[index]};pending_=true;return true;
}
bool WeatherSetup::Cancel(){std::lock_guard<std::mutex> l(mutex_);if(state_.state==WeatherSetupState::Saving)return false;
    pending_=false;job_={};++state_.generation;++state_.revision;state_.state=WeatherSetupState::Idle;state_.message.clear();state_.results.clear();return true;}
bool WeatherSetup::Take(WeatherJob& job){std::lock_guard<std::mutex> l(mutex_);if(!pending_)return false;job=job_;pending_=false;return true;}
bool WeatherSetup::Current(uint32_t g)const{std::lock_guard<std::mutex> l(mutex_);return g==state_.generation;}
void WeatherSetup::Searched(uint32_t g,std::vector<WeatherPlace> places,const char* error) {
    std::lock_guard<std::mutex> l(mutex_);if(g!=state_.generation||state_.state!=WeatherSetupState::Searching)return;
    if(places.size()>5)places.resize(5);
    state_.results=std::move(places);state_.state=error?WeatherSetupState::Error:WeatherSetupState::Results;
    state_.message=error?error:(state_.results.empty()?"未找到城市，试试拼音或英文":"");++state_.revision;
}
void WeatherSetup::Saved(uint32_t g,bool ok){std::lock_guard<std::mutex> l(mutex_);if(g!=state_.generation||state_.state!=WeatherSetupState::Saving)return;
    if(ok){state_.selected=job_.place;state_.results.clear();}state_.state=ok?WeatherSetupState::Idle:WeatherSetupState::Error;
    state_.message=ok?"":"保存失败，请重试";job_={};++state_.revision;}
}
