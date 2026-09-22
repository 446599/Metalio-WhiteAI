#pragma once
#include "ai_ui_assets.h"
#include <cstdint>
#include <cstddef>

namespace raw_font {
struct Glyph {
    const uint8_t* bitmap = nullptr;
    uint16_t source_width = 0, source_height = 0;
    int width = 0, height = 0, x = 0, y = 0, advance = 0;
    uint8_t bpp = 1;
    bool missing = false;
};
// Native-size HarmonyOS Sans SC UI bitmaps are preferred; font_data supplies
// the remaining characters. Binary pixels only; no LVGL or runtime TTF engine.
bool Init();
bool InitMemory(const uint8_t* data, size_t size);
bool Ready();
uint32_t Count();
Glyph Lookup(const ui_font_t& font, uint32_t codepoint);
bool Pixel(const Glyph& glyph, int x, int y);
}  // namespace raw_font
