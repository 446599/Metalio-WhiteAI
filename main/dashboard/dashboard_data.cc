#include "dashboard_data.h"

#include <algorithm>
#include <cstring>
#include <cstdio>

namespace dashboard {
namespace {

size_t Utf8SequenceLength(const unsigned char first) {
    if (first < 0x80U) return 1;
    if (first >= 0xC2U && first <= 0xDFU) return 2;
    if (first >= 0xE0U && first <= 0xEFU) return 3;
    if (first >= 0xF0U && first <= 0xF4U) return 4;
    return 1;
}

void CopyField(char* destination, size_t destination_size, const char* source) {
    CopyText(destination, destination_size, source);
}

bool TextEquals(const char* destination, size_t destination_size, const char* source) {
    if (destination == nullptr || destination_size == 0) return source == nullptr || source[0] == '\0';
    if (source == nullptr) source = "";
    size_t index = 0;
    while (index < destination_size && destination[index] != '\0' && source[index] != '\0' &&
           destination[index] == source[index]) {
        ++index;
    }
    return index < destination_size && destination[index] == '\0' && source[index] == '\0';
}

}  // namespace

Freshness EvaluateFreshness(bool valid, uint32_t updated_epoch, uint32_t now_epoch,
                           uint32_t fresh_seconds) {
    if (!valid) return Freshness::NoData;
    if (now_epoch < kEarliestTrustedEpoch || updated_epoch < kEarliestTrustedEpoch ||
        updated_epoch > now_epoch) return Freshness::UnknownTime;
    return now_epoch - updated_epoch < fresh_seconds ? Freshness::Fresh : Freshness::Stale;
}

namespace {
void FormatStatus(bool valid, bool from_cache, RequestState request_state, Freshness freshness,
                  char* destination, size_t size) {
    const char* request = "";
    switch (request_state) {
        case RequestState::Disabled: request = "未配置"; break;
        case RequestState::Refreshing: request = "更新中"; break;
        case RequestState::Failed: request = "更新失败"; break;
        case RequestState::Offline: request = "离线"; break;
        default: break;
    }
    const char* age = !valid ? "无数据" :
                      freshness == Freshness::Stale ? "已过期" :
                      freshness == Freshness::Fresh ? (from_cache ? "缓存" : "已更新") :
                      "时间未知";
    char status[64];
    if (*request != '\0') {
        std::snprintf(status, sizeof(status), "%s·%s", request, age);
    } else if (from_cache && freshness != Freshness::Fresh) {
        std::snprintf(status, sizeof(status), "缓存·%s", age);
    } else {
        std::snprintf(status, sizeof(status), "%s", age);
    }
    CopyText(destination, size, status);
}
}  // namespace

void FormatWeatherStatus(const Weather& weather, char* destination, size_t size) {
    FormatStatus(weather.valid, weather.from_cache, weather.request_state,
                 weather.freshness, destination, size);
}

void FormatQuotaStatus(const Quota& quota, char* destination, size_t size) {
    FormatStatus(quota.valid, quota.from_cache, quota.request_state,
                 quota.freshness, destination, size);
}

void CopyText(char* destination, size_t destination_size, const char* source) {
    if (destination == nullptr || destination_size == 0) return;
    destination[0] = '\0';
    if (source == nullptr) return;

    const auto* cursor = reinterpret_cast<const unsigned char*>(source);
    size_t written = 0;
    while (*cursor != 0) {
        size_t sequence_length = Utf8SequenceLength(*cursor);
        if (sequence_length > 1) {
            bool valid = true;
            for (size_t i = 1; i < sequence_length; ++i) {
                if (cursor[i] == 0 || (cursor[i] & 0xC0U) != 0x80U) {
                    valid = false;
                    break;
                }
            }
            if (valid) {
                const unsigned char lead = cursor[0];
                const unsigned char second = cursor[1];
                if ((lead == 0xE0U && second < 0xA0U) ||
                    (lead == 0xEDU && second >= 0xA0U) ||
                    (lead == 0xF0U && second < 0x90U) ||
                    (lead == 0xF4U && second >= 0x90U)) {
                    valid = false;
                }
            }
            if (!valid) sequence_length = 1;
        }
        if (written + sequence_length >= destination_size) break;
        std::memcpy(destination + written, cursor, sequence_length);
        written += sequence_length;
        cursor += sequence_length;
    }
    destination[written] = '\0';
}

DashboardData& DashboardData::GetInstance() {
    static DashboardData instance;
    return instance;
}

DashboardData::DashboardData() {
    ResetDefaults();
}

void DashboardData::MarkChangedLocked() {
    ++snapshot_.revision;
    if (snapshot_.revision == 0) ++snapshot_.revision;
}

void DashboardData::RecomputeAiCountLocked() {
    size_t count = kAiSummaryCount;
    while (count > 0 && snapshot_.ai_summary[count - 1][0] == '\0') --count;
    snapshot_.ai_count = static_cast<uint8_t>(count);
}

void DashboardData::ResetDefaults() {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_ = Snapshot{};
    now_epoch_ = 0;
    CopyField(snapshot_.network, sizeof(snapshot_.network), "离线");
    CopyField(snapshot_.ai_status, sizeof(snapshot_.ai_status), "小智待连接");
    CopyField(snapshot_.ai_summary[0], sizeof(snapshot_.ai_summary[0]),
              "今天先整理最重要的一件事");
    CopyField(snapshot_.ai_summary[1], sizeof(snapshot_.ai_summary[1]),
              "完成后再交给小智安排下一步");
    CopyField(snapshot_.ai_summary[2], sizeof(snapshot_.ai_summary[2]),
              "联网后会自动更新天气与提醒");
    snapshot_.ai_count = 3;

    CopyField(snapshot_.schedule[0].time, sizeof(snapshot_.schedule[0].time), "09:00");
    CopyField(snapshot_.schedule[0].title, sizeof(snapshot_.schedule[0].title), "整理今日重点");
    CopyField(snapshot_.schedule[0].detail, sizeof(snapshot_.schedule[0].detail), "把任务分成可执行的小步");
    CopyField(snapshot_.schedule[1].time, sizeof(snapshot_.schedule[1].time), "14:00");
    CopyField(snapshot_.schedule[1].title, sizeof(snapshot_.schedule[1].title), "阅读 30 分钟");
    CopyField(snapshot_.schedule[1].detail, sizeof(snapshot_.schedule[1].detail), "保持安静，完成当前章节");
    CopyField(snapshot_.schedule[2].time, sizeof(snapshot_.schedule[2].time), "18:30");
    CopyField(snapshot_.schedule[2].title, sizeof(snapshot_.schedule[2].title), "回顾与同步");
    CopyField(snapshot_.schedule[2].detail, sizeof(snapshot_.schedule[2].detail), "记录今天的进展与明日计划");
    snapshot_.schedule_count = kScheduleCount;

    CopyField(snapshot_.weather.location, sizeof(snapshot_.weather.location), "北京");
    CopyField(snapshot_.weather.condition, sizeof(snapshot_.weather.condition), "等待更新");
    CopyField(snapshot_.quota.source, sizeof(snapshot_.quota.source), "未连接");

    snapshot_.custom[0].enabled = true;
    CopyField(snapshot_.custom[0].title, sizeof(snapshot_.custom[0].title), "专注计时");
    CopyField(snapshot_.custom[0].value, sizeof(snapshot_.custom[0].value), "25 min");
    CopyField(snapshot_.custom[0].detail, sizeof(snapshot_.custom[0].detail), "点击开始一段专注时间");
    snapshot_.custom[1].enabled = true;
    CopyField(snapshot_.custom[1].title, sizeof(snapshot_.custom[1].title), "阅读进度");
    CopyField(snapshot_.custom[1].value, sizeof(snapshot_.custom[1].value), "--");
    CopyField(snapshot_.custom[1].detail, sizeof(snapshot_.custom[1].detail), "等待同步阅读记录");
    snapshot_.revision = 1;
}

Snapshot DashboardData::GetSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

uint32_t DashboardData::Revision() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_.revision;
}

