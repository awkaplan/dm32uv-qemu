# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# Copyright (c) 2026 Alex Kaplan
"""DMR baseband RAM window at 0x16000000, 64 KB (hw/misc/dm32_dmrram.c; SVD peripheral
Modem_Buffer and CODEC; docs/devices.md "DMR RAM"; docs/environment.md "DMR baseband RAM")."""

import unittest

from dm32qtest import DM32TestCase

BASE = 0x16000000
SIZE = 0x10000
CODEC = BASE + 0x9C0            # CODEC registers: dm32_codec, mapped over this window


class DmrRamTest(DM32TestCase):
    def test_zero_at_reset(self):
        """The window is zeroed at reset (dm32_dmrram.c header, "Reset state": a model choice,
        since the datasheet gives no reset value for the baseband RAM)."""
        m = self.start()
        for off in (0x0000, 0x04B0, 0x0960, 0x09C0, SIZE - 4):
            self.assertEqual(m.readl(BASE + off), 0, hex(off))

    def test_byte_word_long_read_back(self):
        """Byte, word and long accesses read back little-endian, in one shared array
        (dm32_dmrram.c header: "byte-addressable RAM"; SVD Modem_Buffer)."""
        m = self.start()
        m.writeb(BASE + 0x10, 0xAB)
        m.writew(BASE + 0x12, 0xCDEF)
        m.writel(BASE + 0x14, 0x01234567)
        self.assertEqual(m.readb(BASE + 0x10), 0xAB)
        self.assertEqual(m.readw(BASE + 0x12), 0xCDEF)
        self.assertEqual(m.readl(BASE + 0x14), 0x01234567)
        self.assertEqual(m.readl(BASE + 0x10), 0xCDEF00AB)

    def test_regions_are_plain_ram(self):
        """The TX RAM (+0x000), RX RAM (+0x4b0), MODEM_RAM (+0x960), SOUND (+0x980) and the
        unnamed upper half are storage (SVD Modem_Buffer; dm32_dmrram.c "Layout")."""
        m = self.start()
        for off in (0x0000, 0x04B0, 0x0960, 0x0980, 0x0A00, SIZE - 4):
            m.writel(BASE + off, 0xA5A55A5A)
            self.assertEqual(m.readl(BASE + off), 0xA5A55A5A, hex(off))

    def test_codec_block_is_not_ram(self):
        """Negative: the CODEC block at +0x9c0 to +0x9ff is the codec model (dm32_codec.c), not
        RAM: read-only SR1 keeps 0, an undefined address keeps nothing, and CR_VIC reads its
        reset value 0x0f."""
        m = self.start()
        m.writeb(CODEC, 0x5A)
        m.writel(BASE + 0x9FC, 0x11223344)
        self.assertEqual(m.readb(CODEC), 0)
        self.assertEqual(m.readl(BASE + 0x9FC), 0)
        self.assertEqual(m.readb(CODEC + 0x12), 0x0F)

    def test_window_does_not_alias(self):
        """Negative: the window is 64 KB and nothing past it is this device. A write at
        0x16010000 is not visible at offset 0 (docs/devices.md "DMR RAM": "Base 0x16000000,
        64 KB"; dm32_dmrram.c header)."""
        m = self.start()
        m.writel(BASE + 0x10000, 0x12345678)
        self.assertEqual(m.readl(BASE), 0)

    def test_no_interrupt_raised(self):
        """Negative: writes raise no interrupt. The dm32_dmrram.c header lists no interrupt
        (capture only). The datasheet's Table 30 gives the baseband PIC sources 27-33 and the
        codec 37 (dm32uv-re specs/dts/hr_c7000.dtsi), and docs/devices.md "DMR RAM" records that
        the model wires none of them; source 30 is dm32_sysreg's TX-slot line, and source 37 is
        dm32_codec's, not this device's."""
        m = self.start()
        m.irq_intercept_in(m.pic_path())
        for off in (0x0000, 0x0030, 0x04B0, 0x0960, 0x0A00):
            m.writel(BASE + off, 0xFFFFFFFF)
        raised = {src for level, src in m.irq_log if level}
        self.assertFalse(raised & {27, 28, 29, 30, 31, 32, 33, 37})
        self.assertEqual(m.irq_log, [])


if __name__ == "__main__":
    unittest.main()
