# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# Copyright (c) 2026 Alex Kaplan
"""I2C controllers (hw/misc/dm32_i2c.c: I2C0 at 0x14060000, I2C1 at 0x14070000, I2C2 at
0x14080000; HR_C7000 v2.7 section 5.1, p.218-243) and the real-time clock slave at 7-bit 0x70
on I2C2 (hw/misc/dm32_i2c_rtc.c; section 4.9). docs/devices.md, "I2C and RTC"."""

import unittest

from dm32qtest import (
    DM32TestCase, ABRT_SRC, CCR, CCR_ENABLE, CCR_INTR_EN, CCR_LOAD, CLR_INTR, CLR_RX_UNDER,
    CLR_TX_ABRT, CLR_TX_OVER, CON, DATA_CMD, ENABLE, EN_STATUS, FS_HCNT, FS_LCNT, I2C0, I2C1, I2C2,
    LOAD_S, READ, RTC, RXFLR, START, STATUS, STOCK_CON, TAR, TXFLR, VAL_S, begin, i2c_read,
    i2c_write, rtc_read, rtc_write, setup
)

INTR_STAT, INTR_MASK, RAW_INTR, RX_TL, TX_TL = 0x2C, 0x30, 0x34, 0x38, 0x3C
CLR_ACTIVITY, CLR_STOP_DET, CLR_START_DET = 0x5C, 0x60, 0x64
IO_DIPLEX0 = 0x11000034
I2C0_PADS, I2C1_PADS = 0x060, 0x180   # IO_DIPLEX0 i2c0 bits 6:5 (PTA5/6), i2c1 bits 8:7 (PTA7/8)

RX_UNDER, RX_OVER, RX_FULL, TX_OVER, TX_EMPTY = 1 << 0, 1 << 1, 1 << 2, 1 << 3, 1 << 4
TX_ABRT, ACTIVITY, STOP_DET, START_DET = 1 << 6, 1 << 8, 1 << 9, 1 << 10
ABRT_7B_ADDR_NOACK, ABRT_10ADDR1_NOACK, ABRT_MASTER_DIS = 1 << 0, 1 << 1, 1 << 11
ST_ACTIVITY, ST_TFNF, ST_TFE, ST_RFNE, ST_RFF, ST_MST = 1, 2, 4, 8, 16, 32

US = 1000

MATCH_S = 0x05
MATCH_VEN, LOAD_VSTAT, RSTAT = 0x0A, 0x12, 0x13
NUMER_L, SOFT_RST_N = 0x14, 0x17
RTC_PIC = 19                     # Table 30
SECOND = 1_000_000_000


class I2cRegisterTest(DM32TestCase):
    def test_reset_values(self):
        """Reset values from DS section 5.1.6 (and the SVD): IC_CON 0x7d, IC_TAR 0x55, SCL counts
        0x5a/0xa0, IC_INTR_MASK 0x8ff, thresholds 6, IC_STATUS 0x6; everything else 0."""
        m = self.start()
        for reg, value in ((CON, 0x7D), (TAR, 0x55), (FS_HCNT, 0x5A), (FS_LCNT, 0xA0),
                           (INTR_MASK, 0x8FF), (RX_TL, 6), (TX_TL, 6), (STATUS, 0x6),
                           (RAW_INTR, 0), (INTR_STAT, 0), (ENABLE, 0), (EN_STATUS, 0),
                           (TXFLR, 0), (RXFLR, 0), (ABRT_SRC, 0), (START, 0)):
            self.assertEqual(m.readl(I2C2 + reg), value, hex(reg))

    def test_field_widths(self):
        """Registers keep only their fields: IC_CON [6:0], IC_TAR [11:0], IC_INTR_MASK [11:0],
        IC_ENABLE and IC_START [0]. Thresholds range 0-7 (p.233), so 0xff reads back 7."""
        m = self.start()
        for reg, kept in ((CON, 0x7F), (TAR, 0xFFF), (INTR_MASK, 0xFFF), (RX_TL, 7),
                          (TX_TL, 7), (START, 1)):
            m.writel(I2C2 + reg, 0xFFFFFFFF)
            self.assertEqual(m.readl(I2C2 + reg), kept, hex(reg))

    def test_negative_locked_while_enabled(self):
        """Negative: IC_CON, IC_TAR and the SCL counts are refused while IC_ENABLE is 1
        (assumption in dm32_i2c.c, from the DS initialisation order)."""
        m = self.start()
        m.writel(I2C2 + ENABLE, 1)
        for reg, reset in ((CON, 0x7D), (TAR, 0x55), (FS_HCNT, 0x5A), (FS_LCNT, 0xA0)):
            m.writel(I2C2 + reg, 0x11)
            self.assertEqual(m.readl(I2C2 + reg), reset, hex(reg))
        m.writel(I2C2 + ENABLE, 0)
        m.writel(I2C2 + TAR, 0x11)
        self.assertEqual(m.readl(I2C2 + TAR), 0x11)

    def test_negative_undefined_offset(self):
        """Negative: an offset with no register (0x08) reads 0 and ignores writes."""
        m = self.start()
        m.writel(I2C2 + 0x08, 0xFFFF)
        self.assertEqual(m.readl(I2C2 + 0x08), 0)

    def test_system_reset(self):
        """A machine reset restores the reset values."""
        m = self.start()
        setup(m)
        m.writel(I2C2 + ENABLE, 1)
        m.writel(I2C2 + DATA_CMD, 1)
        m.qmp("system_reset")
        m.clock_step(1_000_000)
        self.assertEqual(m.readl(I2C2 + CON), 0x7D)
        self.assertEqual(m.readl(I2C2 + TXFLR), 0)
        self.assertEqual(m.readl(I2C2 + EN_STATUS), 0)


