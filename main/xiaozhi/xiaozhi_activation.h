#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include <esp_err.h>

namespace xiaozhi {

// Device activation against the Xiaozhi OTA endpoint.
//
// A Xiaozhi deployment will not answer speech until the device is bound to an
// account, and the binding code only arrives from that HTTP endpoint: the
// WebSocket layer never carries it.  This module performs the documented
// exchange and hands the rest of the firmware everything it learned:
//
//   POST <ota_url>          board system info  -> activation code + websocket
//                                                url/token + server time
//   POST <ota_url>/activate challenge payload  -> 202 while waiting, 200 bound
//
// The websocket credentials are persisted into the `xiaozhi` NVS namespace, so
// the transport picks them up without any hardcoded endpoint.
class Activation final {
public:
    struct State {
        bool checked = false;      // the OTA endpoint was reached
        bool has_code = false;     // the server handed out a binding code
        bool has_challenge = false;
        bool bound = false;        // /activate answered 200
        bool has_server_time = false;
        char code[24]{};
        char message[96]{};
        int timeout_ms = 30000;
    };

    static Activation& GetInstance();

    Activation(const Activation&) = delete;
    Activation& operator=(const Activation&) = delete;

    // Blocking; call from a task that already has a working network.
    bool FetchConfig();
    // One poll of the activation endpoint.  ESP_ERR_TIMEOUT means "still
    // waiting for the user to enter the code".
    esp_err_t Poll();
    // False when the endpoint rejects the polling form outright (this
    // deployment answers 400 without a burned serial number), in which case
    // binding is detected by re-fetching the config instead.
    bool ActivateSupported() const;
    // Milliseconds since the last /activate poll, and the configured interval.
    bool PollDue() const;
    void MarkPolled();

    State Snapshot() const;
    std::string OtaUrl() const;
    // Drops the cached state so the next boot re-checks the server.
    void ClearBound();

private:
    Activation() = default;

    bool StoreWebsocketConfig(const char* url, const char* token);
    esp_err_t Activate();
    // `{}` when the deployment has no serial number to attest with, otherwise
    // the documented challenge envelope.
    std::string BuildChallengeJson(const std::string& challenge) const;

    mutable std::mutex mutex_;
    State state_;
    std::string challenge_;
    int64_t last_poll_ms_ = 0;
    bool first_poll_done_ = false;
    bool activate_supported_ = true;
};

// Copies the activation state into the dashboard AI card and status line so the
// binding code is visible on the panel without any host tooling.
void PublishActivationState(const Activation::State& state);

}  // namespace xiaozhi
