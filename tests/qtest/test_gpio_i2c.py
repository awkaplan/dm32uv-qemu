# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# Copyright (c) 2026 Alex Kaplan
"""Bit-banged I2C on GPIOA pads (hw/gpio/dm32_gpio.c, dm32_gpio_i2c_create; docs/devices.md
"GPIO"). The board creates bus "alpu" on GPIOA 5 (SCL) / 6 (SDA) and bus "fm" on GPIOA 26
(SCL) / 25 (SDA) (dm32uv-re specs/dts/dm32uv.dts). The tests put QEMU's AT24C EEPROM on a bus
and drive it the way the stock bit-bang routines do, through SWPORTA_DR/DDR and EXT_PORTA."""

import unittest

from dm32qtest import DM32TestCase, BitBang, DDR, EEPROM_ADDR, EXT


class GpioI2cTest(DM32TestCase):
    def machine(self, bus):
        return self.start(args=["-device",
                                f"at24c-eeprom,bus={bus},address={EEPROM_ADDR:#x},rom-size=256"])

    def test_alpu_bus_eeprom_round_trip(self):
        """A write then a random read through GPIOA 5/6 reaches the EEPROM on bus "alpu"."""
        m = self.machine("alpu")
        bb = BitBang(m, scl=5, sda=6)
        self.assertTrue(bb.eeprom_write(0x10, b"\xa5\x5a\x01"))
        self.assertEqual(bb.eeprom_read(0x10, 3), b"\xa5\x5a\x01")

    def test_fm_bus_eeprom_round_trip(self):
        """Bus "fm" is on GPIOA 26 (SCL) / 25 (SDA)."""
        m = self.machine("fm")
        bb = BitBang(m, scl=26, sda=25)
        self.assertTrue(bb.eeprom_write(0x20, b"\x42"))
        self.assertEqual(bb.eeprom_read(0x20, 1), b"\x42")

    def test_negative_pads_given_to_spi1(self):
        """Negative: GPIOA 25/26 are SPI1's MOSI/SCLK pads. With IO_DIPLEX1 [16:13] = 0 they belong
        to the SPI1 master, so the GPIO levels never reach the bus (pad_conflict events) and a
        write through them never reaches the EEPROM on "fm": with the pads back on GPIO, the byte
        reads its initial 0 (hw/gpio/dm32_gpio.c: a pin reaches its pad only while the pad
        selects GPIO)."""
        m = self.machine("fm")
        bb = BitBang(m, scl=26, sda=25)
        m.writel(0x11000038, 0xFFFFFFFF & ~(0xF << 13))
        bb.eeprom_write(0x30, b"\x42")
        m.writel(0x11000038, 0xFFFFFFFF)
        self.assertEqual(bb.eeprom_read(0x30, 1), b"\x00")
        self.assertGreater(m.events()["pad_conflict"], 0)

    def test_buses_are_separate(self):
        """Negative: the EEPROM on "fm" does not answer on the ALPU pads."""
        m = self.machine("fm")
        bb = BitBang(m, scl=5, sda=6)
        bb.start()
        self.assertFalse(bb.write_byte(EEPROM_ADDR << 1))
        bb.stop()

    def test_empty_bus_naks(self):
        """Negative: with no slave (the board default) every address NAKs: SDA stays high."""
        m = self.start()
        bb = BitBang(m, scl=5, sda=6)
        bb.start()
        self.assertFalse(bb.write_byte(0x7A))
        bb.stop()

    def test_ack_wins_over_driven_high(self):
        """Wired-AND: during the ACK slot the slave's low wins even while the SoC still drives
        SDA high, as the stock RDA5807M driver does (its ACK slot drives SDA high and never
        samples, rda5807m ACK slot @0x030654d8)."""
        m = self.machine("alpu")
        bb = BitBang(m, scl=5, sda=6)
        bb.start()
        self.assertTrue(bb.write_byte(EEPROM_ADDR << 1, release_for_ack=False))
        bb.stop()

    def test_released_sda_reads_high(self):
        """With no transfer, a released SDA reads its idle high level (bus pull-up)."""
        m = self.start()
        m.writel(DDR, 0)
        self.assertEqual(m.readl(EXT) & (1 << 6), 1 << 6)


if __name__ == "__main__":
    unittest.main()
