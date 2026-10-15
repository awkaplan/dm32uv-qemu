# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# Copyright (c) 2026 Alex Kaplan
"""Six-channel timer at 0x14000000 (hw/timer/dm32_timer.c; HR_C7000 v2.7 section 4.8; SVD
peripheral TIMER; docs/devices.md, "Timer" and "PIC"). Channel n drives PIC source n + 1."""

import unittest

from dm32qtest import DM32TestCase

BASE = 0x14000000
TIMERS_STATUS, TIMERS_EOI, TIMERS_RAW, COMP_VERSION = (BASE + o for o in
                                                         (0xA0, 0xA4, 0xA8, 0xAC))
LOAD, CURRENT, CTRL, EOI, INT_STATUS = 0x00, 0x04, 0x08, 0x0C, 0x10
ENABLE_PERIODIC = 0x3          # enable | mode (1 = periodic), mask bit clear
ENABLE_PERIODIC_MASKED = 0x7
CLOCK_HZ = 84_000_000          # dm32_timer.c: APB timer clock

PIC = 0x17000000
PIC_MODE, PIC_MASK, PIC_EOI = PIC + 0x00, PIC + 0x08, PIC + 0x10
EOI_BIT = 0x4                  # PIC_COW1.eoi
VEC_INT = 1 << 10              # csky_vic_v1 interrupt-request bit on the PIC's core output

MS = 1_000_000                 # ns per millisecond
TICK_NS = 100_000              # 0.1 ms margin around a deadline


def ch(n, off=0):
    """Address of register `off` in timer channel n (0x14-byte stride, SVD TIMER_CH)."""
    return BASE + 0x14 * n + off


def load_for_ms(ms):
    """LoadCount that gives an exact period of `ms` milliseconds at 84 MHz."""
    return ms * CLOCK_HZ // 1000


def stock_ch0(m):
    """Program channel 0 as stock does at boot (timer_ch0_configure_periodic @0x0306cef4):
    ControlReg = 2 (user-defined, disabled, unmasked), LoadCount = 10 ms, then enable."""
    m.writel(ch(0, CTRL), 0x2)
    m.writel(ch(0, LOAD), load_for_ms(10))
    m.writel(ch(0, CTRL), ENABLE_PERIODIC)


def line(m, src):
    """The level last reported on PIC input `src` (False until it has changed)."""
    return m.irq_level.get(src, False)


class TimerRegisterTest(DM32TestCase):
    def test_comp_version(self):
        """TIMERS_COMP_VERSION reads 0x3230322a (HR_C7000 v2.7 section 4.8.5.34, p.200; SVD)."""
        m = self.start()
        self.assertEqual(m.readl(COMP_VERSION), 0x3230322A)

    def test_reset_state(self):
        """At reset every channel is disabled with LoadCount 0 (SVD reset values); stock enables
        channel 0 itself (timer_ch0_configure_periodic @0x0306cef4). The SVD's live read of a
        running DM-32UV (ControlReg 0x03) is that programmed state."""
        m = self.start()
        for n in range(6):
            self.assertEqual(m.readl(ch(n, LOAD)), 0, n)
            self.assertEqual(m.readl(ch(n, CTRL)), 0, n)
        m.clock_step(100 * MS)
        self.assertEqual(m.readl(TIMERS_RAW), 0)

    def test_load_and_control_read_back(self):
        """LoadCount and ControlReg store the written value (SVD TIMER_CH registers)."""
        m = self.start()
        m.writel(ch(2, LOAD), 0x1234)
        m.writel(ch(2, CTRL), ENABLE_PERIODIC_MASKED)
        self.assertEqual(m.readl(ch(2, LOAD)), 0x1234)
        self.assertEqual(m.readl(ch(2, CTRL)), ENABLE_PERIODIC_MASKED)

    def test_channels_have_separate_registers(self):
        """Each channel has its own LoadCount at a 0x14 stride (SVD TIMER_CH dimIncrement
        0x14): writing channel 1 does not change channel 2."""
        m = self.start()
        m.writel(ch(1, LOAD), 0x1111)
        m.writel(ch(2, LOAD), 0x2222)
        self.assertEqual(m.readl(ch(1, LOAD)), 0x1111)
        self.assertEqual(m.readl(ch(2, LOAD)), 0x2222)


