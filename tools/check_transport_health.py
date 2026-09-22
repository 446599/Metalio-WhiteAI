#!/usr/bin/env python3
"""Execute production WebSocket/control and TLS shutdown paths with fake sockets."""
from pathlib import Path
import os, subprocess, tempfile
from render_ui_preview import function
ROOT=Path(__file__).resolve().parents[1]
ws=(ROOT/'components/esp-ml307/src/web_socket.cc').read_text().replace('bool WebSocket::Send(const void*','bool WebSocket::SendData(const void*')
ssl=(ROOT/'components/esp-ml307/src/esp/esp_ssl.cc').read_text()
code=r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <cerrno>
#include <sys/socket.h>
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
void xEventGroupSetBits(int,int){}
struct Tcp {bool fail=false,short_write=false;std::vector<std::string> sent;int Send(const std::string& s){if(fail)return -1;sent.push_back(s);return int(s.size())-(short_write?1:0);}};
struct WebSocket {
 std::unique_ptr<Tcp> tcp_{new Tcp};std::string receive_buffer_,pending_pong_;std::vector<char> current_message_;
 bool continuation_=false,rx_fragmented_=false,rx_binary_=false,handshake_completed_=true,pong_pending_=false;
 std::atomic<bool> connected_{true};std::atomic<uint32_t> pings_{0},pongs_{0},ping_failed_{0},rx_frames_{0};std::atomic<uint16_t> close_code_{0};
 int handshake_event_group_=0;static constexpr int HANDSHAKE_SUCCESS_BIT=1,HANDSHAKE_FAILED_BIT=2;
 std::mutex send_mutex_,control_mutex_;
 std::function<void(const char*,size_t,bool)> on_data_;
 std::function<void()> on_disconnected_;
 bool SendData(const void*,size_t,bool,bool);bool SendControlFrame(uint8_t,const void*,size_t);bool Ping();bool ServiceControl();void OnTcpData(const std::string&);
};
''' .replace('#include <cerrno>','#include <cerrno>\n#include <functional>')
code+='\n'.join(function(ws,'WebSocket::'+name) for name in ('SendData','SendControlFrame','Ping','ServiceControl','OnTcpData'))
code+=r'''
struct esp_tls_t{};
constexpr int ESP_OK=0,ESP_SSL_EVENT_RECEIVE_TASK_EXIT=1,pdFALSE=0,portMAX_DELAY=0x7fffffff;
int closed=0,stopped=0,waited=0;
int esp_tls_get_conn_sockfd(esp_tls_t*,int* fd){*fd=73;return 0;}
int fake_shutdown(int fd,int how){assert(fd==73&&how==SHUT_RDWR);++stopped;return 0;}
void esp_tls_conn_destroy(esp_tls_t*){assert(stopped>0&&waited>0);++closed;}
void xEventGroupWaitBits(int,int,int,int,int ticks){assert(ticks==portMAX_DELAY);++waited;}
struct EspSsl{bool connected_=true;esp_tls_t* tls_client_=reinterpret_cast<esp_tls_t*>(1);void* receive_task_handle_=reinterpret_cast<void*>(1);int event_group_=1;void Disconnect();};
#define shutdown fake_shutdown
'''
code+=function(ssl,'EspSsl::Disconnect')
code+=r'''
#undef shutdown
std::string frame(int opcode,const std::string& s,bool fin=true){assert(s.size()<126);return std::string(1,char(opcode|(fin?128:0)))+char(s.size())+s;}
std::string unmask(const std::string& s){assert(uint8_t(s[1])&128);size_t n=uint8_t(s[1])&127;assert(n<126&&s.size()==n+6);std::string out;for(size_t i=0;i<n;++i)out+=char(s[6+i]^s[2+i%4]);return out;}
int main(){
 WebSocket socket;assert(socket.Ping());assert(socket.pings_==1&&socket.ping_failed_==0);assert(uint8_t(socket.tcp_->sent.back()[0])==0x89);
 socket.OnTcpData(frame(0xA,""));assert(socket.pongs_==1);
 size_t sent=socket.tcp_->sent.size();socket.OnTcpData(frame(9,"old"));socket.OnTcpData(frame(9,std::string("a\0b",3)));
 assert(socket.tcp_->sent.size()==sent);assert(socket.ServiceControl());assert(unmask(socket.tcp_->sent.back())==std::string("a\0b",3));
 socket.tcp_->short_write=true;assert(!socket.Ping()&&socket.ping_failed_==1);assert(!socket.SendData("text",4,false,true));
 socket.tcp_->short_write=false;assert(socket.SendData("text",4,false,true));assert(unmask(socket.tcp_->sent.back())=="text");
 socket.tcp_->fail=true;assert(!socket.Ping()&&socket.ping_failed_==2);socket.tcp_->fail=false;
 std::vector<std::string> messages;socket.on_data_=[&](const char* s,size_t n,bool){messages.emplace_back(s,n);};
 socket.OnTcpData(frame(1,"abc",false)+frame(9,"interleaved")+frame(0,"def"));assert(messages.back()=="abcdef");assert(socket.ServiceControl());
 int disconnected=0;socket.on_disconnected_=[&]{++disconnected;};
 socket.OnTcpData(frame(8,std::string("\x03\xe8",2)));assert(!socket.connected_&&socket.close_code_==1000&&disconnected==1);assert(!socket.Ping());
 WebSocket too_big;too_big.OnTcpData(std::string("\x81\x7f\x00\x00\x00\x00\x00\x01\x00\x00",10));assert(!too_big.connected_&&too_big.receive_buffer_.empty());
 WebSocket bad_control;bad_control.OnTcpData(frame(9,"x",false));assert(!bad_control.connected_);
 EspSsl tls;tls.Disconnect();assert(!tls.connected_&&!tls.tls_client_&&!tls.receive_task_handle_&&closed==1&&stopped==1&&waited==1);tls.Disconnect();assert(closed==1);
 std::puts("Transport health PASS: actual Ping/Pong/send/receive bounds and TLS shutdown/join/single-close ownership (fake sockets, no live server)");
}
'''
with tempfile.TemporaryDirectory(prefix='whiteai-transport-') as d:
 p=Path(d);(p/'test.cc').write_text(code)
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-g','-O1','-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(p/'test.cc'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
