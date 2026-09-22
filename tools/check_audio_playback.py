#!/usr/bin/env python3
"""Execute production playback/allocator branches with simulated RTOS/codec/I2S.

No radio, real Opus stack peaks or physical audio are claimed by this test.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile
from render_ui_preview import function
ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'main/xiaozhi/xiaozhi_audio.cc').read_text()
# Stack sizes and placement are deliberately not part of this repair.
for name,size in [('kCaptureStackBytes',28672),('kPlaybackStackBytes',24576),('kSelfTestStackBytes',49152)]:
    assert re.search(rf'constexpr int {name} = {size};',source),name
allocator=function(source.replace('BaseType_t CreateAudioTask(', 'int CreateAudioTask('),'CreateAudioTask')
assert 'xTaskCreatePinnedToCore(' in allocator and 'SPIRAM' not in allocator and 'WithCaps' not in allocator
bodies='\n'.join(function(source,name) for name in ['AudioSession::EnsurePlaybackTask','AudioSession::RecordPlaybackStack','AudioSession::PlaybackLoop'])
TEST=r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "audio/stack_watermark.h"
#include "audio/reminder_chime.h"
#include "audio/recorder_state.h"
#include "system/build_features.h"
using TaskFunction_t=void(*)(void*);using TaskHandle_t=void*;using UBaseType_t=unsigned;
constexpr int pdTRUE=1,pdPASS=1,kPlaybackStackBytes=24576,kAudioTaskPriority=4;
constexpr unsigned kPlaybackIdleCloseTicks=10;
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
enum class Case {Idle,Chime,DecoderFail,DecodeFail,EmptyFrame,ResampleFail,WriteFail,Direct,Resampled,ZeroWatermark,RecorderBusy};
Case scenario=Case::Idle;
uint32_t low=24000;unsigned reads=0,writes=0,allocations=0;size_t largest=13824;
bool queue_ok=true;
void Peak(uint32_t remaining){low=std::min(low,remaining);}
int pdMS_TO_TICKS(int v){return v;}
uint32_t uxTaskGetStackHighWaterMark(void*){return low;}
int xTaskCreatePinnedToCore(TaskFunction_t,const char*,uint32_t stack,void*,UBaseType_t,TaskHandle_t* handle,int core){
 assert(stack==24576 && core==1);++allocations;
 if(largest<stack){*handle=nullptr;return 0;}*handle=reinterpret_cast<void*>(1);return pdPASS;
}
''' + allocator + r'''
struct esp_audio_dec_in_raw_t {uint8_t* buffer=nullptr;uint32_t len=0;};
struct esp_audio_dec_out_frame_t {uint8_t* buffer=nullptr;uint32_t len=0,decoded_size=0;};
struct esp_audio_dec_info_t {};
using esp_audio_err_t=int;constexpr int ESP_AUDIO_ERR_OK=0;
int esp_opus_dec_decode(void*,esp_audio_dec_in_raw_t*,esp_audio_dec_out_frame_t* frame,esp_audio_dec_info_t*){
 Peak(scenario==Case::ZeroWatermark ? 0 : 6000);
 frame->decoded_size=scenario==Case::EmptyFrame ? 0 : 16;
 return scenario==Case::DecodeFail ? -1 : 0;
}
struct Resampler {std::vector<int16_t> out=std::vector<int16_t>(32);int Process(const int16_t*,int n){Peak(5300);return scenario==Case::ResampleFail ? 0 : n;}};
class AudioSession {
public:
 struct Packet {uint16_t length=8;uint8_t data[512]{};};
 struct Decoder {void* handle=nullptr;int sample_rate=24000,output_rate=16000;std::vector<uint8_t> buffer=std::vector<uint8_t>(8192);}decoder_;
 std::atomic<audio::RecorderMode> recorder_mode_{audio::RecorderMode::Idle};
 std::atomic<bool> reminder_requested_{false},capture_requested_{false},playback_open_{false},self_test_active_{false},reminder_playing_{false};
 std::atomic<uint32_t> reminder_errors_{0},reminder_frames_{0},decode_errors_{0},packets_decoded_{0};
 std::atomic<uint32_t> playback_create_failures_{0},playback_tts_stack_samples_{0};
 audio::StackWatermark playback_stack_,playback_tts_stack_;
 std::mutex mutex_;void* rx_queue_=reinterpret_cast<void*>(2);TaskHandle_t playback_task_=nullptr;
 Resampler resampler_;
 static void PlaybackTaskEntry(void*){}
 int OutputRate(){return 16000;}
 void EnsureQueue(){if(!queue_ok)rx_queue_=nullptr;}
 bool EnsureDecoder(){Peak(8000);decoder_.handle=reinterpret_cast<void*>(3);return scenario!=Case::DecoderFail;}
 void CloseDecoder(){decoder_.handle=nullptr;}
 bool EnsurePlaybackTask();void RecordPlaybackStack(bool);void PlaybackLoop();
};
AudioSession* active=nullptr;
void vTaskDelay(int){if(scenario==Case::RecorderBusy)active->recorder_mode_.store(audio::RecorderMode::Idle);}
unsigned uxQueueMessagesWaiting(void*){return 0;}
int xQueueReceive(void*,void*,int){
 ++reads;assert(reads<20);
 if(reads>1 && scenario!=Case::Idle && scenario!=Case::Chime && scenario!=Case::RecorderBusy){
   // The sample must already be visible while the worker is still alive.
   assert(active->playback_tts_stack_samples_.load()==1);
 }
 return reads==1 && scenario!=Case::Idle && scenario!=Case::Chime && scenario!=Case::RecorderBusy;
}
struct FakeHal {int WriteSpk(const int16_t*,int){++writes;Peak(scenario==Case::Chime ? 22300 : 4900);active->reminder_requested_.store(false);return scenario==Case::WriteFail ? -1 : 16;}};
FakeHal& GetHAL(){static FakeHal h;return h;}
''' + bodies + r'''
int main(){
 static_assert(!device::kBleDiscoveryEnabled,"product defaults must not enable BLE");
 // A 28 KiB total heap is not enough if its largest block is only 13.5 KiB.
 AudioSession allocation;largest=13824;assert(!allocation.EnsurePlaybackTask());
 assert(allocation.playback_task_==nullptr && allocation.playback_create_failures_==1);
 largest=32768;assert(allocation.EnsurePlaybackTask());assert(allocation.EnsurePlaybackTask());
 assert(allocations==2 && allocation.playback_create_failures_==1);
 queue_ok=false;AudioSession noqueue;assert(!noqueue.EnsurePlaybackTask());assert(allocations==2);queue_ok=true;
 unsigned cases=0;
 for(Case c:{Case::Idle,Case::Chime,Case::DecoderFail,Case::DecodeFail,Case::EmptyFrame,Case::ResampleFail,Case::WriteFail,Case::Direct,Case::Resampled,Case::ZeroWatermark,Case::RecorderBusy}){
   scenario=c;low=24000;reads=writes=0;AudioSession session;active=&session;
   if(c==Case::Chime)session.reminder_requested_=true;
   if(c==Case::Direct)session.decoder_.sample_rate=16000;
   if(c==Case::RecorderBusy)session.recorder_mode_=audio::RecorderMode::Recording;
   session.PlaybackLoop();++cases;
   const bool tts=c!=Case::Idle && c!=Case::Chime && c!=Case::RecorderBusy;
   assert(session.playback_tts_stack_samples_.load()==(tts?1U:0U));
   if(tts)assert(session.playback_tts_stack_.FreeBytes()==low);
   assert(session.playback_stack_.FreeBytes()==low && session.playback_task_==nullptr);
   if(c==Case::Direct || c==Case::Resampled || c==Case::ZeroWatermark){assert(session.packets_decoded_==1 && writes==1);}
   else if(tts)assert(session.decode_errors_==1 && session.packets_decoded_==0);
   if(c==Case::Chime)assert(session.reminder_frames_>=1 && session.playback_tts_stack_samples_==0);
 }
 audio::StackWatermark mark;assert(mark.FreeBytes()==0);mark.Observe(23000);mark.Observe(10000);mark.Observe(22000);assert(mark.FreeBytes()==10000);
 std::vector<std::thread> threads;for(unsigned i=1;i<=8;++i)threads.emplace_back([&mark,i]{mark.Observe(i*100);});for(auto& t:threads)t.join();assert(mark.FreeBytes()==100);
 mark.Observe(0);mark.Observe(20000);assert(mark.FreeBytes()==0);
 std::printf("Playback diagnostics PASS: %u real-loop scenarios, failed allocation/retry, TTS/error/zero-watermark and concurrent minima (RTOS/codec/I2S mocked)\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='whiteai-audio-') as folder:
    p=Path(folder);(p/'test.cc').write_text(TEST)
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread','-I',str(ROOT/'main'),str(p/'test.cc'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
subprocess.run(['python3',str(ROOT/'tools/tests/audio_memory_budget_test.py')],check=True)
