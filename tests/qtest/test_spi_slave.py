# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# Copyright (c) 2026 Alex Kaplan
"""SPI slave controllers at 0x14120000, 0x14130000, 0x14140000 (hw/misc/dm32_spi_slave.c;
HR_C7000 v2.7 section 5.2, p.243-263; Table 26; Table 30, p.169; SVD SPI_Slave0-2, derived from
SPI_Master0). No master is attached, so the tests check what a slave does on its own."""

import unittest

from dm32qtest import DM32TestCase

SLAVES = [(0x14120000, 34), (0x14130000, 35), (0x14140000, 36)]
CTRLR0, SSIENR, TXFTLR, TXFLR, RXFLR, SR = 0x00, 0x08, 0x18, 0x20, 0x24, 0x28
IMR, ISR, RISR, TXOICR, RXOICR, RXUICR, ICR = 0x2C, 0x30, 0x34, 0x38, 0x3C, 0x40, 0x48
IDR, VERSION, DR = 0x58, 0x5C, 0x60
TXE, TXO, RXU = 0x1, 0x2, 0x4
DEPTH = 8                                  # "spi_slave FIFO depth is 8", p.244


class SpiSlaveTest(DM32TestCase):
    def test_reset_values(self):
        """SVD/DS reset values on all three: CTRLR0 0x7, SR 0x6 (TFNF, TFE), IMR 0x3f,
        IDR 0x11223344, SSI_COMP_VERSION 0x3331312a (p.254-263)."""
        m = self.start()
        for base, _ in SLAVES:
            self.assertEqual(m.readl(base + CTRLR0), 0x7)
            self.assertEqual(m.readl(base + SR), 0x6)
            self.assertEqual(m.readl(base + IMR), 0x3F)
            self.assertEqual(m.readl(base + IDR), 0x11223344)
            self.assertEqual(m.readl(base + VERSION), 0x3331312A)
            self.assertEqual(m.readl(base + RISR), 0)

    def test_id_registers_read_only(self):
        """Negative: IDR and SSI_COMP_VERSION are read-only (DS, SVD)."""
        m = self.start()
        base = SLAVES[0][0]
        m.writel(base + IDR, 0)
        m.writel(base + VERSION, 0)
        self.assertEqual(m.readl(base + IDR), 0x11223344)
        self.assertEqual(m.readl(base + VERSION), 0x3331312A)

    def test_enable_raises_txe_on_own_source(self):
        """An enabled slave with an empty TX FIFO (at or below TXFTLR = 0) raises TXEI, which is
        unmasked at reset, on its own PIC source (34, 35, 36; Table 30)."""
        m = self.start()
        m.irq_intercept_in(m.pic_path())
        for base, src in SLAVES:
            self.assertFalse(m.irq_level.get(src, False))
            m.writel(base + SSIENR, 1)
            self.assertEqual(m.readl(base + RISR) & TXE, TXE)
            self.assertTrue(m.irq_level[src], src)
            m.writel(base + IMR, 0)
            self.assertFalse(m.irq_level[src], src)
            self.assertEqual(m.readl(base + ISR), 0)

    def test_tx_fifo_fills_and_overflows(self):
        """DR writes queue in the 8-entry TX FIFO and wait for a master that never comes: TXFLR
        counts them, SR drops TFE then TFNF, and a ninth write raises TXOI."""
        m = self.start()
        base = SLAVES[1][0]
        m.writel(base + SSIENR, 1)
        for i in range(DEPTH):
            m.writel(base + DR, i)
        self.assertEqual(m.readl(base + TXFLR), DEPTH)
        self.assertEqual(m.readl(base + SR), 0)
        self.assertEqual(m.readl(base + RISR) & TXO, 0)
        m.writel(base + DR, 0xFF)
        self.assertEqual(m.readl(base + TXFLR), DEPTH)
        self.assertEqual(m.readl(base + RISR) & TXO, TXO)
        self.assertEqual(m.readl(base + TXOICR), 1)
        self.assertEqual(m.readl(base + RISR) & TXO, 0)

    def test_rx_underflow(self):
        """Negative: with no master the RX FIFO stays empty; a DR read returns 0 and raises
        RXUI, cleared by reading RXUICR."""
        m = self.start()
        base = SLAVES[2][0]
        m.writel(base + SSIENR, 1)
        self.assertEqual(m.readl(base + RXFLR), 0)
        self.assertEqual(m.readl(base + DR), 0)
        self.assertEqual(m.readl(base + RISR) & RXU, RXU)
        self.assertEqual(m.readl(base + RXUICR), 1)
        self.assertEqual(m.readl(base + RXUICR), 0)

    def test_icr_clears_all(self):
        """ICR clears TXOI, RXOI and RXUI together and returns whether any was set."""
        m = self.start()
        base = SLAVES[0][0]
        m.writel(base + SSIENR, 1)
        m.readl(base + DR)
        for _ in range(DEPTH + 1):
            m.writel(base + DR, 0)
        self.assertEqual(m.readl(base + RISR) & (TXO | RXU), TXO | RXU)
        self.assertEqual(m.readl(base + ICR), 1)
        self.assertEqual(m.readl(base + RISR) & (TXO | RXU), 0)

    def test_disabled_slave_drops_writes(self):
        """Negative: with SSIENR clear, DR writes are dropped, no TXEI is raised, and clearing
        SSIENR empties the TX FIFO."""
        m = self.start()
        base = SLAVES[0][0]
        m.writel(base + DR, 1)
        self.assertEqual(m.readl(base + TXFLR), 0)
        self.assertEqual(m.readl(base + RISR), 0)
        m.writel(base + SSIENR, 1)
        m.writel(base + DR, 1)
        m.writel(base + SSIENR, 0)
        self.assertEqual(m.readl(base + TXFLR), 0)
        self.assertEqual(m.readl(base + SR), 0x6)

    def test_txftlr_threshold(self):
        """TXEI follows TXFTLR: with a threshold of 2, it stays raised for up to 2 entries."""
        m = self.start()
        base = SLAVES[0][0]
        m.writel(base + TXFTLR, 2)
        m.writel(base + SSIENR, 1)
        m.writel(base + DR, 0)
        m.writel(base + DR, 0)
        self.assertEqual(m.readl(base + RISR) & TXE, TXE)
        m.writel(base + DR, 0)
        self.assertEqual(m.readl(base + RISR) & TXE, 0)

    def test_no_events_counted(self):
        """The blocks are modelled now, so accessing them is not an unimp event."""
        m = self.start()
        for base, _ in SLAVES:
            m.readl(base + SR)
        self.assertEqual(m.events()["unimp"], 0)


if __name__ == "__main__":
    unittest.main()
