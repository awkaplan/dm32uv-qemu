# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# Copyright (c) 2026 Alex Kaplan
"""PWM controller at 0x140C0000, channels at 0x20 * n (hw/misc/dm32_pwm.c; HR_C7000 v2.7
section 5.8; SVD peripheral PWM; docs/devices.md "PWM"; docs/environment.md "PWM")."""

import os
import unittest

from dm32qtest import DM32TestCase

BASE = 0x140C0000
CH_STRIDE = 0x20
CFG, RPT, PER, FP, STATUS = 0x00, 0x04, 0x08, 0x0C, 0x10
INTR_STA = BASE + 0xFF0
START, SOFT_RST, CFG_UPDATE, INTEN = 1 << 0, 1 << 1, 1 << 2, 1 << 3
MODE_ONESHOT, MODE_CONT = 1 << 4, 2 << 4
MODE_RESERVED0, MODE_RESERVED3 = 0, 3 << 4
STATUS_BUSY = 1 << 8
PIC_PWM = 18                     # SVD PWM interrupt; not wired in the model


def reg(ch, off):
    return BASE + CH_STRIDE * ch + off


def trace_lines(path):
    """The parsed rows of a DM32_PWM_TRACE file: (act_per, act_fp, cfg) for channel 0."""
    rows = []
    with open(path) as f:
        for line in f:
            _, ch, per, fp, cfg = line.split()
            if ch == "0":
                rows.append((int(per), int(fp), int(cfg)))
    return rows


