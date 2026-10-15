# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# Copyright (c) 2026 Alex Kaplan
"""SSI controllers and the W25Q128 SPI-NOR data chip (hw/misc/dm32_ssi.c; docs/devices.md,
"SSI and SPI-NOR"; docs/environment.md, "SPI-NOR data chip"; SVD SSIENR, SER, BAUDR, SR, DR and
IO_DIPLEX1; W25Q128JV datasheet section 8.2.27, hardware/W25Q128JV-datasheet/).

The data chip is behind SSI0 at 0x140A0000. Every test follows stock's set-up (spi_m0_init
@0x0301c94c, then the per-transfer enable): it muxes the SPI0 pads, programs BAUDR 2 with the block
disabled, enables it, sets SER, and selects the chip by driving GPIOA 18 low. Each frame takes
(DFS + 1) * SCKDV / 84 MHz of virtual time, so xfer() steps the clock before reading DR.
"""

import os
import unittest

from dm32qtest import DM32TestCase, run_plain

SSI0 = 0x140A0000
SSI1 = 0x140B0000
SSI3 = 0x14150000
SSI_PROG = 0x04000000     # the instance the stock firmware uses for the program flash
GPIOA = 0x14020000
IO_DIPLEX1 = 0x11000038

CTRLR0, CTRLR1, ENABLE, SER, BAUDR = 0x00, 0x04, 0x08, 0x10, 0x14
TXFTLR, RXFTLR, TXFLR, RXFLR, SR = 0x18, 0x1C, 0x20, 0x24, 0x28
IMR, ISR, RISR, TXOICR, RXOICR, RXUICR, ICR, IDR, VERSION, DR = (0x2C, 0x30, 0x34, 0x38, 0x3C, 0x40,
                                                                 0x48, 0x58, 0x5C, 0x60)
GPIO_DR, GPIO_DDR = 0x00, 0x04
CS_BIT = 1 << 18                # GPIOA 18, the data chip's select (stock pin 0x12)
US = 1000
SPI0_PAD_BITS = 0x3F << 3       # IO_DIPLEX1 bits 8:3: SPI0 SCLK/MOSI/MISO (dm32_ssi.c ssi_pads_muxed)
SPI1_PAD_BITS = 0x3F << 13      # IO_DIPLEX1 bits 18:13: SPI1 SCLK/MOSI/MISO
SR_BUSY, SR_TFNF, SR_TFE, SR_RFNE, SR_RFF = 0x1, 0x2, 0x4, 0x8, 0x10

WREN, WRDI, RDSR, READ, PAGE_PROGRAM = 0x06, 0x04, 0x05, 0x03, 0x02
SECTOR_ERASE, BLOCK_ERASE, RDID = 0x20, 0xD8, 0x9F
SECTOR = 0x1000
BLOCK = 0x10000
NOR_SIZE = 16 << 20


def pattern(n):
    """Store contents that never read 0xff, so an erase is visible at every byte."""
    return bytes((i * 13 + 1) & 0x7F for i in range(n))


def pads_muxed(m):
    """Hand the SPI0 and SPI1 pads to their masters: IO_DIPLEX1 bits 8:3 and 18:13 are zero."""
    m.writel(IO_DIPLEX1, 0xFFFFFFFF & ~(SPI0_PAD_BITS | SPI1_PAD_BITS))


def cs(m, low):
    """Drive GPIOA 18, the data chip's select, as stock does around every operation."""
    m.writel(GPIOA + GPIO_DDR, CS_BIT)
    m.writel(GPIOA + GPIO_DR, 0 if low else CS_BIT)


def ssi_on(m, base=SSI0, baudr=2):
    pads_muxed(m)
    m.writel(base + ENABLE, 0)
    m.writel(base + BAUDR, baudr)
    m.writel(base + ENABLE, 1)
    m.writel(base + SER, 1)
    if base == SSI0:
        cs(m, True)


def xfer(m, base, b):
    """One DR write (one byte out), time for the frame, and the DR read of the byte shifted
    back."""
    m.writel(base + DR, b)
    m.clock_step(1 * US)
    return m.readl(base + DR)