class I2cFifoTest(DM32TestCase):
    def test_negative_data_dropped_while_disabled(self):
        """Negative: IC_DATA_CMD writes are dropped while the controller is disabled."""
        m = self.start()
        m.writel(I2C2 + DATA_CMD, 0x12)
        self.assertEqual(m.readl(I2C2 + TXFLR), 0)

    def test_data_waits_for_start(self):
        """Queued commands wait in the TX FIFO until IC_START is set (p.242)."""
        m = self.start()
        setup(m)
        begin(m, RTC)
        for b in (1, 2, 3):
            m.writel(I2C2 + DATA_CMD, b)
        m.clock_step(1_000_000)
        self.assertEqual(m.readl(I2C2 + TXFLR), 3)
        self.assertEqual(m.readl(I2C2 + STATUS), ST_TFNF)

    def test_tx_overflow(self):
        """The TX FIFO holds 8: a ninth write is dropped and sets TX_OVER; IC_STATUS.TFNF
        clears when full. IC_CLR_TX_OVER clears TX_OVER."""
        m = self.start()
        setup(m)
        begin(m, RTC)
        for b in range(9):
            m.writel(I2C2 + DATA_CMD, b)
        self.assertEqual(m.readl(I2C2 + TXFLR), 8)
        self.assertFalse(m.readl(I2C2 + STATUS) & ST_TFNF)
        self.assertTrue(m.readl(I2C2 + RAW_INTR) & TX_OVER)
        m.readl(I2C2 + CLR_TX_OVER)
        self.assertFalse(m.readl(I2C2 + RAW_INTR) & TX_OVER)

    def test_tx_empty_follows_threshold(self):
        """TX_EMPTY is set while the TX FIFO holds no more than IC_TX_TL entries (p.232)."""
        m = self.start()
        setup(m)
        begin(m, RTC)
        self.assertTrue(m.readl(I2C2 + RAW_INTR) & TX_EMPTY)
        for b in range(7):
            m.writel(I2C2 + DATA_CMD, b)
        self.assertFalse(m.readl(I2C2 + RAW_INTR) & TX_EMPTY)

    def test_negative_rx_underflow(self):
        """Negative: reading an empty RX FIFO returns 0 and sets RX_UNDER; IC_CLR_RX_UNDER
        clears it."""
        m = self.start()
        self.assertEqual(m.readl(I2C2 + DATA_CMD), 0)
        self.assertTrue(m.readl(I2C2 + RAW_INTR) & RX_UNDER)
        m.readl(I2C2 + CLR_RX_UNDER)
        self.assertFalse(m.readl(I2C2 + RAW_INTR) & RX_UNDER)

    def test_rx_full_threshold(self):
        """RX_FULL is set while the RX FIFO holds more than IC_RX_TL entries (p.233)."""
        m = self.start()
        setup(m)
        m.writel(I2C2 + RX_TL, 1)
        begin(m, RTC)
        m.writel(I2C2 + DATA_CMD, VAL_S)
        m.writel(I2C2 + DATA_CMD, READ)
        m.writel(I2C2 + DATA_CMD, READ)
        m.writel(I2C2 + START, 1)
        m.clock_step(100 * US)                  # first byte in at 95 us
        self.assertEqual(m.readl(I2C2 + RXFLR), 1)
        self.assertFalse(m.readl(I2C2 + RAW_INTR) & RX_FULL)
        m.clock_step(30 * US)                   # second at 117.5 us
        self.assertEqual(m.readl(I2C2 + RXFLR), 2)
        self.assertTrue(m.readl(I2C2 + RAW_INTR) & RX_FULL)

    def test_rx_overflow(self):
        """A byte received with the RX FIFO full (8) is dropped and sets RX_OVER."""
        m = self.start()
        setup(m)
        begin(m, RTC)
        m.writel(I2C2 + DATA_CMD, VAL_S)
        for _ in range(7):
            m.writel(I2C2 + DATA_CMD, READ)
        m.writel(I2C2 + START, 1)
        m.clock_step(80 * US)                   # the index and first read are on the bus
        m.writel(I2C2 + DATA_CMD, READ)
        m.writel(I2C2 + DATA_CMD, READ)
        m.clock_step(1_000_000)
        self.assertEqual(m.readl(I2C2 + RXFLR), 8)
        self.assertTrue(m.readl(I2C2 + STATUS) & ST_RFF)
        self.assertTrue(m.readl(I2C2 + RAW_INTR) & RX_OVER)


