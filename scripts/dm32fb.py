# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright (c) 2026 Alex Kaplan
"""Shared helpers for the DM-32UV display scripts: RGB565 frames, PNG output, panel geometry.

Imported by gram2png.py, ppm2png.py, gramdiff.py and build_datachip.py, which live in the same
directory. This is a library, not a command. Standard library only, Python 3.9+.

Usage example (from another script in this directory):
    import dm32fb
    panel = dm32fb.to_panel(dm32fb.split_rows(dm32fb.read_gram_values("gram.raw"),
                                              dm32fb.GRAM_STRIDE))

Geometry: GRAM is kept upright, in memory coordinates, with GRAM_STRIDE (256) pixels per row.
The panel is PANEL_W x PANEL_H (240 x 320) in portrait, and the visible panel is its top-left
corner. to_panel() returns that corner unchanged. GRAM rows are 256 pixels apart, not 240, so a
GRAM dump read at a 240-pixel stride is sheared.
"""

import argparse
import struct
import zlib
from typing import List, Sequence, Tuple, TypeVar

RGB = Tuple[int, int, int]
Pixel = TypeVar("Pixel")

# Row stride of a DM32_GRAMDUMP capture, in pixels (GRAM_W in dm32_lcd.c).
GRAM_STRIDE = 256
# The visible panel, in portrait orientation.
PANEL_W = 240
PANEL_H = 320

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def positive_int(text: str) -> int:
    """argparse type: a whole number of at least 1."""
    try:
        value = int(text)
    except ValueError:
        raise argparse.ArgumentTypeError(f"expected a whole number, got {text!r}")
    if value < 1:
        raise argparse.ArgumentTypeError(f"must be at least 1, got {value}")
    return value


