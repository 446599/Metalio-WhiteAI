#include "xiaozhi/xiaozhi_audio.h"

#include "dashboard/dashboard_data.h"
#include "hal/hal.h"
#include "audio/reminder_chime.h"
#include "audio/recorder_file.h"

#include <esp_audio_dec.h>
#include <esp_audio_enc.h>
#include <esp_audio_types.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_opus_dec.h>
#include <esp_opus_enc.h>
#include <esp_timer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <freertos/semphr.h>

namespace xiaozhi {
namespace {

constexpr const char* kTag = "XiaozhiAudio";

// 24 kbps mono keeps one 60 ms Opus frame near 180 bytes.  Eight queued packets
// cover roughly half a second of speech; beyond that the oldest packet is
// dropped so a burst can never grow the queue or add unbounded latency.
constexpr int kBitrateBps = 24000;
constexpr UBaseType_t kRxQueueDepth = 8;
constexpr size_t kMaxPacketBytes = 512;
constexpr size_t kDecodeBufferBytes = 8192;
// Xiaozhi protocol version 3 frames every binary audio packet with
// {type, reserved, payload_size_be16} in front of the Opus payload.
constexpr size_t kAudioHeaderBytes = 4;
constexpr uint8_t kAudioPacketTypeOpus = 0;
// BinaryProtocol2 is {version_be16, type_be16, reserved_u32, timestamp_u32,
// payload_size_be32}; kept for deployments that select protocol version 2.
constexpr size_t kAudioHeaderV2Bytes = 16;
// libopus keeps large working sets on the caller's stack, so these sizes come
// from measured high-water marks rather than guesses.
//
// The capture task peaks at 21940 bytes (XIAOZHI_STATS cap_stack), so the
// original 40 KiB was oversized and held internal RAM the playback task could
// not get: at TTS time only 16 KiB stayed contiguous, the 24 KiB playback
// stack failed to allocate, and the speaker stayed silent. 28 KiB keeps a
// 6.7 KiB margin over the observed peak while leaving room for playback.
constexpr int kCaptureStackBytes = 28672;
constexpr int kPlaybackStackBytes = 24576;
constexpr UBaseType_t kAudioTaskPriority = 4;

// These stacks must stay in internal RAM: the audio path reaches NVS
// (AudioCodec::Start reads the stored volume) and the Opus codec, and any code
// that runs with the flash cache disabled asserts when the current task stack
// lives in PSRAM. Freeing internal RAM is therefore the only way to make room.
BaseType_t CreateAudioTask(TaskFunction_t entry, const char* name, uint32_t stack_bytes,
                           void* arg, UBaseType_t priority, TaskHandle_t* handle)
{
    return xTaskCreatePinnedToCore(entry, name, stack_bytes, arg, priority, handle, 1);
}
constexpr uint32_t kIdleWaitMs = 200;
// Ten empty 500 ms queue reads: a TTS window that produced no audio at all.
constexpr uint32_t kPlaybackIdleCloseTicks = 10;
// The reference client only streams frames the voice-activity detector passes,
// while a raw microphone feeds silence forever.  Flooding the server with
// silence made it close the session and emit an empty stt event, so the uplink
// is gated on an adaptive noise floor: the gate opens above max(min, 3*floor)
// and stays open through a short hangover so word tails are never clipped.
constexpr int kSpeechGateMinPeak = 600;
constexpr uint32_t kSpeechGateHangoverFrames = 8;  // ~480 ms at 60 ms frames
constexpr uint32_t kLevelReportFrames = 80;        // ~5 s into a listen window
constexpr int kReadAttemptsPerFrame = 8;
constexpr uint32_t kNotReadyLimit = 2;
constexpr uint32_t kNoInputLimit = 3;
constexpr int kSelfTestMs = 2000;
constexpr int kToneMs = 1000;
constexpr int kSelfTestStackBytes = 49152;
constexpr uint32_t kSelfTestTimeoutMs = 60000;
constexpr double kPi = 3.14159265358979323846;

struct FrameSpec {
    int ms;
    esp_opus_enc_frame_duration_t enc;
    esp_opus_dec_frame_duration_t dec;
};

// Opus accepts only these frame durations; anything else falls back to 60 ms,
// which is what the Xiaozhi hello advertises.
constexpr FrameSpec kFrameSpecs[] = {
    {5, ESP_OPUS_ENC_FRAME_DURATION_5_MS, ESP_OPUS_DEC_FRAME_DURATION_5_MS},
    {10, ESP_OPUS_ENC_FRAME_DURATION_10_MS, ESP_OPUS_DEC_FRAME_DURATION_10_MS},
    {20, ESP_OPUS_ENC_FRAME_DURATION_20_MS, ESP_OPUS_DEC_FRAME_DURATION_20_MS},
    {40, ESP_OPUS_ENC_FRAME_DURATION_40_MS, ESP_OPUS_DEC_FRAME_DURATION_40_MS},
    {60, ESP_OPUS_ENC_FRAME_DURATION_60_MS, ESP_OPUS_DEC_FRAME_DURATION_60_MS},
    {80, ESP_OPUS_ENC_FRAME_DURATION_80_MS, ESP_OPUS_DEC_FRAME_DURATION_80_MS},
    {100, ESP_OPUS_ENC_FRAME_DURATION_100_MS, ESP_OPUS_DEC_FRAME_DURATION_100_MS},
    {120, ESP_OPUS_ENC_FRAME_DURATION_120_MS, ESP_OPUS_DEC_FRAME_DURATION_120_MS},
};

constexpr size_t kDefaultFrameSpec = 4;  // 60 ms

FrameSpec ResolveFrameSpec(int requested_ms) {
    for (const FrameSpec& spec : kFrameSpecs) {
        if (spec.ms == requested_ms) {
            return spec;
        }
    }
    return kFrameSpecs[kDefaultFrameSpec];
}

bool ValidSampleRate(int rate) {
    return rate == 8000 || rate == 12000 || rate == 16000 || rate == 24000 || rate == 48000;
}

void EncoderClose(OpusEncoderState* state) {
    if (state == nullptr) {
        return;
    }
    if (state->handle != nullptr) {
        esp_opus_enc_close(state->handle);
        state->handle = nullptr;
    }
    state->sample_rate = 0;
    state->frame_ms = 0;
    state->frame_samples = 0;
    // Release the codec heap as well; an idle dashboard should not carry Opus.
    std::vector<uint8_t>().swap(state->pcm);
    std::vector<uint8_t>().swap(state->out);
}

bool EncoderOpen(OpusEncoderState* state, int sample_rate, int frame_ms) {
    EncoderClose(state);
    const FrameSpec spec = ResolveFrameSpec(frame_ms);

    esp_opus_enc_config_t config = ESP_OPUS_ENC_CONFIG_DEFAULT();
    config.sample_rate = sample_rate;
    config.channel = ESP_AUDIO_MONO;
    config.bits_per_sample = ESP_AUDIO_BIT16;
    config.bitrate = kBitrateBps;
    config.frame_duration = spec.enc;
    config.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    config.complexity = 0;
    config.enable_fec = false;
    config.enable_dtx = false;
    config.enable_vbr = false;

    void* handle = nullptr;
    const esp_audio_err_t opened = esp_opus_enc_open(&config, sizeof(config), &handle);
    if (opened != ESP_AUDIO_ERR_OK || handle == nullptr) {
        ESP_LOGE(kTag, "opus encoder open failed: %d", static_cast<int>(opened));
        return false;
    }

    int input_bytes = 0;
    int output_bytes = 0;
    const esp_audio_err_t sized = esp_opus_enc_get_frame_size(handle, &input_bytes, &output_bytes);
    if (sized != ESP_AUDIO_ERR_OK || input_bytes <= 0 || output_bytes <= 0) {
        ESP_LOGE(kTag, "opus encoder sizing failed: %d", static_cast<int>(sized));
        esp_opus_enc_close(handle);
        return false;
    }

    state->handle = handle;
    state->sample_rate = sample_rate;
    state->frame_ms = spec.ms;
    state->frame_samples = input_bytes / static_cast<int>(sizeof(int16_t));
    state->pcm.assign(static_cast<size_t>(input_bytes), 0);
    state->out.assign(static_cast<size_t>(output_bytes), 0);
    ESP_LOGI(kTag, "opus encoder ready: %d Hz %d ms frame, %d samples, %d byte budget",
             sample_rate, spec.ms, state->frame_samples, output_bytes);
    return true;
}

void DecoderClose(OpusDecoderState* state) {
    if (state == nullptr) {
        return;
    }
    if (state->handle != nullptr) {
        esp_opus_dec_close(state->handle);
        state->handle = nullptr;
    }
    state->sample_rate = 0;
    state->frame_ms = 0;
    state->output_rate = 0;
    std::vector<uint8_t>().swap(state->buffer);
}

bool DecoderOpen(OpusDecoderState* state, int sample_rate, int frame_ms, int output_rate) {
    DecoderClose(state);
    const FrameSpec spec = ResolveFrameSpec(frame_ms);

    esp_opus_dec_cfg_t config = ESP_OPUS_DEC_CONFIG_DEFAULT();
    config.sample_rate = static_cast<uint32_t>(sample_rate);
    config.channel = ESP_AUDIO_MONO;
    config.frame_duration = spec.dec;
    config.self_delimited = false;

    void* handle = nullptr;
    const esp_audio_err_t opened = esp_opus_dec_open(&config, sizeof(config), &handle);
    if (opened != ESP_AUDIO_ERR_OK || handle == nullptr) {
        ESP_LOGE(kTag, "opus decoder open failed: %d", static_cast<int>(opened));
        return false;
    }

    state->handle = handle;
    state->sample_rate = sample_rate;
    state->frame_ms = spec.ms;
    state->output_rate = output_rate;
    state->buffer.assign(kDecodeBufferBytes, 0);
    ESP_LOGI(kTag, "opus decoder ready: %d Hz %d ms frame, speaker %d Hz", sample_rate, spec.ms,
             output_rate);
    return true;
}

int PeakAmplitude(const uint8_t* pcm, int samples) {
    const int16_t* values = reinterpret_cast<const int16_t*>(pcm);
    int peak = 0;
    for (int i = 0; i < samples; ++i) {
        const int value = values[i] < 0 ? -values[i] : values[i];
        if (value > peak) {
            peak = value;
        }
    }
    return peak;
}

// Microphone paths on this board carry a DC offset that is much larger than
// quiet-room noise, so the gate has to look at the AC component only: the peak
// is measured against a slowly tracked mean instead of against zero.
int PeakAmplitudeAc(const int16_t* pcm, int samples, int32_t* dc) {
    int64_t sum = 0;
    for (int i = 0; i < samples; ++i) {
        sum += pcm[i];
    }
    const int32_t mean = static_cast<int32_t>(sum / samples);
    const int32_t estimate = dc == nullptr ? 0 : *dc;
    int peak = 0;
    for (int i = 0; i < samples; ++i) {
        const int value = static_cast<int>(pcm[i]) - static_cast<int>(estimate);
        const int magnitude = value < 0 ? -value : value;
        if (magnitude > peak) {
            peak = magnitude;
        }
    }
    if (dc != nullptr) {
        *dc = (estimate * 63 + mean) / 64;
    }
    return peak;
}

// Board rates are fixed by the I2S clock; the server only negotiates the TTS
// rate it encodes with.
int BoardInputRate() {
    const int rate = GetHAL().AudioInputSampleRate();
    return rate > 0 ? rate : 16000;
}

int BoardOutputRate() {
    const int rate = GetHAL().AudioOutputSampleRate();
    return rate > 0 ? rate : 16000;
}

}  // namespace

void LinearResampler::Configure(int from_rate, int to_rate) {
    input_rate = from_rate;
    output_rate = to_rate;
    step_q16 = from_rate > 0 ? static_cast<uint32_t>((static_cast<uint64_t>(from_rate) << 16) /
                                                    static_cast<uint32_t>(to_rate))
                             : 0;
    pos_q16 = 0;
    primed = false;
    previous = 0;
    out.clear();
    if (to_rate > 0 && from_rate > 0) {
        // One frame of input can never need more than this many output samples.
        out.reserve(static_cast<size_t>(from_rate / 1000 * 120) * to_rate / from_rate + 64);
    }
}

int LinearResampler::Process(const int16_t* input, int samples) {
    if (input == nullptr || samples <= 0 || step_q16 == 0 || input_rate == output_rate) {
        return 0;
    }
    if (!primed) {
        previous = input[0];
        primed = true;
    }

    // The virtual sample immediately before this frame is the previous frame's
    // last sample, which is what keeps the interpolation continuous across
    // packet boundaries.
    const uint32_t frame_end_q16 = static_cast<uint32_t>(samples) << 16;
    const size_t needed = static_cast<size_t>(static_cast<uint64_t>(samples) * output_rate /
                                              input_rate) +
                          2;
    if (out.size() < needed) {
        out.resize(needed);
    }

    int produced = 0;
    uint32_t pos = pos_q16;
    while (pos < frame_end_q16 && static_cast<size_t>(produced) < out.size()) {
        const int index = static_cast<int>(pos >> 16);
        const int32_t fraction = static_cast<int32_t>(pos & 0xffffU);
        const int16_t left = index == 0 ? previous : input[index - 1];
        const int16_t right = input[index];
        const int32_t delta = static_cast<int32_t>(right) - static_cast<int32_t>(left);
        out[produced++] = static_cast<int16_t>(left + ((delta * fraction) >> 16));
        pos += step_q16;
    }

    previous = input[samples - 1];
    pos_q16 = pos >= frame_end_q16 ? pos - frame_end_q16 : 0;
    return produced;
}

AudioSession& AudioSession::GetInstance() {
    static AudioSession instance;
    return instance;
}

AudioSession::AudioSession() {
    EnsureQueue();
    // The capture task already stays resident after the first conversation.
    // Reserve its contiguous 40 KiB before TLS and ringtone playback fragment
    // internal RAM. It stays idle: no microphone or encoder is opened here.
    (void)EnsureCaptureTask();
}

AudioSession::~AudioSession() = default;

void AudioSession::EnsureQueue() {
    if (rx_queue_ != nullptr) {
        return;
    }
    rx_queue_ = xQueueCreate(kRxQueueDepth, sizeof(Packet));
    if (rx_queue_ == nullptr) {
        ESP_LOGE(kTag, "audio packet queue allocation failed");
    }
}

void AudioSession::SetSender(Sender sender) {
    std::lock_guard<std::mutex> lock(mutex_);
    sender_ = std::move(sender);
}

void AudioSession::SetParams(int server_sample_rate, int frame_duration_ms) {
    if (!ValidSampleRate(server_sample_rate)) {
        server_sample_rate = 24000;
    }
    const FrameSpec spec = ResolveFrameSpec(frame_duration_ms);
    std::lock_guard<std::mutex> lock(mutex_);
    if (server_sample_rate == downlink_rate_ && spec.ms == frame_duration_ms_) {
        // Still record that hello carried parameters; otherwise the defaults
        // above would look like an unnegotiated link.
        params_seen_.store(true);
        return;
    }
    ESP_LOGI(kTag, "server audio params: %d Hz downlink, %d ms frame; uplink stays %d Hz",
             server_sample_rate, spec.ms, BoardInputRate());
    downlink_rate_ = server_sample_rate;
    frame_duration_ms_ = spec.ms;
    params_seen_.store(true);
}

int AudioSession::UplinkRate() const { return BoardInputRate(); }

int AudioSession::DownlinkRate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return downlink_rate_;
}

