# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# Copyright (c) 2026 Alex Kaplan
"""System and modem registers at 0x11000000 (hw/misc/dm32_sysreg.c; docs/devices.md, "System
registers"; docs/modem-timing.md for the DMR Layer-2 slot counter; docs/environment.md,
"System and modem"; SVD SystemControl in data/registers/HR_C7000-DM32.svd)."""

import os
import unittest

from dm32qtest import DM32TestCase

BASE = 0x11000000
CLK_MGR = BASE + 0x04
IO_DIPLEX1 = BASE + 0x38
IO_DIPLEX2 = BASE + 0x3C
INTERP_CLEAR = BASE + 0x3B0
L2_CONTROL = BASE + 0x400
L2_SLOTON = BASE + 0x404
L2_TXRX_CTRL = BASE + 0x408
L2_SLOT_CNT = BASE + 0x40C
L2_STATUS = BASE + 0x41C

SLOT_NS = 30_000_000                 # docs/modem-timing.md: 30 ms per slot
TX_MASTER = 0x2                      # LAYER2_CONTROL bit 1, tx_master_mode
PASSIVE_CONTROL = 0xC4               # docs/modem-timing.md: stock boot wrote 0xc4 (passive)
TXSLOT_PIC_SRC = 30                  # dm32.c: sysreg TX-slot IRQ -> intc[30]


class SysregRegisterTest(DM32TestCase):
    def test_clk_mgr_reads_locked(self):
        """CLK_MGR_REG0x04 reads bits 31:30 set, keeping the low bits as written (sysreg header,
        item 1; devices.md, "System registers"). Reset reads 0xC0000000."""
        m = self.start()
        self.assertEqual(m.readl(CLK_MGR), 0xC0000000)
        m.writel(CLK_MGR, 0x12345678)
        self.assertEqual(m.readl(CLK_MGR), 0xD2345678)

    def test_negative_clk_mgr_lock_bits_not_clearable(self):
        """Negative: a write cannot clear bits 31:30 of CLK_MGR_REG0x04; the PLL reads locked
        whatever the firmware writes (sysreg header, item 1)."""
        m = self.start()
        m.writel(CLK_MGR, 0)
        self.assertEqual(m.readl(CLK_MGR), 0xC0000000)
        m.writel(CLK_MGR, 0xFFFFFFFF)
        self.assertEqual(m.readl(CLK_MGR), 0xFFFFFFFF)

    def test_pinmux_reset_values(self):
        """IO_DIPLEX1 resets to 0xFFFFFFFF and IO_DIPLEX2 to 0x3FFFFFFB (DS 4.4.4.5-4.4.4.6, SVD),
        so every muxed pad starts in its GPIO selection; bit 2 of IO_DIPLEX2 is reserved."""
        m = self.start()
        self.assertEqual(m.readl(IO_DIPLEX1), 0xFFFFFFFF)
        self.assertEqual(m.readl(IO_DIPLEX2), 0x3FFFFFFB)

    def test_plain_storage(self):
        """The window is storage except for the registers the model names (devices.md: "The window
        is storage, except for the following registers"). Stored values read back, including
        the ends of the 64 KB window and the stored-only modem registers TXRX_CTRL (+0x408) and
        SLOT_CNT (+0x40c)."""
        m = self.start()
        for addr, val in ((BASE + 0x200, 0x0BADF00D), (BASE + 0xFFFC, 0x8000FFFF),
                          (L2_SLOT_CNT, 0x000001FF), (L2_TXRX_CTRL, 0x41)):
            m.writel(addr, val)
            self.assertEqual(m.readl(addr), val, hex(addr))

    def test_narrow_access_addresses_its_bytes(self):
        """Byte and halfword accesses act on the bytes they address in the containing 32-bit
        register (include/hw/csky/dm32.h, dm32_reg_lane_read/write): a narrow read returns those
        bytes, and a narrow write replaces only them."""
        m = self.start()
        addr = BASE + 0x200
        m.writel(addr, 0x11223344)
        self.assertEqual(m.readb(addr + 1), 0x33)
        self.assertEqual(m.readw(addr + 2), 0x1122)
        m.writeb(addr + 1, 0xAB)
        self.assertEqual(m.readl(addr), 0x1122AB44)
        m.writew(addr + 2, 0xCDEF)
        self.assertEqual(m.readl(addr), 0xCDEFAB44)

    def test_negative_narrow_write_keeps_other_bytes(self):
        """Negative: a byte write to IO_DIPLEX2 must not zero the register's other bytes, which
        would hand every other pad in the register to its select-0 function."""
        m = self.start()
        before = m.readl(IO_DIPLEX2)
        m.writeb(IO_DIPLEX2 + 3, (before >> 24) & 0xFF)
        self.assertEqual(m.readl(IO_DIPLEX2), before)

    def test_pinmux_storage(self):
        """IO_DIPLEX2 is stored in full, so a driver's pad selects read back (devices.md: "Stored.
        Reset 0x3FF80100")."""
        m = self.start()
        m.writel(IO_DIPLEX2, 0x12345678)
        self.assertEqual(m.readl(IO_DIPLEX2), 0x12345678)


