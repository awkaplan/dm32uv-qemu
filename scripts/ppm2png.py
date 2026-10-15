#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright (c) 2026 Alex Kaplan
"""Convert a QEMU screendump (binary PPM, P6) to PNG.

Three modes:
  default     the whole image, scaled by --scale (default 1)
  --crop      the rectangle X,Y,W,H of the image, scaled by --scale (default 1)
  --panel     the visible 240x320 panel, checked to be that size and passed through as is
              (default scale 3). A QEMU screendump is already upright, so this only adds the
              default scale. Generic crops do not need this.

usage example:
    python3 scripts/ppm2png.py --panel --scale 3 screen.ppm panel.png
    python3 scripts/ppm2png.py --crop 10,20,100,50 --scale 2 screen.ppm crop.png
"""

import argparse
import sys
from typing import List, Tuple

import dm32fb

PANEL_DEFAULT_SCALE = 3
PLAIN_DEFAULT_SCALE = 1
COMPRESS_LEVEL = 9


def parse_crop(text: str) -> Tuple[int, int, int, int]:
    """argparse type for --crop: 'X,Y,W,H'."""
    parts = text.split(",")
    if len(parts) != 4:
        raise argparse.ArgumentTypeError(f"expected X,Y,W,H, got {text!r}")
    try:
        x_origin, y_origin, crop_width, crop_height = (int(part) for part in parts)
    except ValueError:
        raise argparse.ArgumentTypeError(f"expected four whole numbers, got {text!r}")
    return x_origin, y_origin, crop_width, crop_height


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--crop", type=parse_crop, metavar="X,Y,W,H",
                      help="crop to this rectangle before scaling")
    mode.add_argument("--panel", action="store_true",
                      help="require a 240x320 input (the visible panel, upright) and scale it "
                           "with the default panel scale of 3")
    parser.add_argument("--scale", type=dm32fb.positive_int,
                        help="integer pixel scale (default: 1, or 3 with --panel)")
    parser.add_argument("input", help="binary PPM (P6) from a QEMU screendump")
    parser.add_argument("output", help="PNG file to write")
    args = parser.parse_args(argv)

    try:
        width, height, rows = dm32fb.read_ppm(args.input)
    except OSError as exc:
        parser.error(f"cannot read {args.input}: {exc.strerror}")
    except ValueError as exc:
        parser.error(f"{args.input}: {exc}")

    try:
        if args.panel:
            rgb_rows, line = _panel_output(parser, rows, width, height, args)
        else:
            rgb_rows, line = _plain_output(rows, width, height, args)
    except ValueError as exc:
        parser.error(str(exc))

    dm32fb.write_png(args.output, rgb_rows, COMPRESS_LEVEL)
    print(line)
    return 0


def _panel_output(parser: argparse.ArgumentParser, rows: List[List[dm32fb.RGB]],
                  width: int, height: int, args: argparse.Namespace) -> Tuple[List[bytes], str]:
    if (width, height) != (dm32fb.PANEL_W, dm32fb.PANEL_H):
        parser.error(f"expected the full 240x320 GRAM view, got {width}x{height}")
    scale = args.scale if args.scale is not None else PANEL_DEFAULT_SCALE
    panel = dm32fb.to_panel(rows)  # the visible panel; a 240x320 input is returned unchanged
    panel_width, panel_height = len(panel[0]), len(panel)
    non_black = dm32fb.count_non_black(panel)
    line = (f"{args.output}: panel {panel_width}x{panel_height} (x{scale}); "
            f"non-black pixels {non_black}/{panel_width * panel_height}")
    return dm32fb.to_rgb_rows(panel, scale), line


def _plain_output(rows: List[List[dm32fb.RGB]], width: int, height: int,
                  args: argparse.Namespace) -> Tuple[List[bytes], str]:
    scale = args.scale if args.scale is not None else PLAIN_DEFAULT_SCALE
    if args.crop is not None:
        x_origin, y_origin, crop_width, crop_height = args.crop
    else:
        x_origin, y_origin, crop_width, crop_height = 0, 0, width, height
    region = dm32fb.crop(rows, x_origin, y_origin, crop_width, crop_height)
    non_black = dm32fb.count_non_black(rows)
    line = (f"{args.output}: {crop_width * scale}x{crop_height * scale} "
            f"(src {width}x{height}, crop {x_origin},{y_origin},{crop_width},{crop_height}, "
            f"scale {scale}); non-black pixels in source: {non_black}/{width * height}")
    return dm32fb.to_rgb_rows(region, scale), line


if __name__ == "__main__":
    sys.exit(main())
