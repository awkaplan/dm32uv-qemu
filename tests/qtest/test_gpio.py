# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# Copyright (c) 2026 Alex Kaplan
"""GPIO banks A, B and C (hw/gpio/dm32_gpio.c; docs/devices.md, "GPIO"; docs/environment.md,
"Keypad and pin injection"; SVD peripheral GPIOA for the register layout and reset values).
The bank IRQ lines go to PIC sources 15 (A), 14 (B) and 17 (C), as wired in hw/csky/dm32.c."""

import unittest

from dm32qtest import DM32TestCase

BANK = {"A": 0x14020000, "B": 0x14100000, "C": 0x14110000}
PIC_SOURCE = {"A": 15, "B": 14, "C": 17}
DR, DDR, EXT = 0x00, 0x04, 0x50                        # SWPORTA_DR, SWPORTA_DDR, EXT_PORTA
INTEN, INTMASK, INTTYPE, INTPOL = 0x30, 0x34, 0x38, 0x3C
INTSTATUS, RAW, DEBOUNCE, EOI, BOTH_EDGE = 0x40, 0x44, 0x48, 0x4C, 0x78
MS = 1_000_000                  # virtual-clock nanoseconds per millisecond
VAL = 0x00000A5A                # a value with no bit 7, 8 or 24 set, for storage checks

# Logical pins for DM32_PINSEQ: logical pin = bank * 32 + bit (A=0, B=1, C=2).
PIN_A9 = 9                     # PTA9 (UART1_RXD pad), on GPIO at reset
JTAG_BITS = 0x1F                # PTA0-4 reset to JTAG (IO_DIPLEX0 [4:0] = 0)
PIN_B3, PIN_B4, PIN_B5 = 35, 36, 37
PIN_C2 = 66


def enable(m, bank, bit, edge=False, rising=False, both=False):
    """Configure one interrupt bit. The type, polarity and both-edge registers are written before
    INTEN, so the bit is never armed with a half-written configuration."""
    base = BANK[bank]
    mask = 1 << bit
    m.writel(base + INTTYPE, mask if edge else 0)
    m.writel(base + INTPOL, mask if rising else 0)
    m.writel(base + BOTH_EDGE, mask if both else 0)
    m.writel(base + INTEN, mask)


