#!/usr/bin/env python3
"""Execute actual board volume callbacks with a timer-context guard.

NVS/audio mutation and display drawing must occur only after the UI queue is
consumed, never inside the small ESP timer callback stack.
"""
from pathlib import Path
import re
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
s = (ROOT/'main/hal/metalio-e-ink-4/metalio_e_ink_4_board.cc').read_text()
callbacks=[]
for button in ('volume_up_button_', 'volume_down_button_'):
    for event in ('OnClick', 'OnLongPress'):
        start=s.index(button+'->'+event+'([this]()')
        begin=s.index('{',start)
        depth=0
        for t in re.finditer(r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*.*?\*/|[{}]',s[begin:],re.S):
            if t.group()=='{':depth+=1
            elif t.group()=='}':
                depth-=1
                if depth==0:
                    callbacks.append(s[start:begin+t.end()]+');')
                    break
code=r'''
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <functional>
#include <string>
#include "ui_work_queue.h"
#include "power/activity.h"
#define ESP_LOGI(...) ((void)0)
bool timer_context=false;
class Application {
public:
    UiWorkQueue queue;
    static Application& GetInstance(){static Application a;return a;}
    bool ScheduleUi(std::function<void()> f){return queue.Push(std::move(f));}
};
struct Button {
    std::function<void()> click,long_press;
    void OnClick(std::function<void()> f){click=std::move(f);}
    void OnLongPress(std::function<void()> f){long_press=std::move(f);}
};
struct Codec {
    int volume=70;
    int output_volume(){assert(!timer_context);return volume;}
    void SetOutputVolume(int v){assert(!timer_context);assert(v>=0&&v<=100);volume=v;}
};
struct Display {
    int notifications=0;
    void ShowNotification(const std::string&){assert(!timer_context);++notifications;}
};
namespace Lang {namespace Strings {
    const std::string VOLUME="Volume ", MAX_VOLUME="Max", MUTED="Muted";
}}
struct Board {
    Codec codec; Display display; Button up,down;
    Button* volume_up_button_=&up;Button* volume_down_button_=&down;
    Codec* GetAudioCodec(){assert(!timer_context);return &codec;}
    Display* GetDisplay(){assert(!timer_context);return &display;}
    void Init(){
'''+'\n'.join(callbacks)+r'''
    }
};
int main(){
    Board b;b.Init();
    auto check=[&](std::function<void()> action,int expected){
        int before=b.codec.volume,notices=b.display.notifications;
        timer_context=true;action();timer_context=false;
        assert(b.codec.volume==before && b.display.notifications==notices);
        std::function<void()> work;
        assert(Application::GetInstance().queue.Pop(work));work();
        assert(b.codec.volume==expected && b.display.notifications==notices+1);
    };
    check(b.up.click,80);check(b.up.long_press,100);
    check(b.down.click,90);check(b.down.long_press,0);
    check(b.down.click,0);check(b.up.click,10);
    // Lock after an event is enqueued: even stale queued volume work must
    // not enable audio or touch NVS behind the sleep coordinator.
    for (auto action : {b.up.click,b.down.click,b.up.long_press,b.down.long_press}) {
        const int volume=b.codec.volume, notices=b.display.notifications;
        timer_context=true;action();timer_context=false;
        power::Gate::Instance().locked.store(true);
        std::function<void()> work;
        assert(Application::GetInstance().queue.Pop(work));work();
        assert(b.codec.volume==volume && b.display.notifications==notices);
        power::Gate::Instance().locked.store(false);
    }
    std::puts("Volume callbacks OK: click/long-press defer NVS and display out of timer context; bounds hold");
}
'''
with tempfile.TemporaryDirectory(prefix='miaoink-volume-test-') as d:
    d=Path(d);(d/'test.cc').write_text(code)
    subprocess.run(['c++','-std=c++17','-I',str(ROOT/'main'),str(d/'test.cc'),'-o',str(d/'test')],check=True)
    subprocess.run([str(d/'test')],check=True)
