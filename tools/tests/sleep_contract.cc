#include <cstdio>
#include <fstream>
#include <thread>
#include <atomic>
#include "platform.h"
#include "power/sleep_service.h"
#include "power/sleep_policy.h"
#include "power/wallpaper.h"
int main(int argc,char** argv){
    assert(argc==2);auto& gate=power::Gate::Instance();auto& service=power::SleepService::Instance();
    {power::Activity lease;assert(lease && !gate.Freeze());lease.Release();assert(gate.Freeze());power::Activity denied;assert(!denied);gate.Thaw();denied.Retry();assert(denied);}
    assert(gate.Users()==0);
    std::atomic<bool> run{true};std::thread worker([&]{while(run){power::Activity work;std::this_thread::yield();}});
    for(int i=0;i<1000;++i)if(gate.Freeze()){assert(gate.Users()==0);gate.Thaw();}
    run=false;worker.join();
    assert(power::SleepWindowUs(100,0)==30000000 && power::SleepWindowUs(100,103)==2000000 && power::SleepWindowUs(100,101)==0);
    assert(!power::AutoLockDue(999,1000,300,false) && !power::AutoLockDue(1000000,0,300,true) && power::AutoLockDue(300001,0,300,false));
    std::string path=std::string(argv[1])+"/lock.pbm";std::vector<uint8_t> pixels;
    auto write=[&](std::string header,size_t count){std::ofstream f(path,std::ios::binary);f<<header;std::string bytes(count,'\n');f.write(bytes.data(),bytes.size());};
    write("P4\n# test\n480 800\n",48000);assert(power::LoadWallpaper(path.c_str(),pixels)&&pixels.size()==48000&&pixels.front()==10);
    write("P4\n480 800\r\n",48000);assert(power::LoadWallpaper(path.c_str(),pixels));
    for(const char* bad:{"P1\n480 800\n","P4\n800 480\n","P4\n480 800x","P4\n480 99999999999999\n"}){write(bad,48000);assert(!power::LoadWallpaper(path.c_str(),pixels)&&pixels.empty());}
    write("P4\n480 800\n",47999);assert(!power::LoadWallpaper(path.c_str(),pixels));write("P4\n480 800\n",48001);assert(!power::LoadWallpaper(path.c_str(),pixels));
    sim::Reset();service.Start();
    auto tick=[&]{sim::us+=1000000;service.Tick();};
    auto unlock=[&]{service.Wake();tick();assert(!power::Locked());};
    auto lock=[&]{sim::us+=5000000;service.Toggle();tick();assert(power::Locked());};
    // Busy recording/network cannot lose current work to a lock request.
    sim::client_busy=true;service.Toggle();tick();assert(!power::Locked()&&!sim::message.empty());sim::client_busy=false;
    sim::usb=true;lock();tick();assert(sim::paused&&sim::audio&&!sim::pa&&sim::sleep_calls==0);unlock();assert(!sim::paused&&!sim::audio&&sim::pa&&!sim::poll);
    sim::Reset();sim::client_ready=false;lock();tick();assert(!sim::paused);sim::us+=21000000;tick();assert(!power::Locked()&&!sim::message.empty());
    sim::Reset();sim::pending=1;lock();tick();assert(!sim::paused);sim::pending=0;tick();assert(sim::sleep_calls==1);unlock();
    sim::Reset();sim::wake_cause=ESP_SLEEP_WAKEUP_GPIO;int before=sim::sleep_calls;lock();tick();assert(!power::Locked()&&sim::sleep_calls==before+1&&sim::pa&&!sim::audio);service.PowerKeyClick();tick();assert(!power::Locked());
    sim::Reset();lock();{power::Activity storage;tick();assert(!sim::paused);}tick();assert(power::Locked());sim::alarm=true;tick();assert(!power::Locked()&&sim::pa&&!sim::paused);
    sim::Reset();sim::reminders.push_back({});sim::reminders.back().at=sim::epoch+6;lock();tick();assert(sim::last_sleep==5000000);unlock();
    sim::Reset();sim::radio_ok=false;lock();tick();assert(!power::Locked()&&!sim::audio);
    sim::Reset();sim::codec_ok=false;lock();tick();assert(!power::Locked()&&!sim::paused);
    sim::Reset();sim::sleep_error=-9;lock();tick();assert(!power::Locked()&&!sim::paused&&sim::pa);
    sim::Reset();sim::button_stop_error=-5;lock();tick();assert(!power::Locked()&&!sim::buttons&&!sim::paused);
    sim::Reset();sim::button_resume_error=-6;lock();tick();assert(!power::Locked()&&sim::buttons);sim::button_resume_error=0;sim::us+=6000000;tick();assert(!sim::buttons);
    sim::Reset();sim::usb=true;lock();tick();sim::resume_ok=false;unlock();assert(sim::paused);sim::resume_ok=true;sim::us+=6000000;tick();assert(!sim::paused);
    sim::Reset();sim::usb=true;lock();tick();sim::codec_ok=false;unlock();assert(sim::audio&&!sim::pa);sim::codec_ok=true;sim::us+=6000000;tick();assert(!sim::audio&&sim::pa);
    sim::Reset();sim::wifi=false;before=sim::sleep_calls;lock();tick();assert(power::Locked()&&sim::sleep_calls==before);unlock();
    sim::Reset();sim::editing=true;sim::us+=600000000;tick();assert(!power::Locked());sim::editing=false;sim::usb=true;tick();assert(power::Locked());unlock();
    std::puts("Sleep PASS: production coordinator with simulated radio/DMA/clock/GPIO, barrier concurrency, USB/4G guards, storage drain, alarm/key wake, rollback, PBM validation and idle policy");
}
