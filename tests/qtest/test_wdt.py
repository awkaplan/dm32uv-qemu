# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# Copyright (c) 2026 Alex Kaplan
"""Watchdog at 0x14010000 (hw/misc/dm32_wdt.c; HR_C7000 v2.7 section 4.10; SVD peripheral WDG)."""

import unittest

from dm32qtest import DM32TestCase, run_plain

BASE = 0x14010000
LOCK, OT_LOAD_H, OT_LOAD_L, RST_LOAD, EN, KNOCK = (BASE + o for o in range(0, 0x18, 4))
UNLOCK_KEY = 0x5ADA7200          # SVD WDG_LOCK
FEED_KEY = 0x55AADD22            # SVD WDG_KNOCK
# The counter counts up from the 35-bit H:L to 2^35 (DS 4.10.3, p.215; OT_LOAD_H/L, p.216-217),
# so the reset values (H=7, L=0xc0000000) give 2^30 ticks, "about 60s". The clock is a model
# assumption (dm32_wdt.c header), 2^30 ticks in 60 s, overridable with DM32_WDT_HZ.
CLOCK_HZ = 17895697


def ticks_ns(ticks, hz=CLOCK_HZ):
    return ticks * 1_000_000_000 // hz


def timeout_ns(h=7, l=0xC0000000, hz=CLOCK_HZ):
    return ticks_ns((1 << 35) - ((h << 32) | l), hz)


SHORT_TIMEOUT_NS = timeout_ns()                       # 2^30 ticks, 60 s
PULSE_NS = ticks_ns(0x04000000 - 0x03F80000)          # RST_LOAD reset value: 2^19 ticks


