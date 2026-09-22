#include "raw_display.h"
#include "dashboard/weather_provider.h"
#include "dashboard/dashboard_service.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

bool RawDisplay::HandleWeatherTap(int x, int y) {
    bool refresh=false;
    {
        DisplayLockGuard lock(this);
        if(product_page_!=ProductPage::Weather || reminder_alert_.active || quick_controls_open_.load())return false;
        const auto hit=[&](int xx,int yy,int w,int h){return x>=xx && x<xx+w && y>=yy && y<yy+h;};
        auto& model=dashboard::WeatherSetup::Instance();
        const auto status=model.Snapshot();
        if(weather_search_ && status.state==dashboard::WeatherSetupState::Idle)weather_search_=false;
        if(hit(32,688,416,48)) {
            if(!model.Cancel())return true;
            weather_search_=false;product_page_=ProductPage::More;navigation_index_=2;
        } else if(weather_search_) {
            if(status.state==dashboard::WeatherSetupState::Results) {
                for(size_t i=0;i<status.results.size();++i)if(hit(32,184+static_cast<int>(i)*84,416,76)) {
                    model.Select(i);break;
                }
            }
            if(hit(32,624,416,48) && status.state!=dashboard::WeatherSetupState::Saving) {
                if(model.Cancel()) {weather_search_=false;OpenEditorLocked(EditTarget::WeatherCity);}
            }
        } else if(hit(32,624,200,48)) {
            OpenEditorLocked(EditTarget::WeatherCity);
        } else if(hit(248,624,200,48)) {
            refresh=true;
        }
        DrawHomeScreenLocked();FlushLocked();
    }
    if(refresh)dashboard::DashboardService::GetInstance().RefreshNow();
    return true;
}

bool RawDisplay::HandleWeatherKey(HardwareKey key) {
    int target_x=-1,target_y=0;
    {
        DisplayLockGuard lock(this);
        if(product_page_!=ProductPage::Weather)return false;
        const auto status=dashboard::WeatherSetup::Instance().Snapshot();
        if(key==HardwareKey::Home || key==HardwareKey::Back || key==HardwareKey::Previous) {
            if(!dashboard::WeatherSetup::Instance().Cancel())return true;
            weather_search_=false;product_page_=key==HardwareKey::Home?ProductPage::Home:ProductPage::More;
            navigation_index_=0;
        } else if(key==HardwareKey::Next) {
            const size_t count=weather_search_ ? status.results.size() : 2;
            if(count)navigation_index_=(navigation_index_+1)%static_cast<int>(count);
        } else if(key==HardwareKey::Select) {
            if(weather_search_ && status.state==dashboard::WeatherSetupState::Results) {
                target_x=40;target_y=192+84*navigation_index_;
            } else if(!weather_search_) {
                target_x=navigation_index_%2 ? 260 : 40;target_y=640;
            }
        }
        DrawHomeScreenLocked();FlushLocked();
    }
    if(target_x>=0)HandleWeatherTap(target_x,target_y);
    return true;
}

void RawDisplay::DrawProductWeatherLocked() {
    std::memset(portrait_fb_,0xff,portrait_size_);
    DrawProductStatusBarLocked();DrawProductHeadingLocked("天气","");
    const auto status=dashboard::WeatherSetup::Instance().Snapshot();
    const auto snapshot=dashboard::DashboardData::GetInstance().GetSnapshot();
    const auto& weather=snapshot.weather;
    if(weather_search_ && status.state==dashboard::WeatherSetupState::Idle)weather_search_=false;
    const auto action=[&](int x,int y,int w,const char* label){
        DrawTextCentered(x,y,w,48,label,ui_font_small);
    };
    if(weather_search_) {
        const char* title=status.state==dashboard::WeatherSetupState::Searching ? "正在搜索…" :
            status.state==dashboard::WeatherSetupState::Saving ? "正在保存…" :
            status.state==dashboard::WeatherSetupState::Error ? status.message.c_str() :
            status.results.empty() ? "未找到城市" : "选择城市";
        DrawProductLabelLocked(32,140,416,title,ui_font_small);
        for(size_t i=0;i<status.results.size();++i) {
            const auto& place=status.results[i];const int y=184+static_cast<int>(i)*84;
            DrawProductLabelLocked(40,y,392,place.name.c_str(),ui_font_body);
            DrawProductLabelLocked(40,y+40,392,place.region.c_str(),ui_font_small);
            FillRect(40,y+76,392,1,true);
            if(navigation_focus_ && navigation_index_==static_cast<int>(i))FillRect(32,y+32,6,2,true);
        }
        if(status.state!=dashboard::WeatherSetupState::Saving)action(32,624,416,"重新搜索");
    } else {
        const std::string city=dashboard::ValidWeatherPlace(status.selected)?status.selected.name:weather.location;
        DrawProductLabelLocked(32,152,416,city.empty()?"选择所在城市":city.c_str(),ui_font_title);
        if(weather.valid) {
            char temperature[24];std::snprintf(temperature,sizeof(temperature),"%d°C",weather.temperature_c);
            DrawProductLabelLocked(32,220,416,temperature,ui_font_h1);
            DrawProductLabelLocked(32,290,416,weather.condition,ui_font_body);
            char value[48];
            std::snprintf(value,sizeof(value),"体感  %d°C",weather.feels_like_c);DrawText(32,374,value,ui_font_status);
            std::snprintf(value,sizeof(value),"湿度  %u%%",weather.humidity);DrawText(32,428,value,ui_font_status);
            std::snprintf(value,sizeof(value),"风速  %d km/h",weather.wind_kmh);DrawText(32,482,value,ui_font_status);
            const time_t timestamp=weather.updated_epoch;struct tm tm{};localtime_r(&timestamp,&tm);
            std::snprintf(value,sizeof(value),"%02d-%02d %02d:%02d%s",tm.tm_mon+1,tm.tm_mday,tm.tm_hour,tm.tm_min,
                weather.freshness==dashboard::Freshness::Stale?" · 已过期":weather.from_cache?" · 缓存":"");
            DrawProductLabelLocked(32,548,416,value,ui_font_small);
        } else {
            DrawProductLabelLocked(32,252,416,city.empty()?"暂无天气":"等待更新",ui_font_body);
        }
        const char* state=weather.request_state==dashboard::RequestState::Refreshing?"更新中…":
            weather.request_state==dashboard::RequestState::Failed?"更新失败，请重试":
            weather.request_state==dashboard::RequestState::Offline?"离线":"";
        DrawProductLabelLocked(32,588,416,state,ui_font_small);
        action(32,624,200,"选择城市");action(248,624,200,"刷新");
        if(navigation_focus_)FillRect(navigation_index_%2?316:108,672,24,2,true);
    }
    action(32,688,416,"返回");
    // Attribution remains visible with cached data, not a tutorial paragraph.
    DrawProductControlRailLocked(dashboard::ValidWeatherPlace(status.selected) || weather_search_ ? "Open-Meteo" : "");
}