class TimerIntervalTest(DM32TestCase):
    def watched(self):
        """A machine whose PIC inputs are observed (one intercept per machine)."""
        m = self.start()
        m.irq_intercept_in(m.pic_path())
        return m

    def test_channel0_period_10ms(self):
        """Channel 0 at boot has LoadCount 840000, which is 10 ms at 84 MHz (devices.md). Its
        raw status and its PIC source 1 go high at 10 ms, not before. The PIC masks source 1 at
        reset, so the CPU is not interrupted, but the timer's own status still shows it."""
        m = self.watched()
        stock_ch0(m)
        m.clock_step(10 * MS - TICK_NS)
        self.assertEqual(m.readl(TIMERS_RAW), 0)
        self.assertFalse(line(m, 1))
        m.clock_step(2 * TICK_NS)
        self.assertEqual(m.readl(TIMERS_RAW), 0x1)
        self.assertEqual(m.readl(TIMERS_STATUS), 0x1)
        self.assertTrue(line(m, 1))

    def test_channel0_periodic_reload(self):
        """Periodic mode reloads LoadCount at each underflow (devices.md: every channel reloads
        periodically). Reading the channel's EOI each period, source 1 rises once per 10 ms."""
        m = self.watched()
        stock_ch0(m)
        m.clock_step(10 * MS + TICK_NS)
        for _ in range(5):
            self.assertTrue(line(m, 1))
            m.readl(ch(0, EOI))
            self.assertFalse(line(m, 1))
            m.clock_step(10 * MS)

    def test_current_value_counts_down(self):
        """CurrentValue counts DOWN from LoadCount (SVD TIMER_CH, CurrentValue). Over 2 ms at
        84 MHz that is 168000 counts, within 1%."""
        m = self.start()
        stock_ch0(m)
        before = m.readl(ch(0, CURRENT))
        m.clock_step(2 * MS)
        after = m.readl(ch(0, CURRENT))
        self.assertLess(after, before)
        self.assertLessEqual(abs((before - after) - 168000), 1680)

    def test_channels_1_to_5_period_and_source(self):
        """Channel n has period n ms (LoadCount n * 84000) and drives PIC source n + 1
        (devices.md, "Timer"; dm32.c wires channel n to intc[n + 1]). Each channel's raw bit and
        PIC input go high at its own deadline, with 0.1 ms of margin on either side."""
        m = self.watched()
        t0 = m.clock_set(0)
        for n in range(1, 6):
            m.writel(ch(n, LOAD), load_for_ms(n))
            m.writel(ch(n, CTRL), ENABLE_PERIODIC)
        now = t0
        for n in range(1, 6):
            deadline = t0 + n * MS
            now = m.clock_step(deadline - TICK_NS - now)
            self.assertEqual(m.readl(TIMERS_RAW) & (1 << n), 0, f"channel {n} early")
            self.assertFalse(line(m, n + 1), f"source {n + 1} early")
            now = m.clock_step(2 * TICK_NS)
            self.assertTrue(m.readl(TIMERS_RAW) & (1 << n), f"channel {n} late")
            self.assertTrue(line(m, n + 1), f"source {n + 1} late")
        self.assertEqual(m.readl(TIMERS_STATUS), 0x3E)
        self.assertFalse(line(m, 1))

    def test_zero_load_full_range(self):
        """devices.md: a LoadCount of 0 is treated as 0xFFFFFFFF. After 1 ms the count is just
        below 0xFFFFFFFF, and the channel does not fire within 900 ms (2^32 counts is 51 s)."""
        m = self.watched()
        m.writel(ch(3, LOAD), 0)
        m.writel(ch(3, CTRL), ENABLE_PERIODIC)
        m.clock_step(1 * MS)
        self.assertGreater(m.readl(ch(3, CURRENT)), 0xFFF00000)
        m.clock_step(900 * MS)
        self.assertFalse(line(m, 4))
        self.assertEqual(m.readl(TIMERS_RAW) & 0x8, 0)