void DashboardData::SetNetwork(const char* text) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (TextEquals(snapshot_.network, sizeof(snapshot_.network), text)) return;
    CopyField(snapshot_.network, sizeof(snapshot_.network), text);
    MarkChangedLocked();
}

void DashboardData::SetAiStatus(const char* text) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (TextEquals(snapshot_.ai_status, sizeof(snapshot_.ai_status), text)) return;
    CopyField(snapshot_.ai_status, sizeof(snapshot_.ai_status), text);
    MarkChangedLocked();
}

void DashboardData::SetAiSummary(size_t index, const char* text) {
    if (index >= kAiSummaryCount) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const bool text_changed = !TextEquals(snapshot_.ai_summary[index], sizeof(snapshot_.ai_summary[index]), text);
    const uint8_t old_count = snapshot_.ai_count;
    if (!text_changed && old_count > index) return;
    CopyField(snapshot_.ai_summary[index], sizeof(snapshot_.ai_summary[index]), text);
    RecomputeAiCountLocked();
    MarkChangedLocked();
}

void DashboardData::ClearAiSummary() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot_.ai_count == 0) return;
    for (auto& item : snapshot_.ai_summary) item[0] = '\0';
    snapshot_.ai_count = 0;
    MarkChangedLocked();
}

void DashboardData::SetSchedule(size_t index, const char* time, const char* title,
                                const char* detail, bool done) {
    if (index >= kScheduleCount) return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto& item = snapshot_.schedule[index];
    if (TextEquals(item.time, sizeof(item.time), time) &&
        TextEquals(item.title, sizeof(item.title), title) &&
        TextEquals(item.detail, sizeof(item.detail), detail) && item.done == done &&
        snapshot_.schedule_count > index) {
        return;
    }
    CopyField(item.time, sizeof(item.time), time);
    CopyField(item.title, sizeof(item.title), title);
    CopyField(item.detail, sizeof(item.detail), detail);
    item.done = done;
    snapshot_.schedule_count = static_cast<uint8_t>(std::max<size_t>(snapshot_.schedule_count, index + 1));
    MarkChangedLocked();
}

