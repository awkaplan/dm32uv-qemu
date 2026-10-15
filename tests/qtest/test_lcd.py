# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# Copyright (c) 2026 Alex Kaplan
"""LCD controller and panel at 0x12000000 (hw/display/dm32_lcd.c; docs/devices.md, "LCD";
docs/environment.md, "LCD and frame capture"; the pin-mux gate is the sysreg helper
dm32_lcd_bus_muxed_to_gpio() in hw/misc/dm32_sysreg.c, documented in docs/devices.md, "System
registers"). The framebuffer is read from the DM32_GRAMDUMP file: the model's 256 x 320 RGB565
GRAM, written once drawing has been idle for DM32_GRAMQUIET ms of virtual time."""

import os
import struct
import unittest

from dm32qtest import DM32TestCase

LCD_CMD = 0x12000000
LCD_DATA = 0x12000004
LCD_CFG = 0x12000010
LCD_STATUS = 0x12000028
IO_DIPLEX2 = 0x1100003C
DIPLEX2_RESET = 0x3FFFFFFB       # DS 4.4.4.6 and SVD: every pad on GPIO
DIPLEX2_LCD = 0x3FF80103         # after stock's claim, IO_DIPLEX2 &= 0xFFF80107 (lcd_controller_power_on_init_sequence)
DIPLEX2_DB_MUX = 0x0200          # bit 9, one of the LCD DB0-DB4 pad selects (bits 9-15)

CASET, RASET, RAMWR, MADCTL = 0x2A, 0x2B, 0x2C, 0x36   # MIPI-DCS, dm32_lcd.c

GRAM_W, GRAM_H = 256, 320        # dm32_lcd.c header; docs/environment.md
DUMP_BYTES = GRAM_W * GRAM_H * 2  # docs/environment.md: 163840 bytes
QUIET_NS = 300_000_000           # longer than DM32_GRAMQUIET's 150 ms default


class LcdTestCase(DM32TestCase):
    def setUp(self):
        self.dir = self.make_tempdir()
        self.dump = os.path.join(self.dir, "gram.raw")

    def gram_env(self):
        return {"DM32_GRAMDUMP": self.dump}

    def enable(self, m):
        """Stock's LCD bring-up: claim the bus pads (IO_DIPLEX2 &= 0xFFF80107), then enable."""
        m.writel(IO_DIPLEX2, DIPLEX2_LCD)
        m.writel(LCD_STATUS, 1)

    def set_window(self, m, cmd, lo, hi):
        m.writeb(LCD_CMD, cmd)
        for b in (lo >> 8, lo & 0xFF, hi >> 8, hi & 0xFF):
            m.writeb(LCD_DATA, b)

    def ramwr(self, m, pixels):
        m.writeb(LCD_CMD, RAMWR)
        for px in pixels:
            m.writeb(LCD_DATA, px >> 8)      # RGB565, high byte first
            m.writeb(LCD_DATA, px & 0xFF)

    def draw(self, m, x0, x1, y0, y1, pixels):
        self.set_window(m, CASET, x0, x1)
        self.set_window(m, RASET, y0, y1)
        self.ramwr(m, pixels)

    def settle(self, m):
        """Let drawing go quiet, then return the GRAM dump as bytes."""
        m.clock_step(QUIET_NS)
        with open(self.dump, "rb") as f:
            data = f.read()
        self.assertEqual(len(data), DUMP_BYTES)
        return data

    @staticmethod
    def pixel(data, x, y):
        # The dump is the uint16_t GRAM array written by fwrite() on a little-endian host.
        return struct.unpack_from("<H", data, (y * GRAM_W + x) * 2)[0]


class LcdControllerTest(LcdTestCase):
    def test_status_follows_enable_bit(self):
        """+0x28 reads 0 before enable, 0x3 (bits 0 and 1) after bit 0 is written, and 0 again
        once bit 0 is cleared (devices.md, LCD table; dm32_lcd.c status read path)."""
        m = self.start()
        self.assertEqual(m.readl(LCD_STATUS), 0)
        m.writel(LCD_STATUS, 1)
        self.assertEqual(m.readl(LCD_STATUS), 0x3)
        m.writel(LCD_STATUS, 0)
        self.assertEqual(m.readl(LCD_STATUS), 0)

    def test_config_reads_back_before_enable(self):
        """+0x10 is stored and read back in full, and the write is not gated by the enable bit
        (devices.md: "Stored and read back. Written by panel init", which enables afterwards)."""
        m = self.start()
        m.writel(LCD_CFG, 0xDEADBEEF)
        self.assertEqual(m.readl(LCD_CFG), 0xDEADBEEF)
        m.writel(LCD_CFG, 0)
        self.assertEqual(m.readl(LCD_CFG), 0)

    def test_negative_writes_before_enable_dropped(self):
        """Negative: command, data and pixel writes before +0x28 bit 0 are dropped, and each
        dropped byte counts as an lcd_off event (devices.md: "Writes are dropped until +0x28 bit 0
        is set"; dm32_lcd.c counts DM32_EV_LCD_OFF)."""
        m = self.start(env=self.gram_env())
        self.set_window(m, CASET, 3, 3)      # 1 command byte + 4 data bytes
        self.set_window(m, RASET, 4, 4)      # 1 + 4
        self.ramwr(m, [0xF800])              # 1 + 2
        self.enable(m)
        self.draw(m, 0, 0, 0, 0, [0x07E0])   # the sentinel, drawn after enable
        data = self.settle(m)
        self.assertEqual(self.pixel(data, 3, 4), 0)
        self.assertEqual(self.pixel(data, 0, 0), 0x07E0)
        self.assertEqual(m.events()["lcd_off"], 13)

    def test_no_dump_without_pixels(self):
        """Negative: a dump is written only after a pixel has been drawn. An enabled controller
        that is never sent RAMWR data leaves no GRAM file (dm32_lcd.c gram_dirty check)."""
        m = self.start(env=self.gram_env())
        self.enable(m)
        self.set_window(m, CASET, 0, 239)
        m.clock_step(QUIET_NS)
        self.assertFalse(os.path.exists(self.dump))
        m.close()