int AudioSession::OutputRate() const { return BoardOutputRate(); }

int AudioSession::TargetFrameMs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frame_duration_ms_;
}

int AudioSession::TargetFrameSamples() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return UplinkRate() * frame_duration_ms_ / 1000;
}

bool AudioSession::StartCapture() {
    if (self_test_active_.load()) {
        ESP_LOGW(kTag, "capture refused while the audio self test owns the codec");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (recorder_mode_.load() != audio::RecorderMode::Idle) return false;
        EnsureQueue();
        if (!sender_) {
            ESP_LOGW(kTag, "capture requested before the transport was ready");
            return false;
        }
    }
    if (!EnsureCaptureTask()) return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (recorder_mode_.load() != audio::RecorderMode::Idle || self_test_active_.load()) return false;
        capture_requested_.store(true);
    }
    xTaskNotifyGive(capture_task_);
    return true;
}

bool AudioSession::EnsureCaptureTask() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (capture_task_ == nullptr) {
        if (CreateAudioTask(CaptureTaskEntry, "xz_capture", kCaptureStackBytes, this,
                            kAudioTaskPriority, &capture_task_) != pdPASS) {
            capture_task_ = nullptr;
            ESP_LOGE(kTag,
                     "capture task creation failed, internal free=%u largest=%u psram free=%u",
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
            return false;
        }
    }
    return true;
}