class TimerEoiTest(DM32TestCase):
    def test_channel_eoi_clears_only_that_channel(self):
        """Reading a channel's EOI (+0x0C) clears that channel's interrupt and its PIC input
        only (SVD TIMER_CH EOI: read to clear this channel's interrupt)."""
        m = self.start()
        m.irq_intercept_in(m.pic_path())
        m.writel(ch(1, LOAD), load_for_ms(1))
        m.writel(ch(2, LOAD), load_for_ms(2))
        m.writel(ch(1, CTRL), ENABLE_PERIODIC)
        m.writel(ch(2, CTRL), ENABLE_PERIODIC)
        m.clock_step(2 * MS + TICK_NS)
        self.assertEqual(m.readl(TIMERS_RAW), 0x6)
        self.assertEqual(m.readl(ch(1, EOI)), 0)
        self.assertEqual(m.readl(TIMERS_RAW), 0x4)
        self.assertFalse(line(m, 2))
        self.assertTrue(line(m, 3))
        self.assertEqual(m.readl(ch(2, INT_STATUS)), 1)

    def test_global_eoi_clears_all_channels(self):
        """TimersEOI (0xA4) read clears every channel's interrupt (SVD TimersEOI: read to
        clear all channels' interrupts)."""
        m = self.start()
        m.irq_intercept_in(m.pic_path())
        for n in (1, 2):
            m.writel(ch(n, LOAD), load_for_ms(n))
            m.writel(ch(n, CTRL), ENABLE_PERIODIC)
        m.clock_step(2 * MS + TICK_NS)
        self.assertEqual(m.readl(TIMERS_EOI), 0)
        self.assertEqual(m.readl(TIMERS_RAW), 0)
        self.assertEqual(m.readl(TIMERS_STATUS), 0)
        self.assertFalse(line(m, 2) or line(m, 3))

    def test_masked_channel_no_status_no_line(self):
        """Negative: a masked channel (ControlReg bit 2 set) still shows in TimersRawIntStatus,
        but its IntStatus, TimersIntStatus and PIC input stay low (SVD ControlReg int_mask,
        TimersIntStatus, TimersRawIntStatus). Unmasking shows the held interrupt again."""
        m = self.start()
        m.irq_intercept_in(m.pic_path())
        m.writel(ch(4, LOAD), load_for_ms(1))
        m.writel(ch(4, CTRL), ENABLE_PERIODIC_MASKED)
        m.clock_step(MS + TICK_NS)
        self.assertEqual(m.readl(TIMERS_RAW), 0x10)
        self.assertEqual(m.readl(TIMERS_STATUS), 0)
        self.assertEqual(m.readl(ch(4, INT_STATUS)), 0)
        self.assertFalse(line(m, 5))
        m.writel(ch(4, CTRL), ENABLE_PERIODIC)
        self.assertEqual(m.readl(ch(4, INT_STATUS)), 1)
        self.assertTrue(line(m, 5))

    def test_negative_global_registers_read_only(self):
        """Negative: writes to TimersIntStatus, TimersRawIntStatus and TimersEOI are dropped
        (SVD: read-only, and TimersEOI is read-to-clear). A pending channel stays pending."""
        m = self.start()
        m.writel(ch(1, LOAD), load_for_ms(1))
        m.writel(ch(1, CTRL), ENABLE_PERIODIC)
        m.clock_step(MS + TICK_NS)
        m.writel(TIMERS_RAW, 0)
        m.writel(TIMERS_STATUS, 0)
        m.writel(TIMERS_EOI, 0)
        self.assertEqual(m.readl(TIMERS_RAW), 0x2)
        self.assertEqual(m.readl(TIMERS_STATUS), 0x2)

    def test_stock_tick_sequence(self):
        """The RTOS tick in stock's order: the PIC is level-triggered (pic_init_core @0x03001ca8
        writes PIC_MODE = 0), the dispatcher (IRAM 0x1064c) writes the PIC EOI before calling
        the ISR, and the ISR then reads channel 0's EOI. Source 1 re-enters service at the PIC
        EOI because its line is still high, and leaves it when the channel EOI drops the line,
        so the core sees one interrupt per period."""
        m = self.start()
        m.irq_intercept_in(m.pic_path())
        m.writel(PIC_MODE, 0)
        stock_ch0(m)
        m.writel(PIC_MASK, 0xFFFFFFFD)
        m.clock_step(10 * MS + TICK_NS)
        self.assertEqual(m.pic_cpu_line(), VEC_INT | 33)
        m.writel(PIC_EOI, EOI_BIT)
        self.assertEqual(m.pic_cpu_line(), VEC_INT | 33)
        m.readl(ch(0, EOI))
        self.assertFalse(line(m, 1))
        self.assertEqual(m.pic_cpu_line(), 0)
        m.clock_step(10 * MS)
        self.assertEqual(m.pic_cpu_line(), VEC_INT | 33)

    def test_negative_pic_eoi_keeps_timer_held(self):
        """Negative: a PIC EOI does not reach the timer. Channel 0's interrupt stays held until
        the channel's own EOI, so a level-triggered source 1 is still requesting afterwards."""
        m = self.start()
        m.irq_intercept_in(m.pic_path())
        m.writel(PIC_MODE, 0)
        stock_ch0(m)
        m.writel(PIC_MASK, 0xFFFFFFFD)
        m.clock_step(10 * MS + TICK_NS)
        m.writel(PIC_EOI, EOI_BIT)
        self.assertEqual(m.readl(TIMERS_RAW), 0x1)
        self.assertTrue(line(m, 1))
        self.assertEqual(m.pic_cpu_line(), VEC_INT | 33)



