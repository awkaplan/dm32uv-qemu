# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# Copyright (c) 2026 Alex Kaplan
"""RDA5807M FM tuner on the bit-banged "fm" bus, GPIOA 26 (SCL) / 25 (SDA), 7-bit 0x10
(hw/misc/dm32_rda5807m.c; RDA5807M datasheet v1.0 sections 2.5 and 6; dm32uv-re
specs/rdl/rda5807m.rdl). Every transfer here is the stock driver's (0x03065444-0x030658eb):
push-pull SDA, driven high in the ACK slot, so the tuner's ACK shows only through the wired-AND."""

import unittest

from dm32qtest import BitBang, DM32TestCase

MS = 1_000_000

# Stock's init table at flash 0x030b790c: 61 words written from register 02H.
INIT_TABLE = [
    0xC001, 0x001A, 0x0400, 0x86AF, 0x4000, 0x56C6, 0, 0, 0, 0, 0, 0, 0, 0,
    0x0006, 0x0019, 0x2A11, 0x002E, 0x2A30, 0xB83C, 0x9000, 0x2A91, 0x8412, 0x00A8, 0xC400,
    0xE000, 0x301D, 0x816A, 0x4608, 0x0086, 0x0661, 0x0000, 0x109E, 0x24C9, 0x0408, 0x0608,
    0xE105, 0x3B6C, 0x2BEC, 0x090F, 0x3414, 0x1450, 0x096D, 0x2D96, 0x01DA, 0x2A7B, 0x0821,
    0x13D5, 0x4891, 0x00BC, 0x0896, 0x153C, 0x0B80, 0x25C7, 0x0000, 0x3C58, 0x2A50, 0x262C,
    0x0608, 0x0200, 0x0000,
]

BAND10_SPACE50 = 0x0A          # 03H: BAND 10 (76-108 MHz), SPACE 10 (50 kHz), as stock tunes
TUNE = 0x10


def chan(khz):
    return (khz - 76000) // 50


class Rda:
    """The stock driver's transfers."""

    def __init__(self, m):
        self.bb = BitBang(m, scl=26, sda=25)

    def write(self, words):
        """START, 0x20, then each word high byte first from 02H, STOP. Returns the ACKs."""
        bb = self.bb
        bb.start()
        acks = [bb.write_byte(0x20, release_for_ack=False)]
        for w in words:
            acks.append(bb.write_byte(w >> 8, release_for_ack=False))
            acks.append(bb.write_byte(w & 0xFF, release_for_ack=False))
        bb.stop()
        return acks

    def read(self, n):
        """START, 0x21, n words from 0AH, ACK every byte but the last, STOP."""
        bb = self.bb
        bb.start()
        bb.write_byte(0x21, release_for_ack=False)
        data = [bb.read_byte(ack=i < 2 * n - 1) for i in range(2 * n)]
        bb.stop()
        return [data[i] << 8 | data[i + 1] for i in range(0, 2 * n, 2)]

    def regs(self):
        """Every register, read from 0AH round the 3AH wrap: a dict register -> value."""
        words = self.read(0x3B)
        return {(0x0A + i) % 0x3B: w for i, w in enumerate(words)}

    def tune(self, khz):
        """Stock's tune (0x030656ec): 02H = 0xC001, 03H = CHAN, BAND 10, SPACE 50 kHz, TUNE."""
        self.write([0xC001, chan(khz) << 6 | BAND10_SPACE50 | TUNE])

    def status(self):
        """Stock's status read (0x03065638): register 0AH only."""
        return self.read(1)[0]


