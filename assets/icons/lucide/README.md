# Lucide for the monochrome UI

Upstream: https://github.com/lucide-icons/lucide

Pinned commit and original file SHA-256 values are in `source.json`. The complete
upstream license is retained in `LICENSE` (ISC, with MIT notices for Feather-derived
icons). Original SVG geometry is unchanged.

`tools/build_ui_icons.py` defaults to **CairoSVG 2.8.2** (optional resvg-js 2.6.2), fills the
background white, resolves `currentColor` to black, and thresholds at 160/255.
24, 28, 32 and 40 px variants have native pixels rather than runtime scaling.
The output `main/display/icons/lucide_icons.h` is compiled into flash; no SVG
renderer or npm dependency runs on the ESP32.

Rebuild with `pip install Pillow CairoSVG==2.8.2`, then `python3 tools/build_ui_icons.py`.
The optional alternative uses Node and a local installation of the pinned resvg:

```sh
python3 tools/build_ui_icons.py --resvg /absolute/path/to/node_modules/@resvg/resvg-js
```

The battery uses its actual raster bounds and the digit glyph's visible bounds
to align their vertical centers. Percentage is exact; bars indicate ranges.