class GpioRegisterTest(DM32TestCase):
    def test_narrow_access_addresses_its_bytes(self):
        """Byte and halfword accesses act on the bytes they address in the containing 32-bit
        register (include/hw/csky/dm32.h, dm32_reg_lane_read/write). INTTYPE is plain storage."""
        m = self.start()
        addr = BANK["C"] + INTTYPE
        m.writel(addr, 0x00000A5A)
        self.assertEqual(m.readb(addr + 1), 0x0A)
        m.writeb(addr + 2, 0x5A)
        self.assertEqual(m.readl(addr), 0x005A0A5A)
        self.assertEqual(m.readw(addr + 2), 0x005A)

    def test_reset_values(self):
        """SVD GPIOA reset values: every register except EXT_PORTA is 0 on each bank. The input
        word is covered by test_idle_levels."""
        m = self.start()
        for bank, base in BANK.items():
            for off in (DR, DDR, INTEN, INTMASK, INTTYPE, INTPOL, INTSTATUS, RAW, DEBOUNCE,
                        BOTH_EDGE):
                self.assertEqual(m.readl(base + off), 0, f"bank {bank} +{off:#x}")

    def test_registers_stored(self):
        """Stored and read back on every bank: SWPORTA_DR, SWPORTA_DDR, DEBOUNCE ("Stored and read
        back. No filtering", docs/devices.md), INTMASK, INTTYPE_LEVEL, INT_POLARITY and
        INTR_BOTH_EDGE (SVD GPIOA)."""
        m = self.start()
        for base in BANK.values():
            for off in (DR, DDR, DEBOUNCE, INTMASK, INTTYPE, INTPOL, BOTH_EDGE):
                m.writel(base + off, VAL)
                self.assertEqual(m.readl(base + off), VAL, f"{base:#x} +{off:#x}")

    def test_idle_levels(self):
        """Inputs idle high (docs/devices.md, "Input levels"; dm32_gpio.c header): EXT_PORTA reads
        0xFFFFFFFF on banks B and C. Bank A also idles high, except bit 16, the power-sense input,
        which idles low by default (DM32_GPIO_IDLE_LOW, default 0:16), and bits 0-4, whose pads
        reset to JTAG and so read 0 from GPIO."""
        m = self.start()
        self.assertEqual(m.readl(BANK["A"] + EXT), 0xFFFEFFFF & ~JTAG_BITS)
        self.assertEqual(m.readl(BANK["B"] + EXT), 0xFFFFFFFF)
        self.assertEqual(m.readl(BANK["C"] + EXT), 0xFFFFFFFF)

    def test_idle_low_override(self):
        """DM32_GPIO_IDLE_LOW="bank:bit,..." replaces the default list (docs/environment.md): B5
        and C0 idle low, and bank A bit 16 goes back to idle high."""
        m = self.start(env={"DM32_GPIO_IDLE_LOW": "1:5,2:0"})
        self.assertEqual(m.readl(BANK["A"] + EXT), 0xFFFFFFFF & ~JTAG_BITS)
        self.assertEqual(m.readl(BANK["B"] + EXT), 0xFFFFFFDF)
        self.assertEqual(m.readl(BANK["C"] + EXT), 0xFFFFFFFE)

    def test_idle_low_none(self):
        """DM32_GPIO_IDLE_LOW=none clears the list (docs/environment.md): bank A reads all ones
        apart from the JTAG pads, which is the state that parks stock's power-off dialog
        (dm32_gpio.c header)."""
        m = self.start(env={"DM32_GPIO_IDLE_LOW": "none"})
        self.assertEqual(m.readl(BANK["A"] + EXT), 0xFFFFFFFF & ~JTAG_BITS)

    def test_negative_ext_porta_read_only(self):
        """Negative: EXT_PORTA is read-only (v2.7 EN 5.7.5; SVD): a write to +0x50 does not change
        the input word."""
        m = self.start()
        m.writel(BANK["B"] + EXT, 0x000000F0)
        self.assertEqual(m.readl(BANK["B"] + EXT), 0xFFFFFFFF)

    def test_window_storage(self):
        """Offsets with no named register are read/write storage (dm32_gpio.c header: "The rest of
        the window is backed by read/write storage"), so a write to +0x80 reads back."""
        m = self.start()
        m.writel(BANK["C"] + 0x80, VAL)
        self.assertEqual(m.readl(BANK["C"] + 0x80), VAL)

    def test_read_only_registers_ignore_writes(self):
        """Negative: INTSTATUS and RAW_INTSTATUS are read-only (SVD GPIOA; docs/devices.md), so a
        write leaves them at 0. PORTA_EOI is write-only and reads 0 (dm32_gpio.c header)."""
        m = self.start()
        base = BANK["B"]
        m.writel(base + INTSTATUS, 0xFFFFFFFF)
        m.writel(base + RAW, 0xFFFFFFFF)
        m.writel(base + EOI, 0xFFFFFFFF)
        self.assertEqual(m.readl(base + INTSTATUS), 0)
        self.assertEqual(m.readl(base + RAW), 0)
        self.assertEqual(m.readl(base + EOI), 0)


