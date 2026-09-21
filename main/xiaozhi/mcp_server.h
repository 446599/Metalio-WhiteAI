#pragma once
#include "reminders/reminder_store.h"
#include <deque>
#include <functional>
#include <string>

namespace xiaozhi {
class SystemTools;
// Xiaozhi's JSON-RPC payload, independent of the websocket and hardware.
class McpServer {
public:
    McpServer(reminders::Store& store, std::function<bool()> stop, SystemTools* system=nullptr)
        : store_(store), stop_(std::move(stop)), system_(system) {}
    std::string Handle(const std::string& payload, int64_t now);
    void Reset() { cache_.clear(); }
private:
    struct Cached { std::string id, request, response; };
    reminders::Store& store_;
    std::function<bool()> stop_;
    SystemTools* system_;
    std::deque<Cached> cache_;
};
}  // namespace xiaozhi
