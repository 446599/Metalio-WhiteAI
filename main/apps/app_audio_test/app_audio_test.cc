#include "app_audio_test.h"

#include "hal/hal.h"
#include "haptic_feedback.h"
#include "screen_common.h"
#include "vk_key_handler.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "fontpack_lvgl.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace {

constexpr const char* TAG = "AppAudioTest";
constexpr const char* kScreenId = "app_audio_test";
constexpr lv_coord_t kBtnH = 64;
constexpr lv_coord_t kBorderW = 2;
constexpr int kMaxRecordSeconds = 30;
constexpr int kMinRecordSamples = 3200;  // 0.2s @ 16kHz
constexpr int kChunkSamples = 320;       // 20ms @ 16kHz
constexpr int kMaxConsecutiveMicTimeouts = 10;  // ~2s @ 200ms I2S timeout

lv_obj_t* s_scr = nullptr;
lv_obj_t* s_status = nullptr;
std::atomic<bool> s_busy{false};
std::atomic<bool> s_alive{false};
std::atomic<bool> s_recording{false};

void SetStatus(const char* text) {
    if (s_status != nullptr && lv_obj_is_valid(s_status) && text != nullptr) {
        lv_label_set_text(s_status, text);
    }
}

void StatusAsync(void* p) {
    auto* msg = static_cast<char*>(p);
    if (s_alive.load() && msg != nullptr) {
        SetStatus(msg);
    }
    free(msg);
}

void PostStatus(const char* text) {
    if (text == nullptr) {
        return;
    }
    char* copy = strdup(text);
    if (copy == nullptr) {
        ESP_LOGW(TAG, "PostStatus strdup failed: %s", text);
        return;
    }
    lv_async_call(StatusAsync, copy);
}

void PostRecordDuration(int seconds) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "录音中 %d:%02d", seconds / 60, seconds % 60);
    PostStatus(buf);
}

