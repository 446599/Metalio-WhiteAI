#pragma once
#include <cstdint>
#include <string>

namespace reminders {
struct AlertSnapshot {
    uint32_t token = 0;
    bool active = false;
    bool audible = false;
    std::string title;
    std::string message;
    int64_t at = 0;
    size_t count = 0;
};
}  // namespace reminders
