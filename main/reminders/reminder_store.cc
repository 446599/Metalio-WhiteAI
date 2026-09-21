#include "reminder_store.h"
#include <cJSON.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>

namespace reminders {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
bool Number(const cJSON* obj, const char* key, int64_t low, int64_t high, int64_t& value) {
    const auto* n = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(n) || !std::isfinite(n->valuedouble) ||
        n->valuedouble < low || n->valuedouble > high || std::trunc(n->valuedouble) != n->valuedouble) return false;
    value = static_cast<int64_t>(n->valuedouble); return true;
}
bool ValidItem(const Item& i) {
    return (i.kind == "alarm" || i.kind == "event") && !i.title.empty() && i.title.size() <= 96 &&
           i.title.find('\0') == std::string::npos &&
           std::none_of(i.title.begin(), i.title.end(), [](unsigned char c) { return c < 32; }) &&
           i.at >= kMinEpoch && i.at < kMaxEpoch && i.weekdays <= 127 &&
           i.duration_minutes >= 0 && i.duration_minutes <= 1440 &&
           i.last_fired >= 0 && i.last_fired < kMaxEpoch &&
           (i.snoozed_until == 0 || ValidClock(i.snoozed_until));
}
std::string Encode(const std::vector<Item>& items, uint32_t next_id) {
    Json root(cJSON_CreateObject(), cJSON_Delete);
    if (!root || !cJSON_AddNumberToObject(root.get(), "schema", 1) ||
        !cJSON_AddNumberToObject(root.get(), "next_id", next_id)) return {};
    auto* array = cJSON_AddArrayToObject(root.get(), "items");
    if (!array) return {};
    for (const auto& i : items) {
        auto* entry = cJSON_CreateObject();
        if (!entry) return {};
        cJSON_AddItemToArray(array, entry);
        if (!cJSON_AddNumberToObject(entry, "id", i.id) ||
            !cJSON_AddStringToObject(entry, "kind", i.kind.c_str()) ||
            !cJSON_AddStringToObject(entry, "title", i.title.c_str()) ||
            !cJSON_AddNumberToObject(entry, "at", static_cast<double>(i.at)) ||
            !cJSON_AddNumberToObject(entry, "weekdays", i.weekdays) ||
            !cJSON_AddNumberToObject(entry, "duration", i.duration_minutes) ||
            !cJSON_AddBoolToObject(entry, "enabled", i.enabled) ||
            !cJSON_AddNumberToObject(entry, "last_fired", static_cast<double>(i.last_fired)) ||
            !cJSON_AddNumberToObject(entry, "snoozed_until", static_cast<double>(i.snoozed_until))) return {};
    }
    char* text = cJSON_PrintUnformatted(root.get());
    if (!text) return {};
    std::string result(text); cJSON_free(text); return result;
}
}  // namespace

