#include "raw_display.h"
#include "reader/reader_service.h"
#include "input/keyboard_layout.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

bool RawDisplay::HandleReaderTap(int x,int y){
    int action=0;std::string filename;
    {
        DisplayLockGuard lock(this);
        if(product_page_!=ProductPage::Reader || quick_controls_open_.load() || reminder_alert_.active || screen_test_mode_ || test_console_mode_ || power_save_)return false;
        const auto book=reader::Service::Instance().Get();
        const auto hit=[&](int l,int t,int w,int h){return input::Inside(x,y,l,t,w,h);};
        if(hit(344,72,104,48))action=1;
        else if(!book.busy && book.opened){
            if(hit(32,672,200,48))action=-2;
            else if(hit(248,672,200,48))action=2;
        } else if(!book.busy) {
            if(hit(32,672,200,48))action=1;
            else if(hit(248,672,200,48)){++book_list_page_;DrawHomeScreenLocked();FlushLocked();}
            else for(int row=0;row<7;++row){
                const auto index=static_cast<size_t>(book_list_page_*7+row);
                if(index<book.files.size() && hit(32,176+row*64,416,56)){filename=book.files[index];action=3;break;}
            }
        }
    }
    if(action==1)reader::Service::Instance().List();
    else if(action==3)reader::Service::Instance().Open(filename);
    else if(action==2 || action==-2)reader::Service::Instance().Turn(action);
    if(action) UpdateStatusBar(true);
    return true;
}
bool RawDisplay::HandleReaderKey(HardwareKey key){
    bool opened=false;
    {
        DisplayLockGuard lock(this);if(product_page_!=ProductPage::Reader || quick_controls_open_.load() || power_save_)return false;
        opened=reader::Service::Instance().Get().opened;
    }
    if(key==HardwareKey::Home || key==HardwareKey::Back)return false;
    if(key==HardwareKey::Previous && !opened) {
        DisplayLockGuard lock(this);book_list_page_=std::max(0,book_list_page_-1);DrawHomeScreenLocked();FlushLocked();return true;
    }
    if(key==HardwareKey::Select)return HandleReaderTap(360,90);
    if(key==HardwareKey::Previous)return HandleReaderTap(40,690);
    if(key==HardwareKey::Next)return HandleReaderTap(260,690);
    return true;
}
void RawDisplay::DrawProductReaderLocked(){
    std::memset(portrait_fb_,0xff,portrait_size_);DrawProductStatusBarLocked();
    const auto book=reader::Service::Instance().Get();
    DrawProductHeadingLocked("阅读","");
    StrokeRoundRect(344,72,104,48,12,1);DrawTextCentered(344,72,104,48,"书库",ui_font_small);
    if(book.opened){
        DrawProductLabelLocked(32,128,416,book.title.c_str(),ui_font_small);
        for(size_t i=0;i<book.lines.size();++i)DrawText(32,172+static_cast<int>(i)*38,book.lines[i].c_str(),ui_font_body);
    } else {
        DrawProductLabelLocked(32,136,416,"SD 卡 / books / UTF-8 TXT",ui_font_small);
        const int pages=std::max(1,(static_cast<int>(book.files.size())+6)/7);book_list_page_=std::max(0,book_list_page_)%pages;
        for(int row=0;row<7;++row){
            const size_t index=book_list_page_*7+row;if(index>=book.files.size())break;
            StrokeRoundRect(32,176+row*64,416,56,12,1);
            DrawProductLabelLocked(48,188+row*64,384,book.files[index].c_str(),ui_font_small);
        }
        if(book.files.empty())DrawProductLabelLocked(32,240,416,"点击书库，读取 SD 卡上的 TXT 文件",ui_font_small);
    }
    char page[64];std::snprintf(page,sizeof(page),book.opened?"%u / %u 页":"书库 %u / %u",static_cast<unsigned>(book.opened?book.page+1:book_list_page_+1),static_cast<unsigned>(book.opened?book.pages:std::max<size_t>(1,(book.files.size()+6)/7)));
    DrawTextCentered(32,632,416,32,book.busy?"正在读取…":page,ui_font_small);
    StrokeRoundRect(32,672,200,48,12,1);DrawTextCentered(32,672,200,48,book.opened?"上一页":"刷新书库",ui_font_small);
    StrokeRoundRect(248,672,200,48,12,1);DrawTextCentered(248,672,200,48,"下一页",ui_font_small);
    DrawProductControlRailLocked(book.message.c_str());
}
