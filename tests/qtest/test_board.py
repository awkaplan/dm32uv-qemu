# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# Copyright (c) 2026 Alex Kaplan
"""Board memory map and permissive events (hw/csky/dm32.c; docs/devices.md, "Memory map",
"Unimplemented windows", "Unassigned addresses", "Permissive events and DM32_STRICT")."""

import unittest

from dm32qtest import DM32TestCase, QTestError, run_plain

FLASH = 0x03000000
USB = 0x13000000          # unimplemented window (dm32_add_unimp), PIC 21
UNASSIGNED = 0x0B000000   # between 0x04001000 and 0x11000000: no region


class MemoryMapTest(DM32TestCase):
    def test_ram_regions(self):
        """Low RAM, IRAM and SRAM are plain RAM (dm32.c)."""
        m = self.start()
        for addr in (0x00000000, 0x00010000, 0x00057FFC, 0x18000000, 0x18007FFC):
            m.writel(addr, 0xA5A55A5A)
            self.assertEqual(m.readl(addr), 0xA5A55A5A, hex(addr))

    def test_flash_store_dropped(self):
        """Negative: the program flash is a ROM device; a guest store is dropped and counted."""
        m = self.start()
        before = m.readl(FLASH + 0x1000)
        m.writel(FLASH + 0x1000, before ^ 0xFFFFFFFF)
        self.assertEqual(m.readl(FLASH + 0x1000), before)
        out = m.close()
        self.assertIn("FLASH WRITE DROPPED addr=0x03001000", out)
        self.assertEqual(m.events()["flash_store"], 1)

    def test_device_discovery(self):
        """The board creates one PIC, three GPIO banks and five SSI instances (dm32.c)."""
        m = self.start()
        self.assertEqual(len(m.find_devices("dm32_pic")), 1)
        self.assertEqual(len(m.find_devices("dm32_gpio")), 3)
        self.assertEqual(len(m.find_devices("dm32_ssi")), 5)


class UnimplementedTest(DM32TestCase):
    def test_unimp_window(self):
        """Negative: an unimplemented window reads 0, drops writes, logs under -d unimp in
        hw/misc/unimp.c's format, and counts an unimp event per access."""
        m = self.start(args=["-d", "unimp"])
        m.writel(USB + 0x10, 0x12345678)
        self.assertEqual(m.readl(USB + 0x10), 0)
        out = m.close()
        self.assertIn("dm32.usb: unimplemented device write (size 4, offset 0x00010, "
                      "value 0x12345678)", out)
        self.assertIn("dm32.usb: unimplemented device read  (size 4, offset 0x00010)", out)
        self.assertEqual(m.events()["unimp"], 2)

    def test_block_windows_do_not_shadow_models(self):
        """Each Table 26 block's unimplemented window sits below its model: the ADC VERID still
        answers (DS 5.9: 0x20151216) and no event is counted."""
        m = self.start()
        self.assertEqual(m.readl(0x140D0040), 0x20151216)
        self.assertEqual(m.events()["unimp"], 0)

    def test_block_tail_is_unimp(self):
        """An offset inside a Table 26 block (HR_C7000 v2.7 section 4.5.3) that the model does
        not cover is an unimplemented-device access named after the block: the timer model maps
        4 KB of its 64 KB block, the PIC model 4 KB of its, and the program-flash image 1 MB of
        the 16 MB LCSFC window."""
        m = self.start(args=["-d", "unimp"])
        for addr in (0x1400F000, 0x17008000, 0x03800000):
            self.assertEqual(m.readl(addr), 0, hex(addr))
        out = m.close()
        for name in ("dm32.timer:", "dm32.pic:", "dm32.lcsfc:"):
            self.assertIn(name + " unimplemented device read", out)
        ev = m.events()
        self.assertEqual((ev["unimp"], ev["unassigned"]), (3, 0))

    def test_gap_between_blocks_is_unassigned(self):
        """Negative: Table 26 lists no reserved ranges, so an address between blocks
        (0x15F00000, 0x16010000, 0x17010000, 0x18008000) is an unassigned access, not unimp."""
        m = self.start()
        for addr in (0x15F00000, 0x16010000, 0x17010000, 0x18008000):
            self.assertEqual(m.readl(addr), 0, hex(addr))
        ev = m.events()
        self.assertEqual((ev["unimp"], ev["unassigned"]), (0, 4))

    def test_block_names_follow_svd(self):
        """Windows are named dm32.<SVD peripheral name>, lower case: past the SPI slave model's
        256-byte register window, its block reads as dm32.spi_slave0."""
        m = self.start(args=["-d", "unimp"])
        m.readl(0x14120200)
        self.assertIn("dm32.spi_slave0: unimplemented device read", m.close())

    def test_unassigned(self):
        """Negative: an address no region covers reads 0, drops writes, logs under
        -d invalid_mem as memory_region_access_valid() does, and counts unassigned events."""
        m = self.start(args=["-d", "invalid_mem"])
        m.writel(UNASSIGNED, 0x1)
        self.assertEqual(m.readl(UNASSIGNED), 0)
        out = m.close()
        self.assertIn("Invalid write at addr 0xB000000, size 4, region 'dm32.unassigned', "
                      "reason: rejected", out)
        self.assertEqual(m.events()["unassigned"], 2)

    def test_events_line_always_printed(self):
        """Every run prints one dm32-events line with every class, all zero when idle."""
        m = self.start()
        ev = m.events()
        self.assertEqual(set(ev), {"unimp", "unassigned", "flash_store", "dac_pd", "lcd_mux",
                                   "lcd_off", "ssi_discard", "nor_wel", "adc_pad",
                                   "wdt_locked", "pad_conflict", "pad_unclaimed", "i2c_nak"})
        self.assertFalse(any(ev.values()))


class StrictTest(DM32TestCase):
    def test_strict_stops_with_status_3(self):
        """DM32_STRICT naming a class: the first event stops QEMU with exit status 3."""
        m = self.start(env={"DM32_STRICT": "unimp"})
        try:
            m.writel(USB, 1)
        except (QTestError, OSError):
            pass                           # the reply can race with the shutdown
        self.assertEqual(m.proc.wait(timeout=10), 3)
        out = m.close()
        self.assertIn("dm32: strict: unimp: dm32.usb write, offset 0x0", out)
        self.assertEqual(m.events()["strict_stop"], "unimp")

    def test_strict_ignores_unselected_class(self):
        """Negative: an event outside the selected classes is only counted."""
        m = self.start(env={"DM32_STRICT": "unassigned"})
        m.writel(USB, 1)
        self.assertEqual(m.readl(0x140D0040), 0x20151216)   # still running
        ev = m.events()
        self.assertEqual(ev["unimp"], 1)
        self.assertEqual(ev["strict_stop"], "none")

    def test_unknown_class_rejected(self):
        """Negative: an unknown DM32_STRICT class is an error at start-up (exit status 1)."""
        rc, out = run_plain(env={"DM32_STRICT": "unimp,nope"}, args=["-accel", "qtest"])
        self.assertEqual(rc, 1)
        self.assertIn("unknown event class 'nope'", out)

    def test_bad_action_rejected(self):
        """Negative: DM32_STRICT_ACTION accepts only exit or pause."""
        rc, out = run_plain(env={"DM32_STRICT_ACTION": "later"}, args=["-accel", "qtest"])
        self.assertEqual(rc, 1)
        self.assertIn("is not exit or pause", out)


if __name__ == "__main__":
    unittest.main()