void AudioSession::StopCapture() {
    capture_requested_.store(false);
}

void AudioSession::OpenPlayback() {
    // The self test drives the same codec pair on its own task; leave it alone.
    if (self_test_active_.load()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (recorder_mode_.load() != audio::RecorderMode::Idle || self_test_active_.load()) return;
        playback_open_.store(true);
    }
    (void)EnsurePlaybackTask();
    PublishStatus("小智说话中");
}

bool AudioSession::SetReminderTone(bool enabled) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (enabled) StopRecorder();
        reminder_requested_.store(enabled);
    }
    if (enabled && !EnsurePlaybackTask()) {
        reminder_requested_.store(false);
        return false;
    }
    return true;
}

bool AudioSession::EnsurePlaybackTask() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        EnsureQueue();
        if (rx_queue_ == nullptr) {
            return false;
        }
        if (playback_task_ == nullptr) {
            if (CreateAudioTask(PlaybackTaskEntry, "xz_playback", kPlaybackStackBytes, this,
                                kAudioTaskPriority, &playback_task_) != pdPASS) {
                playback_task_ = nullptr;
                ESP_LOGE(kTag,
                         "playback task creation failed, internal free=%u largest=%u psram free=%u",
                         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                         static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
                return false;
            }
        }
    }
    return true;
}

void AudioSession::EndPlayback() {
    // The playback task drains what is already queued, then closes the decoder.
    playback_open_.store(false);
}

void AudioSession::OnServerPacket(const uint8_t* data, size_t length) {
    if (recorder_mode_.load() != audio::RecorderMode::Idle) {
        packets_dropped_.fetch_add(1);
        return;
    }
    if (data == nullptr || length == 0) {
        return;
    }
    packets_received_.fetch_add(1);
    if (rx_queue_ == nullptr || length == 0) {
        packets_dropped_.fetch_add(1);
        return;
    }

    // The first few frames are logged once so the framing actually used by a
    // deployment can be confirmed from the wire instead of assumed.
    if (packets_received_.load() <= 3) {
        ESP_LOGI(kTag, "rx frame len=%u head=%02x %02x %02x %02x %02x %02x",
                 static_cast<unsigned>(length), data[0], length > 1 ? data[1] : 0,
                 length > 2 ? data[2] : 0, length > 3 ? data[3] : 0,
                 length > 4 ? data[4] : 0, length > 5 ? data[5] : 0);
    }

    // Accept every documented layout and strip the header only when it is
    // self-consistent, so a server on protocol 1, 2 or 3 all work: version 1 is
    // a bare Opus payload, version 3 is {type, reserved, size_be16} and version
    // 2 is {version_be16, type_be16, reserved_u32, timestamp_u32, size_be32}.
    size_t payload_offset = 0;
    size_t payload_length = length;
    if (length > kAudioHeaderV2Bytes && data[0] == 0 && data[1] == 2) {
        const size_t declared = (static_cast<size_t>(data[12]) << 24) |
                                (static_cast<size_t>(data[13]) << 16) |
                                (static_cast<size_t>(data[14]) << 8) | data[15];
        if (declared > 0 && declared <= length - kAudioHeaderV2Bytes) {
            payload_offset = kAudioHeaderV2Bytes;
            payload_length = declared;
        }
    } else if (length > kAudioHeaderBytes && data[0] == kAudioPacketTypeOpus) {
        const size_t declared = (static_cast<size_t>(data[2]) << 8) | data[3];
        if (declared > 0 && declared <= length - kAudioHeaderBytes) {
            payload_offset = kAudioHeaderBytes;
            payload_length = declared;
        }
    }
    const uint8_t* payload = data + payload_offset;
    if (payload_length == 0 || payload_length > kMaxPacketBytes) {
        packets_dropped_.fetch_add(1);
        return;
    }
    if (!playback_open_.load()) {
        OpenPlayback();
    }

    // Only the transport task produces packets, so one staging buffer keeps
    // the 4 KiB receive stack free of a 514 byte local.
    bool dropped_oldest = false;
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (uxQueueSpacesAvailable(rx_queue_) == 0) {
            // Queue full: drop the oldest frame so live speech stays close to
            // real time instead of drifting further behind the server.
            dropped_oldest = xQueueReceive(rx_queue_, &rx_scratch_, 0) == pdTRUE;
        }
        rx_scratch_.length = static_cast<uint16_t>(payload_length);
        std::memcpy(rx_scratch_.data, payload, payload_length);
        queued = xQueueSend(rx_queue_, &rx_scratch_, 0) == pdTRUE;
    }
    if (!queued || dropped_oldest) {
        packets_dropped_.fetch_add(1);
    }
}