class I2cTimingTest(DM32TestCase):
    def _start_read(self, m):
        begin(m, RTC)
        m.writel(I2C2 + DATA_CMD, SOFT_RST_N)
        m.writel(I2C2 + DATA_CMD, READ)
        m.writel(I2C2 + START, 1)

    def test_one_byte_read_timeline(self):
        """A one-byte register read at stock's 2.5 us period: START, address and index (19
        periods), RESTART, address and data (19), STOP (1). The byte lands at 95 us and the
        controller is idle at 97.5 us, with START_DET, STOP_DET and ACTIVITY raised."""
        m = self.start()
        setup(m)
        self._start_read(m)
        self.assertTrue(m.readl(I2C2 + STATUS) & (ST_ACTIVITY | ST_MST))
        m.clock_step(94 * US)
        self.assertEqual(m.readl(I2C2 + RXFLR), 0)
        m.clock_step(2 * US)
        self.assertEqual(m.readl(I2C2 + RXFLR), 1)
        self.assertTrue(m.readl(I2C2 + STATUS) & ST_ACTIVITY)
        m.clock_step(2 * US)
        self.assertFalse(m.readl(I2C2 + STATUS) & ST_ACTIVITY)
        raw = m.readl(I2C2 + RAW_INTR)
        self.assertEqual(raw & (START_DET | STOP_DET | ACTIVITY), START_DET | STOP_DET | ACTIVITY)
        self.assertEqual(m.readl(I2C2 + DATA_CMD), 1)      # SOFT_RST_N resets to 1

    def test_period_follows_scl_counts(self):
        """The SCL period is (HCNT + 8) + (LCNT + 1) APB cycles (p.220): counts 202/209 give 420
        cycles, 5 us, so the byte that lands at 38 periods arrives at 190 us, not 95 us."""
        m = self.start()
        setup(m, hcnt=202, lcnt=209)
        self._start_read(m)
        m.clock_step(188 * US)
        self.assertEqual(m.readl(I2C2 + RXFLR), 0)
        m.clock_step(4 * US)
        self.assertEqual(m.readl(I2C2 + RXFLR), 1)

    def test_counts_held_at_minimum(self):
        """Counts below the minimums (HCNT 6, LCNT 8; p.220) time as the minimums: with both 0
        the period is 23 cycles, so the 39-period read takes 10.7 us, not 4.2 us."""
        m = self.start()
        setup(m, hcnt=0, lcnt=0)
        self._start_read(m)
        m.clock_step(10 * US)
        self.assertEqual(m.readl(I2C2 + RXFLR), 0)
        m.clock_step(1 * US)
        self.assertEqual(m.readl(I2C2 + RXFLR), 1)

    def test_disable_finishes_byte_then_stops(self):
        """Clearing IC_ENABLE mid-transfer lets the byte in flight finish and sends STOP;
        IC_ENABLE_STATUS stays 1 until then, and the FIFOs are flushed when it drops."""
        m = self.start()
        setup(m)
        begin(m, RTC)
        m.writel(I2C2 + DATA_CMD, VAL_S)
        for _ in range(5):
            m.writel(I2C2 + DATA_CMD, READ)
        m.writel(I2C2 + START, 1)
        m.clock_step(30 * US)                   # inside the first byte
        m.writel(I2C2 + ENABLE, 0)
        self.assertEqual(m.readl(I2C2 + EN_STATUS), 1)
        m.clock_step(20 * US)                   # byte ends at 47.5 us, STOP at 50 us
        self.assertEqual(m.readl(I2C2 + EN_STATUS), 0)
        self.assertEqual(m.readl(I2C2 + TXFLR), 0)
        self.assertEqual(m.readl(I2C2 + RXFLR), 0)
        self.assertTrue(m.readl(I2C2 + RAW_INTR) & STOP_DET)

    def test_read_without_restart(self):
        """With IC_CON.RESTART_EN clear the direction change uses STOP and START; the read
        still returns the register."""
        m = self.start()
        setup(m, con=STOCK_CON & ~0x20)
        self.assertEqual(rtc_read(m, SOFT_RST_N, 1), [1])