void DashboardData::SetScheduleCount(size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint8_t bounded = static_cast<uint8_t>(std::min(count, kScheduleCount));
    if (snapshot_.schedule_count == bounded) return;
    snapshot_.schedule_count = bounded;
    MarkChangedLocked();
}

void DashboardData::SetWeather(const Weather& weather) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot_.weather.valid == weather.valid &&
        snapshot_.weather.from_cache == weather.from_cache &&
        snapshot_.weather.request_state == weather.request_state &&
        TextEquals(snapshot_.weather.location, sizeof(snapshot_.weather.location), weather.location) &&
        TextEquals(snapshot_.weather.condition, sizeof(snapshot_.weather.condition), weather.condition) &&
        snapshot_.weather.temperature_c == weather.temperature_c &&
        snapshot_.weather.feels_like_c == weather.feels_like_c &&
        snapshot_.weather.humidity == weather.humidity &&
        snapshot_.weather.wind_kmh == weather.wind_kmh &&
        snapshot_.weather.updated_epoch == weather.updated_epoch) {
        return;
    }
    snapshot_.weather = weather;
    snapshot_.weather.freshness = EvaluateFreshness(weather.valid, weather.updated_epoch,
                                                   now_epoch_, kWeatherFreshSeconds);
    snapshot_.weather.location[sizeof(snapshot_.weather.location) - 1] = '\0';
    snapshot_.weather.condition[sizeof(snapshot_.weather.condition) - 1] = '\0';
    MarkChangedLocked();
}

void DashboardData::SetQuota(const Quota& quota) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot_.quota.valid == quota.valid &&
        snapshot_.quota.from_cache == quota.from_cache &&
        snapshot_.quota.request_state == quota.request_state &&
        TextEquals(snapshot_.quota.source, sizeof(snapshot_.quota.source), quota.source) &&
        snapshot_.quota.five_hour_remaining == quota.five_hour_remaining &&
        snapshot_.quota.weekly_remaining == quota.weekly_remaining &&
        snapshot_.quota.updated_epoch == quota.updated_epoch) {
        return;
    }
    snapshot_.quota = quota;
    snapshot_.quota.freshness = EvaluateFreshness(quota.valid, quota.updated_epoch,
                                                 now_epoch_, kQuotaFreshSeconds);
    snapshot_.quota.source[sizeof(snapshot_.quota.source) - 1] = '\0';
    MarkChangedLocked();
}

void DashboardData::SetWeatherRequestState(RequestState state) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot_.weather.request_state == state) return;
    snapshot_.weather.request_state = state;
    MarkChangedLocked();
}

void DashboardData::SetQuotaRequestState(RequestState state) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot_.quota.request_state == state) return;
    snapshot_.quota.request_state = state;
    MarkChangedLocked();
}

void DashboardData::UpdateFreshness(uint32_t now_epoch) {
    std::lock_guard<std::mutex> lock(mutex_);
    now_epoch_ = now_epoch;
    const auto weather = EvaluateFreshness(snapshot_.weather.valid, snapshot_.weather.updated_epoch,
                                           now_epoch, kWeatherFreshSeconds);
    const auto quota = EvaluateFreshness(snapshot_.quota.valid, snapshot_.quota.updated_epoch,
                                         now_epoch, kQuotaFreshSeconds);
    if (weather == snapshot_.weather.freshness && quota == snapshot_.quota.freshness) return;
    snapshot_.weather.freshness = weather;
    snapshot_.quota.freshness = quota;
    MarkChangedLocked();
}

void DashboardData::SetCustomCard(size_t index, const CustomCard& card) {
    if (index >= kCustomCardCount) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto& current = snapshot_.custom[index];
    if (current.enabled == card.enabled &&
        TextEquals(current.title, sizeof(current.title), card.title) &&
        TextEquals(current.value, sizeof(current.value), card.value) &&
        TextEquals(current.detail, sizeof(current.detail), card.detail)) {
        return;
    }
    snapshot_.custom[index] = card;
    snapshot_.custom[index].title[sizeof(snapshot_.custom[index].title) - 1] = '\0';
    snapshot_.custom[index].value[sizeof(snapshot_.custom[index].value) - 1] = '\0';
    snapshot_.custom[index].detail[sizeof(snapshot_.custom[index].detail) - 1] = '\0';
    MarkChangedLocked();
}

}  // namespace dashboard