def end_command(m, base):
    """Release and re-assert the chip select (the command boundary): GPIOA 18 for SSI0, SER for
    the other instances."""
    if base == SSI0:
        cs(m, False)
        cs(m, True)
    else:
        m.writel(base + SER, 0)
        m.writel(base + SER, 1)


def addr_bytes(m, base, addr):
    for shift in (16, 8, 0):
        xfer(m, base, (addr >> shift) & 0xFF)


def read_bytes(m, base, addr, n):
    """READ 0x03: the fourth transfer returns the preloaded byte, which the driver discards in
    the address phase, then each 0xff transfer clocks out the next byte."""
    xfer(m, base, READ)
    addr_bytes(m, base, addr)
    data = [xfer(m, base, 0xFF) for _ in range(n)]
    end_command(m, base)
    return data


def wren(m, base=SSI0):
    xfer(m, base, WREN)
    end_command(m, base)


def program(m, base, addr, data):
    xfer(m, base, PAGE_PROGRAM)
    addr_bytes(m, base, addr)
    for b in data:
        xfer(m, base, b)
    end_command(m, base)


def erase(m, base, cmd, addr):
    xfer(m, base, cmd)
    addr_bytes(m, base, addr)
    end_command(m, base)


def rdsr(m, base=SSI0):
    v = xfer(m, base, RDSR)
    end_command(m, base)
    return v


class SsiRegisterTest(DM32TestCase):
    def test_reset_values(self):
        """Reset: SSIENR, SER, BAUDR and the FIFO levels are 0 (SVD SSIENR, SER, BAUDR); SR
        reads 0x06, TFNF and TFE set and BUSY clear (dm32_ssi.c header, devices.md "SSI")."""
        m = self.start()
        for off in (ENABLE, SER, BAUDR, TXFLR, RXFLR):
            self.assertEqual(m.readl(SSI0 + off), 0, hex(off))
        self.assertEqual(m.readl(SSI0 + SR), SR_TFNF | SR_TFE)

    def test_registers_stored(self):
        """Configuration registers keep their fields while the block is disabled: BAUDR [15:0],
        SER [1:0] (p.256); IDR and SSI_COMP_VERSION read 0x11223344 and 0x3331312A (p.262)."""
        m = self.start()
        m.writel(SSI0 + BAUDR, 0x12344)
        m.writel(SSI0 + SER, 0x5)
        self.assertEqual(m.readl(SSI0 + BAUDR), 0x2344)
        self.assertEqual(m.readl(SSI0 + SER), 0x1)
        self.assertEqual(m.readl(SSI0 + CTRLR0), 0x7)
        self.assertEqual(m.readl(SSI0 + IMR), 0x3F)
        self.assertEqual(m.readl(SSI0 + IDR), 0x11223344)
        self.assertEqual(m.readl(SSI0 + VERSION), 0x3331312A)

    def test_sr_follows_the_frame(self):
        """A frame leaves the TX FIFO when it starts (TFE set at once), BUSY is set while it
        shifts, and its received byte reaches the RX FIFO when it ends: 8 bits at SCKDV 2 from
        84 MHz take 190 ns."""
        m = self.start()
        ssi_on(m)
        m.writel(SSI0 + DR, RDID)
        self.assertEqual(m.readl(SSI0 + SR), SR_TFNF | SR_TFE | SR_BUSY)
        m.clock_step(100)
        self.assertEqual(m.readl(SSI0 + RXFLR), 0)
        m.clock_step(100)
        self.assertEqual(m.readl(SSI0 + SR), SR_TFNF | SR_TFE | SR_RFNE)
        m.readl(SSI0 + DR)
        self.assertEqual(m.readl(SSI0 + SR), SR_TFNF | SR_TFE)

    def test_same_layout_on_program_flash_block(self):
        """The 0x04000000 block has the same register layout (dm32_ssi.c header; devices.md
        "SSI" lists it with the other SSI instances)."""
        m = self.start()
        m.writel(SSI_PROG + ENABLE, 1)
        m.writel(SSI_PROG + SER, 0x3)
        self.assertEqual(m.readl(SSI_PROG + ENABLE), 1)
        self.assertEqual(m.readl(SSI_PROG + SER), 0x3)
        self.assertEqual(m.readl(SSI_PROG + SR), SR_TFNF | SR_TFE)

    def test_instances_have_separate_stores(self):
        """Each SSI instance owns its own 16 MB store (dm32_ssi.c, "Backing store"): a program
        on SSI1 does not change the byte SSI0 reads at the same address."""
        m = self.start()
        ssi_on(m, SSI1)
        wren(m, SSI1)
        program(m, SSI1, 0x1000, [0x00])
        ssi_on(m, SSI0)
        self.assertEqual(read_bytes(m, SSI0, 0x1000, 1), [0xFF])
        self.assertEqual(read_bytes(m, SSI1, 0x1000, 1), [0x00])


