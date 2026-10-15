# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# Copyright (c) 2026 Alex Kaplan
"""Audio codec registers at 0x160009c0 (hw/misc/dm32_codec.c; HR_C7000 v2.7 "DS" section 6.4
and Register Table 26, 6.5.2.1-6.5.2.37; SystemControl 4.6.5.5-4.6.5.6; Table 30 source 37).
Byte-wide registers inside the Modem_Buffer window."""

import unittest

from dm32qtest import DM32TestCase

BASE = 0x160009C0
SYS = 0x11000000
SOFT_RSTN, LINEOUT_CTRL, CODEC_I2C_MUX = SYS + 0x00, SYS + 0x88, SYS + 0x8C
SRC = 37
MS = 1_000_000
US = 1_000

SR1, SR2, MR, AICR_DAC, AICR_ADC = 0x00, 0x01, 0x07, 0x08, 0x09
CR_MIC1, CR_DAC, CR_ADC, CR_MIX, DR_MIX, CR_VIC, CR_CK = 0x0B, 0x0D, 0x0F, 0x10, 0x11, 0x12, 0x13
CR_TIMER_H, CR_TIMER_L, ICR, IMR, IFR, IMR2, IFR2 = 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E
CR_DAC_AGC, DR_DAC_AGC, CR_ADC_AGC, DR_ADC_AGC = 0x2B, 0x2C, 0x2F, 0x30

# DS 6.5.2 reset values of all 37 registers. DR_ADC_AGC (0x30) reads the register its control
# register indexes while load is 0, so at reset it reads ADC_AGC_0 (0x34), not its 0x00 latch.
RESET = {
    0x00: 0x00, 0x01: 0x00, 0x07: 0x00, 0x08: 0xD3, 0x09: 0xD3, 0x0B: 0x30, 0x0C: 0x30,
    0x0D: 0xB0, 0x0F: 0xB0, 0x10: 0x00, 0x11: 0x00, 0x12: 0x0F, 0x13: 0x40, 0x14: 0x00,
    0x15: 0x00, 0x16: 0x00, 0x17: 0x00, 0x18: 0x00, 0x19: 0x00, 0x1A: 0x00, 0x1B: 0xFF,
    0x1C: 0x00, 0x1D: 0xFF, 0x1E: 0x00, 0x1F: 0x00, 0x20: 0x00, 0x23: 0x00, 0x25: 0x00,
    0x26: 0x00, 0x27: 0x00, 0x28: 0x00, 0x29: 0x00, 0x2A: 0x00, 0x2B: 0x00, 0x2C: 0x00,
    0x2F: 0x00, 0x30: 0x34,
}


class Codec:
    def __init__(self, m):
        self.m = m

    def r(self, off):
        return self.m.readb(BASE + off)

    def w(self, off, val):
        self.m.writeb(BASE + off, val)

    def playback(self):
        """DS 6.4.2.4, sleep to playback: SB = 0, SB_AICR_DAC = 0, SB_DAC = 0, soft mute off."""
        self.w(CR_VIC, 0x0E)
        self.w(AICR_DAC, 0xC3)
        self.w(CR_DAC, 0x20)


class CodecRegisterTest(DM32TestCase):
    def start(self, *a, **kw):
        m = super().start(*a, **kw)
        self.c = Codec(m)
        return m

    def test_reset_values(self):
        """All 37 registers read their DS 6.5.2 reset values, 0x27-0x30 included (absent from
        the SVD); everything powered down and soft-muted."""
        self.start()
        for off, val in RESET.items():
            self.assertEqual(self.c.r(off), val, f"0x{off:02x}")

    def test_undefined_addresses(self):
        """Negative: addresses Table 26 does not define read 0 and keep no write."""
        self.start()
        for off in (0x02, 0x06, 0x0A, 0x0E, 0x21, 0x24, 0x2D, 0x31, 0x3F):
            self.c.w(off, 0x5A)
            self.assertEqual(self.c.r(off), 0, f"0x{off:02x}")

    def test_read_only_status(self):
        """Negative: SR1, SR2 and MR are read-only (DS 6.5.2.1-3)."""
        self.start()
        for off in (SR1, SR2, MR):
            self.c.w(off, 0xFF)
            self.assertEqual(self.c.r(off), 0)

    def test_read_write(self):
        """Read-write registers keep a written byte, the gains at 0x1f-0x2a included."""
        self.start()
        for off in (0x13, 0x17, 0x1F, 0x20, 0x23, 0x25, 0x27, 0x2A):
            self.c.w(off, 0xA5)
            self.assertEqual(self.c.r(off), 0xA5, f"0x{off:02x}")

    def test_dmr_ram_around_window_unchanged(self):
        """The DMR RAM below and above the 64-byte codec window is still RAM."""
        m = self.start()
        for addr in (0x160009BF, 0x16000A00):
            m.writeb(addr, 0x77)
            self.assertEqual(m.readb(addr), 0x77)

    def test_stock_power_up(self):
        """Stock's boot sequence (logged): CR_VIC 0x0f -> 0x0e -> 0x0c, CR_ADC 0xa0 -> 0x20, then
        the DAC path. With the ADC on and soft mute off the codec is working: pon_ack sets."""
        m = self.start()
        for off, val in ((CR_VIC, 0x0E), (CR_VIC, 0x0C), (AICR_ADC, 0xC0), (CR_ADC, 0xA0),
                         (CR_ADC, 0x20), (AICR_DAC, 0xC0), (CR_DAC, 0xA0), (CR_DAC, 0x20)):
            self.c.w(off, val)
        m.clock_step(60 * MS)
        self.assertEqual(self.c.r(SR1), 0x80)
        self.assertEqual(self.c.r(MR), 0x00)


