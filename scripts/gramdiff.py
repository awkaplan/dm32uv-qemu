#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright (c) 2026 Alex Kaplan
"""Compare two DM32_GRAMDUMP raw captures and report exactly what changed.

Intended for checking a UI change: run the emulator once on the stock image and once on the
patched one, then diff the settled frames. Reports the changed bounding box, the number of
changed pixels and the new colours, which is enough to confirm a change landed where it was
meant to without comparing screenshots by eye.

With --png, writes a highlight image: unchanged pixels are dimmed to a quarter brightness and
changed pixels keep their full colour. The image is scaled by 2.

usage example:
    python3 scripts/gramdiff.py stock.raw patched.raw --png changes.png
"""

import argparse
import sys
from collections import Counter

import dm32fb

PNG_SCALE = 2
COMPRESS_LEVEL = 9


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("before", help="DM32_GRAMDUMP capture before the change")
    parser.add_argument("after", help="DM32_GRAMDUMP capture after the change")
    parser.add_argument("--stride", type=dm32fb.positive_int, default=dm32fb.GRAM_STRIDE,
                        help="row stride of the captures in pixels (default: %(default)s)")
    parser.add_argument("--png", metavar="OUT.png",
                        help="also write a highlight image of the changed pixels")
    args = parser.parse_args(argv)

    try:
        before = dm32fb.read_gram_values(args.before)
        after = dm32fb.read_gram_values(args.after)
    except OSError as exc:
        parser.error(f"cannot read capture: {exc.strerror}")
    if len(before) != len(after):
        parser.error(f"size mismatch: {len(before)} vs {len(after)} pixels")

    changed = [index for index in range(len(before)) if before[index] != after[index]]
    print(f"frames: {args.before}  ->  {args.after}")
    print(f"changed pixels: {len(changed)} / {len(before)}")
    if not changed:
        print("NO DIFFERENCE")
        return 0

    stride = args.stride
    xs = [index % stride for index in changed]
    ys = [index // stride for index in changed]
    print(f"bounding box: x[{min(xs)}..{max(xs)}] y[{min(ys)}..{max(ys)}] "
          f"({max(xs) - min(xs) + 1}x{max(ys) - min(ys) + 1})")

    print("new colors (RGB565 -> count):")
    for value, count in Counter(after[index] for index in changed).most_common(6):
        print(f"  {value:#06x} rgb{dm32fb.rgb565_to_rgb888(value)}  {count}")

    if args.png is not None:
        _write_highlight(args.png, after, changed, stride)
        print(f"wrote {args.png}")
    return 0


def _write_highlight(path: str, after, changed, stride: int) -> None:
    changed_set = set(changed)
    height = len(after) // stride
    rows = []
    for row in range(height):
        pixels = []
        for column in range(stride):
            index = row * stride + column
            red, green, blue = dm32fb.rgb565_to_rgb888(after[index])
            if index not in changed_set:
                red, green, blue = red // 4, green // 4, blue // 4
            pixels.append((red, green, blue))
        rows.append(pixels)
    dm32fb.write_png(path, dm32fb.to_rgb_rows(rows, PNG_SCALE), COMPRESS_LEVEL)


if __name__ == "__main__":
    sys.exit(main())