class SsiGatingTest(DM32TestCase):
    def test_pads_not_muxed_loses_frame(self):
        """Negative: with IO_DIPLEX1 at reset (SPI0 pads on GPIO) the frame still shifts, but on
        pads that do not reach the controller: it is lost (ssi_discard) and reads 0xFF."""
        m = self.start()
        ssi_on(m)
        m.writel(IO_DIPLEX1, 0xFFFFFFFF)
        self.assertEqual(xfer(m, SSI0, RDID), 0xFF)
        self.assertEqual(xfer(m, SSI0, 0xFF), 0xFF)
        self.assertEqual(m.events()["ssi_discard"], 2)

    def test_ssienr_clear_discards_write(self):
        """Negative: a DR write while SSIENR is 0 is dropped and counted as ssi_discard."""
        m = self.start()
        pads_muxed(m)
        m.writel(SSI0 + SER, 1)
        m.writel(SSI0 + DR, RDID)
        self.assertEqual(m.readl(SSI0 + TXFLR), 0)
        self.assertEqual(m.events()["ssi_discard"], 1)

    def test_frame_waits_for_ser(self):
        """With SER 0 a frame waits in the TX FIFO; setting SER starts it."""
        m = self.start()
        ssi_on(m)
        m.writel(SSI0 + SER, 0)
        m.writel(SSI0 + DR, RDID)
        m.clock_step(10 * US)
        self.assertEqual(m.readl(SSI0 + TXFLR), 1)
        self.assertFalse(m.readl(SSI0 + SR) & SR_BUSY)
        m.writel(SSI0 + SER, 1)
        m.clock_step(1 * US)
        self.assertEqual(m.readl(SSI0 + TXFLR), 0)
        self.assertEqual(m.readl(SSI0 + RXFLR), 1)

    def test_negative_baudr_zero_stops_clock(self):
        """Negative: BAUDR 0 disables the clock (p.256), so a frame never starts: it stays in
        the TX FIFO and nothing is received."""
        m = self.start()
        ssi_on(m, baudr=0)
        m.writel(SSI0 + DR, RDID)
        m.clock_step(100 * US)
        self.assertEqual(m.readl(SSI0 + TXFLR), 1)
        self.assertEqual(m.readl(SSI0 + RXFLR), 0)

    def test_frame_time_follows_baudr(self):
        """SCLK = APB clock / SCKDV (p.247): SCKDV 32 makes an 8-bit frame 3.05 us."""
        m = self.start()
        ssi_on(m, baudr=32)
        m.writel(SSI0 + DR, RDID)
        m.clock_step(3 * US)
        self.assertEqual(m.readl(SSI0 + RXFLR), 0)
        m.clock_step(100)
        self.assertEqual(m.readl(SSI0 + RXFLR), 1)

    def test_config_locked_while_enabled(self):
        """Negative: CTRLR0, CTRLR1, BAUDR, TXFTLR and RXFTLR are refused while SSIENR is 1
        ("when 1 disable configuration register", p.256)."""
        m = self.start()
        ssi_on(m)
        for off, reset in ((CTRLR0, 0x7), (CTRLR1, 0), (BAUDR, 2), (TXFTLR, 0), (RXFTLR, 0)):
            m.writel(SSI0 + off, 0x55)
            self.assertEqual(m.readl(SSI0 + off), reset, hex(off))

    def test_chip_not_selected_reads_ff(self):
        """Negative: with GPIOA 18 high the data chip is not selected; RDID returns 0xFF."""
        m = self.start()
        ssi_on(m)
        cs(m, False)
        xfer(m, SSI0, RDID)
        self.assertEqual(xfer(m, SSI0, 0xFF), 0xFF)

    def test_cs_release_ends_command(self):
        """Releasing GPIOA 18 ends the command: after a RDID is cut short, the next byte is taken
        as a new command, not as RDID data."""
        m = self.start()
        ssi_on(m)
        xfer(m, SSI0, RDID)
        end_command(m, SSI0)
        self.assertEqual(xfer(m, SSI0, 0xFF), 0xFF)     # 0xff is an unknown command now
        self.assertEqual(xfer(m, SSI0, RDID), 0xFF)
        self.assertEqual(xfer(m, SSI0, 0xFF), 0xEF)

    def test_other_gpio_writes_do_not_end_command(self):
        """Negative: only GPIOA 18 selects the data chip; writing another GPIO bank's data
        register does not end the command."""
        m = self.start()
        ssi_on(m)
        xfer(m, SSI0, RDID)
        m.writel(0x14100000, 0)                         # GPIOB SWPORTA_DR
        self.assertEqual(xfer(m, SSI0, 0xFF), 0xEF)


