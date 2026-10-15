# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# Copyright (c) 2026 Alex Kaplan
"""Interrupt controller at 0x17000000 (hw/intc/dm32_pic.c; HR_C7000 v2.7 section 4.7, p.166-183;
SVD peripheral PIC; docs/devices.md, "PIC"). The core input is observed through the PIC's
read-only "cpu-line" property: the vector in bits 7:0 ORed with bit 10, 0 when nothing is in
service."""

import unittest

from dm32qtest import DM32TestCase

BASE = 0x17000000
MODE, PO, MASK, VECTOR, COW1 = (BASE + o for o in (0x00, 0x04, 0x08, 0x0C, 0x10))
PRIOR0_3, PRIOR28_31, COW2, SYNC = BASE + 0x14, BASE + 0x30, BASE + 0x34, BASE + 0x38
FFLAG_L, RECORD_SEL, INT_ST, INT_ST_1, INT_CNT = (BASE + o for o in (0x3C, 0x40, 0x44, 0x48,
                                                                     0x4C))
MODE_1, PO_1, MASK_1 = BASE + 0x60, BASE + 0x64, BASE + 0x68
PRIOR32_35, PRIOR60_63, FFLAG_H = BASE + 0x6C, BASE + 0x88, BASE + 0x8C
ALL = 0xFFFFFFFF
EOI, FEOI = 0x4, 0x8                    # PIC_COW1 eoi [2], feoi [3]
COW2_PRIORITY, COW2_AVEC = 0x1, 0x2
INT = 1 << 10                           # csky_vic_v1 interrupt-request bit

# Sources with no device wired in dm32.c, so the test alone drives them.
SRC_LOW = 31                            # Table 30: Time_slot_inter_rx
SRC_A, SRC_B = 40, 50                  # Table 30: unassigned


def mask_words(*srcs):
    """PIC_MASK and PIC_MASK_1 with the given sources enabled (bit 1 = masked)."""
    low = high = ALL
    for s in srcs:
        if s < 32:
            low &= ~(1 << s)
        else:
            high &= ~(1 << (s - 32))
    return low, high


def unmask(m, *srcs):
    low, high = mask_words(*srcs)
    m.writel(MASK, low)
    m.writel(MASK_1, high)


def level_mode(m):
    """PIC_MODE and PIC_MODE_1 = 0 (level), as stock's pic_init_core @0x03001ca8 writes."""
    m.writel(MODE, 0)
    m.writel(MODE_1, 0)


def line(m, src, level):
    m.set_irq_in(m.pic_path(), src, level)


def pulse(m, src):
    line(m, src, 1)
    line(m, src, 0)


def vec(src, base=32):
    return INT | (src + base)


def records(m):
    return m.readl(INT_ST) | (m.readl(INT_ST_1) << 32)


