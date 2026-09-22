#include "power/activity.h"
#include "wifi_setup.h"
#include "input/text_input.h"
#include "application.h"
#include "hal/hal.h"
#include "dashboard/dashboard_service.h"
#include <wifi_station.h>
#include <ssid_manager.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <memory>
#include <new>
namespace network {
struct WifiSetup::Job {
    WifiSetup* owner;
    uint32_t operation;
    bool scan;
    AccessPoint ap;
    std::string password;
    ~Job() {input::Wipe(password);}
};
WifiSetup& WifiSetup::Instance() {static WifiSetup service;return service;}
bool WifiSetup::Launch(Job* job) {
    if (!job) return false;
    if (xTaskCreate(Worker,"wifi_setup",8192,job,2,nullptr)!=pdPASS) {
        if(job->scan) model_.Scanned(job->operation,false,{});
        else model_.Finish(job->operation,false,false);
        delete job;return false;
    }
    return true;
}
bool WifiSetup::Scan() {
    if(!GetHAL().IsWifiMode()) return false;
    std::unique_ptr<Job> job(new(std::nothrow) Job{this,0,true,{},{}});
    if(!job) return false;
    job->operation=model_.Begin(true);if(!job->operation) return false;
    return Launch(job.release());
}
bool WifiSetup::Connect(const AccessPoint& ap,const std::string& password) {
    if(!GetHAL().IsWifiMode() || !ValidCredentials(ap.ssid,password,ap.security)) return false;
    std::unique_ptr<Job> job(new(std::nothrow) Job{this,0,false,ap,password});
    if(!job) return false;
    job->operation=model_.Begin(false,ap.ssid);if(!job->operation) return false;
    return Launch(job.release());
}
void WifiSetup::Worker(void* arg) {
    {
        power::Activity activity;
        while(!activity){vTaskDelay(pdMS_TO_TICKS(20));activity.Retry();}
        std::unique_ptr<Job> job(static_cast<Job*>(arg));
        auto& self=*job->owner;auto& station=WifiStation::GetInstance();
        if(job->scan) {
            std::vector<WifiScanAp> scanned;
            const bool ok=station.ScanForList(scanned,8000);
            std::vector<AccessPoint> aps;
            for(const auto& ap:scanned) {
                Security security=Security::Unsupported;
                switch(ap.authmode) {
                    case WIFI_AUTH_OPEN: security=Security::Open;break;
                    case WIFI_AUTH_WPA_PSK: case WIFI_AUTH_WPA2_PSK: case WIFI_AUTH_WPA_WPA2_PSK:
                    case WIFI_AUTH_WPA3_PSK: case WIFI_AUTH_WPA2_WPA3_PSK: security=Security::Personal;break;
                    default: break;
                }
                aps.push_back({ap.ssid,ap.rssi,security});
            }
            self.model_.Scanned(job->operation,ok,std::move(aps));
            if(!station.IsConnected()) station.StartAutoConnectScan();
        } else {
            const auto cancelled=[&](){return self.model_.Cancelled(job->operation);};
            bool connected=station.ConnectForSetup(job->ap.ssid,job->password,
                job->ap.security==Security::Open,20000,cancelled);
            bool saved=false;
            if(connected && self.model_.BeginSave(job->operation)) {
                // Point of no cancellation. UI says "saving" until commit returns.
                saved=SsidManager::GetInstance().AddSsid(job->ap.ssid,job->password);
            } else if(cancelled()) connected=false;
            const auto ip=connected ? station.GetIpAddress() : "";
            station.FinishSetup(connected);
            self.model_.Finish(job->operation,connected,saved,ip);
            if(connected) dashboard::DashboardService::GetInstance().RefreshNow();
        }
        Application::GetInstance().RequestStatusUpdate(true);
    }
    vTaskDelete(nullptr);
}
}