class SsiFifoTest(DM32TestCase):
    def test_tx_fifo_depth_and_overflow(self):
        """The TX FIFO holds 128 frames (p.244); a write to a full FIFO is lost and sets TXO,
        which TXOICR reads and clears."""
        m = self.start()
        ssi_on(m)
        m.writel(SSI0 + SER, 0)                         # hold the frames in the FIFO
        for _ in range(130):
            m.writel(SSI0 + DR, 0)
        self.assertEqual(m.readl(SSI0 + TXFLR), 128)
        self.assertFalse(m.readl(SSI0 + SR) & SR_TFNF)
        self.assertTrue(m.readl(SSI0 + RISR) & 0x2)
        self.assertEqual(m.readl(SSI0 + TXOICR), 1)
        self.assertFalse(m.readl(SSI0 + RISR) & 0x2)

    def test_rx_fifo_overflow(self):
        """129 frames with no read: the RX FIFO stops at 128 (RFF) and RXO is set."""
        m = self.start()
        ssi_on(m)
        for _ in range(129):
            m.writel(SSI0 + DR, 0)
        m.clock_step(100 * US)
        self.assertEqual(m.readl(SSI0 + RXFLR), 128)
        self.assertTrue(m.readl(SSI0 + SR) & SR_RFF)
        self.assertTrue(m.readl(SSI0 + RISR) & 0x8)
        self.assertEqual(m.readl(SSI0 + RXOICR), 1)

    def test_negative_rx_underflow(self):
        """Negative: reading an empty RX FIFO returns 0xFF and sets RXU; ICR clears it."""
        m = self.start()
        ssi_on(m)
        self.assertEqual(m.readl(SSI0 + DR), 0xFF)
        self.assertTrue(m.readl(SSI0 + RISR) & 0x4)
        self.assertEqual(m.readl(SSI0 + ICR), 1)
        self.assertFalse(m.readl(SSI0 + RISR) & 0x4)

    def test_ssienr_zero_flushes(self):
        """SSIENR 0 empties both FIFOs and clears the interrupt status (p.256)."""
        m = self.start()
        ssi_on(m)
        xfer(m, SSI0, 0)
        m.readl(SSI0 + DR)                              # RXU
        m.writel(SSI0 + DR, 0)
        m.clock_step(1 * US)
        m.writel(SSI0 + ENABLE, 0)
        self.assertEqual(m.readl(SSI0 + RXFLR), 0)
        self.assertEqual(m.readl(SSI0 + RISR), 0)


