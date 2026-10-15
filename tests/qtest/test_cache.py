# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# Copyright (c) 2026 Alex Kaplan
"""CK803 cache controller at 0xE000F000 (hw/misc/dm32_cache.c). The register layout is XuanTie's
CSI core header (core_803.h, CACHE_Type and the CACHE_* field definitions); the HR_C7000
datasheet does not describe the block."""

import unittest

from dm32qtest import DM32TestCase

BASE = 0xE000F000
CER, CIR = BASE + 0x000, BASE + 0x004
CRCR = [BASE + 0x008 + 4 * n for n in range(4)]
CPFCR, CPFATR, CPFMTR = BASE + 0xFF4, BASE + 0xFF8, BASE + 0xFFC


class CacheTest(DM32TestCase):
    def test_reset_values(self):
        """Every register reads 0 after reset (assumption in dm32_cache.c: the header gives no
        reset values)."""
        m = self.start()
        for reg in [CER, CIR, CPFCR, CPFATR, CPFMTR] + CRCR:
            self.assertEqual(m.readl(reg), 0, hex(reg))

    def test_cer_keeps_defined_bits(self):
        """CER has EN [0], CFIG [1], WB [2], DCW [4] and WA [5]; other bits read 0."""
        m = self.start()
        m.writel(CER, 0xFFFFFFFF)
        self.assertEqual(m.readl(CER), 0x37)

    def test_crcr_fields(self):
        """CRCRn has EN [0], SIZE [5:1] and base [31:10]; bits 9:6 read 0. The four regions are
        separate registers."""
        m = self.start()
        m.writel(CRCR[0], 0xFFFFFFFF)
        m.writel(CRCR[3], 0x03000027)          # 0x03000000, 1 MB (code 0x13), enabled
        self.assertEqual(m.readl(CRCR[0]), 0xFFFFFC3F)
        self.assertEqual(m.readl(CRCR[1]), 0)
        self.assertEqual(m.readl(CRCR[3]), 0x03000027)

    def test_cir_operation_completes(self):
        """A CIR operation completes at once and CIR reads 0 (the CSI helpers write CIR without
        polling; the header defines no status bit)."""
        m = self.start()
        for op in (0x1, 0x2 | 0x03001000, 0x4, 0x8 | 0x03001000, 0x5):
            m.writel(CIR, op)
            self.assertEqual(m.readl(CIR), 0, hex(op))

    def test_profiling(self):
        """CPFCR keeps PFEN; PFRST resets the counters and reads 0. No accesses are counted, so
        CPFATR and CPFMTR read 0, and writes to them are ignored."""
        m = self.start()
        m.writel(CPFCR, 0x3)
        self.assertEqual(m.readl(CPFCR), 0x1)
        m.writel(CPFATR, 0x1234)
        self.assertEqual(m.readl(CPFATR), 0)
        self.assertEqual(m.readl(CPFMTR), 0)

    def test_reserved_space(self):
        """Negative: the reserved space between CRCR3 and CPFCR reads 0 and ignores writes."""
        m = self.start()
        m.writel(BASE + 0x100, 0xFFFFFFFF)
        self.assertEqual(m.readl(BASE + 0x100), 0)

    def test_word_access_only(self):
        """Negative: the registers are 32-bit; a byte write is rejected and leaves CER alone."""
        m = self.start()
        m.writeb(CER, 0x01)
        self.assertEqual(m.readl(CER), 0)

    def test_system_reset(self):
        """A machine reset returns the registers to 0."""
        m = self.start()
        m.writel(CER, 0x5)
        m.writel(CRCR[0], 0x03000027)
        m.qmp("system_reset")
        m.clock_step(1_000_000)
        self.assertEqual(m.readl(CER), 0)
        self.assertEqual(m.readl(CRCR[0]), 0)

    def test_log_decodes_configuration(self):
        """DM32_CACHE_LOG prints the decoded enable, operation and region."""
        m = self.start(env={"DM32_CACHE_LOG": "1"})
        m.writel(CIR, 0x1)
        m.writel(CER, 0x4)
        m.writel(CRCR[0], 0x03000027)
        m.writel(CER, 0x5)
        out = m.close()
        self.assertIn("CIR: invalidate-all", out)
        self.assertIn("region 0: 0x03000000, 1048576 bytes", out)
        self.assertIn("CER = 0x05 (on WB)", out)


if __name__ == "__main__":
    unittest.main()
