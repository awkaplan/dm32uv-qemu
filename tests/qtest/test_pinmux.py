# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# Copyright (c) 2026 Alex Kaplan
"""Pad control in the system registers (hw/misc/dm32_sysreg.c, "Pad control"; HR_C7000 v2.7
sections 4.4.4.4-4.4.4.12; SVD SystemControl IO_DIPLEX0-2). The pad API itself is internal, so
these tests check the reset values and the per-pad decisions each block makes through it: SSI per
instance, ADC, LCD, GPIO (pad_conflict, input enables), I2C0/1, PWM and UART1-3 (pad_unclaimed)."""

import unittest

from dm32qtest import DM32TestCase, ABRT_SRC, I2C0, i2c_read, setup

SYS = 0x11000000
IO_DIPLEX0, IO_DIPLEX1, IO_DIPLEX2 = SYS + 0x34, SYS + 0x38, SYS + 0x3C
REN0, REN1, REN2, IE0, IE1, IE2 = (SYS + o for o in (0x40, 0x44, 0x48, 0x4C, 0x50, 0x54))

SSI0 = 0x140A0000
SSI_ENABLE, SSI_SER, SSI_BAUDR, SSI_DR = 0x08, 0x10, 0x14, 0x60

ADC = 0x140D0000
ADC_START, ADC_CH_VLD, ADC_DATA_AB = ADC + 0x14, ADC + 0x24, ADC + 0x30

LCD = 0x12000000


def field(lsb, width, value):
    return (value & ((1 << width) - 1)) << lsb


class ResetTest(DM32TestCase):
    def test_io_diplex0_reset(self):
        """IO_DIPLEX0 resets to 0x800fffe0 (v2.7 EN 4.4.4.4; SVD IO_DIPLEX0): the JTAG selects
        [4:0] read 0 (JTAG) and the I2C, UART, PWM and PTB7 selects their GPIO values."""
        m = self.start()
        self.assertEqual(m.readl(IO_DIPLEX0), 0x800FFFE0)

    def test_iomgr_reset(self):
        """IOMGR_REN_REG0-2 and IOMGR_IE_IE0-2 reset with every defined bit 1: all pulls off,
        all inputs on. Bit 31 of REN_REG2 and IE_IE2 is reserved (v2.7 EN 4.4.4.7-4.4.4.12)."""
        m = self.start()
        for reg in (REN0, REN1, IE0, IE1):
            self.assertEqual(m.readl(reg), 0xFFFFFFFF, hex(reg))
        for reg in (REN2, IE2):
            self.assertEqual(m.readl(reg), 0x7FFFFFFF, hex(reg))

    def test_iomgr_storage(self):
        """The pull and input-enable registers read back what is written."""
        m = self.start()
        for reg in (REN0, REN1, REN2, IE0, IE1, IE2):
            m.writel(reg, 0x12345678)
            self.assertEqual(m.readl(reg), 0x12345678, hex(reg))


class Spi0PadTest(DM32TestCase):
    """SPI0 transfers only while PTA20-22 (spi0_sclk/mosi/miso_sel, IO_DIPLEX1 [8:3]) all select
    the SPI0 master, value 0 (v2.7 EN 4.4.4.5)."""

    def transfer(self, diplex1):
        """One frame at SCKDV 2; a frame on pads that do not select SPI0 is lost."""
        m = self.start()
        m.writel(IO_DIPLEX1, diplex1)
        m.writel(SSI0 + SSI_BAUDR, 2)
        m.writel(SSI0 + SSI_ENABLE, 1)
        m.writel(SSI0 + SSI_SER, 1)
        m.writel(SSI0 + SSI_DR, 0x9F)
        m.clock_step(1000)
        m.readl(SSI0 + SSI_DR)
        return m.events()["ssi_discard"]

    def test_all_master_transfers(self):
        """All three pads on the master: the byte moves."""
        self.assertEqual(self.transfer(0xFFFFFFFF & ~field(3, 6, 0x3F)), 0)

    def test_one_pad_on_slave(self):
        """Negative: MISO on the SPI0 slave (select 1) is enough to drop the byte."""
        diplex1 = (0xFFFFFFFF & ~field(3, 6, 0x3F)) | field(7, 2, 1)
        self.assertEqual(self.transfer(diplex1), 1)

    def test_other_fields_do_not_matter(self):
        """CSN0 (PTA18) and the SPI1 fields are not part of the decision (the model uses GPIOA 18
        as the data chip's select)."""
        diplex1 = 0xFFFFFFFF & ~field(3, 6, 0x3F)
        diplex1 &= ~field(0, 2, 3) & ~field(9, 8, 0xFF)
        self.assertEqual(self.transfer(diplex1), 0)