bool ValidClock(int64_t now) { return now >= kMinEpoch && now < kMaxEpoch; }
bool ParseLocalTime(const std::string& text, int64_t& epoch) {
    if (text.size() != 16 && text.size() != 19) return false;
    for (size_t i = 0; i < text.size(); ++i) {
        if (i == 4 || i == 7) { if (text[i] != '-') return false; }
        else if (i == 10) { if (text[i] != ' ' && text[i] != 'T') return false; }
        else if (i == 13 || i == 16) { if (text[i] != ':') return false; }
        else if (text[i] < '0' || text[i] > '9') return false;
    }
    int y, m, d, h, minute, second = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%d%*c%d:%d:%d", &y, &m, &d, &h, &minute, &second) < 5) return false;
    if (y < 2024 || y > 2099 || m < 1 || m > 12 || d < 1 || d > 31 ||
        h > 23 || minute > 59 || second > 59) return false;
    struct tm local{};
    local.tm_year = y - 1900; local.tm_mon = m - 1; local.tm_mday = d;
    local.tm_hour = h; local.tm_min = minute; local.tm_sec = second; local.tm_isdst = -1;
    const time_t value = mktime(&local);
    // mktime normalizes impossible dates; reject rather than silently reschedule.
    if (local.tm_year != y - 1900 || local.tm_mon != m - 1 || local.tm_mday != d ||
        local.tm_hour != h || local.tm_min != minute || local.tm_sec != second || !ValidClock(value)) return false;
    epoch = value; return true;
}
std::string LocalTime(int64_t epoch) {
    const time_t value = epoch; struct tm local{}; char text[24]{};
    if (!localtime_r(&value, &local)) return {};
    strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local); return text;
}
int64_t NextOccurrence(const Item& item, int64_t after) {
    if (!item.weekdays) return 0;
    time_t anchor = item.at, current = after;
    struct tm original{}, day{};
    if (!localtime_r(&anchor, &original) || !localtime_r(&current, &day)) return 0;
    day.tm_hour = original.tm_hour; day.tm_min = original.tm_min; day.tm_sec = original.tm_sec;
    for (int n = 0; n <= 7; ++n) {
        struct tm candidate = day; candidate.tm_mday += n; candidate.tm_isdst = -1;
        const time_t at = mktime(&candidate);
        const int weekday = (candidate.tm_wday + 6) % 7;
        if (at > after && ValidClock(at) && (item.weekdays & (1U << weekday))) return at;
    }
    return 0;
}
bool Store::Restore(const std::string& json) {
    std::lock_guard<std::mutex> lock(mutex_);
    ready_ = false;
    if (json.empty()) { items_.clear(); next_id_ = 1; ready_ = true; ++revision_; return true; }
    if (json.size() > 8192) return false;
    const char* end = nullptr;
    Json root(cJSON_ParseWithLengthOpts(json.c_str(), json.size() + 1, &end, true), cJSON_Delete);
    int64_t schema, next;
    if (!root || !Number(root.get(), "schema", 1, 1, schema) ||
        !Number(root.get(), "next_id", 1, UINT32_MAX, next)) return false;
    const auto* array = cJSON_GetObjectItemCaseSensitive(root.get(), "items");
    if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) > static_cast<int>(kMaxItems)) return false;
    std::vector<Item> loaded;
    const cJSON* entry;
    cJSON_ArrayForEach(entry, array) {
        Item i; int64_t id, at, days, duration, fired;
        const auto* kind = cJSON_GetObjectItemCaseSensitive(entry, "kind");
        const auto* title = cJSON_GetObjectItemCaseSensitive(entry, "title");
        const auto* enabled = cJSON_GetObjectItemCaseSensitive(entry, "enabled");
        if (!cJSON_IsString(kind) || !cJSON_IsString(title) || !cJSON_IsBool(enabled) ||
            !Number(entry, "id", 1, UINT32_MAX - 1, id) || id >= next ||
            !Number(entry, "at", kMinEpoch, kMaxEpoch - 1, at) ||
            !Number(entry, "weekdays", 0, 127, days) || !Number(entry, "duration", 0, 1440, duration) ||
            !Number(entry, "last_fired", 0, kMaxEpoch - 1, fired)) return false;
        i.id = id; i.kind = kind->valuestring; i.title = title->valuestring; i.at = at;
        i.weekdays = days; i.duration_minutes = duration; i.enabled = cJSON_IsTrue(enabled); i.last_fired = fired;
        if (cJSON_GetObjectItemCaseSensitive(entry, "snoozed_until") &&
            !Number(entry, "snoozed_until", 0, kMaxEpoch - 1, i.snoozed_until)) return false;
        if (!ValidItem(i) || std::any_of(loaded.begin(), loaded.end(), [&](const Item& other) { return other.id == i.id; })) return false;
        loaded.push_back(std::move(i));
    }
    items_ = std::move(loaded); next_id_ = next; ready_ = true; ++revision_; return true;
}
std::vector<Item> Store::List() const {
    std::lock_guard<std::mutex> lock(mutex_); auto result = items_;
    std::sort(result.begin(), result.end(), [](const Item& a, const Item& b) {
        const auto at = a.snoozed_until ? a.snoozed_until : a.at;
        const auto bt = b.snoozed_until ? b.snoozed_until : b.at;
        return at != bt ? at < bt : a.id < b.id;
    });
    return result;
}
uint32_t Store::Revision() const { std::lock_guard<std::mutex> lock(mutex_); return revision_; }
bool Store::Ready() const { std::lock_guard<std::mutex> lock(mutex_); return ready_; }
bool Store::Commit(const std::vector<Item>& items, uint32_t next_id) {
    const auto json = Encode(items, next_id);
    if (json.empty() || json.size() > 8192 || !save_(json)) return false;
    items_ = items; next_id_ = next_id; ++revision_; return true;
}
bool Store::Put(Item item, int64_t now, Item& saved, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_) { error = "提醒存储不可用，请检查设备"; return false; }
    if (!ValidClock(now)) { error = "设备时间未校准，请先联网对时"; return false; }
    if (!ValidItem(item)) { error = "提醒参数无效，标题最多96字节，日期需在2024至2099年"; return false; }
    if (item.enabled && item.at <= now) { error = "提醒时间必须晚于当前设备时间"; return false; }
    if (item.weekdays) {
        const time_t at = item.at; struct tm local{}; localtime_r(&at, &local);
        if (!(item.weekdays & (1U << ((local.tm_wday + 6) % 7)))) {
            item.at = NextOccurrence(item, item.at - 1);
            if (!item.at) { error = "无法计算下一次重复日期"; return false; }
        }
    }
    auto next = items_; uint32_t next_id = next_id_;
    if (item.id) {
        const auto found = std::find_if(next.begin(), next.end(), [&](const Item& i) { return i.id == item.id; });
        if (found == next.end()) { error = "未找到该提醒，请先查询列表"; return false; }
        item.last_fired = found->last_fired;
        item.snoozed_until = 0;
        *found = item;
    } else {
        for (const auto& i : next) {
            if (i.enabled && i.kind == item.kind && i.title == item.title && i.at == item.at &&
                i.weekdays == item.weekdays && i.duration_minutes == item.duration_minutes) { saved = i; return true; }
        }
        if (next.size() >= kMaxItems || next_id == UINT32_MAX) { error = "提醒已满，请先删除旧条目"; return false; }
        item.id = next_id++; next.push_back(item);
    }
    if (!Commit(next, next_id)) { error = "保存失败，设备存储空间可能不足，未创建或修改提醒"; return false; }
    saved = item; return true;
}
bool Store::Remove(uint32_t id, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_) { error = "提醒存储不可用"; return false; }
    auto next = items_;
    const auto found = std::find_if(next.begin(), next.end(), [id](const Item& i) { return i.id == id; });
    if (found == next.end()) { error = "未找到该提醒，请先查询列表"; return false; }
    next.erase(found);
    if (!Commit(next, next_id_)) { error = "删除保存失败，原提醒保持不变"; return false; }
    return true;
}
bool Store::Snooze(const std::vector<uint32_t>& ids, int64_t now, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_ || !ValidClock(now) || !ValidClock(now + 300) || ids.empty()) {
        error = "稍后提醒失败，请检查设备时间和存储";
        return false;
    }
    auto next = items_;
    for (const auto id : ids) {
        const auto found = std::find_if(next.begin(), next.end(), [id](const Item& i) { return i.id == id; });
        if (found == next.end()) { error = "提醒已被删除，请选择停止"; return false; }
        found->snoozed_until = now + 300;
    }
    if (!Commit(next, next_id_)) { error = "保存失败，闹钟仍在提醒"; return false; }
    return true;
}