class SsiInterruptTest(DM32TestCase):
    def test_rxf_threshold_and_pic16(self):
        """RXF is set while the RX FIFO holds more than RXFTLR frames (p.249); with IMR enabling
        only RXF, PIC source 16 follows ISR (Table 30)."""
        m = self.start()
        m.irq_intercept_in(m.pic_path())
        pads_muxed(m)
        m.writel(SSI0 + BAUDR, 2)
        m.writel(SSI0 + RXFTLR, 1)
        m.writel(SSI0 + IMR, 0x10)
        m.writel(SSI0 + ENABLE, 1)
        m.writel(SSI0 + SER, 1)
        cs(m, True)
        m.writel(SSI0 + DR, 0)
        m.writel(SSI0 + DR, 0)
        m.clock_step(250)
        self.assertEqual(m.readl(SSI0 + ISR), 0)
        self.assertFalse(m.irq_level.get(16, False))
        m.clock_step(1 * US)
        self.assertEqual(m.readl(SSI0 + ISR), 0x10)
        self.assertTrue(m.irq_level.get(16))
        m.readl(SSI0 + DR)
        self.assertFalse(m.irq_level.get(16))

    def test_txe_follows_threshold(self):
        """TXE is set while the TX FIFO holds no more than TXFTLR frames and the block is
        enabled; IMR 0 keeps it out of ISR."""
        m = self.start()
        ssi_on(m)
        self.assertTrue(m.readl(SSI0 + RISR) & 0x1)
        self.assertEqual(m.readl(SSI0 + ISR) & 0x1, 0x1)
        m.writel(SSI0 + IMR, 0)
        self.assertEqual(m.readl(SSI0 + ISR), 0)


class SsiTransferModeTest(DM32TestCase):
    def rdid_then(self, m, ctrlr0, ndf, word):
        """Send RDID in transmit-and-receive mode, then switch mode (the chip stays selected)
        and start a reception with one DR write."""
        ssi_on(m)
        xfer(m, SSI0, RDID)
        m.writel(SSI0 + ENABLE, 0)
        m.writel(SSI0 + CTRLR0, ctrlr0)
        m.writel(SSI0 + CTRLR1, ndf)
        m.writel(SSI0 + ENABLE, 1)
        m.writel(SSI0 + SER, 1)
        m.writel(SSI0 + DR, word)
        m.clock_step(10 * US)

    def test_receive_only(self):
        """TMOD 2 (p.250): one DR write starts NDF + 1 received frames: the JEDEC ID."""
        m = self.start()
        self.rdid_then(m, 0x207, 2, 0xFF)
        self.assertEqual([m.readl(SSI0 + DR) for _ in range(3)], [0xEF, 0x40, 0x18])
        self.assertEqual(m.readl(SSI0 + RXFLR), 0)

    def test_eeprom_read(self):
        """TMOD 3 (p.250): the TX FIFO is sent with nothing received, then NDF + 1 frames are
        received: RDID sent this way returns the three ID bytes only."""
        m = self.start()
        ssi_on(m)
        m.writel(SSI0 + ENABLE, 0)
        m.writel(SSI0 + CTRLR0, 0x307)
        m.writel(SSI0 + CTRLR1, 2)
        m.writel(SSI0 + ENABLE, 1)
        m.writel(SSI0 + SER, 1)
        m.writel(SSI0 + DR, RDID)
        m.clock_step(10 * US)
        self.assertEqual([m.readl(SSI0 + DR) for _ in range(3)], [0xEF, 0x40, 0x18])
        self.assertEqual(m.readl(SSI0 + RXFLR), 0)

    def test_negative_transmit_only(self):
        """Negative: TMOD 1 drops every received frame."""
        m = self.start()
        ssi_on(m)
        m.writel(SSI0 + ENABLE, 0)
        m.writel(SSI0 + CTRLR0, 0x107)
        m.writel(SSI0 + ENABLE, 1)
        m.writel(SSI0 + SER, 1)
        for b in (RDID, 0, 0):
            m.writel(SSI0 + DR, b)
        m.clock_step(10 * US)
        self.assertEqual(m.readl(SSI0 + RXFLR), 0)