class PicRegisterTest(DM32TestCase):
    def test_reset_values(self):
        """Reset values from the datasheet register descriptions (p.172-183) and the SVD: all
        sources edge, rising/high and masked; vector0 0x20; avec 1; PIC_PRIORn byte = n;
        everything else 0."""
        m = self.start()
        for reg in (MODE, PO, MASK, MODE_1, PO_1, MASK_1):
            self.assertEqual(m.readl(reg), ALL, hex(reg))
        self.assertEqual(m.readl(VECTOR), 0x20)
        self.assertEqual(m.readl(COW2), 0x2)
        self.assertEqual(m.readl(PRIOR0_3), 0x03020100)
        self.assertEqual(m.readl(PRIOR28_31), 0x1F1E1D1C)
        self.assertEqual(m.readl(PRIOR60_63), 0x3F3E3D3C)
        for reg in (SYNC, FFLAG_L, FFLAG_H, RECORD_SEL, INT_ST, INT_ST_1, INT_CNT):
            self.assertEqual(m.readl(reg), 0, hex(reg))
        self.assertEqual(m.pic_cpu_line(), 0)

    def test_prior32_35_reset_follows_svd(self):
        """PIC_PRIOR32_35 resets to 0x23222120 (SVD). The datasheet prints 0x27262524, a copy
        of the PIC_PRIOR36_39 row (p.183); every other row has byte n = n."""
        m = self.start()
        self.assertEqual(m.readl(PRIOR32_35), 0x23222120)

    def test_field_widths(self):
        """Narrow registers keep only their fields: vector0 [6:0], COW2 [1:0], min_interval
        [7:0], irq_record_sel [6:0] (p.174, 179-180). Wide registers keep all 32 bits."""
        m = self.start()
        for reg, kept in ((VECTOR, 0x7F), (COW2, 0x3), (SYNC, 0xFF), (RECORD_SEL, 0x7F),
                          (FFLAG_L, ALL), (FFLAG_H, ALL), (MODE, ALL), (PO_1, ALL)):
            m.writel(reg, ALL)
            self.assertEqual(m.readl(reg), kept, hex(reg))

    def test_priority_registers_read_back(self):
        """PIC_PRIORn store one byte per source, source n in byte n % 4."""
        m = self.start()
        m.writel(PRIOR0_3, 0x12345678)
        m.writel(PRIOR32_35, 0x9ABCDEF0)
        self.assertEqual(m.readl(PRIOR0_3), 0x12345678)
        self.assertEqual(m.readl(PRIOR32_35), 0x9ABCDEF0)

    def test_negative_write_only_and_read_only(self):
        """Negative: PIC_COW1 is write-only and reads 0; PIC_INT_CNT is read-only."""
        m = self.start()
        m.writel(COW1, ALL)
        self.assertEqual(m.readl(COW1), 0)
        m.writel(INT_CNT, 5)
        self.assertEqual(m.readl(INT_CNT), 0)

    def test_negative_undefined_offset(self):
        """Negative: an offset with no register (0x50) reads 0 and ignores writes."""
        m = self.start()
        m.writel(BASE + 0x50, ALL)
        self.assertEqual(m.readl(BASE + 0x50), 0)

    def test_negative_word_access_only(self):
        """Negative: the registers are 32-bit; a byte write is rejected and PIC_MASK keeps its
        reset value."""
        m = self.start()
        m.writeb(MASK, 0)
        self.assertEqual(m.readl(MASK), ALL)

    def test_system_reset(self):
        """A machine reset restores the reset values and empties the in-service register."""
        m = self.start()
        unmask(m, SRC_A)
        m.writel(VECTOR, 0x40)
        pulse(m, SRC_A)
        self.assertNotEqual(m.pic_cpu_line(), 0)
        m.qmp("system_reset")
        m.clock_step(1_000_000)
        self.assertEqual(m.readl(MASK_1), ALL)
        self.assertEqual(m.readl(VECTOR), 0x20)
        self.assertEqual(records(m), 0)
        self.assertEqual(m.pic_cpu_line(), 0)


