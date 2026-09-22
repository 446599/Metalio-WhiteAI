#pragma once
#include <atomic>
#include <cstdint>
#include <limits>

namespace audio {
// Keep the lowest observed free-stack value across task lifetimes. UINT32_MAX
// means unmeasured internally: zero is a valid and important low-water sample.
class StackWatermark {
public:
    void Observe(uint32_t bytes) {
        auto old = minimum_.load(std::memory_order_relaxed);
        while (bytes < old && !minimum_.compare_exchange_weak(
            old, bytes, std::memory_order_relaxed, std::memory_order_relaxed)) {}
    }
    uint32_t FreeBytes() const {
        const auto value = minimum_.load(std::memory_order_relaxed);
        return value == kUnmeasured ? 0 : value;
    }
private:
    static constexpr uint32_t kUnmeasured = std::numeric_limits<uint32_t>::max();
    std::atomic<uint32_t> minimum_{kUnmeasured};
};
}