class SpiNorDataTest(DM32TestCase):
    def test_rdid(self):
        """RDID 0x9f returns the JEDEC ID EF 40 18: Winbond, SPI-NOR, 128 Mbit (dm32_ssi.c
        header; W25Q128JV datasheet section 8.2.27)."""
        m = self.start()
        ssi_on(m)
        self.assertEqual(xfer(m, SSI0, RDID), 0xFF)
        self.assertEqual([xfer(m, SSI0, 0xFF) for _ in range(3)], [0xEF, 0x40, 0x18])
        end_command(m, SSI0)

    def test_erased_store_reads_ff(self):
        """With no DM32_SPINOR file, every byte of the 16 MB store reads 0xff, including the
        top of the chip (dm32_ssi.c header, "Backing store")."""
        m = self.start()
        ssi_on(m)
        self.assertEqual(read_bytes(m, SSI0, 0x123456, 8), [0xFF] * 8)
        self.assertEqual(read_bytes(m, SSI0, NOR_SIZE - 4, 4), [0xFF] * 4)

    def test_wel_latch_and_rdsr(self):
        """RDSR bit 1 (WEL) is 0 at reset, set by WREN and cleared by WRDI (dm32_ssi.c header,
        SPI-NOR commands)."""
        m = self.start()
        ssi_on(m)
        self.assertEqual(rdsr(m), 0x00)
        wren(m)
        self.assertEqual(rdsr(m), 0x02)
        xfer(m, SSI0, WRDI)
        end_command(m, SSI0)
        self.assertEqual(rdsr(m), 0x00)

    def test_program_completes_and_wip_polls(self):
        """A completed PROGRAM clears WEL, and the next two RDSR reads report WIP=1, then
        WIP=0 (dm32_ssi.c, rdsr_latch and DM32_SPINOR_WIP_READS default 2; devices.md: "the
        poll completes on the third read")."""
        m = self.start()
        ssi_on(m)
        wren(m)
        program(m, SSI0, 0x2000, [0x5A])
        self.assertEqual([rdsr(m) for _ in range(3)], [0x01, 0x01, 0x00])

    def test_program_clears_bits_only(self):
        """PAGE PROGRAM ANDs the data into the store: programming clears bits and cannot set
        them, so a stored 0x00 stays 0x00 (dm32_ssi.c header, "Programming clears bits only")."""
        d = self.make_tempdir()
        path = os.path.join(d, "store.bin")
        with open(path, "wb") as f:
            f.write(bytes([0x0F, 0xF0, 0xAA, 0x00]))
        m = self.start(env={"DM32_SPINOR": path})
        ssi_on(m)
        wren(m)
        program(m, SSI0, 0x0, [0xF3, 0x0F, 0x55, 0xFF])
        self.assertEqual(read_bytes(m, SSI0, 0x0, 4), [0x03, 0x00, 0x00, 0x00])

    def test_sector_erase_is_aligned(self):
        """SECTOR ERASE 0x20 erases the aligned 4 KB block that holds its address (dm32_ssi.c,
        nor_erase: a &= ~(len-1)). An address at 0x2345 clears 0x2000-0x2FFF only."""
        d = self.make_tempdir()
        path = os.path.join(d, "store.bin")
        with open(path, "wb") as f:
            f.write(pattern(0x4000))
        m = self.start(env={"DM32_SPINOR": path})
        ssi_on(m)
        wren(m)
        erase(m, SSI0, SECTOR_ERASE, 0x2345)
        self.assertEqual(read_bytes(m, SSI0, 0x1FFF, 1), [pattern(0x4000)[0x1FFF]])
        self.assertEqual(read_bytes(m, SSI0, 0x2000, 2), [0xFF, 0xFF])
        self.assertEqual(read_bytes(m, SSI0, 0x2FFF, 1), [0xFF])
        self.assertEqual(read_bytes(m, SSI0, 0x3000, 1), [pattern(0x4000)[0x3000]])

    def test_block_erase_is_aligned(self):
        """BLOCK ERASE 0xd8 erases the aligned 64 KB block (dm32_ssi.c, command table)."""
        d = self.make_tempdir()
        path = os.path.join(d, "store.bin")
        data = pattern(0x30000)
        with open(path, "wb") as f:
            f.write(data)
        m = self.start(env={"DM32_SPINOR": path})
        ssi_on(m)
        wren(m)
        erase(m, SSI0, BLOCK_ERASE, 0x012345)
        self.assertEqual(read_bytes(m, SSI0, 0xFFFF, 1), [data[0xFFFF]])
        self.assertEqual(read_bytes(m, SSI0, 0x10000, 1), [0xFF])
        self.assertEqual(read_bytes(m, SSI0, 0x1FFFF, 1), [0xFF])
        self.assertEqual(read_bytes(m, SSI0, 0x20000, 1), [data[0x20000]])

    def test_addresses_above_1mb_reach_the_16mb_store(self):
        """A 24-bit address above 1 MB reaches the 16 MB W25Q128 store and does not alias
        onto the low megabyte (dm32_ssi.c, NOR_SIZE comment)."""
        m = self.start()
        ssi_on(m)
        wren(m)
        program(m, SSI0, 0x800123, [0x5A])
        self.assertEqual(read_bytes(m, SSI0, 0x800123, 1), [0x5A])
        self.assertEqual(read_bytes(m, SSI0, 0x000123, 1), [0xFF])

    def test_read_starts_at_first_byte(self):
        """READ 0x03 streams from the addressed byte: the preloaded byte is not skipped, and
        the fourth transfer's read (in the address phase) is discarded (dm32_ssi.c, case 3
        comment on first_data_pending)."""
        d = self.make_tempdir()
        path = os.path.join(d, "store.bin")
        data = pattern(0x200)
        with open(path, "wb") as f:
            f.write(data)
        m = self.start(env={"DM32_SPINOR": path})
        ssi_on(m)
        self.assertEqual(read_bytes(m, SSI0, 0x100, 16), list(data[0x100:0x110]))

    def test_preloaded_file_and_short_file_fill(self):
        """DM32_SPINOR loads over the 0xff store: bytes past the end of a short file read 0xff
        (dm32_ssi.c, "Backing store"; environment.md, DM32_SPINOR)."""
        d = self.make_tempdir()
        path = os.path.join(d, "store.bin")
        with open(path, "wb") as f:
            f.write(bytes(range(64)))
        m = self.start(env={"DM32_SPINOR": path})
        ssi_on(m)
        self.assertEqual(read_bytes(m, SSI0, 0x10, 4), [0x10, 0x11, 0x12, 0x13])
        self.assertEqual(read_bytes(m, SSI0, 0x4000, 2), [0xFF, 0xFF])

    def test_missing_preload_file_is_ignored(self):
        """A DM32_SPINOR path that does not exist leaves the store erased (environment.md,
        DM32_SPINOR: a file that does not exist is ignored)."""
        m = self.start(env={"DM32_SPINOR": "/nonexistent/dm32-spinor-missing.bin"})
        ssi_on(m)
        self.assertEqual(read_bytes(m, SSI0, 0x0, 2), [0xFF, 0xFF])


