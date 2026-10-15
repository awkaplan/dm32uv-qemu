# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# Copyright (c) 2026 Alex Kaplan
"""ADC controller at 0x140D0000 (hw/misc/dm32_adc.c; HR_C7000 v2.7 section 5.9; SVD peripheral
ADC; docs/devices.md "ADC"; docs/environment.md "ADC")."""

import unittest

from dm32qtest import DM32TestCase, run_plain

BASE = 0x140D0000
CTRL, INTR, INTR_STA, START = BASE + 0x00, BASE + 0x08, BASE + 0x0C, BASE + 0x14
CTRL_STATE, CTRL_STOP, CH_VLD = BASE + 0x18, BASE + 0x20, BASE + 0x24
PD_SEOC_TIME, DATA_AB, DATA_CD = BASE + 0x28, BASE + 0x30, BASE + 0x34
DATA_EF, DATA_GH, VERID = BASE + 0x38, BASE + 0x3C, BASE + 0x40
IO_DIPLEX2 = 0x1100003C          # bit (19 + ch) set selects GPIO for ADC channel ch
PD_FORCE_HIGH = 1 << 3
INTR_MASTER = 1 << 16            # ADC_INTR bit 16, the master enable
PIC_ADC = 26                     # SVD ADC interrupt; not wired in the model


def claim_pad(m, ch):
    """Mux channel `ch`'s pad to the analogue input by clearing its IO_DIPLEX2 bit."""
    m.writel(IO_DIPLEX2, m.readl(IO_DIPLEX2) & ~(1 << (19 + ch)))


def convert(m, ch_mask):
    """Select channels in ADC_CH_VLD and start one conversion."""
    m.writel(CH_VLD, ch_mask)
    m.writel(START, 1)


