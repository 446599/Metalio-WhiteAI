#pragma once
#include "audio/recorder_state.h"
#include "audio/stack_watermark.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace xiaozhi {

// Opus audio path between the board codec and the Xiaozhi WebSocket.
//
// The transport never touches PCM: it hands compressed packets to this session,
// which owns both board-facing tasks.  Codec handles are opened lazily and
// released when their direction goes idle, so a quiet dashboard keeps no Opus
// heap and no I2S work.  Buffers, queue depth and stacks are fixed so a slow
// server can drop audio but never grow memory.
struct AudioSessionStats {
    uint32_t frames_sent = 0;
    uint32_t send_errors = 0;
    uint32_t packets_received = 0;
    uint32_t packets_dropped = 0;
    uint32_t packets_decoded = 0;
    uint32_t encode_errors = 0;
    uint32_t decode_errors = 0;
    uint32_t input_frames = 0;
    uint32_t input_failures = 0;
    // Frames the uplink gate suppressed as non-speech, and how often speech
    // opened it.  Both are the honest measure of whether the gate is helping.
    uint32_t frames_gated = 0;
    uint32_t gate_opens = 0;
    int input_peak = 0;
    // Uplink runs at the microphone rate; the server's own rate only applies
    // to the TTS stream the device decodes and resamples back to the speaker.
    int uplink_rate = 16000;
    int downlink_rate = 24000;
    int output_rate = 16000;
    int frame_duration_ms = 60;
    // Binary framing on the wire: 1 = raw Opus, 2/3 = metadata header.
    int protocol_version = 1;
    uint32_t capture_stack_free = 0;
    uint32_t playback_stack_free = 0;
    // Task-lifetime low-water marks observed after TTS processing, not an
    // isolated measurement of Opus. A zero sample count means unmeasured.
    uint32_t playback_tts_stack_free = 0, playback_tts_stack_samples = 0;
    uint32_t playback_create_failures = 0, playback_stack_bytes = 0;
    uint32_t internal_largest_free = 0;
    uint32_t internal_heap_free = 0;
    uint32_t psram_free = 0;
    bool capturing = false;
    bool playback_open = false;
    bool encoder_open = false;
    bool decoder_open = false;
    // True once a server hello was processed by the audio session.  Without it
    // the rates above are just defaults and prove nothing about the link; the
    // session id in the transport stats is the other half of that evidence.
    bool params_seen = false;
    bool reminder_tone = false;
    uint32_t reminder_frames = 0;
    uint32_t reminder_errors = 0;
    // Transport view, filled in by the client when it prints the stats.
    bool transport_connected = false;
    bool session_ready = false;
};

// One lazily opened Opus encoder plus the buffers that belong to it.  Both
// structures are owned by a single task at a time.
struct OpusEncoderState {
    void* handle = nullptr;
    int sample_rate = 0;
    int frame_ms = 0;
    int frame_samples = 0;
    std::vector<uint8_t> pcm;
    std::vector<uint8_t> out;
};

struct OpusDecoderState {
    void* handle = nullptr;
    int sample_rate = 0;
    int frame_ms = 0;
    int output_rate = 0;
    std::vector<uint8_t> buffer;
};

// Streaming linear resampler used to fold the server's TTS rate onto the fixed
// speaker clock.  The fractional phase carries across frames so consecutive
// packets do not click at their boundaries.
struct LinearResampler {
    int input_rate = 0;
    int output_rate = 0;
    uint32_t step_q16 = 0;
    uint32_t pos_q16 = 0;
    int16_t previous = 0;
    bool primed = false;
    std::vector<int16_t> out;

    void Configure(int from_rate, int to_rate);
    // Resamples one frame; the result stays valid until the next call.
    int Process(const int16_t* input, int samples);
};

class AudioSession final {
public:
    // Installed by the transport; must be safe to call from the capture task
    // and must not block for longer than one audio frame.
    using Sender = std::function<bool(const void* data, size_t length)>;

    static AudioSession& GetInstance();

    // Local, offline clip; retained in PSRAM until replaced or power is lost.
    bool StartRecorder(bool playback = false);
    void RestoreRecorder();
    void StopRecorder();
    audio::RecorderSnapshot RecorderState() const;

    AudioSession(const AudioSession&) = delete;
    AudioSession& operator=(const AudioSession&) = delete;

    void SetSender(Sender sender);
    // Applies the audio parameters from the server hello.  Only the downlink
    // decoder follows the server rate: the microphone stays on the board clock
    // at the rate declared in hello, exactly like the reference client.
    void SetParams(int server_sample_rate, int frame_duration_ms);
    // 1 = raw Opus (protocol default), 2/3 = metadata header on every frame.
    void SetProtocolVersion(int version) { protocol_version_.store(ClampVersion(version)); }
    int ProtocolVersion() const { return protocol_version_.load(); }

    // Listen window: capture runs while requested, the encoder follows it.
    bool StartCapture();
    void StopCapture();
    bool capturing() const { return capture_requested_.load(); }

