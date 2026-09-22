#include "raw_display.h"
#include <algorithm>
#include <cstring>
#include <ctime>

void RawDisplay::SetLockScreen(bool on,const std::vector<uint8_t>& wallpaper) {
    DisplayLockGuard lock(this);
    if(!portrait_fb_)return;
    lock_screen_.store(on);
    password_reveal_=false;quick_controls_open_.store(false);
    notification_text_[0]=0;notification_deadline_ms_=0;
    if(!on){DrawHomeScreenLocked();FlushLocked();return;}
    // PBM P4 black=1; our framebuffer black=0. Do not retain chat/password
    // pixels behind an opaque lock page. No per-second wallpaper redraw.
    std::memset(portrait_fb_,0xff,portrait_size_);
    if(wallpaper.size()==portrait_size_){
        for(size_t i=0;i<portrait_size_;++i)portrait_fb_[i]=static_cast<uint8_t>(~wallpaper[i]);
    }else{
        DrawTextCentered(32,270,416,80,"休息一下",ui_font_h1);
        DrawTextCentered(32,364,416,48,"让想法静静留在纸上",ui_font_small);
    }
    FillRect(0,664,480,136,false);
    DrawTextCentered(32,680,416,40,"已锁屏",ui_font_body);
    DrawTextCentered(32,732,416,36,"短按电源键唤醒",ui_font_small);
    FlushLocked();
}