void AudioSession::Reset() {
    capture_requested_.store(false);
    playback_open_.store(false);
    if (rx_queue_ != nullptr) {
        xQueueReset(rx_queue_);
    }
    if (capture_task_ != nullptr) {
        xTaskNotifyGive(capture_task_);
    }
}

AudioSessionStats AudioSession::Stats() const {
    AudioSessionStats stats;
    stats.frames_sent = frames_sent_.load();
    stats.send_errors = send_errors_.load();
    stats.packets_received = packets_received_.load();
    stats.packets_dropped = packets_dropped_.load();
    stats.packets_decoded = packets_decoded_.load();
    stats.encode_errors = encode_errors_.load();
    stats.decode_errors = decode_errors_.load();
    stats.input_frames = input_frames_.load();
    stats.input_failures = input_failures_.load();
    stats.frames_gated = frames_gated_.load();
    stats.gate_opens = gate_opens_.load();
    stats.input_peak = input_peak_.load();
    stats.uplink_rate = UplinkRate();
    stats.downlink_rate = DownlinkRate();
    stats.output_rate = OutputRate();
    stats.frame_duration_ms = TargetFrameMs();
    stats.capture_stack_free = capture_stack_free_.load();
    stats.playback_stack_free = playback_stack_free_.load();
    stats.internal_heap_free = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    stats.psram_free = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    stats.capturing = capture_requested_.load();
    stats.playback_open = playback_open_.load();
    stats.encoder_open = encoder_.handle != nullptr;
    stats.decoder_open = decoder_.handle != nullptr;
    stats.params_seen = params_seen_.load();
    stats.reminder_tone = reminder_playing_.load();
    stats.reminder_frames = reminder_frames_.load();
    stats.reminder_errors = reminder_errors_.load();
    stats.protocol_version = ProtocolVersion();
    return stats;
}

bool AudioSession::SendOpusFrame(const uint8_t* payload, size_t length) {
    if (payload == nullptr || length == 0 || length > 0xffffU) {
        return false;
    }
    const int version = ProtocolVersion();
    size_t header = 0;
    if (version >= 3) {
        // Version 3: {type, reserved, payload_size_be16}.
        header = kAudioHeaderBytes;
        if (tx_frame_.size() < header + length) {
            tx_frame_.resize(header + length);
        }
        tx_frame_[0] = kAudioPacketTypeOpus;
        tx_frame_[1] = 0;
        tx_frame_[2] = static_cast<uint8_t>((length >> 8) & 0xffU);
        tx_frame_[3] = static_cast<uint8_t>(length & 0xffU);
    } else if (version == 2) {
        header = kAudioHeaderV2Bytes;
        if (tx_frame_.size() < header + length) {
            tx_frame_.resize(header + length);
        }
        tx_frame_[0] = 0;
        tx_frame_[1] = 2;
        tx_frame_[2] = 0;
        tx_frame_[3] = 0;
        tx_frame_[4] = tx_frame_[5] = tx_frame_[6] = tx_frame_[7] = 0;
        const uint32_t timestamp = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        tx_frame_[8] = static_cast<uint8_t>((timestamp >> 24) & 0xffU);
        tx_frame_[9] = static_cast<uint8_t>((timestamp >> 16) & 0xffU);
        tx_frame_[10] = static_cast<uint8_t>((timestamp >> 8) & 0xffU);
        tx_frame_[11] = static_cast<uint8_t>(timestamp & 0xffU);
        tx_frame_[12] = static_cast<uint8_t>((length >> 24) & 0xffU);
        tx_frame_[13] = static_cast<uint8_t>((length >> 16) & 0xffU);
        tx_frame_[14] = static_cast<uint8_t>((length >> 8) & 0xffU);
        tx_frame_[15] = static_cast<uint8_t>(length & 0xffU);
    } else {
        // Version 1: raw Opus, the WebSocket layer already tells the two apart.
        header = 0;
        if (tx_frame_.size() < length) {
            tx_frame_.resize(length);
        }
    }
    std::memcpy(tx_frame_.data() + header, payload, length);

    Sender sender;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sender = sender_;
    }
    if (!sender) {
        return false;
    }
    return sender(tx_frame_.data(), header + length);
}

int AudioSession::ClampVersion(int version) {
    return version == 2 || version == 3 ? version : 1;
}

void AudioSession::PublishStatus(const char* status) {
    dashboard::DashboardData::GetInstance().SetAiStatus(status);
}

bool AudioSession::EnsureEncoder() {
    const int rate = UplinkRate();
    const int frame_ms = TargetFrameMs();
    if (encoder_.handle != nullptr && encoder_.sample_rate == rate && encoder_.frame_ms == frame_ms) {
        return true;
    }
    return EncoderOpen(&encoder_, rate, frame_ms);
}

void AudioSession::CloseEncoder() {
    EncoderClose(&encoder_);
}

bool AudioSession::EnsureDecoder() {
    const int rate = DownlinkRate();
    const int frame_ms = TargetFrameMs();
    const int output_rate = OutputRate();
    if (decoder_.handle != nullptr && decoder_.sample_rate == rate &&
        decoder_.frame_ms == frame_ms && decoder_.output_rate == output_rate) {
        return true;
    }
    if (!DecoderOpen(&decoder_, rate, frame_ms, output_rate)) {
        return false;
    }
    if (rate != output_rate) {
        ESP_LOGI(kTag, "resampling TTS %d Hz to speaker %d Hz", rate, output_rate);
    }
    resampler_.Configure(rate, output_rate);
    return true;
}

void AudioSession::CloseDecoder() {
    DecoderClose(&decoder_);
}

void AudioSession::CaptureTaskEntry(void* arg) {
    static_cast<AudioSession*>(arg)->CaptureLoop();
    vTaskDelete(nullptr);
}