class GpioInterruptTest(DM32TestCase):
    def irq_machine(self, env=None):
        m = self.start(env=env)
        m.irq_intercept_in(m.pic_path())
        return m

    def test_level_interrupt_follows_pin(self):
        """Level-type bit, active low (dm32_gpio.c header; docs/devices.md: "Level bits follow the
        pin"): B5 forced low at 1 ms raises PIC source 14 and sets RAW; the release at 3 ms lowers
        the line and clears RAW."""
        m = self.irq_machine(env={"DM32_PINSEQ": f"1:{PIN_B5}:0;3:{PIN_B5}:-"})
        enable(m, "B", 5)
        m.clock_step(2 * MS)
        self.assertTrue(m.irq_level[14])
        self.assertTrue(m.readl(BANK["B"] + RAW) & (1 << 5))
        m.clock_step(2 * MS)
        self.assertFalse(m.irq_level[14])
        self.assertFalse(m.readl(BANK["B"] + RAW) & (1 << 5))

    def test_edge_interrupt_latches_until_eoi(self):
        """Edge-type bit, falling (docs/devices.md: "edge bits latch until EOI"): B3 falls at 1 ms,
        is released at 3 ms, and the bit stays latched. PORTA_EOI clears it and lowers the line."""
        m = self.irq_machine(env={"DM32_PINSEQ": f"1:{PIN_B3}:0;3:{PIN_B3}:-"})
        enable(m, "B", 3, edge=True)
        m.clock_step(2 * MS)
        self.assertTrue(m.irq_level[14])
        self.assertTrue(m.readl(BANK["B"] + INTSTATUS) & (1 << 3))
        m.clock_step(2 * MS)
        self.assertTrue(m.irq_level[14])
        m.writel(BANK["B"] + EOI, 1 << 3)
        self.assertFalse(m.irq_level[14])
        self.assertEqual(m.readl(BANK["B"] + RAW), 0)

    def test_rising_polarity_selects_release(self):
        """INT_POLARITY 1 selects the rising edge (docs/devices.md, "INT_POLARITY"): the falling
        edge at 1 ms does not latch, and the release at 3 ms does."""
        m = self.irq_machine(env={"DM32_PINSEQ": f"1:{PIN_B3}:0;3:{PIN_B3}:-"})
        enable(m, "B", 3, edge=True, rising=True)
        m.clock_step(2 * MS)
        self.assertEqual(m.readl(BANK["B"] + RAW), 0)
        m.clock_step(2 * MS)
        self.assertTrue(m.readl(BANK["B"] + RAW) & (1 << 3))
        self.assertTrue(m.irq_level[14])

    def test_both_edges_trigger_when_selected(self):
        """INTR_BOTH_EDGE=1 makes an edge bit trigger on either edge (docs/devices.md): after the
        falling edge is cleared by EOI, the rising edge at 3 ms latches it again."""
        m = self.irq_machine(env={"DM32_PINSEQ": f"1:{PIN_B3}:0;3:{PIN_B3}:-"})
        enable(m, "B", 3, edge=True, both=True)
        m.clock_step(2 * MS)
        self.assertTrue(m.readl(BANK["B"] + RAW) & (1 << 3))
        m.writel(BANK["B"] + EOI, 1 << 3)
        m.clock_step(2 * MS)
        self.assertTrue(m.readl(BANK["B"] + RAW) & (1 << 3))
        self.assertTrue(m.irq_level[14])

    def test_single_edge_ignores_opposite_edge(self):
        """Negative: with INTR_BOTH_EDGE=0 and polarity 0 (falling), the rising edge on release does
        not latch the bit (docs/devices.md, "INTR_BOTH_EDGE")."""
        m = self.irq_machine(env={"DM32_PINSEQ": f"1:{PIN_B3}:0;3:{PIN_B3}:-"})
        enable(m, "B", 3, edge=True)
        m.clock_step(2 * MS)
        m.writel(BANK["B"] + EOI, 1 << 3)
        m.clock_step(2 * MS)
        self.assertEqual(m.readl(BANK["B"] + RAW), 0)
        self.assertFalse(m.irq_level[14])

    def test_eoi_does_not_clear_level_bit(self):
        """Negative: PORTA_EOI clears edge bits only (docs/devices.md: "Write 1 to clear edge
        bits"). A level bit whose pin is still low stays raised after an EOI write."""
        m = self.irq_machine(env={"DM32_PINSEQ": f"1:{PIN_B5}:0"})
        enable(m, "B", 5)
        m.clock_step(2 * MS)
        m.writel(BANK["B"] + EOI, 1 << 5)
        self.assertTrue(m.readl(BANK["B"] + RAW) & (1 << 5))
        self.assertTrue(m.irq_level[14])

    def test_mask_hides_intstatus_not_raw(self):
        """INTMASK masks INTSTATUS, not RAW (docs/devices.md: "Masks INTSTATUS, not RAW"). A masked
        level bit leaves the line low, and clearing the mask raises it again."""
        m = self.irq_machine(env={"DM32_PINSEQ": f"1:{PIN_B5}:0"})
        enable(m, "B", 5)
        m.writel(BANK["B"] + INTMASK, 1 << 5)
        m.clock_step(2 * MS)
        self.assertTrue(m.readl(BANK["B"] + RAW) & (1 << 5))
        self.assertEqual(m.readl(BANK["B"] + INTSTATUS), 0)
        self.assertNotIn((True, 14), m.irq_log)
        m.writel(BANK["B"] + INTMASK, 0)
        self.assertTrue(m.irq_level[14])

    def test_type_change_clears_raw(self):
        """Changing a bit's trigger type clears its raw state (docs/devices.md, "INTTYPE_LEVEL").
        A latched edge bit whose pin is released drops when it is switched to level."""
        m = self.irq_machine(env={"DM32_PINSEQ": f"1:{PIN_B3}:0;3:{PIN_B3}:-"})
        enable(m, "B", 3, edge=True)
        m.clock_step(4 * MS)
        self.assertTrue(m.readl(BANK["B"] + RAW) & (1 << 3))
        m.writel(BANK["B"] + INTTYPE, 0)
        self.assertEqual(m.readl(BANK["B"] + RAW), 0)
        self.assertFalse(m.irq_level[14])

    def test_output_pin_excluded(self):
        """Negative: an output pin (SWPORTA_DDR bit set) never interrupts, even with INTEN set and
        its input forced to the active level (docs/devices.md: "Output pins are excluded from
        interrupts")."""
        m = self.irq_machine(env={"DM32_PINSEQ": f"1:{PIN_B5}:0"})
        m.writel(BANK["B"] + DDR, 1 << 5)
        enable(m, "B", 5)
        m.clock_step(2 * MS)
        self.assertEqual(m.readl(BANK["B"] + RAW), 0)
        self.assertNotIn((True, 14), m.irq_log)

    def test_inten_required(self):
        """Negative: a pin with INTEN clear never sets RAW, even when its input is at the active
        level (docs/devices.md: "Per-bit enable")."""
        m = self.irq_machine(env={"DM32_PINSEQ": f"1:{PIN_B5}:0"})
        m.clock_step(2 * MS)
        self.assertEqual(m.readl(BANK["B"] + RAW), 0)
        self.assertEqual(m.irq_log, [])

    def test_banks_drive_their_pic_lines(self):
        """Each bank drives its own PIC source: A to 15, B to 14 and C to 17 (docs/devices.md,
        "Interrupt wiring"). One level bit is enabled and forced low on each bank."""
        m = self.irq_machine(
            env={"DM32_PINSEQ": f"1:{PIN_A9}:0;1:{PIN_B3}:0;1:{PIN_C2}:0"})
        enable(m, "A", 9)
        enable(m, "B", 3)
        enable(m, "C", 2)
        m.clock_step(2 * MS)
        for bank, src in PIC_SOURCE.items():
            self.assertTrue(m.irq_level[src], f"bank {bank} -> PIC {src}")


