#!/usr/bin/env python3
"""Check the portrait UI geometry contract without building firmware.

This host-side check catches drift in the 480x800 logical coordinate system,
product-page touch hit boxes, safe areas, and diagnostic test buttons.
Navigation uses the cover touch strip; the on-screen rail is passive. It does not claim that pixels were observed on an SSD1677
panel.
"""

from __future__ import annotations

import ast
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "main" / "display" / "raw_display.cc"


def _constant(source: str, name: str) -> int:
    match = re.search(rf"constexpr\s+int\s+{re.escape(name)}\s*=\s*([^;]+);", source)
    if not match:
        raise AssertionError(f"missing constexpr {name}")
    expression = match.group(1).strip()
    return _expression(source, expression, name)


def _expression(source: str, expression: str, context: str = "expression") -> int:
    names = {
        key: _constant(source, key)
        for key in re.findall(r"\bk[A-Za-z0-9_]+\b", expression)
        if key != context and re.search(rf"constexpr\s+int\s+{re.escape(key)}\s*=", source)
    }
    try:
        tree = ast.parse(expression, mode="eval")
        allowed = (ast.Expression, ast.Constant, ast.Name, ast.Load, ast.BinOp, ast.Add,
                   ast.Sub, ast.Mult, ast.FloorDiv, ast.Div, ast.USub, ast.UAdd)
        if any(not isinstance(node, allowed) for node in ast.walk(tree)):
            raise ValueError
        return int(eval(compile(tree, "<geometry>", "eval"), {"__builtins__": {}}, names))
    except (SyntaxError, TypeError, ValueError, NameError, ZeroDivisionError) as exc:
        raise AssertionError(f"cannot resolve {context}={expression!r}") from exc


def _array(source: str, name: str) -> list[int]:
    match = re.search(rf"constexpr\s+int\s+{re.escape(name)}\[[^]]*\]\s*=\s*\{{([^}}]+)\}};", source)
    if not match:
        raise AssertionError(f"missing constexpr array {name}")
    values = []
    for token in match.group(1).split(","):
        token = token.strip()
        if token:
            values.append(_expression(source, token, name))
    return values


def _inside(box: tuple[int, int, int, int], width: int, height: int) -> bool:
    x, y, w, h = box
    return 0 <= x < width and 0 < w <= width - x and 0 <= y < height and 0 < h <= height - y