class I2cAbortTest(DM32TestCase):
    def test_address_nak_on_read(self):
        """No slave at 0x71: ABRT_7B_ADDR_NOACK and TX_ABRT, the TX FIFO is flushed, nothing is
        received, and STOP ends the transfer."""
        m = self.start()
        setup(m)
        i2c_read(m, 0x71, VAL_S, 1)
        self.assertTrue(m.readl(I2C2 + RAW_INTR) & TX_ABRT)
        self.assertEqual(m.readl(I2C2 + ABRT_SRC), ABRT_7B_ADDR_NOACK)
        self.assertTrue(m.readl(I2C2 + RAW_INTR) & STOP_DET)
        self.assertEqual(m.readl(I2C2 + TXFLR), 0)
        self.assertEqual(m.readl(I2C2 + RXFLR), 0)

    def test_address_nak_on_write(self):
        """A write to an address with no slave aborts the same way and stores nothing."""
        m = self.start()
        setup(m)
        i2c_write(m, 0x71, [LOAD_S, 0x2A])
        self.assertEqual(m.readl(I2C2 + ABRT_SRC), ABRT_7B_ADDR_NOACK)
        self.assertEqual(rtc_read(m, LOAD_S, 1), [0x00])

    def test_negative_writes_dropped_until_abort_cleared(self):
        """Negative: while TX_ABRT is set, IC_DATA_CMD writes are dropped; IC_CLR_TX_ABRT clears
        TX_ABRT and IC_TX_ABRT_SOURCE (p.236)."""
        m = self.start()
        setup(m)
        begin(m, 0x71)
        m.writel(I2C2 + DATA_CMD, 0)
        m.writel(I2C2 + START, 1)
        m.clock_step(1_000_000)
        m.writel(I2C2 + DATA_CMD, 0)
        self.assertEqual(m.readl(I2C2 + TXFLR), 0)
        m.readl(I2C2 + CLR_TX_ABRT)
        self.assertEqual(m.readl(I2C2 + ABRT_SRC), 0)
        self.assertFalse(m.readl(I2C2 + RAW_INTR) & TX_ABRT)

    def test_ten_bit_addressing_naks(self):
        """With the reset IC_CON (10-bit addressing) no slave acknowledges:
        ABRT_10ADDR1_NOACK (assumption: QEMU buses carry 7-bit addresses)."""
        m = self.start()
        setup(m, con=0x7D)
        i2c_read(m, RTC, CCR, 1)
        self.assertEqual(m.readl(I2C2 + ABRT_SRC), ABRT_10ADDR1_NOACK)

    def test_negative_master_disabled(self):
        """Negative: with IC_CON.MASTER_MODE 0 a transfer aborts with ABRT_MASTER_DIS."""
        m = self.start()
        setup(m, con=STOCK_CON & ~1)
        i2c_read(m, RTC, CCR, 1)
        self.assertEqual(m.readl(I2C2 + ABRT_SRC), ABRT_MASTER_DIS)

    def test_clr_intr_clears_latched_bits(self):
        """IC_CLR_INTR clears every latched interrupt and IC_TX_ABRT_SOURCE (p.234), but not
        the level-driven TX_EMPTY."""
        m = self.start()
        setup(m)
        i2c_read(m, 0x71, VAL_S, 1)
        m.writel(I2C2 + ENABLE, 1)
        m.readl(I2C2 + CLR_INTR)
        self.assertEqual(m.readl(I2C2 + RAW_INTR), TX_EMPTY)
        self.assertEqual(m.readl(I2C2 + ABRT_SRC), 0)