class PicLevelTest(DM32TestCase):
    """Level-triggered sources (PIC_MODE 0), the configuration stock uses for every source."""

    def test_level_source_delivered(self):
        """An unmasked source at its active level is recorded in PIC_INT_ST and enters service:
        the core sees vector source + vector0 (p.174)."""
        m = self.start()
        level_mode(m)
        unmask(m, SRC_LOW)
        line(m, SRC_LOW, 1)
        self.assertEqual(m.readl(INT_ST), 1 << SRC_LOW)
        self.assertEqual(m.pic_cpu_line(), vec(SRC_LOW))

    def test_negative_masked_source_not_detected(self):
        """Negative: PIC_MASK disables request detection (p.170, step 5), so a masked source
        is neither recorded nor delivered."""
        m = self.start()
        level_mode(m)
        line(m, SRC_A, 1)
        self.assertEqual(records(m), 0)
        self.assertEqual(m.pic_cpu_line(), 0)

    def test_unmask_while_active_is_a_request(self):
        """Unmasking a source whose line is already active starts a request."""
        m = self.start()
        level_mode(m)
        line(m, SRC_A, 1)
        unmask(m, SRC_A)
        self.assertEqual(records(m), 1 << SRC_A)
        self.assertEqual(m.pic_cpu_line(), vec(SRC_A))

    def test_level_source_leaves_service_when_line_drops(self):
        """A level source's request ends with its line: it leaves service without an EOI, and
        its PIC_INT_ST record stays until cleared (the record is status)."""
        m = self.start()
        level_mode(m)
        unmask(m, SRC_A)
        line(m, SRC_A, 1)
        line(m, SRC_A, 0)
        self.assertEqual(m.pic_cpu_line(), 0)
        self.assertEqual(records(m), 1 << SRC_A)

    def test_masking_ends_a_level_request(self):
        """Masking a level source in service ends its request, so it leaves service."""
        m = self.start()
        level_mode(m)
        unmask(m, SRC_A)
        line(m, SRC_A, 1)
        unmask(m)
        self.assertEqual(m.pic_cpu_line(), 0)

    def test_negative_unacknowledged_level_reenters(self):
        """Negative: an EOI while the device still drives its line does not end the request;
        the source re-enters service at once."""
        m = self.start()
        level_mode(m)
        unmask(m, SRC_A)
        line(m, SRC_A, 1)
        m.writel(COW1, EOI)
        self.assertEqual(m.pic_cpu_line(), vec(SRC_A))
        line(m, SRC_A, 0)
        self.assertEqual(m.pic_cpu_line(), 0)

    def test_active_low_level(self):
        """PIC_PO 0 selects low-level triggering (p.173): a source idling low requests as soon
        as it is unmasked, and driving it high ends the request."""
        m = self.start()
        level_mode(m)
        m.writel(PO_1, ~(1 << (SRC_A - 32)) & ALL)
        unmask(m, SRC_A)
        self.assertEqual(m.pic_cpu_line(), vec(SRC_A))
        line(m, SRC_A, 1)
        self.assertEqual(m.pic_cpu_line(), 0)


class PicEdgeTest(DM32TestCase):
    """Edge-triggered sources (PIC_MODE 1, the reset value)."""

    def test_edge_latched(self):
        """A rising edge is latched: the source stays in service after its line drops, until
        the EOI."""
        m = self.start()
        unmask(m, SRC_A)
        pulse(m, SRC_A)
        self.assertEqual(m.pic_cpu_line(), vec(SRC_A))
        m.writel(COW1, EOI)
        self.assertEqual(m.pic_cpu_line(), 0)

    def test_held_edge_does_not_reenter(self):
        """An edge source whose line stays high is delivered once; after the EOI it does not
        re-enter (contrast the level source above)."""
        m = self.start()
        unmask(m, SRC_A)
        line(m, SRC_A, 1)
        m.writel(COW1, EOI)
        self.assertEqual(m.pic_cpu_line(), 0)

    def test_negative_wrong_polarity_loses_edge(self):
        """Negative: with PIC_PO 0 (falling edge) a rising edge is not a request: nothing is
        recorded or delivered until the line falls."""
        m = self.start()
        m.writel(PO_1, ~(1 << (SRC_A - 32)) & ALL)
        line(m, SRC_A, 1)
        unmask(m, SRC_A)
        self.assertEqual(records(m), 0)
        self.assertEqual(m.pic_cpu_line(), 0)
        line(m, SRC_A, 0)
        self.assertEqual(m.pic_cpu_line(), vec(SRC_A))

    def test_negative_masked_edge_lost(self):
        """Negative: an edge while the source is masked is not detected, so unmasking it later
        delivers nothing."""
        m = self.start()
        pulse(m, SRC_A)
        unmask(m, SRC_A)
        self.assertEqual(records(m), 0)
        self.assertEqual(m.pic_cpu_line(), 0)

    def test_latched_edge_survives_mask(self):
        """An edge latched while another source is in service waits; masked, it is held, and
        delivered once unmasked."""
        m = self.start()
        unmask(m, SRC_A, SRC_B)
        pulse(m, SRC_A)
        pulse(m, SRC_B)
        unmask(m, SRC_A)
        m.writel(COW1, EOI)
        self.assertEqual(m.pic_cpu_line(), 0)
        unmask(m, SRC_A, SRC_B)
        self.assertEqual(m.pic_cpu_line(), vec(SRC_B))