lv_obj_t* MakeHoldBtn(lv_obj_t* parent, const char* title, lv_event_cb_t press_cb,
                      lv_event_cb_t release_cb) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, kBtnH);
    lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, kBorderW, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_margin_bottom(btn, 12, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, press_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(btn, release_cb, LV_EVENT_RELEASED, nullptr);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, fontpack_lv_font_ui(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

void RecordPlayTask(void* /*arg*/) {
    ESP_LOGI(TAG, "RecordPlayTask start");
    auto& hal = GetHAL();
    if (!hal.EnsureAudioStarted()) {
        ESP_LOGW(TAG, "audio start failed (BT mode / codec)");
        PostStatus("BT 音频未就绪");
        s_recording.store(false);
        s_busy.store(false);
        vTaskDelete(nullptr);
        return;
    }

    const int rate = hal.AudioInputSampleRate();
    const int capacity = rate * kMaxRecordSeconds;
    int16_t* pcm = static_cast<int16_t*>(
        heap_caps_malloc(static_cast<size_t>(capacity) * sizeof(int16_t),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (pcm == nullptr) {
        pcm = static_cast<int16_t*>(
            heap_caps_malloc(static_cast<size_t>(capacity) * sizeof(int16_t), MALLOC_CAP_8BIT));
    }
    if (pcm == nullptr) {
        ESP_LOGE(TAG, "PCM alloc failed capacity=%d", capacity);
        PostStatus("内存不足");
        s_recording.store(false);
        s_busy.store(false);
        vTaskDelete(nullptr);
        return;
    }
    std::memset(pcm, 0, static_cast<size_t>(capacity) * sizeof(int16_t));

    PostRecordDuration(0);
    ESP_LOGI(TAG, "recording… rate=%d capacity=%d", rate, capacity);
    const int64_t record_start_us = esp_timer_get_time();
    int filled = 0;
    int last_sec = -1;
    int consecutive_timeouts = 0;
    while (s_recording.load() && filled < capacity && s_alive.load()) {
        const int n = std::min(kChunkSamples, capacity - filled);
        const int got = hal.ReadMic(pcm + filled, n);
        if (got > 0) {
            consecutive_timeouts = 0;
            filled += got;
            const int sec = (rate > 0) ? (filled / rate) : 0;
            if (sec != last_sec) {
                last_sec = sec;
                PostRecordDuration(sec);
            }
        } else {
            ++consecutive_timeouts;
            if (consecutive_timeouts >= kMaxConsecutiveMicTimeouts) {
                ESP_LOGW(TAG, "mic read timeout x%d (check BT mode / I2S clock)",
                         consecutive_timeouts);
                heap_caps_free(pcm);
                PostStatus("读麦超时（检查 BT 模式）");
                s_recording.store(false);
                s_busy.store(false);
                vTaskDelete(nullptr);
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    s_recording.store(false);
    const int64_t record_end_us = esp_timer_get_time();
    const int wall_ms = static_cast<int>((record_end_us - record_start_us) / 1000);
    const int sample_ms = (rate > 0) ? static_cast<int>((filled * 1000LL) / rate) : 0;
    ESP_LOGI(TAG, "record stop filled=%d wall_ms=%d sample_ms=%d", filled, wall_ms, sample_ms);

    if (!s_alive.load()) {
        heap_caps_free(pcm);
        s_busy.store(false);
        vTaskDelete(nullptr);
        return;
    }

    if (filled < kMinRecordSamples) {
        heap_caps_free(pcm);
        ESP_LOGI(TAG, "record too short");
        PostStatus("录音太短");
        s_busy.store(false);
        vTaskDelete(nullptr);
        return;
    }

    if (wall_ms > 0 && sample_ms < static_cast<int>(wall_ms * 0.85)) {
        ESP_LOGW(TAG, "record underrun: sample_ms=%d wall_ms=%d", sample_ms, wall_ms);
        PostStatus("录音有丢包，仍回放…");
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    PostStatus("播放中…");
    ESP_LOGI(TAG, "playback %d samples", filled);
    const int64_t play_start_us = esp_timer_get_time();
    int played = 0;
    while (played < filled && s_alive.load()) {
        const int n = std::min(kChunkSamples, filled - played);
        (void)hal.WriteSpk(pcm + played, n);
        played += n;
    }
    const int play_ms = static_cast<int>((esp_timer_get_time() - play_start_us) / 1000);
    ESP_LOGI(TAG, "playback done play_ms=%d sample_ms=%d wall_ms=%d", play_ms, sample_ms, wall_ms);

    heap_caps_free(pcm);
    PostStatus(s_alive.load() ? "完成：录音并回放结束" : "已取消");
    ESP_LOGI(TAG, "RecordPlayTask done");
    s_busy.store(false);
    vTaskDelete(nullptr);
}

void OnRecordPressed(lv_event_t* /*e*/) {
    if (s_busy.exchange(true)) {
        ESP_LOGW(TAG, "record ignored: busy");
        SetStatus("忙碌中，请稍候");
        return;
    }
    s_recording.store(true);
    SetStatus("准备录音…");
    ESP_LOGI(TAG, "record pressed");
    if (xTaskCreatePinnedToCore(RecordPlayTask, "aud_test", 8192, nullptr, 5, nullptr, 0) !=
        pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate aud_test failed");
        PostStatus("创建任务失败");
        s_recording.store(false);
        s_busy.store(false);
    }
}

void OnRecordReleased(lv_event_t* /*e*/) {
    if (!s_recording.load()) {
        return;
    }
    ESP_LOGI(TAG, "record released");
    s_recording.store(false);
}

void OnDeleted(lv_event_t* e) {
    if (lv_event_get_target(e) != s_scr) {
        return;
    }
    s_recording.store(false);
    s_alive.store(false);
    s_scr = nullptr;
    s_status = nullptr;
}

}  // namespace

lv_obj_t* AppAudioTest::Create() {
    ESP_LOGI(TAG, "create audio test");
    ScreenSetIsHome(false);
    s_alive.store(true);
    s_busy.store(false);
    s_recording.store(false);

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_scr = scr;
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, OnDeleted, LV_EVENT_DELETE, nullptr);

    EpdStatusBar status = ScreenCreateStatusBar(scr);
    if (status.status_label != nullptr) {
    }

    lv_obj_t* body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, LV_VER_RES - status.height);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, status.height);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(body, 20, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* desc = lv_label_create(body);
    lv_label_set_text(desc, "按住按钮录音，松手自动回放\n最长 30 秒，请对着麦克风说话");
    lv_obj_set_style_text_align(desc, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(desc, lv_pct(100));
    lv_obj_set_style_margin_bottom(desc, 16, 0);

    s_status = lv_label_create(body);
    lv_label_set_text(s_status, "待命");
    lv_obj_set_width(s_status, lv_pct(100));
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_bottom(s_status, 24, 0);

    MakeHoldBtn(body, "按住录音，松手回放", OnRecordPressed, OnRecordReleased);

    VkKey_AttachScreen(scr, kScreenId, VkKeyScreenDesc{AppAudioTest::Create});
    return scr;
}
