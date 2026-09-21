#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace reminders {
constexpr size_t kMaxItems = 16;
constexpr int64_t kMinEpoch = 1704067200;
constexpr int64_t kMaxEpoch = 4102444800;
struct Item {
    uint32_t id = 0;
    std::string kind = "alarm";
    std::string title;
    int64_t at = 0;
    // Monday bit 0 ... Sunday bit 6; zero means one-shot.
    uint8_t weekdays = 0;
    int duration_minutes = 0;
    bool enabled = true;
    int64_t last_fired = 0;
    // Independent of the recurring anchor: snooze never moves tomorrow's alarm.
    int64_t snoozed_until = 0;
};
bool ValidClock(int64_t now);
bool ParseLocalTime(const std::string& text, int64_t& epoch);
std::string LocalTime(int64_t epoch);
int64_t NextOccurrence(const Item& item, int64_t after);

// Persistence is injected so the scheduler and failure paths run on the host.
// A mutation becomes visible only after the complete snapshot is committed.
class Store {
public:
    using Save = std::function<bool(const std::string&)>;
    explicit Store(Save save) : save_(std::move(save)) {}
    bool Restore(const std::string& json);
    std::vector<Item> List() const;
    uint32_t Revision() const;
    bool Ready() const;
    bool Put(Item item, int64_t now, Item& saved, std::string& error);
    bool Remove(uint32_t id, std::string& error);
    bool Snooze(const std::vector<uint32_t>& ids, int64_t now, std::string& error);
    bool TakeDue(int64_t now, std::vector<Item>& due);
private:
    bool Commit(const std::vector<Item>& items, uint32_t next_id);
    mutable std::mutex mutex_;
    Save save_;
    std::vector<Item> items_;
    uint32_t next_id_ = 1;
    uint32_t revision_ = 0;
    bool ready_ = false;
};
}  // namespace reminders
