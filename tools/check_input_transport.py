#!/usr/bin/env python3
"""Host regressions for input backpressure and actual WebSocket RX assembly."""
from pathlib import Path
import subprocess
import tempfile
from render_ui_preview import function
ROOT = Path(__file__).resolve().parents[1]
body = function((ROOT/'components/esp-ml307/src/web_socket.cc').read_text(), 'WebSocket::OnTcpData')
code = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include "ui_work_queue.h"
#define ESP_LOGE(...) ((void)0)
void xEventGroupSetBits(int,int) {}
class WebSocket {
public:
    std::string receive_buffer_;
    std::vector<char> current_message_;
    bool rx_fragmented_=false, rx_binary_=false, handshake_completed_=true, connected_=true;
    int handshake_event_group_=0;
    static constexpr int HANDSHAKE_SUCCESS_BIT=1, HANDSHAKE_FAILED_BIT=2;
    std::function<void(const char*,size_t,bool)> on_data_;
    std::function<void()> on_disconnected_;
    bool SendControlFrame(int,const void*,size_t) { return true; }
    void OnTcpData(const std::string& data);
};
''' + body + r'''
std::string frame(int opcode, bool fin, const std::string& s) {
    std::string f; f += char(opcode | (fin?128:0));
    if (s.size()<126) f += char(s.size());
    else { f+=char(126); f+=char(s.size()>>8); f+=char(s.size()&255); }
    return f+s;
}
int main() {
    UiWorkQueue queue;
    std::vector<int> executed;
    for (int i=0;i<8;++i) assert(queue.Push([&,i]{executed.push_back(i);}));
    assert(!queue.Push([]{})); assert(queue.Dropped()==1);
    std::function<void()> work;
    assert(queue.Pop(work)); work();
    assert(queue.Push([&]{executed.push_back(8);}));
    while(queue.Pop(work)) work();
    for (int i=0;i<9;++i) assert(executed[i]==i);
    assert(!queue.Pop(work));
    WebSocket a,b;
    std::vector<std::string> first, second;
    a.on_data_=[&](const char* p,size_t n,bool binary){assert(!binary);first.emplace_back(p,n);};
    b.on_data_=[&](const char* p,size_t n,bool binary){assert(!binary);second.emplace_back(p,n);};
    a.OnTcpData(frame(1,false,"old-"));
    b.OnTcpData(frame(1,true,"new"));
    a.OnTcpData(frame(0,true,"turn"));
    assert(first==std::vector<std::string>{"old-turn"});
    assert(second==std::vector<std::string>{"new"});
    b.OnTcpData(frame(0,true,"orphan")); assert(second.size()==1);
    auto large=frame(1,true,std::string(255,'z'));
    b.OnTcpData(large.substr(0,3)); assert(second.size()==1);
    b.OnTcpData(large.substr(3)); assert(second.back()==std::string(255,'z'));
    std::puts("Input/transport OK: bounded FIFO, overflow, independent fragments, orphan continuation, split extended length");
}
'''
with tempfile.TemporaryDirectory(prefix='miaoink-input-test-') as d:
    d=Path(d); (d/'test.cc').write_text(code)
    subprocess.run(['c++','-std=c++17','-fsanitize=undefined','-I',str(ROOT/'main'),str(d/'test.cc'),'-o',str(d/'test')],check=True)
    subprocess.run([str(d/'test')],check=True)
