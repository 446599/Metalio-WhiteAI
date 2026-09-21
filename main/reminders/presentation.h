#pragma once
#include "reminder_store.h"
#include <algorithm>

namespace reminders {
inline int64_t UpcomingOccurrence(const Item& item, int64_t after) {
    int64_t next = 0;
    if (item.enabled) {
        next = item.weekdays ? NextOccurrence(item, std::max(after, item.at - 1)) :
                             item.at > after ? item.at : 0;
    }
    if (item.snoozed_until > after && (!next || item.snoozed_until < next)) next = item.snoozed_until;
    return next;
}
inline int64_t OccurrenceOnDay(const Item& item, int64_t begin) {
    if (item.kind != "event") return 0;
    const auto next = item.weekdays ? (item.enabled ? NextOccurrence(item, begin - 1) : 0) : item.at;
    return next >= begin && next < begin + 86400 ? next : 0;
}
}  // namespace reminders