class PicArbitrationTest(DM32TestCase):
    def _two_pending(self, m, first, second):
        """Latch `first` and `second` behind a third source in service, then end it."""
        unmask(m, SRC_LOW, first, second)
        pulse(m, SRC_LOW)
        pulse(m, first)
        pulse(m, second)
        m.writel(COW1, EOI)

    def test_default_order_lowest_source_first(self):
        """With PIC_COW2.priority 0 the order is IRQ0 > IRQ1 > ... > IRQ63 (p.179), whatever
        the PIC_PRIOR bytes say: source 40 goes before 50 although its byte is the largest."""
        m = self.start()
        m.writel(BASE + 0x74, 0x2B2A29FF)       # PIC_PRIOR40_43: source 40 = 0xff
        m.writel(BASE + 0x7C, 0x33320030)       # PIC_PRIOR48_51: source 50 = 0
        self._two_pending(m, SRC_B, SRC_A)
        self.assertEqual(m.pic_cpu_line(), vec(SRC_A))
        m.writel(COW1, EOI)
        self.assertEqual(m.pic_cpu_line(), vec(SRC_B))

    def test_user_priority_inverts_order(self):
        """Negative (priority inversion visible): with PIC_COW2.priority 1 the smallest
        PIC_PRIOR byte wins (p.179), so source 50 (priority 0) beats source 40 (priority 0x40),
        the reverse of the default order."""
        m = self.start()
        m.writel(COW2, COW2_AVEC | COW2_PRIORITY)
        m.writel(BASE + 0x74, 0x2B2A2940)       # PIC_PRIOR40_43: source 40 = 0x40
        m.writel(BASE + 0x7C, 0x33320030)       # PIC_PRIOR48_51: source 50 = 0
        self._two_pending(m, SRC_A, SRC_B)
        self.assertEqual(m.pic_cpu_line(), vec(SRC_B))
        m.writel(COW1, EOI)
        self.assertEqual(m.pic_cpu_line(), vec(SRC_A))

    def test_equal_priority_lower_source_wins(self):
        """Assumption (dm32_pic.c): equal PIC_PRIOR bytes go to the lower source number."""
        m = self.start()
        m.writel(COW2, COW2_AVEC | COW2_PRIORITY)
        m.writel(BASE + 0x74, 0x2B2A2907)       # source 40 = 7
        m.writel(BASE + 0x7C, 0x33320730)       # source 50 = 7
        self._two_pending(m, SRC_B, SRC_A)
        self.assertEqual(m.pic_cpu_line(), vec(SRC_A))

    def test_negative_no_preemption(self):
        """Negative: a source in service is not preempted (one in-service register, p.174): a
        higher-priority request waits for the EOI, and the EOI ends the source in service."""
        m = self.start()
        unmask(m, SRC_A, SRC_B)
        pulse(m, SRC_B)
        pulse(m, SRC_A)
        self.assertEqual(m.pic_cpu_line(), vec(SRC_B))
        m.writel(COW1, EOI)
        self.assertEqual(m.pic_cpu_line(), vec(SRC_A))
        m.writel(COW1, EOI)
        self.assertEqual(m.pic_cpu_line(), 0)


class PicVectorTest(DM32TestCase):
    def test_vector_base(self):
        """VEC = source + vector0 (p.174): with vector0 0x40, source 40 is vector 0x68."""
        m = self.start()
        m.writel(VECTOR, 0x40)
        unmask(m, SRC_A)
        pulse(m, SRC_A)
        self.assertEqual(m.pic_cpu_line(), vec(SRC_A, 0x40))

    def test_autovector(self):
        """PIC_COW2.avec 0: the core uses its autovector, vector-table offset 0x28 (p.179),
        vector 10."""
        m = self.start()
        m.writel(COW2, 0)
        unmask(m, SRC_A)
        pulse(m, SRC_A)
        self.assertEqual(m.pic_cpu_line(), INT | 10)


