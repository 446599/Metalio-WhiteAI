#pragma once

#include "conversation.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class WebSocket;

namespace xiaozhi {

// Small-footprint Xiaozhi v1 transport.  The protocol, session and server
// event handling are kept independent from the display; audio codecs can be
// attached later without changing the dashboard or WebSocket lifecycle.
class Client final {
public:
    static Client& GetInstance();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    void Start();
    bool IsConnected() const { return connected_.load(); }
    // True once the server hello supplied a session id, which is what the
    // listen/abort envelopes need before they can be sent at all.
    bool IsSessionReady() const;
    bool Enabled() const { return enabled_; }
    // True while a listen window is pending or actually capturing audio.
    bool IsListening() const;

    // These methods enqueue protocol actions for the owning task.  They are
    // safe to call from a button/touch callback and never block on TLS.
    bool ListenStart();
    bool ListenStop();
    bool Abort();
    bool RunQuickAction(QuickAction action);
    void SaveCapsule();
    void RestoreCapsule();

private:
    Client() = default;

    static void TaskEntry(void* arg);
    void Run();
    void LoadConfig();
    bool ConnectOnce();
    bool NetworkReady() const;
    // Runs the OTA/activation exchange before the first WebSocket attempt and
    // keeps polling while the server waits for the binding code to be entered.
    void ServiceActivation();
    void HandleData(uint32_t epoch, const char* data, size_t length, bool binary);
    void HandleDisconnect(uint32_t epoch);
    void PublishStatus(const char* status);
    void SendPendingAction();
    bool SendText(const std::string& message);
    bool SendAudio(const void* data, size_t length);
    void ReleaseTransport();
    void SavePendingCapsule();
    void SendPendingMcp();
    // Closes the microphone side of a manual window without telling the server
    // to stop: used when the utterance is already on its way.
    void EndListenWindowLocked();
    void InvalidateTransportLocked();

    std::atomic<bool> started_{false};
    std::atomic<bool> connected_{false};
    std::atomic<uint8_t> pending_action_{0};
    // Protect intent/check/start against a release racing the network worker.
    // TLS and display operations never run under this mutex.
    std::mutex intent_mutex_;
    uint32_t intent_generation_ = 0;
    // The intent mutex serializes callbacks and UI intent. Lock order is
    // intent -> session/audio; never hold it across a socket send or close.
    uint32_t transport_epoch_ = 0;
    struct McpRequest { uint32_t epoch; std::string session; std::string payload; };
    std::deque<McpRequest> mcp_requests_;
    bool turn_in_flight_ = false;
    bool playback_receiving_ = false;
    std::string pending_prompt_;
    std::atomic<bool> accept_response_{false};
    std::atomic<bool> followup_turn_{false};
    std::atomic<bool> capture_started_{false};
    std::mutex capsule_mutex_;
    ConversationSnapshot pending_capsule_;
    std::atomic<bool> save_requested_{false};
    std::atomic<bool> restore_requested_{false};
    std::atomic<int64_t> listen_started_ms_{0};
    std::atomic<int64_t> response_started_ms_{0};
    // A release or disconnect clears intent. Reconnect never opens the mic.
    std::atomic<bool> listen_requested_{false};
    std::atomic<uint32_t> json_events_logged_{0};
    TaskHandle_t task_handle_ = nullptr;
    // Guards websocket_ against the capture task, which sends audio frames from
    // its own task while the client task owns connect/reconnect lifetimes.
    std::mutex ws_mutex_;
    std::unique_ptr<WebSocket> websocket_;
    mutable std::mutex session_mutex_;
    std::string session_id_;
    std::string url_;
    std::string token_;
    // Binary protocol version: 1 = raw Opus (the documented default), 3 = the
    // version 3 header.  Overridable through NVS `xiaozhi/version`.
    int audio_version_ = 1;
    bool enabled_ = true;
    bool connect_attempted_ = false;
    bool activation_done_ = false;
    int64_t last_connect_attempt_ms_ = 0;
    std::atomic<int64_t> last_ping_ms_{0};
};

}  // namespace xiaozhi