class SpiNorNegativeTest(DM32TestCase):
    def test_program_without_wren_is_dropped(self):
        """Negative: PAGE PROGRAM without WREN changes nothing. Each dropped byte is counted as
        nor_wel (dm32_ssi.c, nor_program; devices.md, "Permissive events")."""
        m = self.start()
        ssi_on(m)
        program(m, SSI0, 0x1000, [0x00, 0x11])
        self.assertEqual(read_bytes(m, SSI0, 0x1000, 2), [0xFF, 0xFF])
        self.assertEqual(m.events()["nor_wel"], 2)

    def test_erase_without_wren_is_dropped(self):
        """Negative: SECTOR ERASE without WREN leaves the sector unchanged and is counted as
        nor_wel (dm32_ssi.c, nor_erase)."""
        d = self.make_tempdir()
        path = os.path.join(d, "store.bin")
        data = pattern(0x2000)
        with open(path, "wb") as f:
            f.write(data)
        m = self.start(env={"DM32_SPINOR": path})
        ssi_on(m)
        erase(m, SSI0, SECTOR_ERASE, 0x1000)
        self.assertEqual(read_bytes(m, SSI0, 0x1000, 1), [data[0x1000]])
        self.assertEqual(m.events()["nor_wel"], 1)

    def test_wel_cleared_by_program_and_wrdi(self):
        """Negative: the latch is cleared when a PROGRAM completes, and by WRDI. A second
        program without a fresh WREN is dropped (dm32_ssi.c, spi_cs_reset and WRDI)."""
        m = self.start()
        ssi_on(m)
        wren(m)
        program(m, SSI0, 0x3000, [0x7E])
        program(m, SSI0, 0x3000, [0x00])
        wren(m)
        xfer(m, SSI0, WRDI)
        end_command(m, SSI0)
        program(m, SSI0, 0x3001, [0x00])
        self.assertEqual(read_bytes(m, SSI0, 0x3000, 2), [0x7E, 0xFF])
        self.assertEqual(m.events()["nor_wel"], 2)


