#!/usr/bin/env python3
"""Exercise real UI flush methods with RAM, BUSY and failure injection.

Checks waveform selection, one black pulse, outline cleanup, committed history,
no-op frames and refresh cadence. This does not measure optical panel quality.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'main/display/raw_display.cc'


def method(source, name):
    match = re.search(r'^(?:bool|void) RawDisplay::' + name + r'\(', source, re.M)
    begin = source.index('{', match.start())
    depth = 0
    for token in re.finditer(r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*.*?\*/|[{}]', source[begin:], re.S):
        if token.group() == '{':
            depth += 1
        elif token.group() == '}':
            depth -= 1
            if depth == 0:
                return source[match.start():begin + token.end()]
    raise AssertionError('Unbalanced method ' + name)


source = SOURCE.read_text()
methods = '\n'.join(method(source, name) for name in (
    'RecoverPanelForBinaryLocked', 'FlushPartialLocked', 'FlushBlackPulseLocked', 'FlushLocked'))
code = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "display/binary_refresh.h"
template<class... T> void IgnoreLog(T...){ }
constexpr const char* TAG="RawDisplay";
#define ESP_LOGI(...) IgnoreLog(__VA_ARGS__)
#define ESP_LOGW(...) ((void)0)
constexpr int ESP_OK=0, kPortraitW=480, kPortraitH=800, kPanelW=800, kPanelH=480;
constexpr uint8_t kWhite=255;
constexpr int SSD1677_EPAPER_BITMAP_CURRENT=0, SSD1677_EPAPER_BITMAP_PREVIOUS=1;
constexpr int SSD1677_EPAPER_REFRESH_FULL=0, SSD1677_EPAPER_REFRESH_PARTIAL=1,
              SSD1677_EPAPER_REFRESH_DU=3;
using esp_err_t=int;
int64_t esp_timer_get_time(){return 0;}
struct Panel {
    int mode=0, full=0, partial=0, recover=0, color=0;
    int x=0,y=0,w=0,h=0,writes=0,refreshes=0,waits=0;
    int fail_write=0,fail_refresh=0,fail_wait=0;
    bool pending=false;
    std::vector<uint8_t> ram[2]={std::vector<uint8_t>(48000,255),std::vector<uint8_t>(48000,255)};
    std::vector<std::vector<uint8_t>> targets,old;
};
int epaper_panel_wait_busy(Panel* p){assert(!p->pending);return ESP_OK;}
int epaper_panel_wait_busy_timeout(Panel* p,int){return epaper_panel_wait_busy(p);}
int epaper_panel_wait_refresh_timeout(Panel* p,int){
    assert(p->pending);p->pending=false;
    if(++p->waits==p->fail_wait)return -1;
    // SSD1677 can swap RAM roles at the end of a differential activation.
    std::swap(p->ram[0],p->ram[1]);return ESP_OK;
}
int epaper_panel_recover(Panel* p){++p->recover;p->pending=false;return ESP_OK;}
void epaper_panel_set_bitmap_color(Panel* p,int color){p->color=color;}
void epaper_panel_set_refresh_mode(Panel* p,int mode){p->mode=mode;}
int esp_lcd_panel_draw_bitmap(Panel* p,int x,int y,int right,int bottom,const uint8_t* data){
    assert(!p->pending); // no write before the BUSY edge-qualified wait
    p->x=x;p->y=y;p->w=right-x;p->h=bottom-y;
    if(++p->writes==p->fail_write)return -1;
    for(int row=0;row<p->h;++row)
        std::memcpy(p->ram[p->color].data()+(y+row)*100+x/8,data+row*(p->w/8),p->w/8);
    return ESP_OK;
}
int epaper_panel_refresh_screen(Panel* p){
    assert(!p->pending);
    if(++p->refreshes==p->fail_refresh)return -1;
    p->pending=true;
    if(p->mode==SSD1677_EPAPER_REFRESH_FULL)++p->full;
    else {assert(p->mode==SSD1677_EPAPER_REFRESH_PARTIAL);++p->partial;}
    p->targets.push_back(p->ram[0]);p->old.push_back(p->ram[1]);return ESP_OK;
}
class RawDisplay {
public:
    Panel hardware;Panel* panel_=&hardware;
    std::vector<uint8_t> portrait=std::vector<uint8_t>(48000,255),
        current=std::vector<uint8_t>(48000,255),previous=std::vector<uint8_t>(48000,255),
        region=std::vector<uint8_t>(48000,255);
    uint8_t* portrait_fb_=portrait.data();uint8_t* panel_fb_=current.data();
    uint8_t* panel_prev_fb_=previous.data();uint8_t* panel_region_fb_=region.data();
    size_t panel_size_=48000,refresh_changed_bytes_=0;
    uint32_t fast_refresh_count_=0;
    bool panel_history_valid_=false,window_baseline_valid_=false,
         panel_custom_waveform_active_=false,animation_running_=false;
    void UpdateGlassBinaryLocked(int,int,int,int){}
    bool RecoverPanelForBinaryLocked();
    bool FlushPartialLocked(int,int,int,int,bool=false);
    bool FlushBlackPulseLocked();
    void FlushLocked();
};
''' + methods + r'''
bool all(const std::vector<uint8_t>& data,uint8_t value){
    return std::all_of(data.begin(),data.end(),[=](uint8_t byte){return byte==value;});
}
void committed(const RawDisplay& ui){
    assert(ui.panel_history_valid_&&ui.window_baseline_valid_);
    assert(ui.current==ui.previous&&ui.current==ui.hardware.ram[0]&&ui.current==ui.hardware.ram[1]);
}
void black_pulse(const RawDisplay& ui,size_t index){
    assert(ui.hardware.targets.size()==index+2);
    assert(all(ui.hardware.targets[index],0));
    assert(all(ui.hardware.old[index+1],0));
    assert(ui.hardware.targets[index+1]==ui.current);
    assert(ui.hardware.full==0);
    committed(ui);
}
void outline_cleanup(){
    // Independent pixel oracle: erased glyph at corners and both sides of a
    // byte boundary; adjacent retained/new ink must never be forced white.
    constexpr int w=32,h=9,stride=w/8;
    for(int px: {0,7,8,15,16,31})for(int py: {0,4,8}){
        std::vector<uint8_t> old(stride*h,255),target=old,out(old.size());
        auto black=[&](std::vector<uint8_t>& frame,int x,int y){frame[y*stride+x/8]&=~(0x80>>(x%8));};
        auto white=[&](const std::vector<uint8_t>& frame,int x,int y){return (frame[y*stride+x/8]&(0x80>>(x%8)))!=0;};
        black(old,px,py);
        int retained=(px+1)%w;black(old,retained,py);black(target,retained,py);
        black(target,(px+2)%w,py);
        auto saved_old=old,saved_target=target;
        epaper::CopyPreviousWithOutlineCleanup(out.data(),old.data(),target.data(),w,h,0,0,w,h);
        for(int y=0;y<h;++y)for(int x=0;x<w;++x){
            bool near=std::abs(x-px)<=3&&std::abs(y-py)<=3;
            bool expected_white=white(old,x,y)&&!(near&&white(target,x,y));
            assert(white(out,x,y)==expected_white);
        }
        assert(old==saved_old&&target==saved_target);
        auto d=epaper::FindBinaryDamage(old.data(),target.data(),w,h,epaper::kBinaryOutlineRadius);
        std::vector<uint8_t> packed(d.width/8*d.height+2,0xA5);
        epaper::CopyPreviousWithOutlineCleanup(packed.data()+1,old.data(),target.data(),w,h,d.x,d.y,d.width,d.height);
        assert(packed.front()==0xA5&&packed.back()==0xA5);
        for(int row=0;row<d.height;++row)for(int col=0;col<d.width/8;++col)
            assert(packed[1+row*(d.width/8)+col]==out[(d.y+row)*stride+d.x/8+col]);
    }
}
int main(){
    outline_cleanup();
    uint8_t a[6]{},b[6]{};
    assert(epaper::FindBinaryDamage(a,b,16,3,3).changed_bytes==0);
    b[5]=1;
    auto d=epaper::FindBinaryDamage(a,b,16,3);
    assert(d.x==8&&d.y==2&&d.width==8&&d.height==1&&d.changed_bytes==1);
    d=epaper::FindBinaryDamage(a,b,16,3,3);
    assert(d.x==0&&d.y==0&&d.width==16&&d.height==3&&d.changed_bytes==1);
    RawDisplay ui;ui.FlushLocked();black_pulse(ui,0);
    assert(all(ui.hardware.old[0],255)); // unknown startup image is driven black
    ui.FlushLocked();assert(ui.hardware.refreshes==2);
    for(int i=0;i<24;++i){
        ui.portrait[0]^=0x80;ui.FlushLocked();
        assert(ui.hardware.refreshes==3+i);
        assert(ui.hardware.x==0&&ui.hardware.y==476&&ui.hardware.w==16&&ui.hardware.h==4);
        committed(ui);
    }
    assert(ui.hardware.partial==26&&ui.hardware.full==0&&ui.refresh_changed_bytes_==24);
    ui.FlushLocked();assert(ui.hardware.refreshes==26);
    ui.fast_refresh_count_=64;ui.portrait[0]^=0x80;ui.FlushLocked();black_pulse(ui,26);
    assert(ui.fast_refresh_count_==0&&ui.refresh_changed_bytes_==0);
    ui.refresh_changed_bytes_=ui.panel_size_*8;ui.portrait[0]^=0x80;ui.FlushLocked();black_pulse(ui,28);
    auto old=ui.previous;
    ui.hardware.fail_wait=ui.hardware.waits+1;ui.portrait[0]^=0x80;ui.FlushLocked();
    assert(!ui.panel_history_valid_&&!ui.window_baseline_valid_&&ui.previous==old);
    auto index=ui.hardware.targets.size();ui.FlushLocked();black_pulse(ui,index);
    // Every cleanup write, activation and wait can fail without committing a
    // half-black frame; a retry must complete a fresh black pulse.
    for(int kind=0;kind<3;++kind)for(int at=1;at<=(kind==0?7:2);++at){
        RawDisplay failed;
        if(kind==0)failed.hardware.fail_write=at;
        if(kind==1)failed.hardware.fail_refresh=at;
        if(kind==2)failed.hardware.fail_wait=at;
        failed.FlushLocked();assert(!failed.panel_history_valid_&&!failed.window_baseline_valid_);
        index=failed.hardware.targets.size();failed.FlushLocked();black_pulse(failed,index);
    }
    // A failed post-partial RAM sync leaves the last committed image intact.
    RawDisplay failed;failed.FlushLocked();failed.hardware.fail_write=failed.hardware.writes+3;
    failed.portrait[0]^=0x80;old=failed.previous;failed.FlushLocked();
    assert(!failed.panel_history_valid_&&failed.previous==old&&failed.fast_refresh_count_==0);
    index=failed.hardware.targets.size();failed.FlushLocked();black_pulse(failed,index);
    RawDisplay no_region;no_region.panel_region_fb_=nullptr;no_region.FlushLocked();
    no_region.portrait[0]^=0x80;no_region.FlushLocked();black_pulse(no_region,2);
    RawDisplay custom;custom.panel_custom_waveform_active_=true;custom.FlushLocked();
    assert(custom.hardware.recover==1);black_pulse(custom,0);
    std::puts("Refresh OK: standard partial only, one black pulse, glyph halo cleanup, unchanged skip, RAM sync and injected failure recovery");
}
'''
with tempfile.TemporaryDirectory(prefix='miaoink-refresh-') as temp:
    path = Path(temp)
    (path/'test.cc').write_text(code)
    subprocess.run(['c++','-std=c++17','-O1','-Wall','-Wextra','-Werror','-I',str(ROOT/'main'),
                    str(path/'test.cc'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