def sample(m, ch):
    """The 10-bit DATA result of channel `ch`, from its pair register.

    Even channels are in bits 9:0 of the pair register, odd channels in bits 25:16.
    """
    word = m.readl(DATA_AB + 4 * (ch // 2))
    return ((word >> 16) if ch % 2 else word) & 0x3FF


class AdcTest(DM32TestCase):
    def test_reset_values(self):
        """Reset values: PD_SEOC_TIME 0x0a20 (dm32_adc.c, "v2.7 EN 5.9.5"); the rest 0."""
        m = self.start()
        self.assertEqual(m.readl(CTRL), 0)
        self.assertEqual(m.readl(INTR), 0)
        self.assertEqual(m.readl(INTR_STA), 0)
        self.assertEqual(m.readl(CH_VLD), 0)
        self.assertEqual(m.readl(PD_SEOC_TIME), 0x0A20)
        self.assertEqual(m.readl(CTRL_STATE), 0)
        for reg in (DATA_AB, DATA_CD, DATA_EF, DATA_GH):
            self.assertEqual(m.readl(reg), 0, hex(reg))

    def test_verid(self):
        """ADC_CTRL_VERID reads 0x20151216 (SVD ADC VERID, DS 5.9.5.17)."""
        m = self.start()
        self.assertEqual(m.readl(VERID), 0x20151216)

    def test_ch_vld_stores_low_byte(self):
        """CH_VLD bits 7:0 select the eight channels (docs/devices.md "ADC"); bit 8 is not kept."""
        m = self.start()
        m.writel(CH_VLD, 0x1FF)
        self.assertEqual(m.readl(CH_VLD), 0xFF)

    def test_single_scan_converts_lowest_selected(self):
        """With channels 1 and 2 selected, only the lowest (1) converts (dm32_adc.c header,
        "Single scan"; HR_C7000 v2.7 section 5.9.5.10 note)."""
        m = self.start(env={"DM32_ADC_CH1": "0x111", "DM32_ADC_CH2": "0x222"})
        claim_pad(m, 1)
        claim_pad(m, 2)
        convert(m, 0b0110)
        self.assertEqual(sample(m, 1), 0x111)
        self.assertEqual(sample(m, 2), 0)

    def test_results_accumulate_per_channel(self):
        """Each conversion latches its own channel; a later conversion keeps earlier results
        and packs both into the pair register (dm32_adc.c "DATA" comment: even bits 9:0, odd
        bits 25:16)."""
        m = self.start(env={"DM32_ADC_CH0": "0x2AB", "DM32_ADC_CH1": "0x155"})
        claim_pad(m, 0)
        claim_pad(m, 1)
        convert(m, 0b01)
        convert(m, 0b10)
        self.assertEqual(sample(m, 0), 0x2AB)
        self.assertEqual(sample(m, 1), 0x155)
        self.assertEqual(m.readl(DATA_AB), (0x155 << 16) | 0x2AB)

    def test_input_above_10_bits_rejected(self):
        """Negative: DM32_ADC_CHn is a 10-bit level (docs/environment.md "ADC"); a larger value,
        or one that is not a number, stops QEMU at start-up with an error naming the variable."""
        for bad in ("0x400", "0x7FF", "high"):
            rc, out = run_plain(env={"DM32_ADC_CH0": bad}, args=["-accel", "qtest"])
            self.assertEqual(rc, 1, bad)
            self.assertIn("DM32_ADC_CH0", out)

    def test_start_self_clears_and_idle(self):
        """ADC_START reads 0 after EOC, and CTRL_STATE reads idle (dm32_adc.c header;
        docs/devices.md "ADC", ADC_START and CTRL_STATE rows)."""
        m = self.start(env={"DM32_ADC_CH0": "0x100"})
        claim_pad(m, 0)
        convert(m, 0b01)
        self.assertEqual(m.readl(START), 0)
        self.assertEqual(m.readl(CTRL_STATE), 0)

    def test_pad_gpio_reads_zero(self):
        """Negative: a channel whose pad is still GPIO reads 0 even with an input present, and
        counts an adc_pad event (docs/devices.md "ADC"; dm32_adc.c header, "pad mux")."""
        m = self.start(env={"DM32_ADC_CH0": "0x3FF"})
        self.assertEqual(m.readl(IO_DIPLEX2) & (1 << 19), 1 << 19)   # reset: pad is GPIO
        convert(m, 0b01)
        self.assertEqual(sample(m, 0), 0)
        self.assertEqual(m.events()["adc_pad"], 1)

    def test_unset_input_reads_zero(self):
        """Negative: a channel with no DM32_ADC_CHn level reads 0, not a plausible voltage
        (docs/environment.md "ADC"; dm32_adc.c header, "Unset channels read as 0")."""
        m = self.start()
        claim_pad(m, 3)
        convert(m, 0b1000)
        self.assertEqual(sample(m, 3), 0)
        self.assertEqual(m.events()["adc_pad"], 0)

    def test_pd_force_high_holds_reset(self):
        """Negative: with PD_FORCE_HIGH (CTRL bit 3) set, START is ignored and DATA reads 0
        (dm32_adc.c header, "PD_FORCE_HIGH"); clearing it lets the next START convert."""
        m = self.start(env={"DM32_ADC_CH0": "0x2AB"})
        claim_pad(m, 0)
        m.writel(CTRL, PD_FORCE_HIGH)
        convert(m, 0b01)
        self.assertEqual(sample(m, 0), 0)
        m.writel(CTRL, 0)
        convert(m, 0b01)
        self.assertEqual(sample(m, 0), 0x2AB)

    def test_ctrl_stop_reset_clears_data(self):
        """CTRL_STOP bit 1 resets the controller, so DATA reads 0 afterwards (docs/devices.md
        "ADC", CTRL_STOP row)."""
        m = self.start(env={"DM32_ADC_CH0": "0x2AB"})
        claim_pad(m, 0)
        convert(m, 0b01)
        m.writel(CTRL_STOP, 0x2)
        self.assertEqual(sample(m, 0), 0)

    def test_intr_sta_latches_and_clears_w1c(self):
        """ADC_INTR_STA latches the channel's bit when enabled in ADC_INTR (bit 16 plus the
        channel bit), and a write of 1 clears it (docs/devices.md "ADC", ADC_INTR rows)."""
        m = self.start(env={"DM32_ADC_CH0": "0x100"})
        claim_pad(m, 0)
        m.writel(INTR, INTR_MASTER | 0x1)
        convert(m, 0b01)
        self.assertEqual(m.readl(INTR_STA), 0x1)
        m.writel(INTR_STA, 0x1)
        self.assertEqual(m.readl(INTR_STA), 0)

    def test_intr_sta_needs_enable(self):
        """Negative: with ADC_INTR at 0 a conversion does not latch ADC_INTR_STA (dm32_adc.c
        header and docs/devices.md: the completion flag needs bit 16 and the channel bit)."""
        m = self.start(env={"DM32_ADC_CH0": "0x100"})
        claim_pad(m, 0)
        convert(m, 0b01)
        self.assertEqual(m.readl(INTR_STA), 0)

    def test_no_interrupt_line(self):
        """ADC_INTR_STA latches, but no PIC line is driven: docs/devices.md "ADC" says
        "Interrupt wiring: none" (the SVD lists PIC source 26)."""
        m = self.start(env={"DM32_ADC_CH0": "0x100"})
        claim_pad(m, 0)
        m.irq_intercept_in(m.pic_path())
        m.writel(INTR, INTR_MASTER | 0x1)
        convert(m, 0b01)
        self.assertEqual(m.readl(INTR_STA), 0x1)
        self.assertNotIn(PIC_ADC, [line for _, line in m.irq_log])


if __name__ == "__main__":
    unittest.main()
