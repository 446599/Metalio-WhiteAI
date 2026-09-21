#pragma once
#include <cstdint>

namespace audio {
enum class RecorderMode : uint8_t { Idle, Recording, Playing, Loading };
struct RecorderSnapshot {
    RecorderMode mode = RecorderMode::Idle;
    uint32_t seconds = 0;
    uint32_t revision = 0;
    bool has_clip = false;
    bool failed = false;
    bool saved = false;
};
}  // namespace audio
