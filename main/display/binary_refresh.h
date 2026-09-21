#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace epaper {

struct BinaryDamage {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    size_t changed_bytes = 0;
};

// Match the outline cleanup radius used by the LVGL panel path. Only pixels
// that should be white are scrubbed; the target image is never dilated.
constexpr int kBinaryOutlineRadius = 3;

// Coordinates are in the panel's native orientation. SSD1677 windows must
// start/end on byte boundaries; unchanged bytes inside the bounds are safe.
inline BinaryDamage FindBinaryDamage(const uint8_t* previous, const uint8_t* current,
                                     int width, int height, int padding = 0) {
    const int stride = width / 8;
    int left = stride, right = -1, top = height, bottom = -1;
    BinaryDamage damage;
    for (int y = 0; y < height; ++y) {
        for (int byte = 0; byte < stride; ++byte) {
            const size_t offset = static_cast<size_t>(y) * stride + byte;
            if (previous[offset] == current[offset]) continue;
            ++damage.changed_bytes;
            left = std::min(left, byte);
            right = std::max(right, byte);
            top = std::min(top, y);
            bottom = y;
        }
    }
    if (damage.changed_bytes) {
        damage.x = std::max(0, left * 8 - padding) & ~7;
        damage.y = std::max(0, top - padding);
        damage.width = std::min(width, ((right + 1) * 8 + padding + 7) & ~7) - damage.x;
        damage.height = std::min(height, bottom + padding + 1) - damage.y;
    }
    return damage;
}

// A digital white->white transition does not erase optical residue around an
// old black glyph. Give those white neighbours a black->white transition in
// the transmitted PREVIOUS plane, in the same activation as the new image.
// Keep the committed history immutable, including when a transfer fails.
inline void CopyPreviousWithOutlineCleanup(uint8_t* window, const uint8_t* previous,
                                          const uint8_t* current, int width, int height,
                                          int x, int y, int w, int h) {
    const int stride = width / 8;
    const int row_bytes = w / 8;
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < row_bytes; ++col) {
            const int byte_x = x / 8 + col;
            uint32_t erased = 0;
            for (int yy = std::max(0, y + row - kBinaryOutlineRadius);
                 yy <= std::min(height - 1, y + row + kBinaryOutlineRadius); ++yy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int bx = byte_x + dx;
                    if (bx < 0 || bx >= stride) continue;
                    const size_t offset = static_cast<size_t>(yy) * stride + bx;
                    erased |= static_cast<uint32_t>(static_cast<uint8_t>(~previous[offset]) &
                                                    current[offset]) << ((1 - dx) * 8);
                }
            }
            uint32_t halo = erased;
            for (int shift = 1; shift <= kBinaryOutlineRadius; ++shift) {
                halo |= (erased << shift) | (erased >> shift);
            }
            const size_t offset = static_cast<size_t>(y + row) * stride + byte_x;
            const uint8_t white_cleanup = static_cast<uint8_t>(halo >> 8) & current[offset];
            window[row * row_bytes + col] = previous[offset] & static_cast<uint8_t>(~white_cleanup);
        }
    }
}

// Small clock/selection updates must not spend a whole page's cleanup budget.
// Retain cleanup after eight frame-equivalents of changed bytes, or at most
// 64 differential updates even when the same small area changes repeatedly.
inline bool NeedsBinaryCleanup(bool history_valid, uint32_t partial_updates,
                               size_t changed_bytes, size_t frame_bytes) {
    return !history_valid || partial_updates >= 64 || changed_bytes >= frame_bytes * 8;
}

}  // namespace epaper