class RdaBusTest(DM32TestCase):
    def start(self, *a, **kw):
        m = super().start(*a, **kw)
        self.rda = Rda(m)
        return m

    def test_address_acked(self):
        """The tuner ACKs its address (0x20) and every data byte (section 2.5)."""
        self.start()
        self.assertTrue(all(self.rda.write([0xC001, 0x001A])))

    def test_negative_no_random_access_address(self):
        """Negative: 0x11 is not a second address; the datasheet has no random-access mode."""
        self.start()
        bb = self.rda.bb
        bb.start()
        self.assertFalse(bb.write_byte(0x22, release_for_ack=False))
        bb.stop()

    def test_reset_values(self):
        """Section 6 reset values: CHIPID 0x58xx, 04H 0x0200, 05H 0x880F, 07H 0x4202, the RDS
        blocks 0x5803/5804/5808/5804; status reads 0 while ENABLE is 0."""
        self.start()
        r = self.rda.regs()
        self.assertEqual(r[0x00] >> 8, 0x58)
        self.assertEqual((r[0x04], r[0x05], r[0x07]), (0x0200, 0x880F, 0x4202))
        self.assertEqual([r[i] for i in range(0x0C, 0x10)], [0x5803, 0x5804, 0x5808, 0x5804])
        self.assertEqual((r[0x0A], r[0x0B]), (0, 0))

    def test_stock_init_table_wraps_at_3a(self):
        """Stock's 61-word init writes 02H onward. The status and RDS registers ignore writes,
        the hidden registers 10H-3AH stay read-only (06H OPEN_MODE is 10, not 11), and the
        counter wraps after 3AH (section 2.5), so the last four words land on 00H (read-only),
        01H, 02H and 03H."""
        self.start()
        self.assertTrue(all(self.rda.write(INIT_TABLE)))
        r = self.rda.regs()
        self.assertEqual(r[0x00] >> 8, 0x58)
        self.assertEqual(r[0x01], 0x0608)
        self.assertEqual((r[0x02], r[0x03]), (0x0200, 0x0000))
        self.assertEqual([r[i] for i in range(0x04, 0x08)], [0x0400, 0x86AF, 0x4000, 0x56C6])
        self.assertEqual([r[i] for i in range(0x10, 0x3B)], [0] * 0x2B)

    def test_open_mode_unlocks_hidden_registers(self):
        """With 06H OPEN_MODE = 11 the hidden registers take writes."""
        self.start()
        words = [0x8000, 0, 0x0200, 0x880F, 0x6000, 0x4202] + [0] * 8 + [0x1234]
        self.rda.write(words)
        self.assertEqual(self.rda.regs()[0x10], 0x1234)

    def test_soft_reset(self):
        """A 0 -> 1 edge of SOFT_RESET (02H bit 1) restores the reset values (section 2.4)."""
        self.start()
        self.rda.write([0xC001, 0x001A, 0x0400, 0x86AF])
        self.rda.write([0xC003])
        r = self.rda.regs()
        self.assertEqual((r[0x02], r[0x05]), (0x0000, 0x880F))