class GpioInjectionTest(DM32TestCase):
    def test_pinseq_is_virtual_time(self):
        """DM32_PINSEQ steps happen at their virtual time (docs/environment.md): B4 forced low at
        5 ms reads low from then until its release at 9 ms, and is idle before and after."""
        m = self.start(env={"DM32_PINSEQ": f"5:{PIN_B4}:0;9:{PIN_B4}:-"})
        bit = 1 << 4
        m.clock_step(4 * MS)
        self.assertTrue(m.readl(BANK["B"] + EXT) & bit)
        m.clock_step(2 * MS)
        self.assertFalse(m.readl(BANK["B"] + EXT) & bit)
        m.clock_step(4 * MS)
        self.assertTrue(m.readl(BANK["B"] + EXT) & bit)

    def test_key_row_follows_column(self):
        """DM32_KEY="colbank:colbit:rowbank:rowbit" (docs/environment.md): with column C7 driven
        low, row B17 reads low on EXT_PORTA reads. Driving the column high releases the row."""
        m = self.start(env={"DM32_KEY": "2:7:1:17"})
        bit = 1 << 17
        m.writel(BANK["C"] + DDR, 1 << 7)
        m.writel(BANK["C"] + DR, 0)
        self.assertEqual(m.readl(BANK["B"] + EXT) & bit, 0)
        m.writel(BANK["C"] + DR, 1 << 7)
        self.assertTrue(m.readl(BANK["B"] + EXT) & bit)

    def test_key_row_idle_when_column_high(self):
        """Negative: with the column never driven low, the row reads its idle level (dm32_gpio.c,
        "Keypress injection"): the forced row bit is applied only while the column is low."""
        m = self.start(env={"DM32_KEY": "2:7:1:17"})
        self.assertTrue(m.readl(BANK["B"] + EXT) & (1 << 17))

    def test_key_hold_releases_after_reads(self):
        """DM32_KEY_HOLD=2 releases the key after two row-bank reads (docs/environment.md): the
        first two reads are low and the third is idle high."""
        m = self.start(env={"DM32_KEY": "2:7:1:17", "DM32_KEY_HOLD": "2"})
        m.writel(BANK["C"] + DDR, 1 << 7)
        m.writel(BANK["C"] + DR, 0)
        bit = 1 << 17
        self.assertEqual(m.readl(BANK["B"] + EXT) & bit, 0)
        self.assertEqual(m.readl(BANK["B"] + EXT) & bit, 0)
        self.assertTrue(m.readl(BANK["B"] + EXT) & bit)

    def test_key_read_is_interrupt_evaluation_point(self):
        """Each EXT_PORTA read re-evaluates the interrupt block (dm32_gpio.c, "Reads are an
        evaluation point"). A level bit on B17 with the key injected only on that read raises PIC
        source 14 on the read, not before it."""
        m = self.start(env={"DM32_KEY": "2:7:1:17"})
        m.irq_intercept_in(m.pic_path())
        enable(m, "B", 17)
        m.writel(BANK["C"] + DDR, 1 << 7)
        m.writel(BANK["C"] + DR, 0)
        self.assertFalse(m.irq_level.get(14, False))
        m.readl(BANK["B"] + EXT)
        self.assertTrue(m.irq_level[14])