class SlotCounterTest(DM32TestCase):
    def start_counter(self, m):
        m.writel(L2_CONTROL, TX_MASTER)
        m.writel(L2_SLOTON, 1)

    def tick(self, m, n):
        for _ in range(n):
            m.clock_step(SLOT_NS)

    def status(self, m):
        return m.readl(L2_STATUS) & 0x1FF

    @staticmethod
    def txslot_log(m):
        """Changes of PIC source 30 only; the board raises other sources at start-up."""
        return [entry for entry in m.irq_log if entry[1] == TXSLOT_PIC_SRC]

    def test_counter_advances_once_per_30ms(self):
        """With tx_master_mode set, SLOTON bit 0 starts the counter, which advances once per 30 ms
        of virtual time (modem-timing.md, "Start" and "Period"; LAYER2_STATUS bits 8:0 read the
        live counter)."""
        m = self.start()
        self.start_counter(m)
        self.assertEqual(self.status(m), 0)
        m.clock_step(SLOT_NS - 1_000_000)
        self.assertEqual(self.status(m), 0)
        m.clock_step(1_000_000)
        self.assertEqual(self.status(m), 1)
        self.tick(m, 4)
        self.assertEqual(self.status(m), 5)

    def test_wrap_after_288_ticks_raises_pic30(self):
        """The counter runs 0..287 and wraps on the 288th tick, which raises the TX-slot line on
        PIC source 30 (modem-timing.md, "Range" and "Interrupt"; devices.md, "System
        registers")."""
        m = self.start()
        m.irq_intercept_in(m.pic_path())
        self.start_counter(m)
        self.tick(m, 287)
        self.assertEqual(self.status(m), 287)
        self.assertFalse(m.irq_level.get(TXSLOT_PIC_SRC, False))
        self.assertEqual(self.txslot_log(m), [])
        self.tick(m, 1)
        self.assertEqual(self.status(m), 0)
        self.assertTrue(m.irq_level[TXSLOT_PIC_SRC])
        self.assertEqual(self.txslot_log(m)[-1], (True, TXSLOT_PIC_SRC))

    def test_interp_clear_lowers_pic30(self):
        """A write of 1 to INTERP_CLEAR bit 0 acknowledges the TX-slot interrupt, lowering PIC
        source 30, and the counter keeps running (modem-timing.md, "Interrupt"; devices.md)."""
        m = self.start()
        m.irq_intercept_in(m.pic_path())
        self.start_counter(m)
        self.tick(m, 288)
        m.writel(INTERP_CLEAR, 1)
        self.assertFalse(m.irq_level[TXSLOT_PIC_SRC])
        self.assertEqual(self.txslot_log(m)[-1], (False, TXSLOT_PIC_SRC))
        self.tick(m, 1)
        self.assertEqual(self.status(m), 1)

    def test_interrupt_held_until_acknowledged(self):
        """The TX-slot line is a held level: it stays high across later ticks until INTERP_CLEAR is
        written (modem-timing.md, "Interrupt": "held high from the wrap until ...")."""
        m = self.start()
        m.irq_intercept_in(m.pic_path())
        self.start_counter(m)
        self.tick(m, 288)
        self.tick(m, 10)
        self.assertTrue(m.irq_level[TXSLOT_PIC_SRC])
        self.assertEqual(self.status(m), 10)

    def test_negative_interp_clear_needs_bit0(self):
        """Negative: INTERP_CLEAR writes without bit 0 set (0x2, and 0) leave the TX-slot line high
        (sysreg header, item 2: "a write of 1 to bit 0")."""
        m = self.start()
        m.irq_intercept_in(m.pic_path())
        self.start_counter(m)
        self.tick(m, 288)
        m.writel(INTERP_CLEAR, 0x2)
        m.writel(INTERP_CLEAR, 0)
        self.assertTrue(m.irq_level[TXSLOT_PIC_SRC])

    def test_negative_passive_mode_does_not_start(self):
        """Negative: with LAYER2_CONTROL bit 1 clear (the passive value 0xC4 that stock writes at
        boot), SLOTON bit 0 does not start the counter, and no interrupt is raised
        (modem-timing.md, "Passive mode" and "Observed with stock firmware")."""
        m = self.start()
        m.irq_intercept_in(m.pic_path())
        m.writel(L2_CONTROL, PASSIVE_CONTROL)
        m.writel(L2_SLOTON, 1)
        self.tick(m, 10)
        self.assertEqual(self.status(m), 0)
        self.assertEqual(self.txslot_log(m), [])

    def test_slotoff_stops_then_restarts_from_zero(self):
        """Clearing SLOTON bit 0 stops the counter where it is, and a later 0-to-1 write starts it
        again from 0 (modem-timing.md, "Stop" and "Start")."""
        m = self.start()
        self.start_counter(m)
        self.tick(m, 3)
        m.writel(L2_SLOTON, 0)
        self.tick(m, 5)
        self.assertEqual(self.status(m), 3)
        m.writel(L2_SLOTON, 1)
        self.assertEqual(self.status(m), 0)
        self.tick(m, 2)
        self.assertEqual(self.status(m), 2)

    def test_negative_status_low_bits_are_live(self):
        """Negative: writes cannot set the live counter in LAYER2_STATUS bits 8:0, which read the
        counter (modem-timing.md, "LAYER2_STATUS"). Bits above 8 read back from storage."""
        m = self.start()
        m.writel(L2_STATUS, 0x1FF)
        self.assertEqual(self.status(m), 0)
        m.writel(L2_STATUS, 0xABCDE000)
        self.assertEqual(m.readl(L2_STATUS), 0xABCDE000)