class RdaTuneTest(DM32TestCase):
    def start(self, *a, **kw):
        env = {"DM32_FM_STATIONS": "94500:40,101100:70", "DM32_FM_RSSI": "5"}
        env.update(kw.pop("env", {}))
        m = super().start(*a, env=env, **kw)
        self.rda = Rda(m)
        return m

    def test_tune_sets_stc_and_station_status(self):
        """Stock's tune to 101.1 MHz: TUNE clears and STC sets after the tune time (50 ms,
        assumption); READCHAN is the channel, the station gives FM_TRUE, ST and its RSSI."""
        m = self.start()
        self.rda.tune(101100)
        self.assertFalse(self.rda.status() & 0x4000)
        m.clock_step(60 * MS)
        r = self.rda.regs()
        self.assertEqual(r[0x0A] & 0x4000, 0x4000)              # STC
        self.assertEqual(r[0x0A] & 0x3FF, chan(101100))         # READCHAN
        self.assertEqual(r[0x0A] & 0x0400, 0x0400)              # ST
        self.assertEqual(r[0x0B] >> 9, 70)                      # RSSI
        self.assertEqual(r[0x0B] & 0x0180, 0x0180)              # FM_TRUE, FM_READY
        self.assertEqual(r[0x03] & TUNE, 0)

    def test_off_station(self):
        """Off a station: no FM_TRUE or ST, and RSSI is DM32_FM_RSSI."""
        m = self.start()
        self.rda.tune(88000)
        m.clock_step(60 * MS)
        r = self.rda.regs()
        self.assertEqual(r[0x0A] & 0x0400, 0)
        self.assertEqual(r[0x0B] >> 8, 5 << 1)

    def test_mono_clears_st(self):
        """MONO (02H bit 13) forces mono: no ST on a station."""
        m = self.start()
        self.rda.write([0xE001, chan(101100) << 6 | BAND10_SPACE50 | TUNE])
        m.clock_step(60 * MS)
        self.assertEqual(self.rda.status() & 0x0400, 0)

    def test_stock_seek_up(self):
        """Stock's seek (0x03065860): 02H = 0xC301, 03H = the current channel. The seek steps
        10 ms per channel (assumption) to the next station; stock's poll (0x03065bb8) accepts
        (status >> 8) & 0x60 == 0x40 and reads the frequency as READCHAN * 5 + 7600 (10 kHz)."""
        m = self.start()
        self.rda.write([0xC301, chan(76000) << 6 | BAND10_SPACE50])
        m.clock_step(3000 * MS)
        self.assertEqual((self.rda.status() >> 8) & 0x60, 0)
        m.clock_step(1000 * MS)
        st = self.rda.status()
        self.assertEqual((st >> 8) & 0x60, 0x40)
        self.assertEqual((st & 0x3FF) * 5 + 7600, 9450)
        self.assertEqual(self.rda.regs()[0x02] & 0x0100, 0)      # SEEK cleared

    def test_seek_down_wraps(self):
        """SKMODE 0 wraps at the band edge: seeking down from 80 MHz finds 101.1 MHz."""
        m = self.start()
        self.rda.write([0xC101, chan(80000) << 6 | BAND10_SPACE50])
        m.clock_step(6000 * MS)
        st = self.rda.status()
        self.assertEqual((st >> 8) & 0x60, 0x40)
        self.assertEqual(st & 0x3FF, chan(101100))

    def test_negative_seek_stops_at_limit(self):
        """Negative: SKMODE 1 stops at the band limit; no station above sets SF."""
        m = self.start()
        self.rda.write([0xC381, chan(107900) << 6 | BAND10_SPACE50])
        m.clock_step(100 * MS)
        self.assertEqual((self.rda.status() >> 8) & 0x60, 0x60)  # STC, SF

    def test_negative_seek_without_stations_fails(self):
        """Negative: with no station the seek goes round the band and sets SF."""
        m = self.start(env={"DM32_FM_STATIONS": ""})
        self.rda.write([0xC301, chan(100000) << 6 | BAND10_SPACE50])
        m.clock_step(7000 * MS)
        self.assertEqual((self.rda.status() >> 8) & 0x60, 0x60)

    def test_power_down_cancels(self):
        """Stock's power-down (0x030657c0) writes 02H = 0x8000: ENABLE 0 cancels a seek and the
        status reads 0."""
        m = self.start()
        self.rda.write([0xC301, chan(76000) << 6 | BAND10_SPACE50])
        m.clock_step(100 * MS)
        self.rda.write([0x8000])
        m.clock_step(5000 * MS)
        self.assertEqual(self.rda.status(), 0)

    def test_negative_tune_while_disabled(self):
        """Negative: TUNE with ENABLE 0 does nothing."""
        m = self.start()
        self.rda.write([0x8000, chan(101100) << 6 | BAND10_SPACE50 | TUNE])
        m.clock_step(100 * MS)
        self.assertEqual(self.rda.regs()[0x03] & TUNE, TUNE)


class BusNakTest(DM32TestCase):
    def test_master_clocking_after_nak_counted(self):
        """A master that keeps clocking after a slave's NAK (no STOP) is an i2c_nak event: on the
        empty ALPU bus, address 0x3d NAKs and another byte follows."""
        m = self.start()
        bb = BitBang(m, scl=5, sda=6)
        bb.start()
        bb.write_byte(0x7A)
        bb.write_byte(0x00)
        bb.stop()
        self.assertEqual(m.events()["i2c_nak"], 1)

    def test_negative_master_nack_not_counted(self):
        """Negative: the master's own NACK at the end of a read, then STOP, is not counted."""
        m = self.start()
        rda = Rda(m)
        rda.read(1)
        self.assertEqual(m.events()["i2c_nak"], 0)


if __name__ == "__main__":
    unittest.main()
