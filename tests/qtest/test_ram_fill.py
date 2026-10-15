# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# Copyright (c) 2026 Alex Kaplan
"""Power-on contents of IRAM and SRAM (hw/csky/dm32.c, DM32_RAM_FILL; docs/environment.md).
On the radio, SRAM the firmware never initialises holds power-on noise (dm32uv-re
docs/live-ram-capture.md); DM32_RAM_FILL=noise reproduces that."""

import os
import unittest

from dm32qtest import DM32TestCase, run_plain

IRAM, IRAM_SIZE = 0x00010000, 0x48000
SRAM, SRAM_SIZE = 0x18000000, 0x8000
GUARD = 0x18007FE0          # the overlay trampoline's guard word (dm32uv-re tools/examples)


class RamFillTest(DM32TestCase):
    def test_default_is_zero(self):
        """Unset DM32_RAM_FILL keeps QEMU's zero fill."""
        m = self.start()
        self.assertEqual(m.read(IRAM, 256), bytes(256))
        self.assertEqual(m.read(SRAM + SRAM_SIZE - 256, 256), bytes(256))

    def test_zero_is_explicit_default(self):
        """DM32_RAM_FILL=zero is the same as unset."""
        m = self.start(env={"DM32_RAM_FILL": "zero"})
        self.assertEqual(m.read(SRAM, 256), bytes(256))

    def test_noise_fills_both(self):
        """noise fills IRAM and SRAM with non-zero-looking bytes: in the captured window
        0x18004c00-0x18007fff, almost every byte is non-zero on the radio."""
        m = self.start(env={"DM32_RAM_FILL": "noise"})
        for base, size in ((IRAM, IRAM_SIZE), (SRAM, SRAM_SIZE)):
            data = m.read(base, size)
            self.assertGreater(sum(1 for b in data if b), size * 0.98, hex(base))

    def test_noise_reproducible(self):
        """The same seed gives the same contents on every run."""
        a = self.start(env={"DM32_RAM_FILL": "noise:7"}).read(SRAM, 64)
        b = self.start(env={"DM32_RAM_FILL": "noise:7"}).read(SRAM, 64)
        self.assertEqual(a, b)

    def test_seed_changes_contents(self):
        """A different seed, or the default seed, gives different contents."""
        a = self.start(env={"DM32_RAM_FILL": "noise:7"}).read(SRAM, 64)
        b = self.start(env={"DM32_RAM_FILL": "noise:8"}).read(SRAM, 64)
        c = self.start(env={"DM32_RAM_FILL": "noise"}).read(SRAM, 64)
        self.assertNotEqual(a, b)
        self.assertNotEqual(a, c)

    def test_regions_differ(self):
        """IRAM and SRAM do not repeat each other's bytes on one seed."""
        m = self.start(env={"DM32_RAM_FILL": "noise"})
        self.assertNotEqual(m.read(IRAM, 64), m.read(SRAM, 64))

    def test_loader_overwrites_its_range_only(self):
        """A -device loader image is written over the fill; bytes it does not cover keep it."""
        tmp = self.make_tempdir()
        path = os.path.join(tmp, "sram.bin")
        with open(path, "wb") as f:
            f.write(bytes(0x4C00))                  # like stock: initialised up to 0x18004bff
        m = self.start(env={"DM32_RAM_FILL": "noise"},
                       args=["-device", f"loader,file={path},addr={SRAM:#x}"])
        self.assertEqual(m.read(SRAM, 0x4C00), bytes(0x4C00))
        self.assertNotEqual(m.read(GUARD, 4), bytes(4))

    def test_other_ram_not_filled(self):
        """Only IRAM and SRAM are filled; the low 64 KB (the boot-ROM area) stays zero."""
        m = self.start(env={"DM32_RAM_FILL": "noise"})
        self.assertEqual(m.read(0x0, 256), bytes(256))

    def test_bad_value_rejected(self):
        """Negative: a value other than zero, noise or noise:<seed> is an error at start-up."""
        for bad in ("random", "noise:x", "noise:"):
            rc, out = run_plain(env={"DM32_RAM_FILL": bad}, args=["-accel", "qtest"])
            self.assertEqual(rc, 1, bad)
            self.assertIn("DM32_RAM_FILL", out)


if __name__ == "__main__":
    unittest.main()