bool AudioSession::StartRecorder(bool playback) {
    if (!EnsureCaptureTask()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (recorder_mode_.load() != audio::RecorderMode::Idle || capture_requested_.load() ||
        playback_open_.load() || playback_task_ != nullptr || self_test_active_.load() ||
        reminder_requested_.load() || (playback && recorder_samples_.load() == 0)) return false;
    recorder_stop_.store(false);
    recorder_loaded_.store(true);
    recorder_failed_.store(false);
    recorder_mode_.store(playback ? audio::RecorderMode::Playing : audio::RecorderMode::Recording);
    ++recorder_revision_;
    xTaskNotifyGive(capture_task_);
    return true;
}

void AudioSession::StopRecorder() { recorder_stop_.store(true); }

void AudioSession::RestoreRecorder() {
    if (recorder_loaded_.load() || !EnsureCaptureTask()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (recorder_loaded_.load() || recorder_mode_.load()!=audio::RecorderMode::Idle ||
        capture_requested_.load() || playback_task_ || playback_open_.load() || self_test_active_.load()) return;
    recorder_loaded_.store(true);
    recorder_mode_.store(audio::RecorderMode::Loading);
    ++recorder_revision_;
    xTaskNotifyGive(capture_task_);
}

audio::RecorderSnapshot AudioSession::RecorderState() const {
    audio::RecorderSnapshot state;
    state.revision = recorder_revision_.load();
    state.mode = recorder_mode_.load();
    state.seconds = recorder_samples_.load() / BoardInputRate();
    state.has_clip = recorder_samples_.load() != 0;
    state.failed = recorder_failed_.load();
    state.saved = recorder_saved_.load();
    return state;
}

void AudioSession::RecorderLoop() {
    const bool loading = recorder_mode_.load() == audio::RecorderMode::Loading;
    const bool playback = recorder_mode_.load() == audio::RecorderMode::Playing;
    const int rate = BoardInputRate();
    constexpr int chunk = 320;
    const uint32_t capacity = rate * 30;
    bool ok = loading || (GetHAL().EnsureAudioStarted() && BoardOutputRate() == rate);
    if (ok && !playback && !recorder_pcm_) {
        recorder_pcm_ = static_cast<int16_t*>(heap_caps_malloc(capacity * sizeof(int16_t),
                                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        ok = recorder_pcm_ != nullptr;
    }
    const auto directory=std::string(GetHAL().GetSdMountPoint())+"/recordings";
    const auto path=directory+"/latest.wav";
    if (loading) {
        auto count = ok && GetHAL().IsSdMounted() ? audio::LoadRecording(path,recorder_pcm_,capacity,rate) : 0;
        if (!count && ok && GetHAL().IsSdMounted()) count=audio::LoadRecording(path+".bak",recorder_pcm_,capacity,rate);
        recorder_samples_.store(count);
        recorder_saved_.store(count != 0);
        ok=true; // No saved clip is a normal initial state.
    } else if (ok && !playback) {
        recorder_samples_.store(0);
        recorder_saved_.store(false);
        uint32_t filled = 0;
        int failures = 0;
        while (!recorder_stop_.load() && filled < capacity) {
            const int wanted = std::min<uint32_t>(chunk, capacity - filled);
            const int got = GetHAL().ReadMic(recorder_pcm_ + filled, wanted);
            if (got <= 0) {
                if (++failures >= 10) { ok = false; break; }
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            failures = 0;
            filled += got;
            recorder_samples_.store(filled);
        }
        if (filled < static_cast<uint32_t>(rate / 5)) {
            recorder_samples_.store(0);
            ok = false;
        }
        if (ok && GetHAL().IsSdMounted()) {
            (void)mkdir(directory.c_str(),0755);
            recorder_saved_.store(audio::SaveRecording(path,recorder_pcm_,filled,rate));
        }
    } else if (ok) {
        // Remove the microphone's DC offset and fade both ends. A stop request
        // uses a final ramp so the button cannot abruptly cut the speaker.
        const uint32_t count = recorder_samples_.load();
        int64_t sum = 0;
        for (uint32_t i = 0; i < count; ++i) sum += recorder_pcm_[i];
        const int mean = count ? sum / count : 0;
        int16_t out[chunk]{};
        int16_t last = 0;
        for (uint32_t offset = 0; offset < count && !recorder_stop_.load();) {
            const int n = std::min<uint32_t>(chunk, count - offset);
            for (int i = 0; i < n; ++i) {
                const uint32_t at = offset + i;
                const int ramp = std::min<uint32_t>(rate / 100, std::min(at + 1, count - at));
                out[i] = std::clamp((static_cast<int>(recorder_pcm_[at]) - mean) * ramp / (rate / 100), -32768, 32767);
            }
            if (GetHAL().WriteSpk(out, n) != n) { ok = false; break; }
            last = out[n - 1];
            offset += n;
        }
        const int fade = std::min(chunk, rate / 100);
        for (int i = 0; i < fade; ++i) out[i] = last * (fade - i - 1) / fade;
        if (GetHAL().WriteSpk(out, fade) != fade) ok = false;
    }
    recorder_failed_.store(!ok);
    recorder_mode_.store(audio::RecorderMode::Idle);
    ++recorder_revision_;
    ESP_LOGI(kTag, "recorder %s done samples=%lu ok=%d saved=%d", loading ? "restore" : playback ? "playback" : "capture",
             static_cast<unsigned long>(recorder_samples_.load()), ok, recorder_saved_.load());
}

void AudioSession::CaptureLoop() {
    ESP_LOGI(kTag, "capture task start");
    bool degraded = false;
    uint32_t not_ready = 0;
    uint32_t no_input = 0;
    uint32_t noise_floor = 0;
    uint32_t hangover = 0;
    bool gate_reported = false;
    bool level_reported = false;
    int32_t dc_estimate = 0;
    uint32_t frames_in_window = 0;

    while (true) {
        if (recorder_mode_.load() != audio::RecorderMode::Idle) {
            CloseEncoder();
            RecorderLoop();
            continue;
        }
        if (!capture_requested_.load()) {
            if (encoder_.handle != nullptr) {
                CloseEncoder();
                ESP_LOGI(kTag, "capture idle, encoder released");
            }
            degraded = false;
            not_ready = 0;
            no_input = 0;
            noise_floor = 0;
            hangover = 0;
            gate_reported = false;
            level_reported = false;
            dc_estimate = 0;
            frames_in_window = 0;
            input_peak_.store(0);
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kIdleWaitMs));
            continue;
        }

        if (!EnsureEncoder()) {
            capture_requested_.store(false);
            PublishStatus("小智音频不可用");
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        const int frame_samples = encoder_.frame_samples;
        if (frame_samples <= 0) {
            capture_requested_.store(false);
            continue;
        }

        // Gather exactly one Opus frame; the I2S read returns whole blocks, so
        // a short read only happens when the codec clock stalls.
        int16_t* pcm = reinterpret_cast<int16_t*>(encoder_.pcm.data());
        int filled = 0;
        int reads = 0;
        bool not_ready_now = false;
        while (filled < frame_samples && reads < kReadAttemptsPerFrame) {
            const int got = GetHAL().ReadMic(pcm + filled, frame_samples - filled);
            ++reads;
            if (got < 0) {
                not_ready_now = true;
                break;
            }
            if (got == 0) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            filled += got;
        }

        if (not_ready_now) {
            input_failures_.fetch_add(1);
            if (!degraded || not_ready == 0) {
                ESP_LOGW(kTag, "audio codec not ready for capture");
            }
            if (++not_ready >= kNotReadyLimit) {
                capture_requested_.store(false);
                degraded = false;
                PublishStatus("音频未就绪");
            } else {
                degraded = true;
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            continue;
        }

        if (filled < frame_samples) {
            input_failures_.fetch_add(1);
            if (++no_input >= kNoInputLimit && !degraded) {
                degraded = true;
                PublishStatus("麦克风无输入");
            }
            continue;
        }

        not_ready = 0;
        no_input = 0;
        input_frames_.fetch_add(1);
        const int peak =
            PeakAmplitudeAc(reinterpret_cast<const int16_t*>(encoder_.pcm.data()),
                            frame_samples, &dc_estimate);
        input_peak_.store(peak);
        if (degraded) {
            degraded = false;
            PublishStatus("小智聆听中");
        }

        // Track the quiet level and gate on it; the floor falls slowly so a
        // noisy room raises the bar but steady speech still opens the gate.
        if (noise_floor == 0) {
            noise_floor = static_cast<uint32_t>(peak);
        } else if (static_cast<uint32_t>(peak) < noise_floor) {
            noise_floor = (noise_floor * 7 + static_cast<uint32_t>(peak)) / 8;
        } else {
            noise_floor = (noise_floor * 31 + static_cast<uint32_t>(peak)) / 32;
        }
        const uint32_t open_threshold =
            std::max<uint32_t>(kSpeechGateMinPeak, noise_floor * 2);
        if (!level_reported && ++frames_in_window >= kLevelReportFrames) {
            level_reported = true;
            ESP_LOGI(kTag, "uplink levels after 5s: dc=%d ac_peak=%d floor=%u threshold=%u",
                     static_cast<int>(dc_estimate), peak,
                     static_cast<unsigned>(noise_floor),
                     static_cast<unsigned>(open_threshold));
        }
        if (static_cast<uint32_t>(peak) >= open_threshold) {
            if (hangover == 0) {
                gate_opens_.fetch_add(1);
                if (!gate_reported) {
                    ESP_LOGI(kTag, "uplink gate open: peak=%d floor=%u threshold=%u",
                             peak, static_cast<unsigned>(noise_floor),
                             static_cast<unsigned>(open_threshold));
                    gate_reported = true;
                }
            }
            hangover = kSpeechGateHangoverFrames;
        } else if (hangover > 0) {
            --hangover;
        } else {
            // Manual push-to-talk owns the capture window. Keep these frames:
            // gating the first syllable can erase a short voice capsule.
            // The server handles silence, and key-up stops capture immediately.
        }

        esp_audio_enc_in_frame_t input = {};
        input.buffer = encoder_.pcm.data();
        input.len = static_cast<uint32_t>(encoder_.pcm.size());
        esp_audio_enc_out_frame_t output = {};
        output.buffer = encoder_.out.data();
        output.len = static_cast<uint32_t>(encoder_.out.size());

        const esp_audio_err_t encoded = esp_opus_enc_process(encoder_.handle, &input, &output);
        if (encoded != ESP_AUDIO_ERR_OK || output.encoded_bytes == 0) {
            encode_errors_.fetch_add(1);
            continue;
        }
        if (!capture_requested_.load()) continue;
        if (SendOpusFrame(output.buffer, output.encoded_bytes)) {
            frames_sent_.fetch_add(1);
        } else {
            send_errors_.fetch_add(1);
        }
        capture_stack_free_.store(uxTaskGetStackHighWaterMark(nullptr));
    }
}

void AudioSession::PlaybackTaskEntry(void* arg) {
    static_cast<AudioSession*>(arg)->PlaybackLoop();
    vTaskDelete(nullptr);
}

void AudioSession::PlaybackLoop() {
    ESP_LOGI(kTag, "playback task start");
    Packet packet;
    uint32_t idle_ticks = 0;
    audio::ReminderChime chime;
    const int rate = OutputRate();
    std::vector<int16_t> cue(static_cast<size_t>(rate / 50));
    bool cue_playing = false;
    while (true) {
        // An arriving alarm requests stop and waits for the local clip task
        // to release I2S before rendering its first chime frame.
        if (recorder_mode_.load() != audio::RecorderMode::Idle) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        const bool cue_allowed = reminder_requested_.load() && !capture_requested_.load() &&
                                 !playback_open_.load() && !self_test_active_.load();
        const bool received = xQueueReceive(rx_queue_, &packet,
            (cue_allowed || cue_playing) ? 0 : pdMS_TO_TICKS(20)) == pdTRUE;
        if (cue_playing && (received || !cue_allowed)) {
            chime.FadeOut(cue.data(),cue.size(),rate);
            if (GetHAL().WriteSpk(cue.data(),cue.size()) < 0) ++reminder_errors_;
            cue_playing = false;
            reminder_playing_.store(false);
        }
        if (!received && cue_allowed) {
            CloseDecoder();
            chime.Render(cue.data(),cue.size(),rate);
            if (GetHAL().WriteSpk(cue.data(),cue.size()) < 0) {
                ++reminder_errors_;
                vTaskDelay(pdMS_TO_TICKS(20));
            } else ++reminder_frames_;
            cue_playing = true;
            reminder_playing_.store(true);
            playback_stack_free_.store(uxTaskGetStackHighWaterMark(nullptr));
            continue;
        }
        if (!received) {
            bool finished = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                // A window that promised audio but never delivered any would
                // otherwise hold this task's 24 KiB stack forever.
                if (playback_open_.load() && ++idle_ticks >= kPlaybackIdleCloseTicks * 25) {
                    ESP_LOGW(kTag, "playback window idle, closing");
                    playback_open_.store(false);
                    idle_ticks = 0;
                }
                if (!playback_open_.load() && !reminder_requested_.load() && uxQueueMessagesWaiting(rx_queue_) == 0) {
                    // Release the decoder before another caller can create a
                    // replacement playback task that uses the same state.
                    CloseDecoder();
                    playback_task_ = nullptr;
                    finished = true;
                }
            }
            if (finished) {
                break;
            }
            continue;
        }
        idle_ticks = 0;

        if (!EnsureDecoder()) {
            decode_errors_.fetch_add(1);
            continue;
        }

        esp_audio_dec_in_raw_t raw = {};
        raw.buffer = packet.data;
        raw.len = packet.length;
        esp_audio_dec_out_frame_t frame = {};
        frame.buffer = decoder_.buffer.data();
        frame.len = static_cast<uint32_t>(decoder_.buffer.size());
        esp_audio_dec_info_t info = {};

        const esp_audio_err_t decoded =
            esp_opus_dec_decode(decoder_.handle, &raw, &frame, &info);
        if (decoded != ESP_AUDIO_ERR_OK || frame.decoded_size == 0) {
            decode_errors_.fetch_add(1);
            continue;
        }

        const int samples = static_cast<int>(frame.decoded_size / sizeof(int16_t));
        const int16_t* pcm = reinterpret_cast<const int16_t*>(decoder_.buffer.data());
        if (decoder_.sample_rate != decoder_.output_rate) {
            // Linear interpolation is enough for 24 kHz speech folded onto the
            // fixed 16 kHz speaker clock, and it keeps the cost per frame tiny.
            const int resampled = resampler_.Process(pcm, samples);
            if (resampled <= 0) {
                decode_errors_.fetch_add(1);
                continue;
            }
            pcm = resampler_.out.data();
            if (GetHAL().WriteSpk(pcm, resampled) < 0) {
                decode_errors_.fetch_add(1);
                continue;
            }
        } else if (GetHAL().WriteSpk(pcm, samples) < 0) {
            decode_errors_.fetch_add(1);
            continue;
        }
        packets_decoded_.fetch_add(1);
    }

    playback_stack_free_.store(uxTaskGetStackHighWaterMark(nullptr));
    ESP_LOGI(kTag, "playback task exit, decoder released");
}

void AudioSession::SelfTestTaskEntry(void* arg) {
    auto* job = static_cast<SelfTestJob*>(arg);
    if (job != nullptr) {
        std::string detail;
        job->ok = job->downlink_loop ? job->session->DownlinkLoopTestBody(&detail)
                                     : job->session->SelfTestBody(&detail);
        std::snprintf(job->detail, sizeof(job->detail), "%s", detail.c_str());
        xSemaphoreGive(job->done);
    }
    vTaskDelete(nullptr);
}

bool AudioSession::RunAudioTestTask(bool downlink_loop, std::string* detail) {
    auto fail = [detail](const char* text) {
        if (detail != nullptr) {
            *detail = text;
        }
        return false;
    };

    {
        std::lock_guard<std::mutex> lock(mutex_);
        bool expected = false;
        if (!self_test_active_.compare_exchange_strong(expected, true)) return fail("busy");
        if (capture_requested_.load() || playback_open_.load() || reminder_requested_.load() ||
            recorder_mode_.load() != audio::RecorderMode::Idle) {
            self_test_active_.store(false);
            return fail("session_active");
        }
    }
    struct Guard {
        std::atomic<bool>* flag;
        ~Guard() { flag->store(false); }
    } guard{&self_test_active_};

    // Opus initialisation and the I2S waits want more stack than the serial
    // task has, so the work runs on its own task and the caller just waits.
    SelfTestJob job;
    job.session = this;
    job.done = xSemaphoreCreateBinary();
    job.downlink_loop = downlink_loop;
    if (job.done == nullptr) {
        return fail("no_memory");
    }
    const BaseType_t created =
        CreateAudioTask(SelfTestTaskEntry, "xz_audiotest", kSelfTestStackBytes, &job, 3, nullptr);
    if (created != pdPASS) {
        vSemaphoreDelete(job.done);
        return fail("task_create");
    }
    if (xSemaphoreTake(job.done, pdMS_TO_TICKS(kSelfTestTimeoutMs)) != pdTRUE) {
        vSemaphoreDelete(job.done);
        return fail("timeout");
    }
    vSemaphoreDelete(job.done);
    if (detail != nullptr) {
        detail->assign(job.detail);
    }
    return job.ok;
}

bool AudioSession::SelfTest(std::string* detail) { return RunAudioTestTask(false, detail); }

bool AudioSession::DownlinkLoopTest(std::string* detail) {
    return RunAudioTestTask(true, detail);
}

// Drives the real downlink path: a tone is encoded at the negotiated server
// rate, wrapped exactly like an incoming frame, pushed through OnServerPacket
// and then decoded, resampled and written to the speaker by the playback task.
bool AudioSession::DownlinkLoopTestBody(std::string* detail) {
    auto fail = [detail](const char* text) {
        if (detail != nullptr) {
            *detail = text;
        }
        return false;
    };

    if (!GetHAL().EnsureAudioStarted()) {
        return fail("audio_not_ready");
    }

    const int server_rate = DownlinkRate();
    const int frame_ms = TargetFrameMs();
    const uint32_t frames = static_cast<uint32_t>(kToneMs / frame_ms);
    const uint32_t received_before = packets_received_.load();
    const uint32_t decoded_before = packets_decoded_.load();

    OpusEncoderState encoder;
    if (!EncoderOpen(&encoder, server_rate, frame_ms)) {
        return fail("encoder_open");
    }

    playback_open_.store(true);
    if (!EnsurePlaybackTask()) {
        EncoderClose(&encoder);
        playback_open_.store(false);
        return fail("playback_task");
    }

    std::vector<uint8_t> frame(kAudioHeaderBytes + 1024);
    double phase = 0.0;
    const double step = 2.0 * kPi * 1000.0 / server_rate;
    uint32_t sent = 0;
    uint32_t encode_failures = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        int16_t* dest = reinterpret_cast<int16_t*>(encoder.pcm.data());
        for (int n = 0; n < encoder.frame_samples; ++n) {
            dest[n] = static_cast<int16_t>(std::sin(phase) * 8000.0);
            phase += step;
            if (phase >= 2.0 * kPi) {
                phase -= 2.0 * kPi;
            }
        }

        esp_audio_enc_in_frame_t input = {};
        input.buffer = encoder.pcm.data();
        input.len = static_cast<uint32_t>(encoder.pcm.size());
        esp_audio_enc_out_frame_t output = {};
        output.buffer = encoder.out.data();
        output.len = static_cast<uint32_t>(encoder.out.size());
        if (esp_opus_enc_process(encoder.handle, &input, &output) != ESP_AUDIO_ERR_OK ||
            output.encoded_bytes == 0) {
            ++encode_failures;
            continue;
        }

        if (frame.size() < kAudioHeaderBytes + output.encoded_bytes) {
            frame.resize(kAudioHeaderBytes + output.encoded_bytes);
        }
        frame[0] = kAudioPacketTypeOpus;
        frame[1] = 0;
        frame[2] = static_cast<uint8_t>((output.encoded_bytes >> 8) & 0xffU);
        frame[3] = static_cast<uint8_t>(output.encoded_bytes & 0xffU);
        std::memcpy(frame.data() + kAudioHeaderBytes, output.buffer, output.encoded_bytes);

        OnServerPacket(frame.data(), kAudioHeaderBytes + output.encoded_bytes);
        ++sent;
        // Space the frames like a real stream so the queue sees normal arrival
        // timing instead of an unrealistic burst.
        vTaskDelay(pdMS_TO_TICKS(frame_ms));
    }

    // Let the queue drain, then close the window the same way a tts stop does.
    uint32_t last_decoded = packets_decoded_.load();
    uint32_t stable = 0;
    for (int i = 0; i < 60 && stable < 3; ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
        const uint32_t now = packets_decoded_.load();
        stable = (now == last_decoded) ? stable + 1 : 0;
        last_decoded = now;
    }
    playback_open_.store(false);
    for (int i = 0; i < 20 && decoder_.handle != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    EncoderClose(&encoder);

    const uint32_t received = packets_received_.load() - received_before;
    const uint32_t decoded = packets_decoded_.load() - decoded_before;
    if (detail != nullptr) {
        char summary[256];
        std::snprintf(summary, sizeof(summary),
                      "server_rate=%d frame_ms=%d sent=%u enc_fail=%u received=%u decoded=%u",
                      server_rate, frame_ms, static_cast<unsigned>(sent),
                      static_cast<unsigned>(encode_failures), static_cast<unsigned>(received),
                      static_cast<unsigned>(decoded));
        detail->assign(summary);
    }
    return sent > 0 && decoded == sent;
}

bool AudioSession::SelfTestBody(std::string* detail) {
    auto fail = [detail](const char* text) {
        if (detail != nullptr) {
            *detail = text;
        }
        return false;
    };

    if (!GetHAL().EnsureAudioStarted()) {
        return fail("audio_not_ready");
    }

    // The self test exercises the board clock on both legs, so it stays at the
    // microphone/speaker rate instead of the server's TTS rate.
    const int rate = UplinkRate();
    const int output_rate = OutputRate();
    const int frame_ms = TargetFrameMs();
    OpusEncoderState encoder;
    OpusDecoderState decoder;
    if (!EncoderOpen(&encoder, rate, frame_ms)) {
        return fail("encoder_open");
    }
    if (!DecoderOpen(&decoder, rate, frame_ms, output_rate)) {
        EncoderClose(&encoder);
        return fail("decoder_open");
    }

    uint32_t mic_frames = 0;
    uint32_t mic_failures = 0;
    uint32_t encoded_bytes = 0;
    int peak = 0;
    int64_t encode_us = 0;
    int64_t decode_us = 0;

    // The capture window is bounded by wall clock as well as frame count: a
    // codec without an I2S clock returns after its own timeout, and the test
    // must still finish and report instead of blocking the caller.
    const int64_t deadline_us =
        esp_timer_get_time() + static_cast<int64_t>(kSelfTestMs + 3000) * 1000;
    const uint32_t mic_target = static_cast<uint32_t>(kSelfTestMs / frame_ms);
    for (uint32_t i = 0; i < mic_target && esp_timer_get_time() < deadline_us; ++i) {
        int filled = 0;
        int reads = 0;
        int16_t* dest = reinterpret_cast<int16_t*>(encoder.pcm.data());
        while (filled < encoder.frame_samples && reads < kReadAttemptsPerFrame &&
               esp_timer_get_time() < deadline_us) {
            const int got = GetHAL().ReadMic(dest + filled, encoder.frame_samples - filled);
            ++reads;
            if (got <= 0) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            filled += got;
        }
        if (filled < encoder.frame_samples) {
            ++mic_failures;
            continue;
        }

        peak = std::max(peak, PeakAmplitude(encoder.pcm.data(), encoder.frame_samples));

        esp_audio_enc_in_frame_t input = {};
        input.buffer = encoder.pcm.data();
        input.len = static_cast<uint32_t>(encoder.pcm.size());
        esp_audio_enc_out_frame_t output = {};
        output.buffer = encoder.out.data();
        output.len = static_cast<uint32_t>(encoder.out.size());
        const int64_t encode_start = esp_timer_get_time();
        const esp_audio_err_t encoded = esp_opus_enc_process(encoder.handle, &input, &output);
        encode_us += esp_timer_get_time() - encode_start;
        if (encoded != ESP_AUDIO_ERR_OK || output.encoded_bytes == 0) {
            ++mic_failures;
            continue;
        }

        esp_audio_dec_in_raw_t raw = {};
        raw.buffer = output.buffer;
        raw.len = output.encoded_bytes;
        esp_audio_dec_out_frame_t frame = {};
        frame.buffer = decoder.buffer.data();
        frame.len = static_cast<uint32_t>(decoder.buffer.size());
        esp_audio_dec_info_t info = {};
        const int64_t decode_start = esp_timer_get_time();
        const esp_audio_err_t decoded =
            esp_opus_dec_decode(decoder.handle, &raw, &frame, &info);
        decode_us += esp_timer_get_time() - decode_start;
        if (decoded != ESP_AUDIO_ERR_OK || frame.decoded_size == 0) {
            ++mic_failures;
            continue;
        }

        encoded_bytes += output.encoded_bytes;
        ++mic_frames;
    }

    // Speaker leg: a 1 kHz tone through the same encoder/decoder pair proves
    // the playback path without feeding the microphone back into the speaker.
    // It only runs once the microphone legs proved the codec has a clock; an
    // unclocked I2S transmitter would otherwise block the task forever.
    uint32_t tone_frames = 0;
    uint32_t tone_failures = 0;
    bool tone_skipped = mic_frames == 0;
    const uint32_t tone_target = static_cast<uint32_t>(kToneMs / frame_ms);
    const int64_t tone_deadline_us =
        esp_timer_get_time() + static_cast<int64_t>(kToneMs + 3000) * 1000;
    double phase = 0.0;
    const double step = 2.0 * kPi * 1000.0 / rate;
    for (uint32_t i = 0; !tone_skipped && i < tone_target && esp_timer_get_time() < tone_deadline_us;
         ++i) {
        int16_t* dest = reinterpret_cast<int16_t*>(encoder.pcm.data());
        for (int n = 0; n < encoder.frame_samples; ++n) {
            dest[n] = static_cast<int16_t>(std::sin(phase) * 8000.0);
            phase += step;
            if (phase >= 2.0 * kPi) {
                phase -= 2.0 * kPi;
            }
        }

        esp_audio_enc_in_frame_t input = {};
        input.buffer = encoder.pcm.data();
        input.len = static_cast<uint32_t>(encoder.pcm.size());
        esp_audio_enc_out_frame_t output = {};
        output.buffer = encoder.out.data();
        output.len = static_cast<uint32_t>(encoder.out.size());
        if (esp_opus_enc_process(encoder.handle, &input, &output) != ESP_AUDIO_ERR_OK ||
            output.encoded_bytes == 0) {
            ++tone_failures;
            continue;
        }

        esp_audio_dec_in_raw_t raw = {};
        raw.buffer = output.buffer;
        raw.len = output.encoded_bytes;
        esp_audio_dec_out_frame_t frame = {};
        frame.buffer = decoder.buffer.data();
        frame.len = static_cast<uint32_t>(decoder.buffer.size());
        esp_audio_dec_info_t info = {};
        if (esp_opus_dec_decode(decoder.handle, &raw, &frame, &info) != ESP_AUDIO_ERR_OK ||
            frame.decoded_size == 0) {
            ++tone_failures;
            continue;
        }

        const int samples = static_cast<int>(frame.decoded_size / sizeof(int16_t));
        if (GetHAL().WriteSpk(reinterpret_cast<const int16_t*>(decoder.buffer.data()), samples) < 0) {
            ++tone_failures;
            continue;
        }
        ++tone_frames;
    }

    DecoderClose(&decoder);
    EncoderClose(&encoder);

    if (detail != nullptr) {
        // This target uses the nano printf, which has no usable 64-bit integer
        // path; every field stays inside 32 bits on purpose.
        char summary[256];
        std::snprintf(summary, sizeof(summary),
                      "rate=%d frame_ms=%d mic_frames=%u mic_fail=%u mic_peak=%d "
                      "encoded=%u enc_ms=%lu dec_ms=%lu tone_frames=%u tone_fail=%u "
                      "tone_skipped=%d stack_free=%u",
                      rate, frame_ms, static_cast<unsigned>(mic_frames),
                      static_cast<unsigned>(mic_failures), peak,
                      static_cast<unsigned>(encoded_bytes),
                      static_cast<unsigned long>(encode_us / 1000),
                      static_cast<unsigned long>(decode_us / 1000),
                      static_cast<unsigned>(tone_frames),
                      static_cast<unsigned>(tone_failures), tone_skipped ? 1 : 0,
                      static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        detail->assign(summary);
    }
    return mic_frames > 0 && (tone_skipped || tone_frames > 0);
}

}  // namespace xiaozhi
