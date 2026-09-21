/* Generated from EegoRead a4_ui bitmap-font format; do not edit by hand. */
#pragma once
#include <stdint.h>

typedef struct { uint32_t offset; uint8_t width; } ui_glyph_t;
typedef struct { const uint8_t* bitmap; const ui_glyph_t* glyphs; const uint16_t* codepoints; uint16_t glyph_count; uint8_t height; uint8_t baseline; uint8_t bits_per_pixel; } ui_font_t;

extern const ui_font_t ui_font_small;
extern const ui_font_t ui_font_status;
extern const ui_font_t ui_font_body;
extern const ui_font_t ui_font_title;
extern const ui_font_t ui_font_h1;
extern const ui_font_t ui_font_clock;