class AdcPadTest(DM32TestCase):
    """ADC channel n converts only while PTC(15 + n) selects ADCn_IN, value 0 (IO_DIPLEX2
    adcN_in_sel, v2.7 EN 4.4.4.6)."""

    def convert_ch2(self, diplex2):
        m = self.start(env={"DM32_ADC_CH2": "0x155"})
        m.writel(IO_DIPLEX2, diplex2)
        m.writel(ADC_CH_VLD, 1 << 2)
        m.writel(ADC_START, 1)
        data = m.readl(ADC_DATA_AB + 4) & 0x3FF
        return data, m.events()["adc_pad"]

    def test_own_pad_claimed(self):
        """Channel 2 with only its own select (bit 21) cleared converts its input."""
        self.assertEqual(self.convert_ch2(0x3FF80100 & ~(1 << 21)), (0x155, 0))

    def test_neighbour_pad_claimed(self):
        """Negative: claiming channel 3's pad (bit 22) does not connect channel 2."""
        self.assertEqual(self.convert_ch2(0x3FF80100 & ~(1 << 22)), (0, 1))


class LcdPadTest(DM32TestCase):
    """LCD data bytes reach the panel only while DB0-DB4 (PTC7-11, IO_DIPLEX2 [15:9]) select the
    LCD, value 0 (v2.7 EN 4.4.4.6; docs/devices.md "LCD")."""

    def data_write(self, diplex2):
        m = self.start()
        m.writel(LCD + 0x28, 1)
        m.writel(IO_DIPLEX2, diplex2)
        m.writeb(LCD + 0x04, 0x00)
        return m.events()["lcd_mux"]

    def test_bus_on_lcd(self):
        """With every DB select 0 the byte goes to the panel."""
        self.assertEqual(self.data_write(0x3FF80100), 0)

    def test_one_db_pad_lent(self):
        """Negative: DB2 alone (PTC9, bit 13) lent to GPIO forces the byte (lcd_mux)."""
        self.assertEqual(self.data_write(0x3FF80100 | (1 << 13)), 1)

    def test_db0_on_spi2(self):
        """Negative: DB0 on the SPI2 master (lcd_db0_sel = 1) is also away from the LCD."""
        self.assertEqual(self.data_write(0x3FF80100 | field(9, 2, 1)), 1)

    def test_other_lcd_pads_do_not_matter(self):
        """LCD_RD (bit 8) and DB5-DB7 (bits 16-18) are not part of the keypad-sharing check."""
        self.assertEqual(self.data_write(0x3FF80100 | 0x70100), 0)


GPIOA = 0x14020000
GPIO_DR, GPIO_DDR, GPIO_EXT = 0x00, 0x04, 0x50
DIPLEX0_RESET, DIPLEX1_RESET, DIPLEX2_RESET = 0x800FFFE0, 0xFFFFFFFF, 0x3FFFFFFB


class Diplex2ResetTest(DM32TestCase):
    def test_io_diplex2_reset(self):
        """IO_DIPLEX2 resets to 0x3FFFFFFB (v2.7 EN 4.4.4.6; SVD): every muxed pad on GPIO, bit 2
        reserved. Stock's LCD claim (&= 0xFFF80107) and CLK_OUT select later give the 0x3FF80100
        read on hardware."""
        m = self.start()
        self.assertEqual(m.readl(IO_DIPLEX2), DIPLEX2_RESET)
        m.writel(IO_DIPLEX2, DIPLEX2_RESET & 0xFFF80107 & ~0x3)
        self.assertEqual(m.readl(IO_DIPLEX2), 0x3FF80100)


class GpioPadTest(DM32TestCase):
    """GPIO consults the pad mux: a pin reaches its pad only while the pad selects GPIO, and a
    pad whose input buffer is off reads 0 (hw/gpio/dm32_gpio.c)."""

    def test_conflict_when_gpio_drives_peripheral_pad(self):
        """Negative: with PTA15 given to PWM_0 (IO_DIPLEX0 bit 17 = 0), making GPIOA 15 an
        output and toggling it are two blocks claiming one pad: two pad_conflict events."""
        m = self.start()
        m.writel(IO_DIPLEX0, DIPLEX0_RESET & ~(1 << 17))
        m.writel(GPIOA + GPIO_DDR, 1 << 15)
        m.writel(GPIOA + GPIO_DR, 1 << 15)
        self.assertEqual(m.events()["pad_conflict"], 2)

    def test_no_conflict_on_gpio_pad(self):
        """The same writes with PTA15 on GPIO (the reset value) are no conflict."""
        m = self.start()
        m.writel(GPIOA + GPIO_DDR, 1 << 15)
        m.writel(GPIOA + GPIO_DR, 1 << 15)
        self.assertEqual(m.events()["pad_conflict"], 0)

    def test_input_disabled_reads_zero(self):
        """IOMGR_IE_IE0 bit 9 is PTA9's input enable: clearing it makes GPIOA bit 9 read 0,
        where an idle input reads 1 (v2.7 EN 4.4.4.10)."""
        m = self.start()
        bit = 1 << 9
        self.assertEqual(m.readl(GPIOA + GPIO_EXT) & bit, bit)
        m.writel(IE0, 0xFFFFFFFF & ~bit)
        self.assertEqual(m.readl(GPIOA + GPIO_EXT) & bit, 0)