class TimerModeTest(DM32TestCase):
    """Counting modes and reloads (dm32_timer.c header: DesignWare DW_apb_timers convention;
    the datasheet lists free-running and user-defined counting, p.188)."""

    def test_free_running_reloads_all_ones(self):
        """Mode 0 (free-running): the first period counts LoadCount, then the counter reloads
        0xffffffff, so the second interrupt does not come one period later."""
        m = self.start()
        m.writel(ch(1, LOAD), load_for_ms(1))
        m.writel(ch(1, CTRL), 0x1)                     # enable, mode 0, unmasked
        m.clock_step(MS + TICK_NS)
        self.assertEqual(m.readl(TIMERS_RAW) & 0x2, 0x2)
        m.readl(ch(1, EOI))
        self.assertGreater(m.readl(ch(1, CURRENT)), 0xF0000000)
        m.clock_step(5 * MS)
        self.assertEqual(m.readl(TIMERS_RAW) & 0x2, 0)

    def test_user_defined_reloads_load_count(self):
        """Mode 1 (user-defined, what stock uses): every period is LoadCount."""
        m = self.start()
        m.writel(ch(1, LOAD), load_for_ms(1))
        m.writel(ch(1, CTRL), ENABLE_PERIODIC)
        for _ in range(3):
            m.clock_step(MS)
            self.assertEqual(m.readl(TIMERS_RAW) & 0x2, 0x2)
            m.readl(ch(1, EOI))

    def test_load_write_while_running_waits_for_reload(self):
        """LoadCount written to a running channel takes effect at the next reload: stock
        re-periods running channels this way (mmio_14000000_timer_config @0x0301d900). The
        current 10 ms period still ends at 10 ms; the next one is 2 ms."""
        m = self.start()
        m.writel(ch(2, LOAD), load_for_ms(10))
        m.writel(ch(2, CTRL), ENABLE_PERIODIC)
        m.clock_step(3 * MS)
        m.writel(ch(2, LOAD), load_for_ms(2))
        m.clock_step(6 * MS)
        self.assertEqual(m.readl(TIMERS_RAW) & 0x4, 0)   # not at 2 ms after the write
        m.clock_step(MS + TICK_NS)
        self.assertEqual(m.readl(TIMERS_RAW) & 0x4, 0x4)
        m.readl(ch(2, EOI))
        m.clock_step(2 * MS)
        self.assertEqual(m.readl(TIMERS_RAW) & 0x4, 0x4)

    def test_enable_loads_load_count(self):
        """An enable starts the count from LoadCount (p.188), in either mode."""
        m = self.start()
        m.writel(ch(3, LOAD), load_for_ms(4))
        m.writel(ch(3, CTRL), 0x1)
        self.assertGreater(m.readl(ch(3, CURRENT)), load_for_ms(4) - 1000)

    def test_current_value_read_only(self):
        """Negative: CurrentValue is read-only (SVD): stock's write of 0 before an enable does
        not change the count."""
        m = self.start()
        m.writel(ch(1, LOAD), load_for_ms(5))
        m.writel(ch(1, CTRL), ENABLE_PERIODIC)
        m.writel(ch(1, CURRENT), 0)
        self.assertGreater(m.readl(ch(1, CURRENT)), load_for_ms(4))

    def test_disabled_channel_keeps_count(self):
        """A disabled channel stops: its count does not move and no interrupt comes."""
        m = self.start()
        m.writel(ch(1, LOAD), load_for_ms(5))
        m.writel(ch(1, CTRL), ENABLE_PERIODIC)
        m.clock_step(MS)
        m.writel(ch(1, CTRL), 0x2)
        held = m.readl(ch(1, CURRENT))
        m.clock_step(20 * MS)
        self.assertEqual(m.readl(ch(1, CURRENT)), held)
        self.assertEqual(m.readl(TIMERS_RAW) & 0x2, 0)

    def test_system_reset_stops_timers(self):
        """A machine reset (e.g. a watchdog bite) returns every channel to its reset state."""
        m = self.start()
        stock_ch0(m)
        m.writel(ch(1, LOAD), load_for_ms(1))
        m.writel(ch(1, CTRL), ENABLE_PERIODIC)
        m.qmp("system_reset")
        m.clock_step(MS)                                  # let the main loop run the reset
        self.assertEqual(m.readl(ch(0, CTRL)), 0)
        self.assertEqual(m.readl(ch(1, CTRL)), 0)
        m.clock_step(20 * MS)
        self.assertEqual(m.readl(TIMERS_RAW), 0)


if __name__ == "__main__":
    unittest.main()