class TxOriginateLogTest(DM32TestCase):
    def log_path(self):
        self.dir = self.make_tempdir()
        return os.path.join(self.dir, "modem.log")

    def test_tx_originate_logged_while_running(self):
        """A write of 0x40 or 0x41 to LAYER2_TXRX_CTRL while the counter runs is logged as
        TX-ORIGINATE with the slot number (modem-timing.md, "Logging"; sysreg header, item 2)."""
        log = self.log_path()
        m = self.start(env={"DM32_MODEM_LOG": log})
        m.writel(L2_CONTROL, TX_MASTER)
        m.writel(L2_SLOTON, 1)
        m.clock_step(2 * SLOT_NS)
        m.writel(L2_TXRX_CTRL, 0x40)
        m.close()
        with open(log) as f:
            text = f.read()
        self.assertIn("slot counter START", text)
        self.assertIn("TX-ORIGINATE: COMMAND=0x40 at slot=2", text)

    def test_negative_tx_originate_needs_running_counter(self):
        """Negative: with the counter stopped, a 0x40 write to LAYER2_TXRX_CTRL is stored but not
        logged as TX-ORIGINATE (modem-timing.md, "Interrupt" and "Logging": the event is only
        logged while the counter runs)."""
        log = self.log_path()
        m = self.start(env={"DM32_MODEM_LOG": log})
        m.writel(L2_TXRX_CTRL, 0x40)
        self.assertEqual(m.readl(L2_TXRX_CTRL), 0x40)
        m.close()
        with open(log) as f:
            self.assertNotIn("TX-ORIGINATE", f.read())


if __name__ == "__main__":
    unittest.main()