class SpiNorPersistenceTest(DM32TestCase):
    def tmpdir(self):
        d = self.make_tempdir()
        return d

    def test_out_written_at_exit(self):
        """DM32_SPINOR_OUT: at exit, a store that a program changed is written to that path,
        16 MB long, with the programmed byte and the erased rest (environment.md,
        DM32_SPINOR_OUT; dm32_ssi.c, spinor_dump_all)."""
        out = os.path.join(self.tmpdir(), "out.bin")
        m = self.start(env={"DM32_SPINOR_OUT": out})
        ssi_on(m)
        wren(m)
        program(m, SSI0, 0x4000, [0x12])
        m.close()
        self.assertTrue(os.path.exists(out))
        with open(out, "rb") as f:
            img = f.read()
        self.assertEqual(len(img), NOR_SIZE)
        self.assertEqual(img[0x4000], 0x12)
        self.assertEqual(img[0x4001], 0xFF)

    def test_sync_writes_after_program(self):
        """DM32_SPINOR_SYNC=1 writes DM32_SPINOR_OUT after each completed program, before the
        machine exits (environment.md, DM32_SPINOR_SYNC; dm32_ssi.c, spinor_sync_point)."""
        out = os.path.join(self.tmpdir(), "sync.bin")
        m = self.start(env={"DM32_SPINOR_OUT": out, "DM32_SPINOR_SYNC": "1"})
        ssi_on(m)
        wren(m)
        program(m, SSI0, 0x5000, [0x34])
        self.assertTrue(os.path.exists(out))
        m.close()
        with open(out, "rb") as f:
            self.assertEqual(f.read(0x5001)[0x5000], 0x34)

    def test_no_write_no_output(self):
        """Negative: a run whose SPI-NOR is only read writes no output file, and says so at
        exit (environment.md, DM32_SPINOR_OUT; dm32_ssi.c, spinor_dump_all)."""
        out = os.path.join(self.tmpdir(), "none.bin")
        m = self.start(env={"DM32_SPINOR_OUT": out})
        ssi_on(m)
        read_bytes(m, SSI0, 0x0, 4)
        text = m.close()
        self.assertFalse(os.path.exists(out))
        self.assertIn("no SPI-NOR writes happened, nothing written", text)

    def test_wip_reads_zero(self):
        """DM32_SPINOR_WIP_READS=0: a completed program never reports WIP (environment.md)."""
        m = self.start(env={"DM32_SPINOR_WIP_READS": "0"})
        ssi_on(m)
        wren(m)
        program(m, SSI0, 0x6000, [0x01])
        self.assertEqual([rdsr(m) for _ in range(2)], [0x00, 0x00])

    def test_wip_reads_invalid_rejected(self):
        """Negative: a DM32_SPINOR_WIP_READS value that is not a count 0..1000 stops QEMU at
        start-up with an error naming the variable (environment.md, DM32_SPINOR_WIP_READS)."""
        for bad in ("many", "1001", "-1"):
            rc, out = run_plain(env={"DM32_SPINOR_WIP_READS": bad}, args=["-accel", "qtest"])
            self.assertEqual(rc, 1, bad)
            self.assertIn("DM32_SPINOR_WIP_READS", out)

if __name__ == "__main__":
    unittest.main()