class PwmTest(DM32TestCase):
    def test_reset_values(self):
        """Reset values: every channel's CFG, RPT, PER, FP and STATUS are 0, and PWM_INTR_STA
        reads 0 (docs/devices.md "PWM"; SVD PWM reset values)."""
        m = self.start()
        for ch in range(3):
            for off in (CFG, RPT, PER, FP, STATUS):
                self.assertEqual(m.readl(reg(ch, off)), 0, (ch, hex(off)))
        self.assertEqual(m.readl(INTR_STA), 0)

    def test_cfg_masked_to_10_bits(self):
        """CFG bits 9:0 are defined (SVD PWM_CFG; dm32_pwm.c header); higher bits are dropped."""
        m = self.start()
        m.writel(reg(0, CFG), 0xFFFFFFFF)
        self.assertEqual(m.readl(reg(0, CFG)), 0x3FF)

    def test_rpt_masked_to_16_bits(self):
        """RPT is pwm_rpt, bits 15:0 (SVD PWM_RPT; dm32_pwm.c header)."""
        m = self.start()
        m.writel(reg(0, RPT), 0xFFFFFFFF)
        self.assertEqual(m.readl(reg(0, RPT)), 0xFFFF)

    def test_per_fp_full_width(self):
        """PER and FP are full 32-bit registers, stored and read back as written (SVD; the
        model keeps them unmasked)."""
        m = self.start()
        m.writel(reg(1, PER), 0xDEADBEEF)
        m.writel(reg(1, FP), 0x80000001)
        self.assertEqual(m.readl(reg(1, PER)), 0xDEADBEEF)
        self.assertEqual(m.readl(reg(1, FP)), 0x80000001)

    def test_channels_are_independent(self):
        """Channel n sits at 0x20 * n, so a write to channel 1 leaves channels 0 and 2 alone
        (docs/devices.md "PWM")."""
        m = self.start()
        m.writel(reg(1, PER), 0x1234)
        self.assertEqual(m.readl(reg(0, PER)), 0)
        self.assertEqual(m.readl(reg(1, PER)), 0x1234)
        self.assertEqual(m.readl(reg(2, PER)), 0)

    def test_status_bits_stored(self):
        """STATUS bits 7:0 and 9 are stored; bit 8 (busy) is derived from CFG
        (docs/devices.md "PWM", STATUS row). 0x2AB has bit 8 clear."""
        m = self.start()
        m.writel(reg(0, STATUS), 0x2AB)
        self.assertEqual(m.readl(reg(0, STATUS)), 0x2AB)

    def test_busy_follows_running_state(self):
        """STATUS bit 8 is 1 while the channel runs (start set, soft_rst clear, mode 1 or 2) and
        0 otherwise (docs/devices.md "PWM"; dm32_pwm.c header, "Running")."""
        m = self.start()
        m.writel(reg(0, CFG), START | MODE_CONT)
        self.assertEqual(m.readl(reg(0, STATUS)) & STATUS_BUSY, STATUS_BUSY)
        m.writel(reg(0, CFG), START | SOFT_RST | MODE_CONT)
        self.assertEqual(m.readl(reg(0, STATUS)) & STATUS_BUSY, 0)
        m.writel(reg(0, CFG), START | MODE_ONESHOT)
        self.assertEqual(m.readl(reg(0, STATUS)) & STATUS_BUSY, STATUS_BUSY)
        m.writel(reg(0, CFG), 0)
        self.assertEqual(m.readl(reg(0, STATUS)) & STATUS_BUSY, 0)

    def test_reserved_modes_never_busy(self):
        """Negative: modes 0 and 3 are "Reserved, no PWM Waveform output" in the SVD, so a
        started channel in those modes never reports busy (dm32_pwm.c header, "Running")."""
        m = self.start()
        for mode in (MODE_RESERVED0, MODE_RESERVED3):
            m.writel(reg(2, CFG), START | mode)
            self.assertEqual(m.readl(reg(2, STATUS)) & STATUS_BUSY, 0, hex(mode))

    def test_status_write_ignores_busy(self):
        """Negative: pwm_busy is derived from CFG, so a STATUS write with bit 8 set does not
        set it on an idle channel and reads back 0 (SVD PWM_STATUS; dm32_pwm.c, "STATUS")."""
        m = self.start()
        m.writel(reg(0, STATUS), STATUS_BUSY)
        self.assertEqual(m.readl(reg(0, STATUS)), 0)

    def test_latch_on_start_and_cfg_update(self):
        """PER and FP latch on the start edge, and in CONTINUOUS mode again on a cfg_update 0->1
        write; the trace shows the latched values (docs/devices.md "PWM"; dm32_pwm.c header,
        "Latched period and phase")."""
        d = self.make_tempdir()
        path = os.path.join(d, "trace.txt")
        m = self.start(env={"DM32_PWM_TRACE": path})
        m.writel(reg(0, PER), 100)
        m.writel(reg(0, FP), 30)
        m.writel(reg(0, CFG), START | MODE_CONT)
        m.writel(reg(0, PER), 200)                    # running: latched value unchanged
        m.writel(reg(0, FP), 60)
        m.writel(reg(0, CFG), START | MODE_CONT | CFG_UPDATE)
        m.close()
        self.assertEqual(trace_lines(path), [
            (100, 0, 0),
            (100, 30, 0),
            (100, 30, START | MODE_CONT),
            (200, 60, START | MODE_CONT | CFG_UPDATE),
        ])

    def test_running_per_write_not_latched(self):
        """Negative: a PER or FP write while a CONTINUOUS channel runs, without a cfg_update
        0->1, does not reach the output (dm32_pwm.c header; trace has no row for 200)."""
        d = self.make_tempdir()
        path = os.path.join(d, "trace.txt")
        m = self.start(env={"DM32_PWM_TRACE": path})
        m.writel(reg(0, PER), 100)
        m.writel(reg(0, CFG), START | MODE_CONT)
        m.writel(reg(0, PER), 200)
        m.writel(reg(0, CFG), START | MODE_CONT)      # no cfg_update edge
        m.close()
        self.assertNotIn(200, [per for per, _, _ in trace_lines(path)])

    def test_intr_sta_reads_zero_and_no_irq(self):
        """Negative: PWM_INTR_STA reads 0 and ignores writes, and a started channel with
        INTEN set drives no PIC line: no waveform event is modelled (dm32_pwm.c header,
        "Interrupt"; docs/devices.md "PWM")."""
        m = self.start()
        m.irq_intercept_in(m.pic_path())
        m.writel(INTR_STA, 0xFF)
        self.assertEqual(m.readl(INTR_STA), 0)
        m.writel(reg(0, CFG), START | MODE_CONT | INTEN)
        self.assertNotIn(PIC_PWM, [line for _, line in m.irq_log])

    def test_reserved_offset_is_storage(self):
        """Offsets inside a channel that the SVD does not name are storage, with a one-time
        note on stderr (dm32_pwm.c header, "Other offsets in the window")."""
        m = self.start()
        m.writel(BASE + 0x14, 0xCAFE)
        self.assertEqual(m.readl(BASE + 0x14), 0xCAFE)
        self.assertIn("write to reserved offset 0x014 (storage only)", m.close())


if __name__ == "__main__":
    unittest.main()