class PicEoiTest(DM32TestCase):
    def test_negative_eoi_needs_bit2(self):
        """Negative: a PIC_COW1 write without eoi [2] (0, or feoi alone) does not end
        service; fast interrupts are not supported (p.170)."""
        m = self.start()
        unmask(m, SRC_A)
        pulse(m, SRC_A)
        for value in (0, FEOI):
            m.writel(COW1, value)
            self.assertEqual(m.pic_cpu_line(), vec(SRC_A), value)

    def test_negative_eoi_with_nothing_in_service(self):
        """Negative: an EOI with nothing in service changes nothing, and a masked source's
        line stays undetected."""
        m = self.start()
        line(m, SRC_A, 1)
        m.writel(COW1, EOI)
        self.assertEqual(records(m), 0)
        self.assertEqual(m.pic_cpu_line(), 0)

    def test_fast_flag_has_no_effect(self):
        """PIC_FFLAG marks fast interrupts, which the C7000 does not support (p.170): a flagged
        source is delivered as a normal interrupt."""
        m = self.start()
        m.writel(FFLAG_H, ALL)
        unmask(m, SRC_A)
        pulse(m, SRC_A)
        self.assertEqual(m.pic_cpu_line(), vec(SRC_A))


class PicRecordTest(DM32TestCase):
    def test_record_once_and_write_one_to_clear(self):
        """PIC_INT_ST records a source's request once (p.170); writing 1 clears that bit only,
        and writing 0 clears nothing."""
        m = self.start()
        unmask(m, SRC_A, SRC_B)
        pulse(m, SRC_A)
        pulse(m, SRC_A)
        pulse(m, SRC_B)
        self.assertEqual(records(m), (1 << SRC_A) | (1 << SRC_B))
        m.writel(INT_ST_1, 0)
        self.assertEqual(records(m), (1 << SRC_A) | (1 << SRC_B))
        m.writel(INT_ST_1, 1 << (SRC_A - 32))
        self.assertEqual(records(m), 1 << SRC_B)

    def test_negative_clearing_record_keeps_request(self):
        """Negative: the record is status; clearing it does not withdraw the request or end
        service."""
        m = self.start()
        level_mode(m)
        unmask(m, SRC_LOW)
        line(m, SRC_LOW, 1)
        m.writel(INT_ST, ALL)
        self.assertEqual(m.readl(INT_ST), 0)
        self.assertEqual(m.pic_cpu_line(), vec(SRC_LOW))

    def test_banks_clear_separately(self):
        """PIC_INT_ST clears sources 0-31 and PIC_INT_ST_1 sources 32-63."""
        m = self.start()
        unmask(m, SRC_LOW, SRC_B)
        pulse(m, SRC_LOW)
        pulse(m, SRC_B)
        m.writel(INT_ST, ALL)
        self.assertEqual(records(m), 1 << SRC_B)

    def test_request_count(self):
        """PIC_INT_CNT counts the requests of the source PIC_RECORD_SEL names (p.180-182):
        unmasked ones only. Clearing that source's record clears the count; clearing another
        source's does not."""
        m = self.start()
        m.writel(RECORD_SEL, SRC_A)
        pulse(m, SRC_A)                         # masked: not counted
        unmask(m, SRC_A, SRC_B)
        for _ in range(3):
            pulse(m, SRC_A)
        pulse(m, SRC_B)
        self.assertEqual(m.readl(INT_CNT), 3)
        m.writel(INT_ST_1, 1 << (SRC_B - 32))
        self.assertEqual(m.readl(INT_CNT), 3)
        m.writel(INT_ST_1, 1 << (SRC_A - 32))
        self.assertEqual(m.readl(INT_CNT), 0)

    def test_negative_record_sel_out_of_range(self):
        """Negative: PIC_RECORD_SEL values 64-127 name no source, so nothing is counted."""
        m = self.start()
        m.writel(RECORD_SEL, 64)
        unmask(m, SRC_A)
        pulse(m, SRC_A)
        self.assertEqual(m.readl(INT_CNT), 0)


if __name__ == "__main__":
    unittest.main()
