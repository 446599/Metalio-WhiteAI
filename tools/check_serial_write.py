#!/usr/bin/env python3
"""Exercise the exact firmware sender with short writes, stalls and failures."""
import subprocess,tempfile
from pathlib import Path
from render_ui_preview import function
ROOT=Path(__file__).resolve().parents[1]
body=function((ROOT/'main/display/raw_display.cc').read_text(),'SerialWriteAll')
source=r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
int64_t clock_us=0;int calls=0;bool driver=true,fail=false,stuck=false;std::string sent;
bool usb_serial_jtag_is_driver_installed(){return driver;}
int64_t esp_timer_get_time(){return clock_us+=100;}
int pdMS_TO_TICKS(int value){return value;}
void vTaskDelay(int ticks){clock_us+=ticks*1000;}
int usb_serial_jtag_write_bytes(const char* p,size_t n,int){
 ++calls;if(fail)return -1;if(stuck || calls%3==0)return 0;
 n=std::min(n,size_t(7));sent.append(p,n);return n;
}
'''+body+r'''
int main(){
 const std::string message="@@MCP_REPLY "+std::string(6000,'x')+"\n";
 assert(SerialWriteAll(1,message.data(),message.size()));assert(sent==message);
 sent.clear();stuck=true;const auto start=clock_us;assert(!SerialWriteAll(1,"x",1));assert(clock_us-start>=2000000 && clock_us-start<2100000 && sent.empty());
 stuck=false;fail=true;assert(!SerialWriteAll(1,"x",1));fail=false;driver=false;assert(!SerialWriteAll(1,"x",1));
 driver=true;assert(SerialWriteAll(1,"done",4));assert(sent=="done");
 std::puts("Serial sender OK: exact bytes across short writes/stalls; bounded timeout/error; output lock released");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cc').write_text(source)
 subprocess.run(['c++','-std=c++17','-fsanitize=undefined',str(p/'test.cc'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
