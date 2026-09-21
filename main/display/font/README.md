# Raw bitmap UI fonts

`ai_ui_assets.c/.h` are converted 2bpp bitmap assets following the raw
framebuffer format used by the sibling EegoRead project.  Each font contains a
sorted UTF-8 codepoint table, glyph offsets, and MSB-first coverage rows. The
renderer applies a fixed midpoint threshold when writing the SSD1677 1bpp
framebuffer, so there is no LVGL dependency and no runtime font rasterizer.

`generate_assets.py` documents the conversion recipe. It reads the sibling
EegoRead HarmonyOS Sans source font and emits only the dashboard/UI charset;
the generated C file is checked in so device builds do not need Python or a
font package.
