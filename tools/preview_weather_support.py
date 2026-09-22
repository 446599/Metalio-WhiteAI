"""Actual weather model and UI; HTTP is not used by framebuffer tests."""
HEADERS=r'''
#include "dashboard/weather_provider.h"
namespace dashboard {
class DashboardService {
public:
    unsigned refreshes=0;
    static DashboardService& GetInstance(){static DashboardService s;return s;}
    void RefreshNow(){assert(ui_lock_depth==0);++refreshes;}
};
}
'''
FIELDS=r'''
    std::string weather_query_;
    bool weather_search_=false,navigation_focus_=false;
    uint32_t last_weather_revision_=0;
'''
METHODS=['HandleWeatherTap','HandleWeatherKey']
EXERCISE=r'''
    display.ClearFormLocked();display.reminder_alert_.active=false;display.quick_controls_open_.store(false);
    auto& weather_setup=dashboard::WeatherSetup::Instance();weather_setup.Cancel();
    display.product_page_=RawDisplay::ProductPage::Home;display.navigation_focus_=false;save("home-borderless");
    for(int y:{296,464,664})for(int x=40;x<216;++x)assert(display.portrait_fb_[y*60+x/8] & (0x80>>(x%8)));
    display.navigation_focus_=true;display.navigation_index_=2;save("home-key-focus");
    display.navigation_focus_=false;display.product_page_=RawDisplay::ProductPage::More;save("settings-borderless");
    display.product_page_=RawDisplay::ProductPage::Weather;dashboard::Weather none;
    dashboard::DashboardData::GetInstance().SetWeather(none);save("weather-unconfigured");
    assert(display.HandleWeatherTap(40,640));assert(display.edit_target_==RawDisplay::EditTarget::WeatherCity);
    assert(display.editor_.Insert("上海"));display.FinishEditorLocked(true);save("weather-searching");
    dashboard::WeatherJob job;assert(weather_setup.Take(job) && job.query=="上海");
    std::vector<dashboard::WeatherPlace> cities={{1796236,"上海","上海市 · 中国",31.22,121.46},
        {42,"同名地点","另一个行政区",20,40}};
    weather_setup.Searched(job.generation,cities,nullptr);save("weather-city-results");
    assert(display.HandleWeatherTap(50,200));assert(weather_setup.Take(job) && !job.search);
    save("weather-saving");assert(display.HandleWeatherTap(40,700));assert(display.product_page_==RawDisplay::ProductPage::Weather);
    weather_setup.Saved(job.generation,false);save("weather-save-error");
    assert(display.HandleWeatherTap(40,640));display.FinishEditorLocked(true);assert(weather_setup.Take(job));
    weather_setup.Searched(job.generation,cities,nullptr);assert(display.HandleWeatherTap(50,200));assert(weather_setup.Take(job));
    weather_setup.Saved(job.generation,true);
    dashboard::Weather live;live.valid=true;live.updated_epoch=static_cast<uint32_t>(preview_epoch);live.temperature_c=23;
    live.feels_like_c=25;live.humidity=64;live.wind_kmh=9;live.request_state=dashboard::RequestState::Succeeded;
    dashboard::CopyText(live.location,sizeof(live.location),"上海");dashboard::CopyText(live.condition,sizeof(live.condition),"多云");
    dashboard::DashboardData::GetInstance().SetWeather(live);save("weather-live-fixture");
    assert(!display.weather_search_);assert(display.HandleWeatherTap(260,640));assert(dashboard::DashboardService::GetInstance().refreshes);
    live.from_cache=true;live.updated_epoch-=7200;live.request_state=dashboard::RequestState::Offline;
    dashboard::DashboardData::GetInstance().SetWeather(live);dashboard::DashboardData::GetInstance().UpdateFreshness(preview_epoch);save("weather-offline-cache");
    assert(display.HandleWeatherTap(40,640));display.FinishEditorLocked(true);assert(weather_setup.Take(job));
    assert(display.HandleWeatherTap(40,700));weather_setup.Searched(job.generation,cities,nullptr);
    assert(weather_setup.Snapshot().state==dashboard::WeatherSetupState::Idle);assert(weather_setup.Snapshot().results.empty());
    std::puts("Weather UI: real editor/search/selection/cancel/save failure, stale cache and HTTP outside UI lock passed");
'''
