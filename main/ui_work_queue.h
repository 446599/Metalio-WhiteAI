#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>

// Input producers never wait for e-paper. Reject overflow explicitly rather
// than accumulating an unbounded number of delayed taps.
class UiWorkQueue {
public:
    static constexpr size_t kCapacity = 8;
    bool Push(std::function<void()> task) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (count_ == kCapacity) { ++dropped_; return false; }
        tasks_[(head_ + count_) % kCapacity] = std::move(task);
        ++count_;
        return true;
    }
    bool Pop(std::function<void()>& task) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!count_) return false;
        task = std::move(tasks_[head_]);
        tasks_[head_] = {};
        head_ = (head_ + 1) % kCapacity;
        --count_;
        return true;
    }
    uint32_t Dropped() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }
private:
    mutable std::mutex mutex_;
    std::array<std::function<void()>, kCapacity> tasks_{};
    size_t head_ = 0;
    size_t count_ = 0;
    uint32_t dropped_ = 0;
};