SYS = 0x11000000
IO_DIPLEX0, REN2 = SYS + 0x34, SYS + 0x48
US = 1000


class GpioPadLevelTest(DM32TestCase):
    """EXT_PORTA is the pad level (dm32_gpio.c, dm32_gpio_input_level)."""

    def test_output_pin_reads_its_drive(self):
        """A pin GPIO drives reads its SWPORTA_DR bit."""
        m = self.start()
        bit = 1 << 7
        m.writel(BANK["C"] + DDR, bit)
        m.writel(BANK["C"] + DR, 0)
        self.assertEqual(m.readl(BANK["C"] + EXT) & bit, 0)
        m.writel(BANK["C"] + DR, bit)
        self.assertEqual(m.readl(BANK["C"] + EXT) & bit, bit)

    def test_soc_pull_down(self):
        """IOMGR_REN_REG2 bit 5 enables PTB5's pull-down (v2.7 EN 4.4.4.9; 0 = on): the undriven
        pad then reads 0 instead of the board's high."""
        m = self.start()
        bit = 1 << 5
        self.assertEqual(m.readl(BANK["B"] + EXT) & bit, bit)
        m.writel(REN2, 0x7FFFFFFF & ~bit)
        self.assertEqual(m.readl(BANK["B"] + EXT) & bit, 0)

    def test_idle_low_overrides_soc_pull_up(self):
        """DM32_GPIO_IDLE_LOW names board pulls the SoC does not know about, so it wins over an
        enabled SoC pull-up (PTB13, REN_REG2 bit 13)."""
        m = self.start(env={"DM32_GPIO_IDLE_LOW": "1:13"})
        bit = 1 << 13
        m.writel(REN2, 0x7FFFFFFF & ~bit)
        self.assertEqual(m.readl(BANK["B"] + EXT) & bit, 0)

    def test_negative_muxed_pad_reads_zero_and_cannot_interrupt(self):
        """Negative: PTA15 given to PWM_0 (IO_DIPLEX0 bit 17 = 0) reads 0 from GPIO, and an
        active-low level interrupt on it does not fire (assumption in dm32_gpio.c: the datasheet
        does not say what GPIO sees on such a pad)."""
        m = self.start()
        m.irq_intercept_in(m.pic_path())
        bit = 1 << 15
        m.writel(IO_DIPLEX0, 0x800FFFE0 & ~(1 << 17))
        enable(m, "A", 15)
        self.assertEqual(m.readl(BANK["A"] + EXT) & bit, 0)
        self.assertEqual(m.readl(BANK["A"] + RAW), 0)
        self.assertFalse(m.irq_level.get(PIC_SOURCE["A"], False))