class LcdPanelTest(LcdTestCase):
    def test_rgb565_pixels_land_in_window(self):
        """CASET, RASET and RAMWR set a window; pixels are RGB565, high byte first, and land at
        their window coordinates in the 256 x 320 GRAM (dm32_lcd.c header; devices.md, LCD)."""
        m = self.start(env=self.gram_env())
        self.enable(m)
        self.draw(m, 10, 11, 20, 20, [0xF800, 0x07E0])
        data = self.settle(m)
        self.assertEqual(self.pixel(data, 10, 20), 0xF800)
        self.assertEqual(self.pixel(data, 11, 20), 0x07E0)
        self.assertEqual(self.pixel(data, 12, 20), 0)
        self.assertEqual(self.pixel(data, 10, 21), 0)

    def test_window_wraps_row_major(self):
        """RAMWR advances across the CASET window and wraps to the next RASET row (dm32_lcd.c
        dm32_lcd_data(), the CMD_RAMWR auto-advance; devices.md, "advances inside the window")."""
        m = self.start(env=self.gram_env())
        self.enable(m)
        self.draw(m, 5, 6, 0, 1, [1, 2, 3, 4])
        data = self.settle(m)
        self.assertEqual([self.pixel(data, 5, 0), self.pixel(data, 6, 0),
                          self.pixel(data, 5, 1), self.pixel(data, 6, 1)], [1, 2, 3, 4])

    def test_two_byte_cursor_set(self):
        """A CASET or RASET with only its two start-coordinate bytes sets the cursor, as
        display_set_cursor_pos (0x03017978) sends it (dm32_lcd.c CMD_CASET comment; devices.md)."""
        m = self.start(env=self.gram_env())
        self.enable(m)
        m.writeb(LCD_CMD, CASET)
        m.writeb(LCD_DATA, 0)
        m.writeb(LCD_DATA, 30)
        m.writeb(LCD_CMD, RASET)
        m.writeb(LCD_DATA, 0)
        m.writeb(LCD_DATA, 40)
        self.ramwr(m, [0x1234])
        data = self.settle(m)
        self.assertEqual(self.pixel(data, 30, 40), 0x1234)
        self.assertEqual(self.pixel(data, 31, 40), 0)

    def test_madctl_does_not_move_pixels(self):
        """MADCTL is decoded but does not change the view: a pixel lands at its CASET/RASET
        coordinate whatever the MADCTL argument (devices.md, LCD, "Not modelled": any effect of
        MADCTL on the displayed image)."""
        m = self.start(env=self.gram_env())
        self.enable(m)
        m.writeb(LCD_CMD, MADCTL)
        m.writeb(LCD_DATA, 0xC0)
        self.draw(m, 7, 7, 9, 9, [0xFFFF])
        data = self.settle(m)
        self.assertEqual(self.pixel(data, 7, 9), 0xFFFF)


class LcdPinMuxTest(LcdTestCase):
    def test_negative_muxed_bus_forces_data_to_ff(self):
        """Negative: with IO_DIPLEX2 bits 9-15 set, DB0-DB4 belong to the keypad, so each panel
        data byte is forced to 0xFF and counts as an lcd_mux event (devices.md, "System
        registers" and "LCD"; dm32_lcd.c DATA write path)."""
        m = self.start(env=self.gram_env())
        self.enable(m)
        self.set_window(m, CASET, 1, 1)      # window set while the bus is still free
        self.set_window(m, RASET, 1, 1)
        m.writel(IO_DIPLEX2, DIPLEX2_LCD | DIPLEX2_DB_MUX)
        self.ramwr(m, [0x0000])
        data = self.settle(m)
        self.assertEqual(self.pixel(data, 1, 1), 0xFFFF)
        self.assertEqual(m.events()["lcd_mux"], 2)

    def test_restored_bus_writes_pixels(self):
        """Restoring IO_DIPLEX2 bits 9-15 to 0, as after a keypad scan, lets pixel data through
        unchanged and counts no lcd_mux events."""
        m = self.start(env=self.gram_env())
        self.enable(m)
        m.writel(IO_DIPLEX2, DIPLEX2_LCD)
        self.draw(m, 2, 2, 2, 2, [0x1234])
        data = self.settle(m)
        self.assertEqual(self.pixel(data, 2, 2), 0x1234)
        self.assertEqual(m.events()["lcd_mux"], 0)


if __name__ == "__main__":
    unittest.main()