class WdtTest(DM32TestCase):
    def unlocked(self, env=None):
        m = self.start(env=env)
        m.writel(LOCK, UNLOCK_KEY)
        return m

    def test_reset_values(self):
        """SVD reset values: locked, H=0x7, L=0xc0000000, RST_LOAD=0x03f80000, EN=0, KNOCK=0."""
        m = self.start()
        self.assertEqual(m.readl(LOCK), 1)
        self.assertEqual(m.readl(OT_LOAD_H), 0x7)
        self.assertEqual(m.readl(OT_LOAD_L), 0xC0000000)
        self.assertEqual(m.readl(RST_LOAD), 0x03F80000)
        self.assertEqual(m.readl(EN), 0)
        self.assertEqual(m.readl(KNOCK), 0)

    def test_unlock_and_relock(self):
        """The unlock key unlocks; any other LOCK value locks again (SVD WDG_LOCK)."""
        m = self.start()
        m.writel(LOCK, UNLOCK_KEY)
        self.assertEqual(m.readl(LOCK), 0)
        m.writel(LOCK, 0x12345678)
        self.assertEqual(m.readl(LOCK), 1)

    def test_locked_write_dropped(self):
        """Negative: while locked, writes other than LOCK are dropped and counted as wdt_locked."""
        m = self.start()
        m.writel(OT_LOAD_H, 0x2)
        m.writel(EN, 1)
        self.assertEqual(m.readl(OT_LOAD_H), 0x7)
        self.assertEqual(m.readl(EN), 0)
        self.assertEqual(m.events()["wdt_locked"], 2)

    def test_ot_load_l_read_only(self):
        """Negative: OT_LOAD_L is read-only (SVD), even when unlocked."""
        m = self.unlocked()
        m.writel(OT_LOAD_L, 0x1)
        self.assertEqual(m.readl(OT_LOAD_L), 0xC0000000)

    def test_field_masks(self):
        """OT_LOAD_H is 3 bits; RST_LOAD is 25 bits (SVD field widths)."""
        m = self.unlocked()
        m.writel(OT_LOAD_H, 0xFFFFFFFF)
        m.writel(RST_LOAD, 0xFFFFFFFF)
        self.assertEqual(m.readl(OT_LOAD_H), 0x7)
        self.assertEqual(m.readl(RST_LOAD), 0x01FFFFFF)

    def test_bite_after_timeout(self):
        """EN=1 arms H:L ticks; expiry logs a bite (DM32_WDT_NORESET keeps the guest running)."""
        m = self.unlocked(env={"DM32_WDT_NORESET": "1"})
        m.writel(EN, 1)
        m.clock_step(SHORT_TIMEOUT_NS - 1_000_000)
        self.assertNotIn("WATCHDOG BITE", m.peek_output())
        m.clock_step(2_000_000)
        self.assertEqual(m.close().count("WATCHDOG BITE"), 1)

    def test_no_bite_while_disabled(self):
        """Negative: with EN=0 the count does not run, however long the clock advances."""
        m = self.unlocked(env={"DM32_WDT_NORESET": "1"})
        m.clock_step(10 * SHORT_TIMEOUT_NS)
        self.assertNotIn("WATCHDOG BITE", m.close())

    def test_feed_restarts_count(self):
        """KNOCK with the feed key while enabled restarts the full timeout."""
        m = self.unlocked(env={"DM32_WDT_NORESET": "1"})
        m.writel(EN, 1)
        m.clock_step(SHORT_TIMEOUT_NS * 3 // 4)
        m.writel(KNOCK, FEED_KEY)
        m.clock_step(SHORT_TIMEOUT_NS * 3 // 4)
        m.writel(KNOCK, 0x1234)                    # wrong key: stored, does not feed
        self.assertEqual(m.readl(KNOCK), 0x1234)
        m.clock_step(SHORT_TIMEOUT_NS // 2)
        self.assertEqual(m.close().count("WATCHDOG BITE"), 1)

    def test_bite_resets_machine(self):
        """Without DM32_WDT_NORESET a bite resets the machine, which re-locks the watchdog."""
        m = self.unlocked()
        m.writel(EN, 1)
        m.clock_step(SHORT_TIMEOUT_NS + PULSE_NS + 1_000_000)
        self.assertEqual(m.readl(EN), 0)
        self.assertEqual(m.readl(LOCK), 1)

    def test_default_timeout_is_60s(self):
        """The reset values give the datasheet's "about 60s" (OT_LOAD_H/L, p.216-217): no bite at
        59.9 s, a bite by 60.1 s."""
        m = self.unlocked(env={"DM32_WDT_NORESET": "1"})
        m.writel(EN, 1)
        m.clock_step(59_900_000_000)
        self.assertNotIn("WATCHDOG BITE", m.peek_output())
        m.clock_step(200_000_000)
        self.assertEqual(m.close().count("WATCHDOG BITE"), 1)

    def test_lower_h_gives_longer_timeout(self):
        """The counter counts up from H:L to the 2^35 overflow (p.215-217), so a smaller load
        gives a longer timeout: H=6 adds 2^32 ticks."""
        m = self.unlocked(env={"DM32_WDT_NORESET": "1"})
        m.writel(OT_LOAD_H, 6)
        m.writel(EN, 1)
        m.clock_step(timeout_ns(7) + 1_000_000)
        self.assertNotIn("WATCHDOG BITE", m.peek_output())
        m.clock_step(timeout_ns(6) - timeout_ns(7))
        self.assertEqual(m.close().count("WATCHDOG BITE"), 1)

    def test_reset_waits_for_pulse(self):
        """The reset takes effect at the end of the reset pulse: RST_LOAD counts up to 0x03ffffff
        (p.218), 2^19 ticks with the reset value. Just after the bite the watchdog is still
        unlocked and enabled; after the pulse the machine has been reset."""
        m = self.unlocked()
        m.writel(EN, 1)
        m.clock_step(SHORT_TIMEOUT_NS + 1_000)
        self.assertEqual(m.readl(LOCK), 0)
        self.assertEqual(m.readl(EN), 1)
        m.clock_step(PULSE_NS)
        self.assertEqual(m.readl(LOCK), 1)
        self.assertEqual(m.readl(EN), 0)

    def test_rst_load_sets_pulse(self):
        """A larger RST_LOAD shortens the pulse: 0x01ffff00 is 0x04000000 - 0x01ffff00 ticks."""
        m = self.unlocked()
        m.writel(RST_LOAD, 0x01FFFF00)
        m.writel(EN, 1)
        m.clock_step(SHORT_TIMEOUT_NS + 1_000)
        pulse = ticks_ns(0x04000000 - 0x01FFFF00)
        m.clock_step(pulse - 1_000_000)
        self.assertEqual(m.readl(LOCK), 0)
        m.clock_step(2_000_000)
        self.assertEqual(m.readl(LOCK), 1)

    def test_clock_override(self):
        """DM32_WDT_HZ sets the counter clock: at 2^30 Hz the reset values time out in 1 s."""
        m = self.unlocked(env={"DM32_WDT_NORESET": "1", "DM32_WDT_HZ": str(1 << 30)})
        m.writel(EN, 1)
        m.clock_step(999_000_000)
        self.assertNotIn("WATCHDOG BITE", m.peek_output())
        m.clock_step(2_000_000)
        self.assertEqual(m.close().count("WATCHDOG BITE"), 1)

    def test_bad_clock_rejected(self):
        """Negative: DM32_WDT_HZ must be a positive number."""
        for bad in ("0", "fast"):
            rc, out = run_plain(env={"DM32_WDT_HZ": bad}, args=["-accel", "qtest"])
            self.assertEqual(rc, 1, bad)
            self.assertIn("DM32_WDT_HZ", out)

    def test_clock_below_4_hz_rejected(self):
        """Negative: 1 to 3 Hz, and anything above 32 bits, is rejected at start-up. At 1 to 3 Hz
        the 2^35-tick deadline in ns exceeds int64 (dm32_wdt.c, DM32_WDT_HZ; muldiv64 takes a
        uint32_t divisor)."""
        for bad in ("1", "2", "3", str(1 << 32)):
            rc, out = run_plain(env={"DM32_WDT_HZ": bad}, args=["-accel", "qtest"])
            self.assertEqual(rc, 1, bad)
            self.assertIn("DM32_WDT_HZ", out)

    def test_clock_4_hz_accepted(self):
        """The lowest accepted clock, 4 Hz, starts the count: EN=1 arms it and reads back
        (dm32_wdt.c, DM32_WDT_HZ)."""
        m = self.unlocked(env={"DM32_WDT_HZ": "4"})
        m.writel(EN, 1)
        self.assertEqual(m.readl(EN), 1)
        self.assertNotIn("WATCHDOG BITE", m.close())


if __name__ == "__main__":
    unittest.main()