class CodecPowerTest(DM32TestCase):
    def start(self, *a, **kw):
        m = super().start(*a, **kw)
        self.c = Codec(m)
        return m

    def test_wake_sets_pon_ack_after_tsbyu(self):
        """SB = 0 and SB_SLEEP = 0 (DS 6.4.2.3) is working mode: pon_ack [7] sets after Tsbyu,
        50 ms, not at once."""
        m = self.start()
        self.c.w(CR_VIC, 0x0C)
        self.assertEqual(self.c.r(SR1), 0)
        m.clock_step(40 * MS)
        self.assertEqual(self.c.r(SR1), 0)
        m.clock_step(20 * MS)
        self.assertEqual(self.c.r(SR1), 0x80)
        self.c.w(CR_VIC, 0x0F)
        self.assertEqual(self.c.r(SR1), 0)

    def test_negative_sleep_is_not_working(self):
        """Negative: sleep mode (SB = 0, SB_SLEEP = 1, DAC off) never sets pon_ack."""
        m = self.start()
        self.c.w(CR_VIC, 0x0E)
        m.clock_step(200 * MS)
        self.assertEqual(self.c.r(SR1), 0)

    def test_playback_and_standby_lo(self):
        """Sleep to playback (DS 6.4.2.4): pon_ack and LINEOUT_CTRL standby_lo [31] change after
        Tstbon, 12 ms. standby_lo reads 1 at reset (4.6.5.5) and again once the DAC is off."""
        m = self.start()
        self.assertEqual(m.readl(LINEOUT_CTRL), 0x80000000)
        self.c.playback()
        self.assertEqual(m.readl(LINEOUT_CTRL) >> 31, 1)
        m.clock_step(13 * MS)
        self.assertEqual(m.readl(LINEOUT_CTRL) >> 31, 0)
        self.assertEqual(self.c.r(SR1), 0x80)
        self.c.w(CR_DAC, 0x30)
        self.assertEqual(m.readl(LINEOUT_CTRL) >> 31, 1)

    def test_lineout_enables_kept(self):
        """LINEOUT_CTRL [1:0] are read-write; [31] is read-only."""
        m = self.start()
        m.writel(LINEOUT_CTRL, 0x00000003)
        self.assertEqual(m.readl(LINEOUT_CTRL), 0x80000003)
        m.writel(LINEOUT_CTRL, 0)
        self.assertEqual(m.readl(LINEOUT_CTRL), 0x80000000)


