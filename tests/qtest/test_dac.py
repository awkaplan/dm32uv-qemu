# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# Copyright (c) 2026 Alex Kaplan
"""CPU-side 12-bit DAC at 0x140F0000 (hw/misc/dm32_dac.c; HR_C7000 v2.7 section 5.10; SVD
peripheral DAC; docs/devices.md "DAC"; docs/environment.md "DAC")."""

import os
import unittest

from dm32qtest import DM32TestCase

BASE = 0x140F0000
PD_CTRL, PD_MODE_EN = BASE + 0x00, BASE + 0x04
DATA_A, DATA_B, DATA_C = BASE + 0x08, BASE + 0x0C, BASE + 0x10
VERID = BASE + 0x14
PIC_DAC = 25                     # SVD DAC interrupt; not wired in the model


class DacTest(DM32TestCase):
    def test_reset_values(self):
        """Reset values: PD_CTRL 0x7 and PD_MODE_EN 0x7, DATA 0, VERID 0x20160229 (SVD DAC;
        dm32_dac.c header, DS 5.10.5.6)."""
        m = self.start()
        self.assertEqual(m.readl(PD_CTRL), 0x7)
        self.assertEqual(m.readl(PD_MODE_EN), 0x7)
        self.assertEqual(m.readl(DATA_A), 0)
        self.assertEqual(m.readl(DATA_B), 0)
        self.assertEqual(m.readl(DATA_C), 0)
        self.assertEqual(m.readl(VERID), 0x20160229)

    def test_pd_registers_masked_to_3_bits(self):
        """PD_CTRL and PD_MODE_EN are 3-bit fields (SVD; dm32_dac.c header, "Stored values are
        masked to the register's real bits")."""
        m = self.start()
        m.writel(PD_CTRL, 0xFFFFFFFF)
        m.writel(PD_MODE_EN, 0xFFFFFFFF)
        self.assertEqual(m.readl(PD_CTRL), 0x7)
        self.assertEqual(m.readl(PD_MODE_EN), 0x7)

    def test_data_masked_to_12_bits(self):
        """DATA_A is 12 bits (SVD; dm32_dac.c header, "DATA: 12 bits")."""
        m = self.start()
        m.writel(PD_CTRL, 0x6)                       # power channel A up
        m.writel(DATA_A, 0xFFFFFFFF)
        self.assertEqual(m.readl(DATA_A), 0xFFF)

    def test_data_per_channel_storage(self):
        """With all channels powered up, A, B and C store their own values at +0x08, +0x0c and
        +0x10 (dm32_dac.c header, "Firmware channel numbering")."""
        m = self.start()
        m.writel(PD_CTRL, 0)
        m.writel(DATA_A, 0x123)
        m.writel(DATA_B, 0x456)
        m.writel(DATA_C, 0xABC)
        self.assertEqual(m.readl(DATA_A), 0x123)
        self.assertEqual(m.readl(DATA_B), 0x456)
        self.assertEqual(m.readl(DATA_C), 0xABC)
        self.assertEqual(m.events()["dac_pd"], 0)

    def test_power_up_then_write_no_event(self):
        """Powering a channel up (clearing its PD bit) and then writing gives no dac_pd event
        (dm32_dac.c header, "A DATA write while that channel's PD bit is 1")."""
        m = self.start()
        m.writel(PD_CTRL, 0x6)
        m.writel(DATA_A, 0x7FF)
        self.assertEqual(m.events()["dac_pd"], 0)

    def test_pd_is_per_channel(self):
        """PD_CTRL bits are per channel (bit 0 A, bit 1 B, bit 2 C): with only A up, writes to
        B and C are counted and A is not (dm32_dac.c header, "Firmware channel numbering")."""
        m = self.start()
        m.writel(PD_CTRL, 0x6)
        m.writel(DATA_A, 0x1)
        m.writel(DATA_B, 0x2)
        m.writel(DATA_C, 0x3)
        self.assertEqual(m.events()["dac_pd"], 2)

    def test_powered_down_write_stored_and_counted(self):
        """Negative: a DATA write to a powered-down channel is stored (not dropped), and counts
        a dac_pd event on every write, not only the first (dm32_dac.c header, "warns once per
        channel" and the dac_pd event)."""
        m = self.start()
        for value in (0x111, 0x222, 0x123):
            m.writel(DATA_A, value)
        self.assertEqual(m.readl(DATA_A), 0x123)
        self.assertEqual(m.events()["dac_pd"], 3)

    def test_word_access_only(self):
        """Negative: only 4-byte accesses are valid (dm32_dac.c ops: valid.min = max = 4).
        A byte write is rejected by the memory core, so DATA keeps its value."""
        m = self.start()
        m.writel(PD_CTRL, 0)
        m.writel(DATA_A, 0x321)
        m.writeb(DATA_A, 0x55)
        self.assertEqual(m.readl(DATA_A), 0x321)

    def test_unmodelled_offset_is_storage(self):
        """Offsets the model does not name are plain storage with a one-time note on stderr
        (dm32_dac.c header, "Registers in the window other than the ones above")."""
        m = self.start()
        m.writel(BASE + 0x20, 0x1234)
        self.assertEqual(m.readl(BASE + 0x20), 0x1234)
        self.assertIn("write to unmodelled offset 0x20 (storage only)", m.close())

    def test_no_interrupt_line(self):
        """No PIC line is driven by the DAC: docs/devices.md "DAC" says "Interrupt wiring:
        none" (the SVD lists PIC source 25)."""
        m = self.start()
        m.irq_intercept_in(m.pic_path())
        m.writel(PD_CTRL, 0)
        m.writel(DATA_A, 0x800)
        self.assertNotIn(PIC_DAC, [line for _, line in m.irq_log])

    def test_trace_records_data_writes(self):
        """DM32_DAC_TRACE appends "<vtime_ns> <A|B|C> <value>" per DATA write (docs/environment.md
        "DAC"; dm32_dac.c header)."""
        d = self.make_tempdir()
        path = os.path.join(d, "trace.txt")
        m = self.start(env={"DM32_DAC_TRACE": path})
        m.writel(PD_CTRL, 0)
        m.writel(DATA_B, 0x291)
        m.close()
        with open(path) as f:
            lines = f.read().split("\n")
        self.assertEqual(lines[0].split()[1:], ["B", "657"])


if __name__ == "__main__":
    unittest.main()
