#include "raw_font.h"
#include "font_loader.h"
#include <algorithm>

namespace raw_font {
namespace {
const ui_glyph_t* Embedded(const ui_font_t& font, uint32_t cp) {
    if(!font.bitmap || !font.codepoints || !font.glyphs) return nullptr;
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
    // The embedded face was rasterized from HarmonyOS Sans SC at each UI
    // size. Prefer it over scaling the universal 26px recovery/full-CJK pack.
    // This keeps the UI's existing line boxes, baseline and black/white output.
    const auto* embedded = Embedded(font, cp);
    if (embedded) {
        return {font.bitmap + embedded->offset, embedded->width, font.height,
                embedded->width, font.height, 0, 0, embedded->width + 1, font.bits_per_pixel, false};
    }
    GlyphInfo info{};
    if (&font != &ui_font_clock && Ready() && get_glyph(cp, 26, 1, &info)) {
        auto scale = [&](int value) { return (value * font.height + 16) / 32; };
        return {info.bitmap, info.width, info.height, scale(info.width), scale(info.height),
                scale(info.x_offset), scale(info.y_offset), scale(info.advance) + 1, 1, false};
    }
    // Unsupported characters are visible boxes, never invisible spaces.
    const int side = std::max(8, static_cast<int>(font.height) * 2 / 3);
    return {nullptr, 0, 0, side, side, 0, (font.height - side) / 2, side + 2, 1, true};
}
bool Pixel(const Glyph& glyph, int x, int y) {
    if (x < 0 || y < 0 || x >= glyph.width || y >= glyph.height) return false;
    if (glyph.missing) return x == 0 || y == 0 || x == glyph.width - 1 || y == glyph.height - 1;
    if (!glyph.bitmap || !glyph.source_width || !glyph.source_height ||
        (glyph.bpp != 1 && glyph.bpp != 2)) return false;
    const int stride = (glyph.source_width * glyph.bpp + 7) / 8;
    auto sample = [&](int sx, int sy) {
        const uint8_t* row = glyph.bitmap + sy * stride;
        return glyph.bpp == 1 ? ((row[sx / 8] & (0x80 >> (sx % 8))) ? 3 : 0) :
            ((row[sx / 4] >> (6 - 2 * (sx % 4))) & 3);
    };
    if (glyph.width == glyph.source_width && glyph.height == glyph.source_height)
        return sample(x, y) >= 2;
    // Exact integer box coverage for the uncommon full-pack fallback glyphs.
    // Sampling only a single nearest neighbour dropped thin strokes when
    // shrinking. Coverage is thresholded ONCE: no gray pixels or dithering.
    const int left=x*glyph.source_width, right=(x+1)*glyph.source_width;
    const int top=y*glyph.source_height, bottom=(y+1)*glyph.source_height;
    int64_t ink=0;
    for (int sy=top/glyph.height;sy<(bottom+glyph.height-1)/glyph.height;++sy) {
        const int wy=std::min(bottom,(sy+1)*glyph.height)-std::max(top,sy*glyph.height);
        for (int sx=left/glyph.width;sx<(right+glyph.width-1)/glyph.width;++sx) {
            const int wx=std::min(right,(sx+1)*glyph.width)-std::max(left,sx*glyph.width);
            ink+=int64_t(sample(sx,sy))*wx*wy;
        }
    }
    return ink*2 >= int64_t(3)*glyph.source_width*glyph.source_height;
}
}  // namespace raw_font