class I2cInterruptTest(DM32TestCase):
    def test_stock_mask_raises_pic9_on_abort(self):
        """With stock's IC_INTR_MASK (0x40, TX_ABRT only), an abort raises PIC source 9
        (Table 30) and IC_INTR_STAT shows it; IC_CLR_TX_ABRT drops the line."""
        m = self.start()
        m.irq_intercept_in(m.pic_path())
        setup(m)
        m.writel(I2C2 + INTR_MASK, TX_ABRT)
        i2c_read(m, 0x71, VAL_S, 1)
        self.assertEqual(m.readl(I2C2 + INTR_STAT), TX_ABRT)
        self.assertTrue(m.irq_level.get(9))
        m.readl(I2C2 + CLR_TX_ABRT)
        self.assertFalse(m.irq_level.get(9))

    def test_reset_mask_tx_empty_raises_line(self):
        """With the reset mask (0x8ff), enabling the controller raises its line: the empty TX
        FIFO is below the threshold. I2C0 is PIC 8 and I2C1 PIC 7."""
        m = self.start()
        m.irq_intercept_in(m.pic_path())
        for base, src in ((I2C0, 8), (I2C1, 7), (I2C2, 9)):
            m.writel(base + ENABLE, 1)
            self.assertTrue(m.irq_level.get(src), src)
            m.writel(base + ENABLE, 0)
            self.assertFalse(m.irq_level.get(src), src)

    def test_negative_masked_interrupt_silent(self):
        """Negative: a masked interrupt is in IC_RAW_INTR_STAT but not IC_INTR_STAT, and the
        line stays low."""
        m = self.start()
        m.irq_intercept_in(m.pic_path())
        m.writel(I2C2 + INTR_MASK, 0)
        m.readl(I2C2 + DATA_CMD)
        self.assertTrue(m.readl(I2C2 + RAW_INTR) & RX_UNDER)
        self.assertEqual(m.readl(I2C2 + INTR_STAT), 0)
        self.assertFalse(m.irq_level.get(9, False))


