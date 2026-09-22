#pragma once
#include <algorithm>
#include <cstdint>
namespace power {
// Sleep is bounded, wakes BEFORE the next persisted alarm, and does not need a
// new RTOS task. The one-second scheduling margin lets the alarm task run.
inline int64_t SleepWindowUs(int64_t now,int64_t next_alarm) {
    if(next_alarm>0 && next_alarm<=now+1)return 0;
    const int64_t seconds=next_alarm>0 ? std::min<int64_t>(30,next_alarm-now-1) : 30;
    return std::max<int64_t>(0,seconds)*1000000;
}
inline bool AutoLockDue(int64_t now,int64_t last,unsigned seconds,bool editing) {
    return !editing && seconds>=60 && now>=last && now-last>=int64_t(seconds)*1000;
}
inline bool CanEnterSleep(bool network_paused,bool storage_idle,bool usb,bool key,bool alarm,bool wifi_mode) {
    return network_paused && storage_idle && !usb && !key && !alarm && wifi_mode;
}
}
