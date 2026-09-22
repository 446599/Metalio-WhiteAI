#include "platform.h"
#include <atomic>
#include <mutex>
#define private public
#include "dashboard/dashboard_service.h"
#undef private
#include "dashboard/weather_provider.h"
#include <cassert>
#include <cstdio>
using namespace dashboard;
using namespace weather_test;
int main(){
    DashboardService svc;auto& data=DashboardData::GetInstance();auto& setup=WeatherSetup::Instance();
    svc.LoadConfig();assert(svc.weather_url_.empty());assert(!data.GetSnapshot().weather.valid);assert(!data.GetSnapshot().weather.location[0]);
    assert(setup.Search("上海"));response=R"({"results":[{"id":1796236,"name":"上海","latitude":31.22,"longitude":121.46,"country":"中国"}]})";
    svc.ServiceWeatherSetup();assert(opens==1&&closes==1);assert(url.find("geocoding-api.open-meteo.com")!=url.npos);assert(setup.Snapshot().results.size()==1);
    assert(setup.Select(0));commit_fail=true;svc.ServiceWeatherSetup();assert(!blobs.count("wx_place"));assert(svc.weather_url_.empty());assert(!setup.Snapshot().selected.id);
    assert(setup.Search("上海"));svc.ServiceWeatherSetup();assert(setup.Select(0));commit_fail=false;svc.ServiceWeatherSetup();
    assert(blobs.count("wx_place"));assert(svc.configured_place_);assert(!svc.weather_url_.empty());assert(setup.Snapshot().selected.id==1796236);
    response=R"({"current_units":{"time":"unixtime","temperature_2m":"°C","apparent_temperature":"°C","relative_humidity_2m":"%","wind_speed_10m":"km/h"},"current":{"time":1790035200,"temperature_2m":23.5,"apparent_temperature":25.4,"relative_humidity_2m":64,"wind_speed_10m":9.2,"weather_code":2}})";
    assert(svc.FetchWeather());assert(blobs.count("wx_cache"));assert(data.GetSnapshot().weather.temperature_c==24);
    DashboardService reboot;data.ResetDefaults();reboot.LoadConfig();assert(reboot.configured_place_&&reboot.weather_url_==svc.weather_url_);
    assert(data.GetSnapshot().weather.valid&&data.GetSnapshot().weather.from_cache);assert(data.GetSnapshot().weather.updated_epoch==1790035200);
    const auto saved=blobs["wx_cache"];auto w=data.GetSnapshot().weather;w.temperature_c=26;commit_fail=true;svc.SaveWeatherSnapshot(w);assert(blobs["wx_cache"]==saved);commit_fail=false;
    response="{bad";assert(!svc.FetchWeather());assert(data.GetSnapshot().weather.valid&&data.GetSnapshot().weather.temperature_c==24);
    status=429;assert(!svc.FetchWeather());status=200;
    std::string body;assert(!svc.FetchJson("http://not-secure.example/",body));
    response=std::string(17000,'x');declared_size=0;assert(!svc.FetchJson(svc.weather_url_,body));declared_size=17000;assert(!svc.FetchJson(svc.weather_url_,body));
    response="{}";declared_size=4;assert(!svc.FetchJson(svc.weather_url_,body));declared_size=0;
    auto next=setup.Snapshot().selected;next.id=22;next.name="新城市";next.latitude=22;blobs["wx_place"]=EncodeWeatherPlace(next);
    data.ResetDefaults();DashboardService changed;changed.LoadConfig();assert(!data.GetSnapshot().weather.valid); // Never reuse another city's cache.
    setup.Cancel();wifi=false;assert(setup.Search("Shanghai"));const int before=opens;svc.ServiceWeatherSetup();assert(opens==before&&setup.Snapshot().state==WeatherSetupState::Error);
    std::puts("Weather service PASS: real HTTP ownership/bounds, committed city, restart/cache and failure isolation with mocked network/NVS");
}