class RtcTest(DM32TestCase):
    """The RTC (hw/misc/dm32_i2c_rtc.c; HR_C7000 v2.7 section 4.9, Register Table 7). Every test
    runs with -rtc clock=vm, so the count follows virtual time exactly."""

    def start(self, *a, **kw):
        """Every RTC test starts from stock's I2C2 set-up, on the virtual RTC clock."""
        kw["args"] = list(kw.get("args", [])) + ["-rtc", "clock=vm"]
        m = super().start(*a, **kw)
        m.irq_intercept_in(m.pic_path())
        setup(m)
        return m

    def load(self, m, s, mi, h, day, ccr=CCR_ENABLE):
        """Stock's set-time sequence (rtc_date_validate_and_write @0x0301c498): LOAD_VSTAT 0, the
        LOAD registers, LOAD_VSTAT 1, then CCR with the load bit."""
        rtc_write(m, LOAD_VSTAT, [0])
        rtc_write(m, LOAD_S, [s, mi, h, day & 0xFF, day >> 8])
        rtc_write(m, LOAD_VSTAT, [1])
        rtc_write(m, CCR, [ccr | CCR_LOAD])

    def test_reset_values(self):
        """Register Table 7: every register resets to 0 except SOFT_RST_N (1), so the count is
        0 and stopped; LOAD_VSTAT is clear, which sends stock down its set-default path."""
        m = self.start()
        regs = []
        for first in range(0, 0x18, 6):                  # the TX FIFO holds 8 commands
            regs += rtc_read(m, first, 6)
        self.assertEqual(regs[:0x17], [0] * 0x17)
        self.assertEqual(regs[0x17], 1)
        m.clock_step(3 * SECOND)
        self.assertEqual(rtc_read(m, VAL_S, 5), [0, 0, 0, 0, 0])

    def test_counts_binary_with_rollover(self):
        """Binary count (section 4.9.2): 23:59:58 on day 5 becomes 00:00:01 on day 6 three seconds
        later; seconds, minutes and hours roll over at 60, 60 and 24."""
        m = self.start()
        self.load(m, 58, 59, 23, 5)
        m.clock_step(3 * SECOND)
        self.assertEqual(rtc_read(m, VAL_S, 5), [1, 0, 0, 6, 0])

    def test_day_wraps_at_16_bits(self):
        """The day register is 16 bits: day 65535 at 23:59:59 wraps to day 0."""
        m = self.start()
        self.load(m, 59, 59, 23, 0xFFFF)
        m.clock_step(int(1.5 * SECOND))
        self.assertEqual(rtc_read(m, VAL_S, 5), [0, 0, 0, 0, 0])

    def test_load_self_clears_and_stop_freezes(self):
        """CCR[2] clears itself (4.9.5.18); CCR[1] = 0 stops the count."""
        m = self.start()
        self.load(m, 10, 0, 0, 0, ccr=0)
        self.assertEqual(rtc_read(m, CCR, 1), [0])
        m.clock_step(3 * SECOND)
        self.assertEqual(rtc_read(m, VAL_S, 1), [10])

    def test_val_registers_read_only(self):
        """Negative: VAL_* are read-only."""
        m = self.start()
        self.load(m, 45, 27, 14, 0x134, ccr=0)
        rtc_write(m, VAL_S, [0x2F])
        self.assertEqual(rtc_read(m, VAL_S, 5), [45, 27, 14, 0x34, 0x01])

    def test_field_widths(self):
        """Writes keep the documented widths: LOAD_S [5:0], LOAD_H [4:0], LOAD_VSTAT [0]."""
        m = self.start()
        rtc_write(m, LOAD_S, [0xFF, 0xFF, 0xFF])
        rtc_write(m, LOAD_VSTAT, [0xFF])
        self.assertEqual(rtc_read(m, LOAD_S, 3), [0x3F, 0x3F, 0x1F])
        self.assertEqual(rtc_read(m, LOAD_VSTAT, 1), [1])

    def test_divider(self):
        """CLKDIV_NUMER with CLKDIV_NUMER_VLD set divides the 32.768 kHz crystal by t
        (4.9.5.21-23): t = 16384 counts at 2 Hz."""
        m = self.start()
        rtc_write(m, NUMER_L, [0x00, 0x40, 1])
        self.load(m, 0, 0, 0, 0)
        m.clock_step(int(2.1 * SECOND))
        self.assertEqual(rtc_read(m, VAL_S, 1), [4])

    def _alarm(self, m, intr=True, ven=True):
        self.load(m, 0, 0, 0, 0, ccr=CCR_ENABLE | (CCR_INTR_EN if intr else 0))
        rtc_write(m, MATCH_S, [3, 0, 0, 0, 0])
        if ven:
            rtc_write(m, MATCH_VEN, [1])

    def test_match_interrupt(self):
        """MATCH_VEN copies MATCH_* in and clears itself; when the count reaches it with CCR[0]
        set, RSTAT is 1 and PIC source 19 goes high (Table 30, level) until RSTAT is read."""
        m = self.start()
        self._alarm(m)
        self.assertEqual(rtc_read(m, MATCH_VEN, 1), [0])
        m.clock_step(int(2.5 * SECOND))
        self.assertFalse(m.irq_level.get(RTC_PIC, False))
        m.clock_step(1 * SECOND)
        self.assertTrue(m.irq_level.get(RTC_PIC))
        self.assertEqual(rtc_read(m, RSTAT, 1), [1])
        self.assertFalse(m.irq_level.get(RTC_PIC))
        self.assertEqual(rtc_read(m, RSTAT, 1), [0])

    def test_negative_match_without_intr_en(self):
        """Negative: with CCR[0] clear the match raises nothing."""
        m = self.start()
        self._alarm(m, intr=False)
        m.clock_step(4 * SECOND)
        self.assertEqual(rtc_read(m, RSTAT, 1), [0])
        self.assertFalse(m.irq_level.get(RTC_PIC, False))

    def test_negative_match_needs_ven(self):
        """Negative: MATCH_* written without MATCH_VEN do not reach the internal match register."""
        m = self.start()
        self._alarm(m, ven=False)
        m.clock_step(4 * SECOND)
        self.assertEqual(rtc_read(m, RSTAT, 1), [0])

    def test_negative_load_onto_match_does_not_fire(self):
        """Negative: loading the count onto the match value is not reaching it by counting."""
        m = self.start()
        rtc_write(m, MATCH_S, [5, 0, 0, 0, 0])
        rtc_write(m, MATCH_VEN, [1])
        self.load(m, 5, 0, 0, 0, ccr=CCR_INTR_EN)
        self.assertEqual(rtc_read(m, RSTAT, 1), [0])

    def test_soft_reset(self):
        """SOFT_RST_N = 0 returns the registers and count to reset values and ignores writes until
        it is 1 again (stock pulses it in rtc_validate_and_set_default_datetime)."""
        m = self.start()
        self.load(m, 9, 9, 9, 9)
        rtc_write(m, SOFT_RST_N, [0])
        rtc_write(m, LOAD_S, [7])
        self.assertEqual(rtc_read(m, VAL_S, 5), [0, 0, 0, 0, 0])
        self.assertEqual(rtc_read(m, LOAD_S, 1), [0])
        rtc_write(m, SOFT_RST_N, [1])
        rtc_write(m, LOAD_S, [7])
        self.assertEqual(rtc_read(m, LOAD_S, 1), [7])

    def test_match_registers_stored(self):
        """MATCH_* store their fields and read back."""
        m = self.start()
        rtc_write(m, MATCH_S, [0x12])
        self.assertEqual(rtc_read(m, MATCH_S, 1), [0x12])

    def test_read_past_last_register(self):
        """Negative: reading past the last register (0x17) returns 0xff."""
        m = self.start()
        self.assertEqual(rtc_read(m, 0x17, 2)[1], 0xFF)