    // TTS window: packets are only queued while playback is open.  A packet
    // arriving first opens the window so a chatty server is never muted.
    void OpenPlayback();
    void EndPlayback();
    void OnServerPacket(const uint8_t* data, size_t length);

    // Drops queued audio and stops both directions; safe to call repeatedly
    // from any task when the transport goes away.
    void Reset();
    // Same playback task as TTS: only one writer owns the speaker. Recording
    // and server audio temporarily suppress the local chime.
    bool SetReminderTone(bool enabled);

    AudioSessionStats Stats() const;

    // Hardware bring-up path: capture and encode real microphone frames, then
    // synthesise a tone through the same encoder/decoder pair and play it.
    // Runs on a dedicated 48 KiB internal stack task and refuses to start while a
    // session window is active.
    bool SelfTest(std::string* detail);
    // Exercises the exact server downlink path locally: a tone is encoded at
    // the negotiated server rate, wrapped in the version 3 header, handed to
    // OnServerPacket and then decoded, resampled and played by the playback
    // task.  Only the network hop is missing.
    bool DownlinkLoopTest(std::string* detail);

private:
    struct Packet {
        uint16_t length = 0;
        uint8_t data[512] = {};
    };

    struct SelfTestJob {
        AudioSession* session = nullptr;
        SemaphoreHandle_t done = nullptr;
        bool ok = false;
        bool downlink_loop = false;
        char detail[224] = {};
    };

    AudioSession();
    ~AudioSession();

    static void CaptureTaskEntry(void* arg);
    static void PlaybackTaskEntry(void* arg);
    static void SelfTestTaskEntry(void* arg);
    void CaptureLoop();
    void RecorderLoop();
    void PlaybackLoop();
    void RecordPlaybackStack(bool tts);
    bool SelfTestBody(std::string* detail);
    bool DownlinkLoopTestBody(std::string* detail);
    bool RunAudioTestTask(bool downlink_loop, std::string* detail);
    bool EnsurePlaybackTask();
    bool EnsureCaptureTask();

    bool EnsureEncoder();
    void CloseEncoder();
    bool EnsureDecoder();
    void CloseDecoder();
    bool SendOpusFrame(const uint8_t* payload, size_t length);
    void EnsureQueue();
    void PublishStatus(const char* status);
    int UplinkRate() const;
    int DownlinkRate() const;
    int OutputRate() const;
    int TargetFrameMs() const;
    int TargetFrameSamples() const;
    static int ClampVersion(int version);

    mutable std::mutex mutex_;
    Sender sender_;
    int downlink_rate_ = 24000;
    int frame_duration_ms_ = 60;
    LinearResampler resampler_;

    std::atomic<bool> capture_requested_{false};
    std::atomic<audio::RecorderMode> recorder_mode_{audio::RecorderMode::Idle};
    std::atomic<bool> recorder_stop_{false}, recorder_failed_{false};
    std::atomic<bool> recorder_saved_{false}, recorder_loaded_{false};
    std::atomic<uint32_t> recorder_samples_{0}, recorder_revision_{0};
    int16_t* recorder_pcm_ = nullptr;  // owned only by the resident capture task
    std::atomic<bool> playback_open_{false};
    std::atomic<bool> self_test_active_{false};
    std::atomic<bool> params_seen_{false};
    std::atomic<bool> reminder_requested_{false};
    std::atomic<bool> reminder_playing_{false};
    std::atomic<uint32_t> reminder_frames_{0}, reminder_errors_{0};
    std::atomic<int> protocol_version_{1};

    std::atomic<uint32_t> frames_sent_{0};
    std::atomic<uint32_t> send_errors_{0};
    std::atomic<uint32_t> packets_received_{0};
    std::atomic<uint32_t> packets_dropped_{0};
    std::atomic<uint32_t> packets_decoded_{0};
    std::atomic<uint32_t> encode_errors_{0};
    std::atomic<uint32_t> decode_errors_{0};
    std::atomic<uint32_t> input_frames_{0};
    std::atomic<uint32_t> input_failures_{0};
    std::atomic<uint32_t> frames_gated_{0};
    std::atomic<uint32_t> gate_opens_{0};
    std::atomic<int> input_peak_{0};
    std::atomic<uint32_t> capture_stack_free_{0};
    audio::StackWatermark playback_stack_;
    audio::StackWatermark playback_tts_stack_;
    std::atomic<uint32_t> playback_tts_stack_samples_{0};
    std::atomic<uint32_t> playback_create_failures_{0};

    TaskHandle_t capture_task_ = nullptr;
    TaskHandle_t playback_task_ = nullptr;
    QueueHandle_t rx_queue_ = nullptr;
    // The transport calls OnServerPacket from its 4 KiB receive task, so the
    // staging packet lives here instead of on that stack.
    Packet rx_scratch_;

    OpusEncoderState encoder_;  // capture task
    OpusDecoderState decoder_;  // playback task

    // Scratch for the version 3 binary header carried by every audio frame.
    std::vector<uint8_t> tx_frame_;
};

}  // namespace xiaozhi