class GpioKeySenseTest(DM32TestCase):
    """A matrix key's row reads low only while its column pad is driven low."""

    def test_negative_wrong_drive_sense(self):
        """Negative: a scan that drives the column high reads nothing (row idle high)."""
        m = self.start(env={"DM32_KEY": "2:7:1:17"})
        m.writel(BANK["C"] + DDR, 1 << 7)
        m.writel(BANK["C"] + DR, 1 << 7)
        self.assertTrue(m.readl(BANK["B"] + EXT) & (1 << 17))

    def test_negative_column_not_driven(self):
        """Negative: a column written low but left an input (SWPORTA_DDR clear) does not reach the
        pad, so the row reads nothing."""
        m = self.start(env={"DM32_KEY": "2:7:1:17"})
        m.writel(BANK["C"] + DR, 0)
        self.assertTrue(m.readl(BANK["B"] + EXT) & (1 << 17))

    def test_negative_column_pad_muxed_away(self):
        """Negative: with PTC7 on the LCD bus (IO_DIPLEX2 [10:9] = 0) the driven column never
        reaches the matrix."""
        m = self.start(env={"DM32_KEY": "2:7:1:17"})
        m.writel(SYS + 0x3C, 0x3FFFFFFB & ~(3 << 9))
        m.writel(BANK["C"] + DDR, 1 << 7)
        m.writel(BANK["C"] + DR, 0)
        self.assertTrue(m.readl(BANK["B"] + EXT) & (1 << 17))


class GpioEdgeTest(DM32TestCase):
    def test_both_edge_is_per_bit(self):
        """INTR_BOTH_EDGE is per bit: with B4 both-edge and B5 falling-edge only, a rising edge on
        B5 latches nothing."""
        m = self.start(env={"DM32_PINSEQ": f"1:{PIN_B5}:0;2:{PIN_B5}:1"})
        base = BANK["B"]
        m.writel(base + INTTYPE, (1 << 4) | (1 << 5))
        m.writel(base + INTPOL, 0)
        m.writel(base + BOTH_EDGE, 1 << 4)
        m.writel(base + INTEN, (1 << 4) | (1 << 5))
        m.clock_step(int(1.5 * MS))
        self.assertEqual(m.readl(base + RAW), 1 << 5)      # the falling edge
        m.writel(base + EOI, 1 << 5)
        m.clock_step(1 * MS)
        self.assertEqual(m.readl(base + RAW), 0)           # the rising edge: not latched


class GpioDebounceTest(DM32TestCase):
    """DEBOUNCE (+0x48): a debounced bit feeds the interrupt block only after its level has held
    for two cycles of the 32.768 kHz debounce clock, 61 us (assumptions in dm32_gpio.c)."""

    def machine(self, seq, debounce=True):
        m = self.start(env={"DM32_PINSEQ": seq})
        base = BANK["B"]
        m.writel(base + DEBOUNCE, (1 << 5) if debounce else 0)
        m.writel(base + INTTYPE, 1 << 5)                   # falling edge
        m.writel(base + INTEN, 1 << 5)
        return m

    def test_short_pulse_rejected(self):
        """Negative: a 30 us low pulse never reaches the interrupt block."""
        m = self.machine(f"1:{PIN_B5}:0;1.03:{PIN_B5}:-")
        m.clock_step(2 * MS)
        self.assertEqual(m.readl(BANK["B"] + RAW), 0)

    def test_short_pulse_without_debounce(self):
        """The same pulse with DEBOUNCE clear latches the edge."""
        m = self.machine(f"1:{PIN_B5}:0;1.03:{PIN_B5}:-", debounce=False)
        m.clock_step(2 * MS)
        self.assertEqual(m.readl(BANK["B"] + RAW), 1 << 5)

    def test_held_level_seen_after_window(self):
        """A held low level is seen 61 us after the edge, not before; EXT_PORTA shows it at once
        (debounce applies to the interrupt block only)."""
        m = self.machine(f"1:{PIN_B5}:0")
        m.clock_step(1 * MS + 50 * US)
        self.assertEqual(m.readl(BANK["B"] + EXT) & (1 << 5), 0)
        self.assertEqual(m.readl(BANK["B"] + RAW), 0)
        m.clock_step(20 * US)
        self.assertEqual(m.readl(BANK["B"] + RAW), 1 << 5)


if __name__ == "__main__":
    unittest.main()