class SsiInstancePadTest(DM32TestCase):
    """Each SSI instance is gated on its own pads (dm32_ssi.c ssi_pads_muxed)."""

    def discards(self, base, diplex1=DIPLEX1_RESET, diplex2=DIPLEX2_RESET):
        m = self.start()
        m.writel(IO_DIPLEX1, diplex1)
        m.writel(IO_DIPLEX2, diplex2)
        m.writel(base + SSI_BAUDR, 2)
        m.writel(base + SSI_ENABLE, 1)
        m.writel(base + SSI_SER, 1)
        m.writel(base + SSI_DR, 0x9F)
        m.clock_step(1000)
        m.readl(base + SSI_DR)
        return m.events()["ssi_discard"]

    def test_spi1_needs_its_own_pads(self):
        """Negative: SPI1 (0x140B0000) does not transfer on SPI0's pads; it needs PTA25-27 on
        its master (IO_DIPLEX1 [18:13] = 0)."""
        self.assertEqual(self.discards(0x140B0000, DIPLEX1_RESET & ~field(3, 6, 0x3F)), 1)
        self.assertEqual(self.discards(0x140B0000, DIPLEX1_RESET & ~field(13, 6, 0x3F)), 0)

    def test_spi2_on_lcd_pads(self):
        """SPI2 (0x140E0000) uses the LCD pads PTC4, PTC8 and PTC7 at select 1 (IO_DIPLEX2
        lcd_rs/db1/db0_sel); at reset (GPIO) it drops the byte."""
        self.assertEqual(self.discards(0x140E0000), 1)
        spi2 = (DIPLEX2_RESET & ~(field(5, 2, 3) | field(9, 4, 0xF))) | \
            field(5, 2, 1) | field(9, 2, 1) | field(11, 2, 1)
        self.assertEqual(self.discards(0x140E0000, diplex2=spi2), 0)

    def test_dedicated_instances_not_gated(self):
        """SPI3 ("EFUSE only") and the program flash's controller at 0x04000000 have no select
        field, so they transfer with every pad at reset."""
        self.assertEqual(self.discards(0x14150000), 0)
        self.assertEqual(self.discards(0x04000000), 0)


class BlockPadTest(DM32TestCase):
    """Blocks that transfer without their pads raise pad_unclaimed."""

    def test_i2c0_without_pads_naks(self):
        """I2C0 with PTA5/6 on GPIO (reset): the address goes out on no pad, the transfer aborts
        with ABRT_7B_ADDR_NOACK and counts pad_unclaimed. With the pads, the empty bus NAKs the
        same way but no event is counted."""
        for diplex0, events in ((DIPLEX0_RESET, 1), (DIPLEX0_RESET & ~0x60, 0)):
            m = self.start()
            m.writel(IO_DIPLEX0, diplex0)
            setup(m, I2C0)
            i2c_read(m, 0x50, 0, 1, base=I2C0)
            self.assertEqual(m.readl(I2C0 + ABRT_SRC), 1)
            self.assertEqual(m.events()["pad_unclaimed"], events, hex(diplex0))

    def test_pwm_start_needs_pad(self):
        """Starting PWM channel 0 with PTA15 on GPIO counts pad_unclaimed; with IO_DIPLEX0 bit
        17 clear (PWM_0) it does not."""
        for diplex0, events in ((DIPLEX0_RESET, 1), (DIPLEX0_RESET & ~(1 << 17), 0)):
            m = self.start()
            m.writel(IO_DIPLEX0, diplex0)
            m.writel(0x140C0008, 100)              # PER
            m.writel(0x140C0000, 0x21)             # CFG: continuous, start
            self.assertEqual(m.events()["pad_unclaimed"], events, hex(diplex0))

    def test_uart1_tx_needs_pad(self):
        """UART1 sends a byte only while PTA10 is UART1_TXD (IO_DIPLEX0 bit 10 = 0): at reset
        the byte is dropped and counted; with the pad it reaches the chardev."""
        import os
        d = self.make_tempdir()
        for diplex0, events, byte in ((DIPLEX0_RESET, 1, b""),
                                      (DIPLEX0_RESET & ~(1 << 10), 0, b"U")):
            path = os.path.join(d, "uart1-%x" % diplex0)
            m = self.start(args=["-serial", "file:" + path])
            m.writel(IO_DIPLEX0, diplex0)
            m.writel(0x14040000, ord("U"))
            self.assertEqual(m.events()["pad_unclaimed"], events, hex(diplex0))
            with open(path, "rb") as f:
                self.assertEqual(f.read(), byte)

    def test_uart_divisor_latch_not_gated(self):
        """Negative: with LCR.DLAB set, offset 0 is the divisor latch, which is forwarded, not a
        transmitted byte: no event."""
        m = self.start()
        m.writel(0x1404000C, 0x83)
        m.writel(0x14040000, 13)
        self.assertEqual(m.events()["pad_unclaimed"], 0)


if __name__ == "__main__":
    unittest.main()