class CodecMuteTest(DM32TestCase):
    def start(self, *a, **kw):
        m = super().start(*a, **kw)
        self.c = Codec(m)
        return m

    def test_dac_soft_mute_ramps(self):
        """MR dac_mute [1:0]: a DAC powered with soft mute on reads 11; clearing dac_soft_mute
        ramps through 10 for 24 ms to 00 and sets IFR dac_mute_event [0]; setting it ramps
        through 01 to 11 (DS 6.4.4, 6.5.2.3)."""
        m = self.start()
        self.c.w(CR_VIC, 0x0E)
        self.c.w(CR_DAC, 0xA0)                    # sb_dac 0, mute on
        self.assertEqual(self.c.r(MR), 0x03)
        self.c.w(CR_DAC, 0x20)
        self.assertEqual(self.c.r(MR), 0x02)
        m.clock_step(20 * MS)
        self.assertEqual(self.c.r(MR), 0x02)
        self.assertEqual(self.c.r(IFR), 0)
        m.clock_step(5 * MS)
        self.assertEqual(self.c.r(MR), 0x00)
        self.assertEqual(self.c.r(IFR), 0x01)
        self.c.w(CR_DAC, 0xA0)
        self.assertEqual(self.c.r(MR), 0x01)
        m.clock_step(25 * MS)
        self.assertEqual(self.c.r(MR), 0x03)

    def test_adc_soft_mute_needs_awake_codec(self):
        """MR adc_mute [4:3] ramps only with the ADC powered: SB = 0, SB_SLEEP = 0, sb_adc = 0.
        Its completion sets IFR adc_mute_event [2]."""
        m = self.start()
        self.c.w(CR_VIC, 0x0E)                    # still asleep
        self.c.w(CR_ADC, 0x20)
        m.clock_step(30 * MS)
        self.assertEqual(self.c.r(MR), 0)
        self.assertEqual(self.c.r(IFR), 0)
        self.c.w(CR_ADC, 0xA0)
        self.c.w(CR_VIC, 0x0C)
        self.assertEqual(self.c.r(MR), 0x18)
        self.c.w(CR_ADC, 0x20)
        self.assertEqual(self.c.r(MR), 0x10)
        m.clock_step(25 * MS)
        self.assertEqual(self.c.r(MR), 0)
        self.assertEqual(self.c.r(IFR), 0x04)

    def test_negative_powered_down_reads_zero(self):
        """Negative: with the DAC powered down MR reads 00 whatever the mute request."""
        m = self.start()
        self.c.w(CR_DAC, 0x80)                    # mute on, sb_dac 0, but SB = 1
        m.clock_step(30 * MS)
        self.assertEqual(self.c.r(MR), 0)

    def test_ifr_write_one_clears(self):
        """IFR flags clear when written with 1, and a 0 leaves them (assumption)."""
        m = self.start()
        self.c.w(CR_VIC, 0x0E)
        self.c.w(CR_DAC, 0xA0)
        self.c.w(CR_DAC, 0x20)
        m.clock_step(25 * MS)
        self.c.w(IFR, 0x00)
        self.assertEqual(self.c.r(IFR), 0x01)
        self.c.w(IFR, 0x01)
        self.assertEqual(self.c.r(IFR), 0x00)


class CodecInterruptTest(DM32TestCase):
    def start(self, *a, **kw):
        m = super().start(*a, **kw)
        self.c = Codec(m)
        m.irq_intercept_in(m.pic_path())
        return m

    def mute_event(self, m):
        self.c.w(CR_VIC, 0x0E)
        self.c.w(CR_DAC, 0xA0)
        self.c.w(CR_DAC, 0x20)
        m.clock_step(25 * MS)

    def test_unmasked_event_raises_source_37(self):
        """With IMR dac_mute_mask [0] clear, the mute event drives PIC source 37 high (ICR
        int_form 00) and SR1 irq_ack [6]; clearing the flag drops both."""
        m = self.start()
        self.c.w(IMR, 0xFE)
        self.mute_event(m)
        self.assertTrue(m.irq_level.get(SRC))
        self.assertEqual(self.c.r(SR1) & 0x40, 0x40)
        self.c.w(IFR, 0x01)
        self.assertFalse(m.irq_level.get(SRC))
        self.assertEqual(self.c.r(SR1) & 0x40, 0)

    def test_negative_masked_event(self):
        """Negative: masked (IMR reset 0xff), the flag sets but the line stays low."""
        m = self.start()
        self.mute_event(m)
        self.assertEqual(self.c.r(IFR), 0x01)
        self.assertFalse(m.irq_level.get(SRC, False))

    def test_int_form_low(self):
        """ICR int_form 01: the line is low while an event is pending, high otherwise."""
        m = self.start()
        self.c.w(ICR, 0x40)
        self.assertTrue(m.irq_level.get(SRC))
        self.c.w(IMR, 0xFE)
        self.mute_event(m)
        self.assertFalse(m.irq_level.get(SRC))

    def test_int_form_pulse(self):
        """ICR int_form 11: idle high, a low pulse of 8 MC_CLK (about 0.8 us) per new event."""
        m = self.start()
        self.c.w(ICR, 0xC0)
        self.c.w(IMR, 0xFE)
        self.c.w(CR_VIC, 0x0E)
        self.c.w(CR_DAC, 0xA0)
        self.c.w(CR_DAC, 0x20)
        m.clock_step(23 * MS)
        self.assertTrue(m.irq_level.get(SRC))
        log = len(m.irq_log)
        m.clock_step(2 * MS)
        self.assertIn((False, SRC), m.irq_log[log:])
        self.assertTrue(m.irq_level.get(SRC))

    def test_timer(self):
        """CR_TIMER: writing count_l starts (3 + count) slow clocks of 512 MCLK at 24 MHz; the
        end sets IFR2 TIMER_END [4], unmasked by IMR2 [4]. count 10: 277 us."""
        m = self.start()
        self.c.w(IMR2, 0xEF)
        self.c.w(CR_TIMER_H, 0)
        self.c.w(CR_TIMER_L, 10)
        m.clock_step(270 * US)
        self.assertEqual(self.c.r(IFR2), 0)
        m.clock_step(10 * US)
        self.assertEqual(self.c.r(IFR2), 0x10)
        self.assertTrue(m.irq_level.get(SRC))
        self.c.w(IFR2, 0x10)
        self.assertFalse(m.irq_level.get(SRC))

    def test_negative_timer_clock_off(self):
        """Negative: with CR_CK shutdown_clock [4] set the timer does not run."""
        m = self.start()
        self.c.w(CR_CK, 0x50)
        self.c.w(CR_TIMER_L, 10)
        m.clock_step(1 * MS)
        self.assertEqual(self.c.r(IFR2), 0)


