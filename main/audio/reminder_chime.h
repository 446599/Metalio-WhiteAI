#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace audio {
// Quiet three-note chime, synthesized at the board's sample rate. Each note
// has a 20 ms attack and 80 ms release; there are no discontinuous square waves.
class ReminderChime {
public:
    static constexpr int kPeak = 3600;
    void Render(int16_t* output, int count, int rate) {
        constexpr double notes[] = {523.25, 659.25, 783.99};
        constexpr double pi = 3.14159265358979323846;
        for (int n = 0; n < count; ++n) {
            const double time = static_cast<double>(position_) / rate;
            const int note = static_cast<int>(time / 0.3);
            const double local = time - note * 0.3;
            double value = 0;
            if (note < 3 && local < 0.24) {
                const double envelope = std::min({1.0, local / 0.02, (0.24-local) / 0.08});
                value = kPeak * envelope * std::sin(2*pi*notes[note]*local);
            }
            output[n] = static_cast<int16_t>(value);
            if (++position_ >= static_cast<uint32_t>(rate * 3)) position_ = 0;
        }
        if (count) last_ = output[count-1];
    }
    // A touch/voice/TTS interruption fades the last sample to zero before the
    // next speaker owner writes. The remainder drains zeroes through the DMA.
    void FadeOut(int16_t* output, int count, int rate) {
        const int ramp = std::min(count, rate / 100);
        for (int n = 0; n < count; ++n) {
            output[n] = n < ramp ? static_cast<int16_t>(last_ * (ramp-1-n) / std::max(1,ramp)) : 0;
        }
        Reset();
    }
    void Reset() { position_ = 0; last_ = 0; }
private:
    uint32_t position_ = 0;
    int16_t last_ = 0;
};
}  // namespace audio