bool Store::TakeDue(int64_t now, std::vector<Item>& due) {
    due.clear(); std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_ || !ValidClock(now)) return false;
    auto next = items_; bool changed = false; std::vector<Item> fired;
    for (auto& i : next) {
        bool snooze_fired = false;
        if (i.snoozed_until && i.snoozed_until <= now) {
            if (now - i.snoozed_until <= 300) {
                Item alert = i;
                alert.at = i.snoozed_until;
                fired.push_back(std::move(alert));
                snooze_fired = true;
            }
            i.snoozed_until = 0;
            changed = true;
        }
        if (!i.enabled || i.at > now) continue;
        changed = true;
        // A long outage must not emit every missed alarm. Recent due items get
        // one alert; older ones expire/advance, without a catch-up storm.
        // A repeating alarm may have been offline for several days. Recover
        // today's occurrence inside the grace window, not just the old anchor.
        int64_t occurrence = i.at;
        if (i.weekdays && now - i.at > 300) {
            occurrence = NextOccurrence(i, now - 301);
        }
        if (occurrence >= i.at && occurrence <= now && now - occurrence <= 300 &&
            occurrence > i.last_fired) {
            Item alert = i;
            alert.at = occurrence;
            if (!snooze_fired) fired.push_back(std::move(alert));
            i.last_fired = occurrence;
        }
        if (i.weekdays) {
            const int64_t at = NextOccurrence(i, std::max(now, i.last_fired));
            if (at) i.at = at; else i.enabled = false;
        } else i.enabled = false;
    }
    if (changed && !Commit(next, next_id_)) return false;
    due = std::move(fired); return true;
}
}  // namespace reminders