class CodecIndirectTest(DM32TestCase):
    def start(self, *a, **kw):
        m = super().start(*a, **kw)
        self.c = Codec(m)
        return m

    def read_ind(self, cr, dr, i):
        self.c.w(cr, i)
        return self.c.r(dr)

    def test_mixer_write_then_read(self):
        """DR_MIX then CR_MIX with mix_load [6] = 1 stores MIX_n; with mix_load = 0, DR_MIX reads
        the indexed register (DS 6.5.2.10-11)."""
        self.start()
        self.c.w(DR_MIX, 0xA1)
        self.c.w(CR_MIX, 0x40 | 1)
        self.assertEqual(self.read_ind(CR_MIX, DR_MIX, 1), 0xA1)
        self.assertEqual(self.read_ind(CR_MIX, DR_MIX, 0), 0)

    def test_adc_agc_resets_and_ds_example(self):
        """ADC_AGC_0-4 reset to 0x34, 0x07, 0x44, 0x1f, 0x00 (DS 6.5.2.37). The DS 6.4.5
        example writes 0x18 to DR_ADC_AGC, latches it with CR_ADC_AGC = 0x40, then enables AGC
        with 0x80, which does not write."""
        self.start()
        self.assertEqual([self.read_ind(CR_ADC_AGC, DR_ADC_AGC, i) for i in range(5)],
                         [0x34, 0x07, 0x44, 0x1F, 0x00])
        self.c.w(DR_ADC_AGC, 0x18)
        self.c.w(CR_ADC_AGC, 0x40)
        self.c.w(CR_ADC_AGC, 0x80)
        self.assertEqual(self.c.r(CR_ADC_AGC), 0x80)
        self.assertEqual(self.c.r(DR_ADC_AGC), 0x18)
        self.assertEqual(self.read_ind(CR_ADC_AGC, DR_ADC_AGC, 1), 0x07)
        self.assertEqual(self.read_ind(CR_ADC_AGC, DR_ADC_AGC, 0), 0x18)

    def test_negative_index_out_of_range(self):
        """Negative: DAC_AGC has 4 registers; index 4 stores nothing and reads 0."""
        self.start()
        self.c.w(DR_DAC_AGC, 0x55)
        self.c.w(CR_DAC_AGC, 0x40 | 4)
        self.assertEqual(self.read_ind(CR_DAC_AGC, DR_DAC_AGC, 4), 0)
        self.assertEqual(self.read_ind(CR_DAC_AGC, DR_DAC_AGC, 3), 0)


class CodecAccessPathTest(DM32TestCase):
    def test_i2c_mux_cuts_window(self):
        """CODEC_I2C_MUX codec_si2c_reg [7] = 1 selects I2C (4.6.5.6): the window then reads 0
        and drops writes (assumption); back on MC the registers are intact."""
        m = self.start()
        c = Codec(m)
        c.w(0x13, 0x45)
        m.writel(CODEC_I2C_MUX, 0x80)
        self.assertEqual(c.r(0x13), 0)
        c.w(0x13, 0x11)
        m.writel(CODEC_I2C_MUX, 0x00)
        self.assertEqual(c.r(0x13), 0x45)

    def test_soft_reset_bit4(self):
        """SYS_SOFT_RSTN bit 4 resets the codec (4.1.4.1); bit 3 (audio interface) does not."""
        m = self.start()
        c = Codec(m)
        c.w(0x13, 0x45)
        m.writel(SOFT_RSTN, 0x1FF & ~(1 << 3))
        self.assertEqual(c.r(0x13), 0x45)
        m.writel(SOFT_RSTN, 0x1FF & ~(1 << 4))
        self.assertEqual(c.r(0x13), 0x40)


if __name__ == "__main__":
    unittest.main()
