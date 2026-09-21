#include "raw_font.h"
#include "font_loader.h"
#include <algorithm>

namespace raw_font {
namespace {
const ui_glyph_t* Embedded(const ui_font_t& font, uint32_t cp) {
    uint16_t lo = 0, hi = font.glyph_count;
    while (lo < hi) {
        const uint16_t mid = lo + (hi - lo) / 2;
        if (font.codepoints[mid] < cp) lo = mid + 1;
        else hi = mid;
    }
    return lo < font.glyph_count && font.codepoints[lo] == cp ? &font.glyphs[lo] : nullptr;
}
}
bool Init() { return font_loader_is_ready() || font_loader_init_flash("font_data") == FONT_LOADER_OK; }
bool InitMemory(const uint8_t* data, size_t size) { return font_loader_init_memory(data, size) == FONT_LOADER_OK; }
bool Ready() { return font_loader_is_ready(); }
uint32_t Count() { return font_loader_item_count(); }
Glyph Lookup(const ui_font_t& font, uint32_t cp) {
    GlyphInfo info{};
    if (&font != &ui_font_clock && Ready() && get_glyph(cp, 26, 1, &info)) {
        auto scale = [&](int value) { return (value * font.height + 16) / 32; };
        return {info.bitmap, info.width, info.height, scale(info.width), scale(info.height),
                scale(info.x_offset), scale(info.y_offset), scale(info.advance) + 1, 1, false};
    }
    const auto* embedded = Embedded(font, cp);
    if (embedded) {
        return {font.bitmap + embedded->offset, embedded->width, font.height,
                embedded->width, font.height, 0, 0, embedded->width + 1, font.bits_per_pixel, false};
    }
    // Unsupported characters are visible boxes, never invisible spaces.
    const int side = std::max(8, static_cast<int>(font.height) * 2 / 3);
    return {nullptr, 0, 0, side, side, 0, (font.height - side) / 2, side + 2, 1, true};
}
bool Pixel(const Glyph& glyph, int x, int y) {
    if (glyph.missing) return x == 0 || y == 0 || x == glyph.width - 1 || y == glyph.height - 1;
    if (!glyph.bitmap || glyph.width <= 0 || glyph.height <= 0) return false;
    const int sx = x * glyph.source_width / glyph.width;
    const int sy = y * glyph.source_height / glyph.height;
    const int stride = (glyph.source_width * glyph.bpp + 7) / 8;
    const uint8_t* row = glyph.bitmap + sy * stride;
    if (glyph.bpp == 1) return (row[sx / 8] & (0x80 >> (sx % 8))) != 0;
    return ((row[sx / 4] >> (6 - 2 * (sx % 4))) & 3) >= 2;
}
}  // namespace raw_font