def rgb565_to_rgb888(value: int) -> RGB:
    """Expand an RGB565 pixel to 8-bit RGB, stretching each channel to the full 0..255 range."""
    red = (value >> 11) & 0x1F
    green = (value >> 5) & 0x3F
    blue = value & 0x1F
    return ((red * 255 + 15) // 31, (green * 255 + 31) // 63, (blue * 255 + 15) // 31)


def rgb565_to_rgb888_shift(value: int) -> RGB:
    """Expand an RGB565 pixel by bit shifting, as screenshot.py always did.

    Kept only so the gram view stays byte-identical to screenshot.py's output. Channels top out
    at 248, 252 and 248 rather than 255.
    """
    return (((value >> 11) & 0x1F) << 3, ((value >> 5) & 0x3F) << 2, (value & 0x1F) << 3)


def read_gram_values(path: str) -> List[int]:
    """Return every little-endian RGB565 value in a DM32_GRAMDUMP file, in row-major order.

    An odd trailing byte is ignored.
    """
    with open(path, "rb") as handle:
        data = handle.read()
    count = len(data) // 2
    return list(struct.unpack_from("<%dH" % count, data))


def split_rows(values: Sequence[int], stride: int) -> List[List[int]]:
    """Split a flat GRAM value list into rows of `stride` pixels. A partial last row is dropped."""
    height = len(values) // stride
    if height == 0:
        raise ValueError(f"capture holds fewer than one row of {stride} pixels")
    return [list(values[row * stride:(row + 1) * stride]) for row in range(height)]


def _ppm_header(data: bytes) -> Tuple[List[bytes], int]:
    """Return the four PPM header tokens (magic, width, height, maxval) and the pixel offset."""
    tokens: List[bytes] = []
    pos = 0
    while len(tokens) < 4:
        while pos < len(data) and data[pos] in b" \t\r\n":
            pos += 1
        if pos < len(data) and data[pos] == ord("#"):
            while pos < len(data) and data[pos] != ord("\n"):
                pos += 1
            continue
        start = pos
        while pos < len(data) and data[pos] not in b" \t\r\n":
            pos += 1
        if start == pos:
            raise ValueError("truncated PPM header")
        tokens.append(data[start:pos])
    # Exactly one whitespace byte separates the header from the pixel data.
    return tokens, pos + 1


def read_ppm(path: str) -> Tuple[int, int, List[List[RGB]]]:
    """Read a binary 8-bit PPM (P6). Return (width, height, rows of (red, green, blue) tuples)."""
    with open(path, "rb") as handle:
        data = handle.read()
    (magic, width_token, height_token, maxval_token), body_start = _ppm_header(data)
    if magic != b"P6":
        raise ValueError("not a binary PPM (expected magic P6)")
    width, height = int(width_token), int(height_token)
    if int(maxval_token) != 255:
        raise ValueError(f"only 8-bit PPM (maxval 255) is supported, got {int(maxval_token)}")
    row_bytes = width * 3
    needed = row_bytes * height
    body = data[body_start:body_start + needed]
    if len(body) < needed:
        raise ValueError(f"PPM pixel data is {len(body)} bytes, expected {needed}")
    rows = []
    for row in range(height):
        line = body[row * row_bytes:(row + 1) * row_bytes]
        rows.append([tuple(line[offset:offset + 3]) for offset in range(0, row_bytes, 3)])
    return width, height, rows


def crop(rows: Sequence[Sequence[Pixel]], x_origin: int, y_origin: int,
         crop_width: int, crop_height: int) -> List[List[Pixel]]:
    """Return the rectangle starting at (x_origin, y_origin), crop_width by crop_height pixels."""
    if not rows:
        raise ValueError("image has no rows")
    image_width, image_height = len(rows[0]), len(rows)
    if (x_origin < 0 or y_origin < 0 or crop_width < 1 or crop_height < 1
            or x_origin + crop_width > image_width or y_origin + crop_height > image_height):
        raise ValueError(f"crop {x_origin},{y_origin},{crop_width},{crop_height} does not fit "
                         f"in the {image_width}x{image_height} image")
    return [list(row[x_origin:x_origin + crop_width])
            for row in rows[y_origin:y_origin + crop_height]]


def to_panel(rows: Sequence[Sequence[Pixel]]) -> List[List[Pixel]]:
    """Return the visible panel of a GRAM-coordinate image, upright and unmodified.

    This is the top-left PANEL_W x PANEL_H (240 x 320) corner. It matches the default LCD origin
    of the board model. The input must be at least that large.
    """
    return crop(rows, 0, 0, PANEL_W, PANEL_H)


def count_non_black(rows: Sequence[Sequence[RGB]]) -> int:
    """Count the pixels that are not (0, 0, 0)."""
    return sum(1 for row in rows for pixel in row if any(pixel))


def to_rgb_rows(rows: Sequence[Sequence[RGB]], scale: int = 1) -> List[bytes]:
    """Turn an image into 8-bit RGB scanlines, each pixel and each row repeated `scale` times."""
    scanlines = []
    for row in rows:
        line = b"".join(bytes(pixel) * scale for pixel in row)
        scanlines.extend([line] * scale)
    return scanlines


def _png_chunk(tag: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def encode_png(rgb_rows: Sequence[bytes], compress_level: int = 9) -> bytes:
    """Encode 8-bit RGB scanlines (width * 3 bytes each) as a PNG file.

    Every scanline uses filter type 0. `compress_level` is the zlib level for the IDAT chunk.
    """
    if not rgb_rows:
        raise ValueError("nothing to encode: the image has no rows")
    width = len(rgb_rows[0]) // 3
    height = len(rgb_rows)
    raw = b"".join(b"\x00" + row for row in rgb_rows)
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return (PNG_SIGNATURE + _png_chunk(b"IHDR", header)
            + _png_chunk(b"IDAT", zlib.compress(raw, compress_level))
            + _png_chunk(b"IEND", b""))


def write_png(path: str, rgb_rows: Sequence[bytes], compress_level: int = 9) -> None:
    """Write scanlines from to_rgb_rows() to a PNG file."""
    png = encode_png(rgb_rows, compress_level)
    with open(path, "wb") as handle:
        handle.write(png)