class I2c01Test(DM32TestCase):
    """I2C0 and I2C1: the I2C2 controller model with nothing on either bus."""

    def test_reset_values(self):
        m = self.start()
        for base in (I2C0, I2C1):
            self.assertEqual(m.readl(base + STATUS), 0x6, hex(base))
            self.assertEqual(m.readl(base + CON), 0x7D, hex(base))

    def test_rtc_only_on_i2c2(self):
        """Negative: the RTC is on I2C2 only. A read at 0x70 on I2C0 or I2C1 NAKs."""
        m = self.start()
        for base in (I2C0, I2C1):
            setup(m, base)
            i2c_read(m, RTC, VAL_S, 1, base=base)
            self.assertEqual(m.readl(base + ABRT_SRC), ABRT_7B_ADDR_NOACK, hex(base))
            self.assertEqual(m.readl(base + RXFLR), 0, hex(base))

    def test_bus_takes_qemu_i2c_devices(self):
        """Each controller drives a QEMU I2C bus named i2c<n>: an AT24C EEPROM added on i2c1
        at 0x50 is written and read back through I2C1."""
        m = self.start(args=["-device", "at24c-eeprom,bus=i2c1,address=0x50,rom-size=256"])
        m.writel(IO_DIPLEX0, 0x800FFFE0 & ~I2C1_PADS)    # PTA7/PTA8 to I2C1
        setup(m, I2C1)
        i2c_write(m, 0x50, [0x10, 0xA5, 0x5A], base=I2C1)
        self.assertEqual(i2c_read(m, 0x50, 0x10, 2, base=I2C1), [0xA5, 0x5A])
        self.assertEqual(m.readl(I2C1 + ABRT_SRC), 0)

    def test_window_past_registers_is_unimp(self):
        """The model covers the 256-byte register file; the rest of each 64 KB block is the
        block's unimplemented window."""
        m = self.start()
        m.readl(I2C0 + 0x1000)
        m.readl(I2C1 + 0x1000)
        self.assertEqual(m.events()["unimp"], 2)


if __name__ == "__main__":
    unittest.main()
