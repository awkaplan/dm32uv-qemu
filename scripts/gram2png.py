#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright (c) 2026 Alex Kaplan
"""Render a DM32_GRAMDUMP raw framebuffer capture as a PNG.

A GRAM capture is little-endian RGB565 with a 256-pixel row stride. The visible panel is the
top-left 240x320 of GRAM, upright. Views:

  panel      the 240x320 visible panel, colours expanded to the full 0..255 range (default)
  gram       the same 240x320 area, colours expanded by bit shifting, as screenshot.py did;
             apart from colour expansion it is identical to panel
  gram-full  all 256 columns of GRAM (columns 240..255 lie outside the panel)

usage example:
    python3 scripts/gram2png.py --view panel --scale 3 gram.raw panel.png
"""

import argparse
import sys
from typing import Callable, List, Sequence, Tuple

import dm32fb

VIEWS = ("panel", "gram", "gram-full")
DEFAULT_SCALE = {"panel": 3, "gram": 2, "gram-full": 3}
# zlib level per view. The gram view has always used 6, so its PNG bytes do not change.
COMPRESS_LEVEL = {"panel": 9, "gram": 6, "gram-full": 9}

Rendered = Tuple[List[bytes], List[str]]


def render_panel(values: Sequence[int], stride: int, scale: int, output: str) -> Rendered:
    """The visible 240x320 panel, upright, with full-range colour expansion."""
    rows = dm32fb.split_rows(values, stride)
    decoded = [[dm32fb.rgb565_to_rgb888(value) for value in row]
               for row in dm32fb.to_panel(rows)]
    return _summarize_full_image(decoded, scale, output)


def render_gram_full(values: Sequence[int], stride: int, scale: int, output: str) -> Rendered:
    """All 256 columns of GRAM, unrotated."""
    rows = dm32fb.split_rows(values, stride)
    decoded = [[dm32fb.rgb565_to_rgb888(value) for value in row] for row in rows]
    return _summarize_full_image(decoded, scale, output)


def render_gram(values: Sequence[int], stride: int, scale: int, output: str) -> Rendered:
    """The 240x320 panel area of GRAM, with the shift-based colour expansion.

    Same pixels as render_panel(); only the colour expansion differs.
    """
    non_black = [(index % stride, index // stride)
                 for index, value in enumerate(values) if value]
    if non_black:
        xs = [x for x, _ in non_black]
        ys = [y for _, y in non_black]
        summary = (f"{len(non_black)} non-black px, "
                   f"bbox x[{min(xs)}..{max(xs)}] y[{min(ys)}..{max(ys)}]")
    else:
        summary = "frame is entirely black"

    if stride < dm32fb.PANEL_W:
        raise ValueError(f"gram view needs a stride of at least {dm32fb.PANEL_W}, got {stride}")
    rows = dm32fb.split_rows(values, stride)
    if len(rows) < dm32fb.PANEL_H:
        raise ValueError(f"gram view needs at least {dm32fb.PANEL_H} rows, the capture has "
                         f"{len(rows)}")
    decoded = [[dm32fb.rgb565_to_rgb888_shift(value) for value in row[:dm32fb.PANEL_W]]
               for row in rows[:dm32fb.PANEL_H]]
    width, height = dm32fb.PANEL_W * scale, dm32fb.PANEL_H * scale
    return (dm32fb.to_rgb_rows(decoded, scale),
            [summary, f"wrote {output}  {width}x{height}"])


def _summarize_full_image(decoded: List[List[dm32fb.RGB]], scale: int, output: str) -> Rendered:
    width, height = len(decoded[0]), len(decoded)
    non_black = dm32fb.count_non_black(decoded)
    line = f"{output}: panel {width}x{height} (x{scale}); non-black {non_black}/{width * height}"
    return dm32fb.to_rgb_rows(decoded, scale), [line]


RENDERERS: dict = {"panel": render_panel, "gram": render_gram, "gram-full": render_gram_full}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--view", choices=VIEWS, default="panel",
                        help="which view to render (default: %(default)s)")
    parser.add_argument("--scale", type=dm32fb.positive_int,
                        help="integer pixel scale (default: 3 for panel and gram-full, "
                             "2 for gram)")
    parser.add_argument("--stride", type=dm32fb.positive_int, default=dm32fb.GRAM_STRIDE,
                        help="row stride of the capture in pixels (default: %(default)s)")
    parser.add_argument("input", help="DM32_GRAMDUMP raw capture (little-endian RGB565)")
    parser.add_argument("output", help="PNG file to write")
    args = parser.parse_args(argv)

    scale = args.scale if args.scale is not None else DEFAULT_SCALE[args.view]
    try:
        values = dm32fb.read_gram_values(args.input)
    except OSError as exc:
        parser.error(f"cannot read {args.input}: {exc.strerror}")

    render: Callable[..., Rendered] = RENDERERS[args.view]
    try:
        rgb_rows, lines = render(values, args.stride, scale, args.output)
    except ValueError as exc:
        parser.error(str(exc))

    dm32fb.write_png(args.output, rgb_rows, COMPRESS_LEVEL[args.view])
    for line in lines:
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
