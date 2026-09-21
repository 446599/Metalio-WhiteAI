#pragma once

#include <cJSON.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dashboard {

enum class QuotaUnit { Percent, Ratio };

inline bool RemainingPercent(double value, QuotaUnit unit, bool used, int16_t& result) {
    const double maximum = unit == QuotaUnit::Ratio ? 1.0 : 100.0;
    // Validate before subtraction, multiplication, rounding or integer conversion.
    if (!std::isfinite(value) || value < 0.0 || value > maximum) return false;
    if (used) value = maximum - value;
    if (unit == QuotaUnit::Ratio) value *= 100.0;
    result = static_cast<int16_t>(std::lround(value));
    return true;
}

namespace json_detail {
struct QuotaField {
    const char* name;
    QuotaUnit unit;
};

// Legacy *_remaining aliases have one fixed unit: percentage points. Ratios
// require an explicit *_remaining_ratio field. Bare five_hour/weekly values
// have no unit contract and are deliberately not accepted.
inline constexpr QuotaField kFiveFields[] = {
    {"five_hour_remaining", QuotaUnit::Percent}, {"fiveHourRemaining", QuotaUnit::Percent},
    {"primary_remaining", QuotaUnit::Percent}, {"primaryRemaining", QuotaUnit::Percent},
    {"five_hour_remaining_percent", QuotaUnit::Percent},
    {"five_hour_remaining_ratio", QuotaUnit::Ratio},
};
inline constexpr QuotaField kWeeklyFields[] = {
    {"weekly_remaining", QuotaUnit::Percent}, {"weeklyRemaining", QuotaUnit::Percent},
    {"secondary_remaining", QuotaUnit::Percent}, {"secondaryRemaining", QuotaUnit::Percent},
    {"weekly_remaining_percent", QuotaUnit::Percent},
    {"weekly_remaining_ratio", QuotaUnit::Ratio},
};

inline const cJSON* ObjectItem(const cJSON* node, const char* key) {
    return cJSON_IsObject(node) ? cJSON_GetObjectItemCaseSensitive(node, key) : nullptr;
}

template <size_t N>
inline bool ReadField(const cJSON* object, const QuotaField (&fields)[N],
                      bool& found, int16_t& result) {
    found = false;
    for (const cJSON* child = object->child; child != nullptr; child = child->next) {
        if (child->string == nullptr) continue;
        for (const auto& field : fields) {
            if (std::strcmp(child->string, field.name) != 0) continue;
            if (found || !cJSON_IsNumber(child) ||
                !RemainingPercent(child->valuedouble, field.unit, false, result)) return false;
            found = true;
        }
    }
    return true;
}

inline bool ReadWindow(const cJSON* window, int16_t& result) {
    if (!cJSON_IsObject(window)) return false;
    const cJSON* value = nullptr;
    for (const cJSON* child = window->child; child != nullptr; child = child->next) {
        if (child->string != nullptr && std::strcmp(child->string, "used_percent") == 0) {
            if (value != nullptr) return false;
            value = child;
        }
    }
    return cJSON_IsNumber(value) &&
           RemainingPercent(value->valuedouble, QuotaUnit::Percent, true, result);
}

inline bool FindQuota(const cJSON* node, unsigned depth, unsigned& count,
                      int16_t& five, int16_t& weekly) {
    if (node == nullptr || depth > 3) return true;
    if (!cJSON_IsObject(node) && !cJSON_IsArray(node)) return true;
    if (cJSON_IsObject(node)) {
        bool have_five = false, have_weekly = false;
        int16_t candidate_five = 0, candidate_weekly = 0;
        if (!ReadField(node, kFiveFields, have_five, candidate_five) ||
            !ReadField(node, kWeeklyFields, have_weekly, candidate_weekly)) return false;
        // Never combine windows from unrelated nested account objects.
        if (have_five != have_weekly) return false;
        if (have_five) {
            if (++count != 1) return false;
            five = candidate_five;
            weekly = candidate_weekly;
        }
        const cJSON* rate_limit = ObjectItem(node, "rate_limit");
        if (rate_limit != nullptr) {
            if (!ReadWindow(ObjectItem(rate_limit, "primary_window"), candidate_five) ||
                !ReadWindow(ObjectItem(rate_limit, "secondary_window"), candidate_weekly) ||
                ++count != 1) return false;
            five = candidate_five;
            weekly = candidate_weekly;
        }
    }
    for (const cJSON* child = node->child; child != nullptr; child = child->next) {
        if (!FindQuota(child, depth + 1, count, five, weekly)) return false;
    }
    return true;
}
}  // namespace json_detail

// Accept one complete, unambiguous pair. On failure leave both outputs intact.
inline bool ParseQuotaRemaining(const cJSON* root, int16_t& five, int16_t& weekly) {
    unsigned count = 0;
    int16_t parsed_five = 0, parsed_weekly = 0;
    if (!json_detail::FindQuota(root, 0, count, parsed_five, parsed_weekly) || count != 1) return false;
    five = parsed_five;
    weekly = parsed_weekly;
    return true;
}

}  // namespace dashboard
