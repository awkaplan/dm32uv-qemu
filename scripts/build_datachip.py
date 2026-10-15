#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright (c) 2026 Alex Kaplan
"""Build a 16 MiB backing image for the emulated W25Q128 data chip (DM32_SPINOR).

The SSI model defaults to no backing store, which reads as all 0xFF. The calibration page then
has no page tag, dm32Calib_load() rejects it, and emulation reports `calibration NOT valid
(tag ff)`. With this image the emulator reproduces the real chip: the calibration page reads
with tag 0x02 at +0xFFF, and calibration loads.

Calibration is per unit. Use dumps from your own radio only. Calibration cannot be shared or
regenerated from another unit's data. The image is an emulator input only: never write it to a
radio. Keep generated images out of the repository; they are 16 MiB.

The calibration page's flash address is not fixed. It has been observed at 0x0A3000, 0x0A7000
and 0x0AA000 on different units. Find it on your own unit from the data chip's block-to-page tag
map, or by its content signature `00 40 00 47 ff ff ff ff ff ff ff ff 58 02 7c 02`. Do not assume
a default address.

A generic factory-defaults data-chip image, for users without a dump of their own, ships in
dm32uv-re: https://github.com/awkaplan/dm32uv-re

usage example:
    python3 scripts/build_datachip.py --cal-addr 0xA7000 calibration_page.bin config_region.bin \\
        dm32_datachip_16m.bin
    DM32_SPINOR=dm32_datachip_16m.bin qemu-system-cskyv2 -M dm32 ...

    calibration_page.bin   your unit's 4096-byte calibration page (required)
    config_region.bin      optional: a larger dump of the config pages, placed at offset 0.
                           Page tags outside the calibration page read 0xff without it.
    out_file               default: dm32_datachip_16m.bin in the current directory
"""

import argparse
import pathlib
import sys

SIZE = 16 << 20
PAGE_SIZE = 0x1000
TAG_OFFSET = PAGE_SIZE - 1
EXPECTED_TAG = 0x02
DEFAULT_OUT = pathlib.Path("dm32_datachip_16m.bin")


def parse_address(text: str) -> int:
    """argparse type: a flash address, decimal or 0x-prefixed."""
    try:
        return int(text, 0)
    except ValueError:
        raise argparse.ArgumentTypeError(f"expected an address such as 0xA7000, got {text!r}")


def read_input(path: pathlib.Path) -> bytes:
    try:
        return path.read_bytes()
    except OSError as exc:
        sys.exit(f"ERROR: cannot read {path}: {exc.strerror}")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("cal_file", type=pathlib.Path,
                        help="your unit's 4096-byte calibration page dump")
    parser.add_argument("cfg_file", type=pathlib.Path, nargs="?",
                        help="optional config-region dump, placed at offset 0")
    parser.add_argument("out_file", type=pathlib.Path, nargs="?", default=DEFAULT_OUT,
                        help="output image (default: %(default)s)")
    parser.add_argument("--cal-addr", type=parse_address, required=True,
                        help="flash address of the calibration page on your unit, 4 KiB "
                             "aligned; it differs between units (0xA3000, 0xA7000 and 0xAA000 "
                             "have been seen), so find it in your own dump")
    args = parser.parse_args(argv)

    cal_addr = args.cal_addr
    if cal_addr % PAGE_SIZE or not 0 <= cal_addr <= SIZE - PAGE_SIZE:
        parser.error(f"--cal-addr 0x{cal_addr:X} must be 4 KiB aligned and inside the 16 MiB chip")

    buf = bytearray(b"\xff" * SIZE)

    if args.cfg_file is not None:
        cfg = read_input(args.cfg_file)
        if len(cfg) > SIZE:
            sys.exit(f"ERROR: config region is {len(cfg)} bytes, larger than the 16 MiB chip")
        buf[0:len(cfg)] = cfg
        print(f"config region: {len(cfg)} bytes from {args.cfg_file.name}")
    else:
        print("no config-region file given; page tags outside the calibration page "
              "will read 0xff")

    cal = read_input(args.cal_file)
    if len(cal) != PAGE_SIZE:
        sys.exit(f"ERROR: calibration page is {len(cal)} bytes, expected 4096")
    buf[cal_addr:cal_addr + PAGE_SIZE] = cal

    tag = buf[cal_addr + TAG_OFFSET]
    if tag != EXPECTED_TAG:
        print(f"WARNING: tag at +0xFFF is 0x{tag:02x}, expected 0x{EXPECTED_TAG:02x} -- "
              f"check --cal-addr is correct for your unit", file=sys.stderr)

    args.out_file.write_bytes(buf)
    print(f"wrote {args.out_file} ({SIZE} bytes), calibration at 0x{cal_addr:06X}, "
          f"tag 0x{tag:02x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