def main() -> int:
    source = SOURCE.read_text(encoding="utf-8")
    width = _constant(source, "kPortraitW")
    height = _constant(source, "kPortraitH")
    assert (width, height) == (480, 800), f"logical canvas changed to {width}x{height}"

    margin = _constant(source, "kMargin")
    content_width = _constant(source, "kContentWidth")
    assert content_width == width - margin * 2
    safe = (margin, 0, content_width, height)
    assert _inside(safe, width, height)

    home = {
        "weather": (margin, _constant(source, "kWeatherY"), content_width,
                    _constant(source, "kWeatherH")),
        "ai": (margin, _constant(source, "kHeroY"), content_width,
               _constant(source, "kHeroH")),
        "quota": (margin, _constant(source, "kQuotaY"), content_width,
                  _constant(source, "kQuotaH")),
    }
    for name, box in home.items():
        assert _inside(box, width, height), f"home {name} hit box outside canvas: {box}"
    assert home["weather"][1] + home["weather"][3] <= home["ai"][1]
    assert home["ai"][1] + home["ai"][3] <= home["quota"][1]
    custom = (margin, _constant(source, "kCustomY"), content_width,
              _constant(source, "kCustomH"))
    assert _inside(custom, width, height)
    assert not any(custom[1] < box[1] + box[3] and box[1] < custom[1] + custom[3]
                   for box in home.values())

    inset = _constant(source, "kUiInset")
    product_width = _constant(source, "kUiContentWidth")
    tile_x = _array(source, "kUiHomeX")
    tile_y = _array(source, "kUiHomeY")
    tile_w = _constant(source, "kUiHomeW")
    tile_h = _constant(source, "kUiHomeH")
    nav_y = _constant(source, "kUiHomeNavY")
    nav_h = _constant(source, "kUiHomeNavH")
    tiles = [(x,y,tile_w,tile_h) for x in tile_x for y in tile_y]
    nav = [(x,nav_y,tile_w,nav_h) for x in tile_x]
    assert inset == 32 and product_width == 416
    assert len(tiles) == 4 and len(nav) == 2
    assert all(_inside(box,width,height) for box in tiles + nav)
    assert tile_x[1] - tile_x[0] - tile_w >= 8
    assert tile_y[1] - tile_y[0] - tile_h >= 8
    assert tile_y[-1] + tile_h + 16 <= nav_y
    assert min(tile_w,tile_h,nav_h) >= 48
    rail_y = _constant(source, "kUiRailY")
    assert nav_y + nav_h < rail_y
    assert rail_y + 10 + 24 <= height - 12
    body_y = _constant(source, "kUiBodyY")
    for count, pitch_name, height_name in [(7, "kUiRowPitch", "kUiRowHeight"),
                                          (3, "kUiCardPitch", "kUiCardHeight")]:
        pitch = _constant(source, pitch_name)
        row_height = _constant(source, height_name)
        assert row_height >= 44 and pitch - row_height >= 8
        assert body_y + (count - 1) * pitch + row_height < rail_y
    ai_x = _array(source, "kAiActionX")
    ai_y = _array(source, "kAiActionY")
    ai_w = _constant(source, "kAiActionW")
    ai_h = _constant(source, "kAiActionH")
    assert ai_w >= 44 and ai_h >= 44
    assert all(_inside((x,y,ai_w,ai_h),width,height) for x in ai_x for y in ai_y)
    assert ai_x[1] - (ai_x[0] + ai_w) >= 8
    assert ai_y[1] - (ai_y[0] + ai_h) >= 8
    assert ai_y[-1] + ai_h < rail_y
    # Rendering and routing must both refer to the same named rectangles.
    tap_source = source[source.index("void RawDisplay::HandleHomeTap("):
                        source.index("bool RawDisplay::HandleHardwareKey(")]
    assert "in_rect(kUiHomeX[i % 2], kUiHomeY[i / 2], kUiHomeW, kUiHomeH)" in tap_source
    assert "in_rect(kUiHomeX[i], kUiHomeNavY, kUiHomeW, kUiHomeNavH)" in tap_source
    for box in [(344,160+row*112,104,48) for row in range(4)] + [(32,496,416,64),(32,584,416,64),(32,672,416,64)]:
        assert _inside(box,width,height) and min(box[2:]) >= 48
    home_render = source[source.index("void RawDisplay::DrawProductHomeLocked"):source.index("void RawDisplay::DrawProductAppIconLocked")]
    assert "FormatQuotaStatus" not in home_render and "FormatWeatherStatus" not in home_render
    apps_render = source[source.index("void RawDisplay::DrawProductAppsLocked"):source.index("void RawDisplay::DrawProductAiLocked")]
    assert '"闹钟", "日历", "录音", "小智"' in apps_render
    assert '"设置"' not in apps_render and '"更多"' not in apps_render
    assert "ProductRowAt(x, y," in tap_source
    assert "footer_index" not in tap_source

    test_x = _constant(source, "kTestButtonX")
    test_w = _constant(source, "kTestButtonW")
    test_h = _constant(source, "kTestButtonH")
    test_y = _array(source, "kTestButtonY")
    assert len(test_y) == 6 and all(_inside((test_x, y, test_w, test_h), width, height) for y in test_y)
    assert all(b - a >= test_h for a, b in zip(test_y, test_y[1:]))

    from render_ui_preview import function
    home_render = function(source, 'RawDisplay::DrawProductHomeLocked')
    row_render = function(source, 'RawDisplay::DrawProductIconRowLocked')
    assert 'StrokeRoundRect' not in home_render, 'Home navigation must remain borderless'
    assert 'FillRoundRect' not in row_render, 'Menu selection must not draw a box'
    assert 'navigation_focus_' in home_render and 'navigation_focus_' in row_render
    assert 'ProductPage::Weather' in source and 'HandleWeatherTap(x,y)' in source
    print("UI contract OK")
    print(f"logical={width}x{height} home_hit={home}")
    print(f"home_tiles={tiles} home_navigation={nav} passive_rail_y={rail_y}")
    print(f"cover_touch_y=900 test_console={[(test_x, y, test_w, test_h) for y in test_y]}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError) as exc:
        print(f"UI contract FAILED: {exc}", file=sys.stderr)
        raise SystemExit(1)
