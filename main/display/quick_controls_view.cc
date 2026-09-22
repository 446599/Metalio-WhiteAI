#include "raw_display.h"
#include "power/sleep_service.h"
#include "system/quick_controls.h"
#include "input/keyboard_layout.h"
#include "input/gesture.h"
#include "network/wifi_setup.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <esp_timer.h>

void RawDisplay::SetQuickControls(bool open) {
    if(power::Locked())return;
    if(open) device::QuickControls::Instance().Refresh();
    {
        DisplayLockGuard lock(this);
        if(!portrait_fb_ || screen_test_mode_ || test_console_mode_ || power_save_ || reminder_alert_.active || discard_pending_) return;
        quick_controls_open_.store(open);quick_bluetooth_=false;quick_ble_page_=0;
        password_reveal_=false;
        DrawHomeScreenLocked();FlushLocked();
    }
    if(!open) device::QuickControls::Instance().CancelBluetooth();
}
bool RawDisplay::HandleQuickPull(int x0,int y0,int x1,int y1,int held_ms) {
    if(power::Locked())return true;
    power::Touch(esp_timer_get_time()/1000);
    const auto pull=input::ControlPull(x0,y0,x1,y1,held_ms,quick_controls_open_.load());
    if(pull==input::Pull::None) return false;
    SetQuickControls(pull==input::Pull::Open);return true;
}
bool RawDisplay::HandleQuickKey(HardwareKey key) {
    if(!quick_controls_open_.load()) return false;
    if(key==HardwareKey::Home || key==HardwareKey::Back || key==HardwareKey::Previous) SetQuickControls(false);
    else if(key==HardwareKey::Next) {
        DisplayLockGuard lock(this);
        if(quick_bluetooth_) ++quick_ble_page_;
        DrawHomeScreenLocked();FlushLocked();
    }
    return true;
}
bool RawDisplay::HandleQuickTap(int x,int y) {
    if(power::Locked())return true;
    {DisplayLockGuard lock(this);if(screen_test_mode_ || test_console_mode_ || power_save_ || reminder_alert_.active || discard_pending_) return false;}
    if(!quick_controls_open_.load()) {
        if(input::Inside(x,y,32,0,416,64)){SetQuickControls(true);return true;}
        return false;
    }
    enum class Action {None,Close,Volume,Ring,Vibration,Network,Scan,Cancel,Sleep};
    Action action=Action::None;int volume=0;bool ring=true,vibration=true;bool busy_form=false;
    {
        DisplayLockGuard lock(this);
        if(reminder_alert_.active) return false;
        const auto hit=[&](int l,int t,int w,int h){return input::Inside(x,y,l,t,w,h);};
        const auto state=device::QuickControls::Instance().Snapshot();
        ring=state.ring;vibration=state.vibration;
        if(hit(344,72,104,48) || hit(32,688,416,48)) action=Action::Close;
        else if(quick_bluetooth_) {
            if(hit(32,608,200,56)) action=state.ble_busy ? Action::Cancel : Action::Scan;
            else if(hit(248,608,200,56)) ++quick_ble_page_;
        } else if(hit(32,192,56,56)){volume=state.volume-10;action=Action::Volume;}
        else if(hit(392,192,56,56)){volume=state.volume+10;action=Action::Volume;}
        else if(hit(96,192,288,56)){volume=((x-96)*100+144)/288;action=Action::Volume;}
        else if(hit(32,280,416,64)) {
            if(form_active_.load()) busy_form=true;
            else {quick_controls_open_.store(false);product_page_=ProductPage::WifiList;wifi_page_=0;wifi_switch_confirm_=false;navigation_index_=0;form_message_.clear();action=Action::Network;}
        } else if(device::kBleDiscoveryEnabled && hit(32,544,416,48)){quick_bluetooth_=true;quick_ble_page_=0;}
        else if(hit(32,368,200,72)){ring=!ring;action=Action::Ring;}
        else if(hit(248,368,200,72)){vibration=!vibration;action=Action::Vibration;}
        else if(hit(32,464,416,64)) action=Action::Sleep;
        if(action!=Action::Close){DrawHomeScreenLocked();FlushLocked();}
    }
    auto& controls=device::QuickControls::Instance();
    if(action==Action::Close) SetQuickControls(false);
    else if(action==Action::Volume) controls.SetVolume(volume);
    else if(action==Action::Ring || action==Action::Vibration) controls.SetAlerts(ring,vibration);
    else if(action==Action::Network){controls.CancelBluetooth();(void)network::WifiSetup::Instance().Scan();}
    else if(action==Action::Sleep){SetQuickControls(false);power::SleepService::Instance().Toggle();}
    else if(action==Action::Scan) controls.ScanBluetooth();
    else if(action==Action::Cancel) controls.CancelBluetooth();
    if(busy_form) ShowNotification("请先完成或取消当前输入，再打开网络设置",3000);
    if(action!=Action::None && action!=Action::Close) UpdateStatusBar(true);
    return true;
}
void RawDisplay::DrawProductQuickControlsLocked() {
    std::memset(portrait_fb_,0xff,portrait_size_);DrawProductStatusBarLocked();
    DrawProductHeadingLocked(quick_bluetooth_ ? "蓝牙发现" : "控制中心","");
    const auto state=device::QuickControls::Instance().Snapshot();
    const auto button=[&](int x,int y,int w,int h,const char* label){StrokeRoundRect(x,y,w,h,12,1);DrawTextCentered(x,y,w,h,label,ui_font_small);};
    button(344,72,104,48,"收起");
    if(quick_bluetooth_) {
        DrawProductLabelLocked(32,136,416,"BLE 扫描；不切换外置音频模块",ui_font_small);
        const int pages=std::max(1,(static_cast<int>(state.nearby.size())+4)/5);
        quick_ble_page_=quick_ble_page_%pages;
        for(int row=0;row<5;++row){
            const auto index=static_cast<size_t>(quick_ble_page_*5+row);if(index>=state.nearby.size()) break;
            const auto& item=state.nearby[index];const int y=192+row*72;
            DrawProductLabelLocked(32,y,416,item.name.c_str(),ui_font_small);
            char detail[64];std::snprintf(detail,sizeof(detail),"%s / %d dBm",item.address.c_str(),item.rssi);
            DrawProductLabelLocked(32,y+30,416,detail,ui_font_small);
        }
        if(state.nearby.empty()) DrawProductLabelLocked(32,240,416,state.ble_busy ? "正在寻找附近设备…" : "点击扫描以获取附近设备",ui_font_body);
        DrawProductLabelLocked(32,560,416,state.message.c_str(),ui_font_small);
        button(32,608,200,56,state.ble_busy ? "停止扫描" : "扫描蓝牙");
        char pages_text[32];std::snprintf(pages_text,sizeof(pages_text),"%d/%d 下一页",quick_ble_page_+1,pages);
        button(248,608,200,56,pages_text);
    } else {
        char text[48];std::snprintf(text,sizeof(text),"音量  %d%%",state.volume);
        DrawProductIconLocked(lucide::Id::Volume2,32,142,28,true);
        DrawProductLabelLocked(76,140,372,text,ui_font_small);
        // Single volume track, generous touch target. No grayscale, animation,
        // or duplicate percentage/technical status paragraphs.
        DrawTextCentered(32,192,56,56,"−",ui_font_body);
        DrawTextCentered(392,192,56,56,"+",ui_font_body);
        StrokeRoundRect(96,214,288,12,6,1);
        const int fill=std::clamp(state.volume,0,100)*280/100;
        if(fill)FillRect(100,218,fill,4,true);
        button(32,280,416,64,"");
        DrawProductIconLocked(lucide::Id::Wifi,48,297,28,true);
        DrawProductLabelLocked(96,292,300,state.network.c_str(),ui_font_small);
        DrawProductIconLocked(lucide::Id::ChevronRight,410,298,24,true);
        button(32,368,200,72,state.ring ? "铃声  开" : "铃声  关");
        button(248,368,200,72,state.vibration ? "震动  开" : "震动  关");
        if(state.ring)FillRect(100,428,64,3,true);
        if(state.vibration)FillRect(316,428,64,3,true);
        button(32,464,416,64,"锁屏休眠");
        if(device::kBleDiscoveryEnabled)button(32,544,416,48,"实验 BLE 发现");
        if(!state.message.empty())DrawProductLabelLocked(32,608,416,state.message.c_str(),ui_font_small);

    }
    DrawTextCentered(32,688,416,48,"上滑收起",ui_font_small);
    DrawProductControlRailLocked("");
}
