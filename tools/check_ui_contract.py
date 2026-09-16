#!/usr/bin/env python3
"""Check the portrait UI geometry contract without building firmware.

This host-side check catches drift in the 480x800 logical coordinate system,
home-page touch hit boxes, the reserved product-page footer, and legacy test
buttons. It does not claim that pixels were observed on an SSD1677 panel.
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
    footer_w = _constant(source, "kUiFooterButtonWidth")
    footer_h = _constant(source, "kUiFooterButtonHeight")
    gap = _constant(source, "kUiFooterGap")
    footer_x = _array(source, "kUiFooterX")
    footer_y = _array(source, "kUiFooterRowY")
    assert footer_x == [inset, inset + footer_w + gap]
    assert footer_y == [_constant(source, "kUiFooterY"), _constant(source, "kUiFooterY") + footer_h + gap]
    footer_boxes = [(x, y, footer_w, footer_h) for y in footer_y for x in footer_x]
    assert all(_inside(box, width, height) for box in footer_boxes)
    assert footer_boxes[-1][1] + footer_h == 784  # 16 px bottom safe area

    test_x = _constant(source, "kTestButtonX")
    test_w = _constant(source, "kTestButtonW")
    test_h = _constant(source, "kTestButtonH")
    test_y = _array(source, "kTestButtonY")
    assert len(test_y) == 6 and all(_inside((test_x, y, test_w, test_h), width, height) for y in test_y)
    assert all(b - a >= test_h for a, b in zip(test_y, test_y[1:]))

    print("UI contract OK")
    print(f"logical={width}x{height} home_hit={home}")
    print(f"product_footer={footer_boxes} test_console={[(test_x, y, test_w, test_h) for y in test_y]}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError) as exc:
        print(f"UI contract FAILED: {exc}", file=sys.stderr)
        raise SystemExit(1)
