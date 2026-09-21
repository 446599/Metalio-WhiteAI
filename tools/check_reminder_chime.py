#!/usr/bin/env python3
"""Check actual chime PCM continuity, amplitude, and alert control hit targets."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TEST = r'''
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
#include "audio/reminder_chime.h"
#include "display/reminder_layout.h"
int main() {
    for (const int rate : {16000,24000,48000}) {
        audio::ReminderChime a,b;
        std::vector<int16_t> continuous(rate*3), blocks(rate*3), fade(rate/50);
        a.Render(continuous.data(),continuous.size(),rate);
        for (int i=0;i<rate*3;i+=rate/50) b.Render(blocks.data()+i,rate/50,rate);
        assert(continuous==blocks && continuous.front()==0 && continuous.back()==0);
        long long energy=0, sum=0;
        for (size_t i=0;i<continuous.size();++i) {
            assert(std::abs(static_cast<int>(continuous[i]))<=audio::ReminderChime::kPeak);
            if(i) assert(std::abs(continuous[i]-continuous[i-1])<1200);
            energy+=continuous[i]*continuous[i]; sum+=continuous[i];
        }
        assert(energy>1000000 && std::abs(sum/static_cast<long long>(continuous.size()))<20);
        b.Render(fade.data(),fade.size(),rate);
        const int last=fade.back(); b.FadeOut(fade.data(),fade.size(),rate);
        assert(std::abs(fade.front()-last)<40 && fade.back()==0);
        b.Render(fade.data(),fade.size(),rate); assert(fade.front()==0);
    }
    using namespace reminder_ui;
    assert(Hit(240,604)==Action::Stop && Hit(240,700)==Action::Snooze);
    for (const int y : {0,100,400,567,640,650,663,736,799,900}) assert(Hit(240,y)==Action::None);
    assert(Hit(31,604)==Action::None && Hit(448,604)==Action::None);
    assert(Hit(32,568)==Action::Stop && Hit(447,639)==Action::Stop);
    assert(kHeight>=64 && kSnoozeY-kStopY-kHeight>=16);
    std::puts("Chime/alert UI OK: three sample rates, bounded smooth PCM, fade interruption, exact stop/snooze targets and inactive gutters");
}
'''
with tempfile.TemporaryDirectory(prefix="miaoink-chime-") as directory:
    path=Path(directory)
    (path/"test.cc").write_text(TEST)
    subprocess.run(["c++","-std=c++17","-O1","-Wall","-Wextra","-fsanitize=undefined","-I",str(ROOT/"main"),str(path/"test.cc"),"-o",str(path/"test")],check=True)
    subprocess.run([str(path/"test")],check=True)
