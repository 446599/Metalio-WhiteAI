#!/usr/bin/env python3
"""Compile the actual client control/event methods with fake audio and transport.

Exercises key-up races, failed/offline starts, prompt JSON, stale responses,
transcript preservation, sentence accumulation and bounded UTF-8 storage.
No microphone, account, network or device is accessed.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from render_ui_preview import function

ROOT = Path(__file__).resolve().parents[1]


def main():
    source = (ROOT / 'main/xiaozhi/xiaozhi_client.cc').read_text()
    header = (ROOT / 'main/xiaozhi/xiaozhi_client.h').read_text()
    header = re.sub(r'#include <freertos/[^>]+>', '', header).replace('#pragma once', '')
    header = header.replace('#include "conversation.h"', '#include "xiaozhi/conversation.h"')
    header = header.replace('private:', 'public:')
    methods = ['BeginTextTask', 'CaptureHistory', 'SubmitText', 'SwitchChat', 'DeleteChat', 'ListenStart', 'ListenStop', 'Abort', 'RunQuickAction', 'SaveCapsule', 'RestoreCapsule',
               'IsListening', 'IsSessionReady', 'EndListenWindowLocked',
               'InvalidateTransportLocked', 'HandleDisconnect', 'SavePendingCapsule',
               'ConnectOnce', 'ReleaseTransport', 'SendPendingAction', 'HandleData']
    # Extract non-void/bool methods with the same balanced parser used by previews.
    parse_source = re.sub(r'^bool Client::', 'int Client::', source, flags=re.M)
    bodies = []
    for name in methods:
        body = function(parse_source, 'Client::' + name).replace('int Client::', 'bool Client::', 1)
        bodies.append(body)
    free_names = ['StringItem', 'IntItem', 'PrefixText', 'JsonEscape', 'BuildHello']
    free_source = source.replace('const char* StringItem(', 'const ui_glyph_t* StringItem(').replace('std::string JsonEscape(', 'uint32_t JsonEscape(').replace('std::string BuildHello(', 'uint32_t BuildHello(')
    helpers = '\n'.join(function(free_source, name) for name in free_names)
    helpers = helpers.replace('const ui_glyph_t* StringItem(', 'const char* StringItem(').replace('uint32_t JsonEscape(', 'std::string JsonEscape(').replace('uint32_t BuildHello(', 'std::string BuildHello(')
    code = r'''
#include <algorithm>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <cJSON.h>
#include <nvs.h>
#include "dashboard/dashboard_data.h"
#include "xiaozhi/conversation.h"
#include "tests/chat_client_history_stub.h"
using TaskHandle_t = void*;
int64_t fake_us = 1000000;
int64_t esp_timer_get_time() { return fake_us; }
void xTaskNotifyGive(TaskHandle_t) {}
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
class WebSocket {
public:
    std::function<void()> on_connected, on_disconnected;
    std::function<void(int)> on_error;
    std::function<void(const char*,size_t,bool)> on_data;
    void SetReceiveBufferSize(size_t) {}
    void SetHeader(const char*,const char*) {}
    void OnConnected(std::function<void()> f) { on_connected=f; }
    void OnDisconnected(std::function<void()> f) { on_disconnected=f; }
    void OnError(std::function<void(int)> f) { on_error=f; }
    void OnData(std::function<void(const char*,size_t,bool)> f) { on_data=f; }
    bool Connect(const char*) { on_connected(); return true; }
    void Close() { on_disconnected(); }
};
class FakeNetwork {
public:
    std::unique_ptr<WebSocket> CreateWebSocket(int) { return std::make_unique<WebSocket>(); }
};
class Board {
public:
    static Board& GetInstance() { static Board b; return b; }
    FakeNetwork* GetNetwork() { static FakeNetwork n; return &n; }
    std::string GetUuid() { return "test-client"; }
};
class SystemInfo { public: static std::string GetMacAddress() { return "test-device"; } };
class FakeHAL { public: int AudioInputSampleRate() { return 16000; } };
FakeHAL& GetHAL() { static FakeHAL h; return h; }
std::string committed_capsule, pending_capsule;
bool fail_commit = false, fail_write = false;
int nvs_open(const char*, int, nvs_handle_t* h) { *h=1; return 0; }
void nvs_close(nvs_handle_t) {}
int nvs_set_blob(nvs_handle_t, const char*, const void* p, size_t n) { if (fail_write) return -1; pending_capsule.assign(static_cast<const char*>(p), n); return 0; }
int nvs_commit(nvs_handle_t) { if (fail_commit) return -1; committed_capsule=pending_capsule; return 0; }
int nvs_get_blob(nvs_handle_t, const char*, void* p, size_t* n) {
    if (committed_capsule.empty()) return -1;
    if (!p) { *n=committed_capsule.size(); return 0; }
    if (*n < committed_capsule.size()) return -1;
    std::memcpy(p, committed_capsule.data(), committed_capsule.size());
    *n=committed_capsule.size(); return 0;
}
''' + header + r'''
namespace xiaozhi {
constexpr uint8_t kActionListenStart=1, kActionListenStop=2, kActionPrompt=4;
constexpr const char* kTag = "test";
constexpr int kHelloFrameDurationMs=60;
class AudioSession {
public:
    static AudioSession& GetInstance() { static AudioSession instance; return instance; }
    bool capture=false, playback=false;
    int starts=0, stops=0;
    bool available=true;
    bool StartCapture() { if (!available) return false; capture=true; ++starts; return true; }
    void StopCapture() { capture=false; ++stops; }
    bool capturing() const { return capture; }
    void Reset() { capture=false; playback=false; }
    void SetParams(int,int) {}
    void OnServerPacket(const uint8_t*,size_t) { playback=true; }
    void OpenPlayback() { playback=true; }
    void EndPlayback() { playback=false; }
};
class Activation {
public:
    static Activation& GetInstance() { static Activation a; return a; }
    int Snapshot() { return 0; }
};
void PublishActivationState(int) {}
std::vector<std::string> sent;
std::function<void(const std::string&)> send_hook;
bool send_ok = true;
bool Client::SendText(const std::string& message) {
    sent.push_back(message);
    if (send_hook) send_hook(message);
    return send_ok;
}
void Client::PublishStatus(const char* status) { dashboard::DashboardData::GetInstance().SetAiStatus(status); }
''' + helpers + '\n' + '\n'.join(bodies) + r'''
}
int main() {
    using namespace xiaozhi;
    Client client;
    auto& audio = AudioSession::GetInstance();
    auto& conversation = Conversation::GetInstance();
    client.url_="wss://test.invalid";
    auto event = [&](const char* text) { client.HandleData(client.transport_epoch_, text, std::strlen(text), false); };
    auto connected = [&]() {
        client.ReleaseTransport();
        assert(client.ConnectOnce());
        event(R"({"type":"hello","session_id":"test-session"})");
    };
    // Offline presses are not deferred or replayed after hello.
    assert(!client.ListenStart());
    assert(!client.listen_requested_.load());
    connected();
    event(R"({"type":"hello","session_id":"test-session"})");
    client.SendPendingAction();
    assert(audio.starts == 0);
    // A rapid complete press before worker scheduling never latches the mic.
    assert(client.ListenStart()); assert(client.ListenStop());
    client.SendPendingAction();
    assert(!audio.capture && !client.IsListening());
    assert(conversation.Snapshot().state == TurnState::Idle);
    // Normal hold/release stops locally before the worker sends protocol stop.
    assert(client.ListenStart()); client.SendPendingAction();
    assert(audio.capture && conversation.Snapshot().state == TurnState::Listening);
    assert(client.ListenStop()); assert(!audio.capture);
    client.SendPendingAction();
    assert(conversation.Snapshot().state == TurnState::Transcribing);
    event(R"({"type":"stt","session_id":"old-session","text":"old turn"})");
    const bool rejected_old_session = conversation.Snapshot().transcript.empty();
    event(R"({"type":"stt","text":"把\"闪念\"整理为待办，包含繁體麒麟龘。"})");
    const auto original = conversation.Snapshot().transcript;
    assert(original.find("闪念") != std::string::npos);
        client.SavePendingCapsule();
    const auto original_saved=conversation.Snapshot();
    assert(original_saved.saved);
    event(R"({"type":"llm","text":"临时片段"})");
    const bool changed_answer_unsaved = !conversation.Snapshot().saved;
    std::fprintf(stderr, "regressions: old_session_rejected=%d changed_answer_unsaved=%d\n",
                 rejected_old_session, changed_answer_unsaved);
    assert(rejected_old_session && changed_answer_unsaved);
    event(R"({"type":"tts","state":"start"})");
    event(R"({"type":"tts","state":"sentence_start","text":"完整第一句。"})");
    event(R"({"type":"tts","state":"sentence_start","text":"完整第二句。"})");
    event(R"({"type":"tts","state":"stop"})");
    assert(conversation.Snapshot().answer == "完整第一句。\n完整第二句。");
    assert(conversation.Snapshot().state == TurnState::Done);
    assert(client.save_requested_.load());
    client.SavePendingCapsule();
    assert(conversation.Snapshot().saved);
    assert(committed_capsule.find("\"schema\":1")!=std::string::npos);
    const auto complete=conversation.Snapshot();
    conversation.Clear(); assert(LoadLastCapsule());
    assert(conversation.Snapshot().answer==complete.answer && conversation.Snapshot().transcript==original);
    assert(client.RunQuickAction(QuickAction::Tasks));
    client.SendPendingAction();
    cJSON* request = cJSON_Parse(sent.back().c_str());
    assert(request);
    assert(std::strcmp(cJSON_GetObjectItem(request,"state")->valuestring,"detect")==0);
    assert(std::strcmp(cJSON_GetObjectItem(request,"text")->valuestring,"待办草稿")==0);
    assert(std::strstr(cJSON_GetObjectItem(request,"text")->valuestring,"闪念")==nullptr);
    assert(!ChatTask::Instance().ReadAll());
    auto task=ChatTask::Instance().Read(0,0);
    assert(task.ok && task.text.find("待办清单")!=std::string::npos && task.text.find("闪念")!=std::string::npos);
    assert(ChatTask::Instance().ReadAll());
    cJSON_Delete(request);
    event(R"({"type":"stt","text":"server echo of the follow-up prompt"})");
    assert(conversation.Snapshot().transcript == original);
    // Release while TLS send is in flight must invalidate the eventual start.
    client.Abort(); client.SendPendingAction();
    assert(!client.ListenStart()); // canceled socket cannot accept a new turn
    connected();
    const int before = audio.starts;
    send_hook = [&](const std::string& msg) {
        if (msg.find("\"state\":\"start\"") != std::string::npos) client.ListenStop();
    };
    assert(client.ListenStart()); client.SendPendingAction();
    send_hook = {};
    assert(audio.starts == before && !audio.capture);
    client.SendPendingAction(); assert(!audio.capture);
    // A failed start never opens capture or survives reconnection.
    connected();
    send_ok=false;
    assert(client.ListenStart()); client.SendPendingAction();
    assert(!client.listen_requested_.load() && !audio.capture);
    send_ok=true; connected();
    event(R"({"type":"hello","session_id":"test-session"})");
    assert(!client.IsListening());
    // Late server events after cancel cannot overwrite content or play speech.
    client.Abort(); client.SendPendingAction();
    const auto unchanged=conversation.Snapshot().answer;
    event(R"({"type":"tts","state":"sentence_start","text":"late"})");
    event(R"({"type":"tts","state":"start"})");
    assert(conversation.Snapshot().answer==unchanged && !audio.playback);
    // Every callback retained by the old socket is stale after reconnect.
    connected();
    const auto old_connected=client.websocket_->on_connected;
    const auto old_disconnected=client.websocket_->on_disconnected;
    const auto old_error=client.websocket_->on_error;
    const auto old_data=client.websocket_->on_data;
    assert(client.ListenStart()); client.SendPendingAction();
    assert(client.ListenStop()); client.SendPendingAction();
    assert(!client.ListenStart()); // unfinished response requires a new socket
    assert(!client.IsListening());
    connected();
    old_connected(); old_disconnected(); old_error(-1);
    assert(client.IsConnected() && client.IsSessionReady() && !client.IsListening());
    assert(client.ListenStart()); client.SendPendingAction();
    assert(client.ListenStop()); client.SendPendingAction();
    auto inject_old=[&](const char* text) { old_data(text,std::strlen(text),false); };
    inject_old(R"({"type":"hello","session_id":"retired"})");
    inject_old(R"({"type":"stt","text":"retired untagged transcript"})");
    inject_old(R"({"type":"tts","state":"start"})");
    inject_old(R"({"type":"tts","state":"stop"})");
    old_data("opus",4,true);
    event(R"({"type":"stt","session_id":"old-session","text":"wrong session"})");
    event(R"({"type":"tts","session_id":"old-session","state":"start"})");
    event(R"({"type":"tts","session_id":"old-session","state":"stop"})");
    assert(conversation.Snapshot().transcript.empty() && !audio.playback);
    assert(conversation.Snapshot().state==TurnState::Transcribing);
    event(R"({"type":"stt","session_id":"test-session","text":"new turn"})");
    assert(conversation.Snapshot().transcript=="new turn");
    event(R"({"type":"tts","state":"start"})");
    client.HandleData(client.transport_epoch_,"opus",4,true);
    assert(audio.playback);
    old_disconnected(); old_error(-1);
    inject_old(R"({"type":"tts","state":"stop"})");
    event(R"({"type":"tts","session_id":"old-session","state":"stop"})");
    assert(audio.playback && conversation.Snapshot().state==TurnState::Speaking && client.IsConnected());
    event(R"({"type":"tts","state":"stop"})");
    assert(!audio.playback);
    client.HandleData(client.transport_epoch_,"late opus",9,true);
    event(R"({"type":"tts","state":"sentence_start","text":"after stop"})");
    assert(!audio.playback && conversation.Snapshot().answer.empty());
    // A save completing while the answer grows cannot mark newer text saved.
    conversation.Begin(); conversation.SetTranscript("revision test");
    const auto draft=conversation.Snapshot();
    conversation.MarkSaving(draft.turn,draft.content_revision);
    conversation.ReceiveAnswer("answer growth",true);
    conversation.MarkSaved(draft.turn,draft.content_revision,true);
    assert(!conversation.Snapshot().saved && conversation.Snapshot().persisted_revision==draft.content_revision);
    assert(conversation.Snapshot().saving_revision==0 && conversation.Snapshot().message!="正在保存到本机");
    const auto grown=conversation.Snapshot();
    conversation.MarkSaving(grown.turn,grown.content_revision);
    conversation.MarkSaved(draft.turn,draft.content_revision,false);
    assert(conversation.Snapshot().saving_revision==grown.content_revision && !conversation.Snapshot().save_failed);
    conversation.MarkSaved(grown.turn,grown.content_revision,false);
    assert(!conversation.Snapshot().saved && conversation.Snapshot().save_failed);
    // All three actions use short approved wake events, fetched MCP text and
    // a terminal response. Unread task responses are not accepted as success.
    for(auto action:{QuickAction::Organize,QuickAction::Tasks,QuickAction::Translate}){
        client.Abort();connected();fake_us+=12000000;
        conversation.Restore("测试原文，包含引号和换行","旧回答");
        assert(client.RunQuickAction(action));client.SendPendingAction();
        if(action==QuickAction::Translate){event(R"({"type":"tts","state":"start"})");assert(!audio.playback);}
        auto pending=ChatTask::Instance().Read(0,0);assert(pending.ok&&!pending.text.empty());
        if(action!=QuickAction::Translate)event(R"({"type":"tts","state":"start"})");
        event(R"({"type":"tts","state":"sentence_start","text":"处理结果"})");
        assert(audio.playback);
        event(R"({"type":"tts","state":"stop"})");
        assert(conversation.Snapshot().state==TurnState::Done && conversation.Snapshot().answer=="处理结果");
        assert(!ChatTask::Instance().ReadAll());
    }
    client.Abort();connected();fake_us+=12000000;conversation.Restore("原文不可丢","旧回答");
    assert(client.RunQuickAction(QuickAction::Translate));client.SendPendingAction();
    event(R"({"type":"tts","state":"sentence_start","text":"没有读取工具的问候"})");
    event(R"({"type":"tts","state":"stop"})");
    assert(conversation.Snapshot().state==TurnState::Error && conversation.Snapshot().transcript=="原文不可丢");
    assert(conversation.Snapshot().answer.empty());
    client.Abort();connected();fake_us+=12000000;conversation.Clear();
    assert(client.SubmitText("你好，今天怎么安排？"));client.SendPendingAction();
    auto typed=ChatTask::Instance().Read(0,0);assert(typed.ok && typed.text.find("今天怎么安排")!=std::string::npos);
    event(R"({"type":"alert","message":"服务拒绝了请求"})");
    assert(conversation.Snapshot().message=="服务拒绝了请求" && conversation.Snapshot().transcript=="你好，今天怎么安排？");
    assert(chat::captured_statuses.end()!=std::find(chat::captured_statuses.begin(),chat::captured_statuses.end(),"complete"));
    // Full bounded strings never end inside a multibyte codepoint.
    conversation.Begin();
    std::string long_text;
    for (int i=0;i<3000;++i) long_text += "麟";
    conversation.SetTranscript(long_text.c_str());
    conversation.ReceiveAnswer(long_text.c_str(),true);
    auto bounded=conversation.Snapshot();
    assert(bounded.transcript.size() <= Conversation::kTranscriptBytes);
    assert(bounded.answer.size() <= Conversation::kAnswerBytes);
    assert(bounded.transcript.size()%3==0 && bounded.answer.size()%3==0 && bounded.truncated);
    const auto turn=bounded.turn;
    conversation.Begin(); conversation.MarkSaved(turn,bounded.content_revision,true);
    assert(!conversation.Snapshot().saved);
    conversation.SetTranscript("已保存的胶囊原文");
    conversation.ReceiveAnswer("完整回答",true);
    auto saved=conversation.Snapshot();
    assert(SaveLastCapsule(saved));
    conversation.Clear();
    assert(LoadLastCapsule());
    assert(conversation.Snapshot().transcript==saved.transcript);
    assert(conversation.Snapshot().answer==saved.answer);
    fail_commit=true;
    conversation.SetTranscript("未提交的下一条");
    client.SaveCapsule(); client.SavePendingCapsule();
    assert(!conversation.Snapshot().saved && conversation.Snapshot().save_failed);
    fail_commit=false; fail_write=true;
    client.SaveCapsule(); client.SavePendingCapsule();
    assert(!conversation.Snapshot().saved && conversation.Snapshot().save_failed);
    fail_write=false;
    assert(LoadLastCapsule());
    assert(conversation.Snapshot().transcript==saved.transcript);
    committed_capsule=R"({"schema":99,"text":"future","answer":""})";
    committed_capsule.push_back('\0'); assert(!LoadLastCapsule());
    committed_capsule=R"({"text":"legacy","answer":"old format"})";
    committed_capsule.push_back('\0'); assert(LoadLastCapsule());
    assert(conversation.Snapshot().saved && conversation.Snapshot().answer=="old format");
    committed_capsule="broken";
    assert(!LoadLastCapsule());
    std::puts("AI contract OK: versioned persistence/final autosave/failed commit/legacy+corrupt blob, callback epochs/session isolation/release races, offline/failed start, prompt JSON, event text, cancel and UTF-8 bounds");
}
'''
    compiler = shutil.which('c++')
    cjson = ROOT / 'managed_components/espressif__cjson/cJSON'
    with tempfile.TemporaryDirectory(prefix='miaoink-ai-test-') as tmp:
        tmp = Path(tmp)
        (tmp/'test.cc').write_text(code)
        (tmp/'nvs.h').write_text("""#pragma once
#include <cstddef>
using nvs_handle_t = unsigned;
#define ESP_OK 0
#define NVS_READWRITE 1
#define NVS_READONLY 0
int nvs_open(const char*, int, nvs_handle_t*);
void nvs_close(nvs_handle_t);
int nvs_set_blob(nvs_handle_t,const char*,const void*,size_t);
int nvs_get_blob(nvs_handle_t,const char*,void*,size_t*);
int nvs_commit(nvs_handle_t);
""")
        subprocess.run([compiler, '-std=c++17', '-O1', '-Wno-deprecated-declarations', '-I', str(tmp), '-I', str(ROOT/'main'), '-I', str(ROOT/'tools'), '-I', str(cjson),
                        str(tmp/'test.cc'), str(ROOT/'main/xiaozhi/conversation.cc'),
                        str(ROOT/'main/xiaozhi/capsule_store.cc'),
                        str(ROOT/'main/dashboard/dashboard_data.cc'), str(cjson/'cJSON.c'),
                        '-o', str(tmp/'test')], check=True)
        subprocess.run([str(tmp/'test')], check=True)

if __name__ == '__main__':
    main()
