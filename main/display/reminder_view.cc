#include "raw_display.h"
#include "reminder_layout.h"
#include "reminders/reminder_service.h"
#include "reminders/reminder_store.h"
#include <esp_timer.h>
#include <cstdio>
#include <cstring>

void RawDisplay::ShowReminderAlert(const reminders::AlertSnapshot& alert) {
    DisplayLockGuard lock(this);
    if (alert.active && (!reminder_alert_.active || reminder_alert_.token != alert.token)) {
        reminder_visible_since_ms_ = esp_timer_get_time()/1000;
    }
    reminder_alert_ = alert;
    if (alert.active) {
        power_save_ = false;
        screen_test_mode_ = false;
        test_console_mode_ = false;
    }
    if (!portrait_fb_) return;
    DrawHomeScreenLocked();
    FlushLocked();
}

bool RawDisplay::HandleReminderTap(int x, int y) {
    if (!reminders::Service::Instance().IsActive()) return false;
    uint32_t token;
    {
        DisplayLockGuard lock(this);
        // A tap that belonged to the previous screen cannot dismiss a newly
        // arriving alarm before the panel has presented its controls.
        if (!reminder_alert_.active || esp_timer_get_time()/1000 - reminder_visible_since_ms_ < 700) return true;
        token = reminder_alert_.token;
    }
    switch (reminder_ui::Hit(x,y)) {
        case reminder_ui::Action::Stop: (void)reminders::Service::Instance().Dismiss(token); break;
        case reminder_ui::Action::Snooze: (void)reminders::Service::Instance().Snooze(token); break;
        case reminder_ui::Action::None: break;
    }
    return true;
}

void RawDisplay::DrawReminderAlertLocked() {
    using namespace reminder_ui;
    std::memset(portrait_fb_,0xff,portrait_size_);
    DrawText(32,24,"MIAO / INK",ui_font_small);
    DrawText(352,24,"ALARM",ui_font_small);
    FillRect(32,64,416,1,true);
    DrawTextCentered(32,88,416,48,reminder_alert_.audible ? "闹钟响了" : "待处理提醒",ui_font_title);
    const auto local = reminders::LocalTime(reminder_alert_.at);
    DrawProductClockLocked(40,164,local.substr(11,5).c_str());
    DrawTextCentered(32,288,416,32,local.substr(0,10).c_str(),ui_font_small);
    char line1[100],line2[100];
    FitTextLines(reminder_alert_.title.c_str(),ui_font_body,384,line1,sizeof(line1),line2,sizeof(line2));
    DrawTextCentered(48,344,384,40,line1,ui_font_body);
    DrawTextCentered(48,384,384,40,line2,ui_font_body);
    char count[64];
    if (reminder_alert_.count > 1) std::snprintf(count,sizeof(count),"共 %u 项提醒 / 一起处理",static_cast<unsigned>(reminder_alert_.count));
    else std::snprintf(count,sizeof(count),"提醒方式 / 按控制栏设置");
    DrawTextCentered(32,448,416,32,count,ui_font_small);
    char message[96];
    FitText(reminder_alert_.message.empty() ? "请触摸下方按钮处理提醒" : reminder_alert_.message.c_str(),
            ui_font_small,416,message,sizeof(message));
    DrawTextCentered(32,496,416,32,message,ui_font_small);

    FillRoundRect(kLeft,kStopY,kWidth,kHeight,20,true);
    const char* stop = "停止闹钟";
    DrawTextInk((480-TextWidth(stop,ui_font_body))/2,kStopY+(kHeight-ui_font_body.height)/2,
                stop,ui_font_body,false);
    StrokeRoundRect(kLeft,kSnoozeY,kWidth,kHeight,20,2);
    DrawTextCentered(kLeft,kSnoozeY,kWidth,kHeight,"稍后 5 分钟",ui_font_body);
    DrawTextCentered(32,756,416,28,"普通按键不会关闭闹钟",ui_font_small);
}
