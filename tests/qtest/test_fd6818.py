# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# Copyright (c) 2026 Alex Kaplan
"""FD6818B RF transceiver (hw/misc/dm32_fd6818.c; docs/devices.md, "FD6818"; docs/environment.md,
"FD6818"). The chip is reached only by bit-banging GPIO bank A (hw/gpio/dm32_gpio.c): CLK = bit 7,
DATA = bit 8, CS = bit 24, active low. Frame: R/W (bit 23, 1 = read), address (bits 22-16),
data (bits 15-0), MSB first, sampled on the CLK rising edge. Register addresses, fields and reset
values are the FD6818 programming guide's (REV 1.0.0, "guide", printed page numbers); where the
populated FD6818B differs (dm32uv-re specs/rdl/fd6818.rdl) the test says so."""

import os
import tempfile
import unittest

from dm32qtest import DM32TestCase, run_plain

GPIOA = 0x14020000
GPIOB = 0x14100000
US = 1_000
MS = 1_000_000
RXON, TXON = 0xBFF1, 0xC1FE                # stock's REG_30H words
DR, DDR, EXT = 0x00, 0x04, 0x50          # SWPORTA_DR, SWPORTA_DDR, EXT_PORTA
CLK, DAT, CS = 1 << 7, 1 << 8, 1 << 24   # CS is active low: bit 24 clear selects the chip


def pins(asserted, clk, dat):
    """DR value for one bus state. CS is asserted (low) when `asserted` is true."""
    return (0 if asserted else CS) | (CLK if clk else 0) | (DAT if dat else 0)


def clock_out(m, base, bits, asserted=True):
    """Clock `bits` onto DATA, one rising edge per bit, MSB first. Each bit is set while CLK is
    low, raised, then lowered, so the chip samples it on the rising edge."""
    for b in bits:
        m.writel(base + DR, pins(asserted, 0, b))
        m.writel(base + DR, pins(asserted, 1, b))
        m.writel(base + DR, pins(asserted, 0, b))


def write_reg(m, addr, value, base=GPIOA):
    """One 24-bit write frame: R/W = 0, the 7-bit address and the 16-bit value."""
    frame = (addr << 16) | (value & 0xFFFF)
    m.writel(base + DDR, DAT)                  # the SoC drives DATA
    m.writel(base + DR, pins(False, 0, 0))
    clock_out(m, base, [(frame >> i) & 1 for i in range(23, -1, -1)])
    m.writel(base + DR, pins(False, 0, 0))     # CS deasserted: the transfer is over


def read_reg(m, addr, base=GPIOA):
    """One read frame. The master clocks R/W = 1 and the address, then releases DATA. The chip
    drives 16 bits on the next 16 rising edges, and each is sampled at EXT_PORTA bit 8 while CLK
    is high (fd6818.c: bit 15 is sampled after the first of these edges)."""
    m.writel(base + DDR, DAT)
    m.writel(base + DR, pins(False, 0, 0))
    clock_out(m, base, [1] + [(addr >> i) & 1 for i in range(6, -1, -1)])
    m.writel(base + DDR, 0)                    # release DATA: the chip drives it from here
    value = 0
    for _ in range(16):
        m.writel(base + DR, pins(True, 1, 0))  # rising edge
        value = (value << 1) | ((m.readl(base + EXT) >> 8) & 1)
        m.writel(base + DR, pins(True, 0, 0))  # CLK low
    m.writel(base + DR, pins(False, 0, 0))
    return value


def rx_on(m):
    """Stock's RX enable (REG_30H = 0xBFF1), then wait out the PLL lock (40 us)."""
    write_reg(m, 0x30, RXON)
    m.clock_step(100 * US)


def ctc_word(hz, slot=0):
    """REG_07H: [13] slot, [12:0] = freq * 2^27 / 6500000 (guide p.43)."""
    return slot << 13 | round(hz * (1 << 27) / 6500000)


def dcs_word(code):
    """The 23-bit Golay word of an octal DCS code: data [11:0] = code | 100 in bits 11:9,
    parity [22:12] from generator 0xC75 (dm32_fd6818.c header)."""
    data = (code & 0x1FF) | 0x800
    rem = data << 11
    for i in range(22, 10, -1):
        if rem & (1 << i):
            rem ^= 0xC75 << (i - 11)
    return data | (rem & 0x7FF) << 12


class RfEnv:
    """A DM32_RF_ENV script in a temporary file."""

    def __init__(self, test, text):
        fd, self.path = tempfile.mkstemp(suffix=".rfenv")
        with os.fdopen(fd, "w") as f:
            f.write(text)
        test.addCleanup(os.unlink, self.path)

class Fd6818RegisterTest(DM32TestCase):
    def test_chip_id_default(self):
        """REG_00H reads 0x4819 by default. The populated radio reports 0x4819; the guide's value
        is 0x6818 (dm32_fd6818.c header; docs/devices.md, REG_00H)."""
        m = self.start()
        self.assertEqual(read_reg(m, 0x00), 0x4819)

    def test_chip_id_override(self):
        """DM32_FD6818_ID sets REG_00H at reset (docs/environment.md): 0x6818 selects the
        guide's value."""
        m = self.start(env={"DM32_FD6818_ID": "0x6818"})
        self.assertEqual(read_reg(m, 0x00), 0x6818)

    def test_reset_values(self):
        """Reset values (guide p.56-72): REG_03H 0x0000, REG_30H 0, REG_38H 0x3A98 and REG_39H
        0x0271 (409.75 MHz), REG_78H 0x44FF, REG_4FH 0x7E34. REG_28H reads 0x6B3F: the guide's
        [5:0] = 0x3F under the live gpio_in_val 0x6B, the populated chip's value."""
        m = self.start()
        for addr, value in ((0x03, 0), (0x30, 0), (0x38, 0x3A98), (0x39, 0x0271),
                            (0x78, 0x44FF), (0x4F, 0x7E34), (0x28, 0x6B3F)):
            self.assertEqual(read_reg(m, addr), value, f"REG_{addr:02X}H")

    def test_rw_round_trip(self):
        """Read-write registers return what was written, full 16-bit data, MSB first. REG_28H
        keeps [7:0] only: [15:8] is gpio_in_val, read-only (guide p.60)."""
        m = self.start()
        for addr, value in ((0x30, 0xBFF1), (0x39, 0xFFFF), (0x3F, 0xA5A5)):
            write_reg(m, addr, value)
            self.assertEqual(read_reg(m, addr), value, f"REG_{addr:02X}H")
        write_reg(m, 0x28, 0x8001)
        self.assertEqual(read_reg(m, 0x28), 0x6B01)

    def test_status_reset_value(self):
        """REG_0CH at reset: PLL off and no signal, so only weak_rssi [7] is set (guide p.58);
        the populated chip reads 0x0080 while unlocked."""
        m = self.start()
        self.assertEqual(read_reg(m, 0x0C), 0x0080)

    def test_reg7f_reads_zero(self):
        """REG_7FH is the page command, not a register, and reads 0 on either page (guide p.15;
        a real radio reads 0)."""
        m = self.start()
        write_reg(m, 0x7F, 0x0001)
        self.assertEqual(read_reg(m, 0x7F), 0)

    def test_readonly_writes_dropped(self):
        """Negative: writes to read-only registers are dropped: REG_0CH, REG_67H, and REG_00H
        without bit 15 (which keeps its ID)."""
        m = self.start()
        write_reg(m, 0x00, 0x1234)
        self.assertEqual(read_reg(m, 0x00), 0x4819)
        write_reg(m, 0x0C, 0xFFFF)
        self.assertEqual(read_reg(m, 0x0C), 0x0080)
        write_reg(m, 0x67, 0x01FF)
        self.assertEqual(read_reg(m, 0x67), 0)


class Fd6818SoftResetTest(DM32TestCase):
    def test_soft_reset_restores_defaults(self):
        """WRITE(0x00, 0x8000) then WRITE(0x00, 0x0000) (guide p.16) restores the reset values,
        page 1 included, and switches the PLL off."""
        m = self.start()
        rx_on(m)
        write_reg(m, 0x38, 0x1234)
        write_reg(m, 0x78, 0x0101)
        write_reg(m, 0x7F, 1)
        write_reg(m, 0x10, 0x1234)                         # REG_90H
        write_reg(m, 0x7F, 0)
        write_reg(m, 0x00, 0x8000)
        write_reg(m, 0x00, 0x0000)
        self.assertEqual(read_reg(m, 0x38), 0x3A98)
        self.assertEqual(read_reg(m, 0x78), 0x44FF)
        self.assertEqual(read_reg(m, 0x30), 0)
        self.assertEqual(read_reg(m, 0x0C), 0x0080)
        self.assertEqual(read_reg(m, 0x00), 0x4819)
        write_reg(m, 0x7F, 1)
        self.assertEqual(read_reg(m, 0x10), 0x8C20)

    def test_negative_page1_address_0_is_not_reset(self):
        """Negative: on page 1, wire address 0x00 is REG_80H, so 0x8000 there resets nothing."""
        m = self.start()
        write_reg(m, 0x38, 0x1234)
        write_reg(m, 0x7F, 1)
        write_reg(m, 0x00, 0x8000)
        write_reg(m, 0x7F, 0)
        self.assertEqual(read_reg(m, 0x38), 0x1234)

    def test_negative_write_without_bit15(self):
        """Negative: a REG_00H write with bit 15 clear resets nothing."""
        m = self.start()
        write_reg(m, 0x38, 0x1234)
        write_reg(m, 0x00, 0x0000)
        self.assertEqual(read_reg(m, 0x38), 0x1234)


class Fd6818PagingTest(DM32TestCase):
    def test_page1_registers(self):
        """After WRITE(0x7F, 1) wire address 0x10 is REG_90H (reset 0x8C20) and 0x36 is REG_B6H
        (0x9D08); WRITE(0x7F, 0) returns to page 0 (guide p.15, p.69-72)."""
        m = self.start()
        write_reg(m, 0x7F, 1)
        self.assertEqual(read_reg(m, 0x10), 0x8C20)
        self.assertEqual(read_reg(m, 0x36), 0x9D08)
        write_reg(m, 0x10, 0x1234)
        self.assertEqual(read_reg(m, 0x10), 0x1234)
        write_reg(m, 0x7F, 0)
        self.assertEqual(read_reg(m, 0x10), 0x0000)        # REG_10H, untouched

    def test_negative_page0_write_does_not_reach_page1(self):
        """Negative: on page 0, wire address 0x10 is REG_10H; REG_90H keeps its reset value."""
        m = self.start()
        write_reg(m, 0x10, 0x5555)
        write_reg(m, 0x7F, 1)
        self.assertEqual(read_reg(m, 0x10), 0x8C20)

    def test_page1_readonly(self):
        """Negative: REG_87H (page 1, cmpd_db_out) is read-only and reads 0: no audio modelled."""
        m = self.start()
        write_reg(m, 0x7F, 1)
        write_reg(m, 0x07, 0x007F)
        self.assertEqual(read_reg(m, 0x07), 0)


class Fd6818PllTest(DM32TestCase):
    def test_lock_takes_time(self):
        """pll_lock [9] sets 40 us after RX is enabled, not at once. 0x0280 is what a real radio
        reads locked with no signal."""
        m = self.start()
        write_reg(m, 0x30, RXON)
        self.assertEqual(read_reg(m, 0x0C), 0x0080)
        m.clock_step(50 * US)
        self.assertEqual(read_reg(m, 0x0C), 0x0280)

    def test_tx_enable_locks(self):
        """Stock's TX word 0xC1FE (txon, pll_en 11111) also runs the PLL."""
        m = self.start()
        write_reg(m, 0x30, TXON)
        m.clock_step(50 * US)
        self.assertEqual(read_reg(m, 0x0C) & 0x0200, 0x0200)

    def test_retune_low_then_high(self):
        """Stock's order, REG_38H then REG_39H, relocks 40 us after the REG_39H write."""
        m = self.start()
        rx_on(m)
        write_reg(m, 0x38, 0xC2C0)
        self.assertEqual(read_reg(m, 0x0C) & 0x0200, 0)
        write_reg(m, 0x39, 0x028F)
        self.assertEqual(read_reg(m, 0x0C) & 0x0200, 0)
        m.clock_step(50 * US)
        self.assertEqual(read_reg(m, 0x0C) & 0x0200, 0x0200)

    def test_negative_high_then_low_stays_unlocked(self):
        """Negative: REG_39H then REG_38H leaves the PLL unlocked (hardware read 0x0080)."""
        m = self.start()
        rx_on(m)
        write_reg(m, 0x39, 0x028F)
        write_reg(m, 0x38, 0xC2C0)
        m.clock_step(1 * MS)
        self.assertEqual(read_reg(m, 0x0C), 0x0080)

    def test_negative_pll_off(self):
        """Negative: rxon without pll_en 11111 does not run the PLL."""
        m = self.start()
        write_reg(m, 0x30, 0x0001)
        m.clock_step(1 * MS)
        self.assertEqual(read_reg(m, 0x0C) & 0x0200, 0)

    def test_lock_time_setting(self):
        """DM32_FD6818_LOCK_US sets the lock time."""
        m = self.start(env={"DM32_FD6818_LOCK_US": "500"})
        write_reg(m, 0x30, RXON)
        m.clock_step(400 * US)
        self.assertEqual(read_reg(m, 0x0C) & 0x0200, 0)
        m.clock_step(200 * US)
        self.assertEqual(read_reg(m, 0x0C) & 0x0200, 0x0200)


class Fd6818RssiStandInTest(DM32TestCase):
    """Without DM32_RF_ENV, REG_62H keeps its deterministic jitter stand-in."""

    def test_rssi_pinned(self):
        """DM32_FD6818_RSSI pins REG_62H bits 15:8 and disables the jitter; the low byte is 0."""
        m = self.start(env={"DM32_FD6818_RSSI": "0x5A"})
        for _ in range(3):
            self.assertEqual(read_reg(m, 0x62), 0x5A00)

    def test_rssi_default_jitter(self):
        """Default REG_62H is base 0x30 with 12 dB peak-to-peak jitter, changing between reads."""
        m = self.start()
        values = [read_reg(m, 0x62) for _ in range(16)]
        for v in values:
            self.assertTrue(0x2A00 <= v <= 0x3500, hex(v))
            self.assertEqual(v & 0xFF, 0)
        self.assertGreater(len(set(values)), 1)

    def test_rssi_jitter_zero(self):
        """DM32_FD6818_RSSI_JITTER=0 turns the jitter off: REG_62H reads the base 0x30."""
        m = self.start(env={"DM32_FD6818_RSSI_JITTER": "0"})
        for _ in range(3):
            self.assertEqual(read_reg(m, 0x62), 0x3000)

    def test_empty_band(self):
        """Without an environment the receiver sees the noise floor (-127 dBm): rssi_sq 66,
        no SNR, noise_sq 60, weak_rssi set, squelch closed."""
        m = self.start()
        rx_on(m)
        self.assertEqual(read_reg(m, 0x67), 66)
        self.assertEqual(read_reg(m, 0x65), 60)
        self.assertEqual(read_reg(m, 0x0C), 0x0280)


class Fd6818RfTest(DM32TestCase):
    """The receiver against a DM32_RF_ENV script (docs/environment.md, "FD6818")."""

    def run_env(self, text, env=None):
        e = {"DM32_RF_ENV": RfEnv(self, text).path}
        e.update(env or {})
        m = self.start(env=e)
        rx_on(m)
        return m

    def test_measurements(self):
        """A -84 dBm carrier on 409.75 MHz: rssi_sq = 2 * (P + 160) = 152 (guide p.47), agc_rssi
        and lna_peak_rssi 76, snr_out and rssi_rel 43 (over -127 dBm), noise_sq 60 - 43 = 17."""
        m = self.run_env("0 409750000 -84 fm\n")
        self.assertEqual(read_reg(m, 0x67), 152)
        self.assertEqual(read_reg(m, 0x62), 76 << 8 | 76)
        self.assertEqual(read_reg(m, 0x61), 43 << 8)
        self.assertEqual(read_reg(m, 0x65), 43 << 8 | 17)
        self.assertEqual(read_reg(m, 0x0C) & 0x0080, 0)          # not weak

    def test_wideband_tap(self):
        """A strong carrier 1 MHz away raises lna_peak_rssi (wideband) but not agc_rssi."""
        m = self.run_env("0 409750000 -84 fm\n0 410750000 -50\n")
        self.assertEqual(read_reg(m, 0x62), 76 << 8 | 110)

    def test_negative_off_channel(self):
        """Negative: a carrier 7 kHz off the tuned frequency is outside the channel."""
        m = self.run_env("0 409757000 -50 fm\n")
        self.assertEqual(read_reg(m, 0x67), 66)
        self.assertEqual(read_reg(m, 0x61), 0)

    def test_negative_receiver_off(self):
        """Negative: with rxon clear nothing is measured and the squelch stays closed."""
        m = self.run_env("0 * -40 fm\n")
        write_reg(m, 0x30, 0)
        self.assertEqual(read_reg(m, 0x67), 0)
        self.assertEqual(read_reg(m, 0x0C) & 0x0002, 0)

    def test_floor_and_wildcard(self):
        """'floor' sets the noise floor; '*' is always on the tuned frequency."""
        m = self.run_env("floor -110\n0 * -80 dmr\n")
        self.assertEqual(read_reg(m, 0x61), 30 << 8)
        self.assertEqual(read_reg(m, 0x67), 160)

    def test_timeline(self):
        """Entries apply at their virtual time; 'off' removes the carrier."""
        m = self.run_env("10 * -60 fm\n20 * off\n")
        self.assertEqual(read_reg(m, 0x67), 66)
        m.clock_step(10 * MS)
        self.assertEqual(read_reg(m, 0x67), 200)
        m.clock_step(10 * MS)
        self.assertEqual(read_reg(m, 0x67), 66)

    def test_squelch_hysteresis(self):
        """SQ opens above rssi_sq_th_in and closes below rssi_sq_th_out (REG_78H, 0.5 dB per
        step, guide p.47): in 150 (-85 dB), out 144 (-88 dB). -84 opens, -87 holds it open, -89
        closes it, -87 keeps it closed."""
        m = self.run_env("0 * -84 fm\n1 * -87 fm\n2 * -89 fm\n3 * -87 fm\n")
        write_reg(m, 0x78, 150 << 8 | 144)
        sq = []
        for _ in range(4):
            sq.append(read_reg(m, 0x0C) & 0x0002)
            m.clock_step(1 * MS)
        self.assertEqual(sq, [2, 2, 0, 0])

    def test_negative_noisy_signal_does_not_open(self):
        """Negative: a carrier 7 dB over the floor has noise_sq 53, above noise_sq_th_in 52
        (REG_4FH reset), so SQ stays closed even with a zero RSSI threshold."""
        m = self.run_env("0 * -120 fm\n")
        write_reg(m, 0x78, 0x0000)
        self.assertEqual(read_reg(m, 0x0C) & 0x0002, 0)

    def test_stock_squelch_setting(self):
        """Stock's blank-codeplug squelch (REG_78H 0xFFFE, REG_4FH 0x302F) opens on a -20 dBm
        carrier (rssi_sq 280) and not on a -40 dBm one (240), which is not weak either."""
        m = self.run_env("0 * -40 fm\n1 * -20 fm\n")
        write_reg(m, 0x78, 0xFFFE)
        write_reg(m, 0x4F, 0x302F)
        self.assertEqual(read_reg(m, 0x0C), 0x0200)
        m.clock_step(1 * MS)
        self.assertEqual(read_reg(m, 0x0C), 0x0202)

    def test_bad_script_exits(self):
        """Negative: a malformed script stops the board with the line number."""
        bad = RfEnv(self, "0 * loud\n")
        rc, out = run_plain(env={"DM32_RF_ENV": bad.path}, args=["-accel", "qtest"])
        self.assertNotEqual(rc, 0)
        self.assertIn(":1: bad level", out)


class Fd6818SubAudioTest(DM32TestCase):
    """CTCSS and CDCSS detection (guide p.42-45), REG_60H."""

    def run_env(self, text):
        m = self.start(env={"DM32_RF_ENV": RfEnv(self, text).path})
        rx_on(m)
        return m

    def test_ctcss_slots(self):
        """With subau_en and ctc_dcs_sel (REG_51H 0x9000), a carrier with 88.5 Hz is detected in
        the slot whose REG_07H word matches: slot 0 -> REG_60H bit 0, slot 1 -> bit 1."""
        m = self.run_env("0 * -60 fm ctcss=88.5\n")
        write_reg(m, 0x51, 0x9000)
        write_reg(m, 0x07, ctc_word(88.5, 0))
        write_reg(m, 0x07, ctc_word(100.0, 1))
        self.assertEqual(read_reg(m, 0x60), 0x0001)
        write_reg(m, 0x07, ctc_word(88.5, 1))
        self.assertEqual(read_reg(m, 0x60), 0x0003)

    def test_ctcss_threshold_hysteresis(self):
        """REG_52H reset 0x028F: ctc_th_in 10 and ctc_th_out 15, in 0.1 Hz, against the 88.48 Hz
        the word for 88.5 Hz gives. 88.5 Hz detects; 89.55 Hz (1.07 Hz off) holds; 90.1 Hz
        (1.62 Hz off) drops it; 89.55 Hz then stays undetected."""
        m = self.run_env("0 * -60 fm ctcss=88.5\n1 * -60 fm ctcss=89.55\n"
                         "2 * -60 fm ctcss=90.1\n3 * -60 fm ctcss=89.55\n")
        write_reg(m, 0x51, 0x9000)
        write_reg(m, 0x07, ctc_word(88.5))
        det = []
        for _ in range(4):
            det.append(read_reg(m, 0x60))
            m.clock_step(1 * MS)
        self.assertEqual(det, [1, 1, 0, 0])

    def test_negative_ctcss_needs_subau_en(self):
        """Negative: with subau_en clear (REG_51H reset 0x1050) nothing is detected."""
        m = self.run_env("0 * -60 fm ctcss=88.5\n")
        write_reg(m, 0x07, ctc_word(88.5))
        self.assertEqual(read_reg(m, 0x60), 0)

    def test_negative_ctcss_weak_carrier(self):
        """Negative: a carrier 5 dB over the floor is too weak to decode."""
        m = self.run_env("0 * -122 fm ctcss=88.5\n")
        write_reg(m, 0x51, 0x9000)
        write_reg(m, 0x07, ctc_word(88.5))
        self.assertEqual(read_reg(m, 0x60), 0)

    def write_dcs(self, m, word):
        write_reg(m, 0x08, word & 0xFFF)
        write_reg(m, 0x08, 0x8000 | (word >> 12) & 0xFFF)

    def test_cdcss_positive_and_negative(self):
        """CDCSS (REG_51H 0x8000: subau_en, ctc_dcs_sel 0), REG_08H holding code 023's word:
        a carrier with 023 sets REG_60H bit 10, one with 023 inverted sets bit 11."""
        m = self.run_env("0 * -60 fm dcs=023\n1 * -60 fm dcs=023i\n")
        write_reg(m, 0x51, 0x8000)
        self.write_dcs(m, dcs_word(0o23))
        self.assertEqual(read_reg(m, 0x60), 0x0400)
        m.clock_step(1 * MS)
        self.assertEqual(read_reg(m, 0x60), 0x0800)

    def test_cdcss_rotation(self):
        """The configured word matches any cyclic rotation of the received one."""
        m = self.run_env("0 * -60 fm dcs=754\n")
        write_reg(m, 0x51, 0x8000)
        w = dcs_word(0o754)
        self.write_dcs(m, ((w << 5) | (w >> 18)) & 0x7FFFFF)
        self.assertEqual(read_reg(m, 0x60), 0x0400)

    def test_negative_cdcss_other_code(self):
        """Negative: a different code is not detected."""
        m = self.run_env("0 * -60 fm dcs=025\n")
        write_reg(m, 0x51, 0x8000)
        self.write_dcs(m, dcs_word(0o23))
        self.assertEqual(read_reg(m, 0x60), 0)


class Fd6818InterruptTest(DM32TestCase):
    """REG_02H vector, REG_3FH mask and int_out (guide p.50-52)."""

    def test_sq_interrupt(self):
        """With irq_mask bit 0, SQ opening latches REG_02H = 0x0010 (SQ received, source 0) and
        sets int_out; any write to REG_02H clears int_out and leaves the vector."""
        m = self.start(env={"DM32_RF_ENV": RfEnv(self, "1 * -60 fm\n").path})
        write_reg(m, 0x78, 0x0000)
        write_reg(m, 0x3F, 0x0001)
        rx_on(m)
        self.assertEqual(read_reg(m, 0x0C) & 0x0003, 0)
        m.clock_step(1 * MS)
        self.assertEqual(read_reg(m, 0x0C) & 0x0003, 0x0003)
        write_reg(m, 0x02, 0)
        self.assertEqual(read_reg(m, 0x0C) & 0x0003, 0x0002)
        self.assertEqual(read_reg(m, 0x02), 0x0010)

    def test_negative_masked(self):
        """Negative: with irq_mask clear the same event raises nothing."""
        m = self.start(env={"DM32_RF_ENV": RfEnv(self, "1 * -60 fm\n").path})
        write_reg(m, 0x78, 0x0000)
        rx_on(m)
        m.clock_step(1 * MS)
        self.assertEqual(read_reg(m, 0x0C) & 0x0001, 0)
        self.assertEqual(read_reg(m, 0x02), 0)

    def test_pll_lock_lost(self):
        """With irq_mask bit 7, a REG_38H write while locked is a PLL lock lost interrupt
        (source 7, guide p.51)."""
        m = self.start()
        write_reg(m, 0x3F, 0x0080)
        rx_on(m)
        write_reg(m, 0x38, 0x1111)
        self.assertEqual(read_reg(m, 0x02), 0x0007)
        self.assertEqual(read_reg(m, 0x0C) & 0x0001, 1)


class Fd6818GpioTest(DM32TestCase):
    """GPIO0/GPIO1 on GPIOB 27/26 and REG_28H gpio_in_val (guide p.53-54)."""

    def pad(self, m, bit):
        return (m.readl(GPIOB + EXT) >> bit) & 1

    def test_undriven_reads_board(self):
        """At reset no pin is driven: GPIOB 27/26 read high (no pull, idle high) and
        gpio_in_val is 0x6B, the populated chip's value."""
        m = self.start()
        self.assertEqual((self.pad(m, 27), self.pad(m, 26)), (1, 1))
        self.assertEqual(read_reg(m, 0x28) >> 8, 0x6B)

    def test_gpio0_sq_output(self):
        """gpio_oen_b[0] = 0 with gpio0_out_sel = 2 puts SQ on GPIO0: GPIOB 27 low while the
        squelch is closed, high when it opens; gpio_in_val bit 0 follows."""
        m = self.start(env={"DM32_RF_ENV": RfEnv(self, "1 * -60 fm\n").path})
        write_reg(m, 0x78, 0x0000)
        write_reg(m, 0x26, 0x0002)
        write_reg(m, 0x25, 0xFE00)
        rx_on(m)
        self.assertEqual(self.pad(m, 27), 0)
        self.assertEqual(read_reg(m, 0x28) >> 8 & 1, 0)
        m.clock_step(1 * MS)
        self.assertEqual(self.pad(m, 27), 1)
        self.assertEqual(read_reg(m, 0x28) >> 8 & 1, 1)

    def test_gpio1_out_val(self):
        """GPIO1 with gpio1_out_sel 0 drives gpio_out_val[1] onto GPIOB 26."""
        m = self.start()
        write_reg(m, 0x25, 0xFD00)
        self.assertEqual(self.pad(m, 26), 0)
        write_reg(m, 0x25, 0xFD02)
        self.assertEqual(self.pad(m, 26), 1)

    def test_input_reads_soc_pad(self):
        """An undriven GPIO0 reads the GPIOB 27 pad: the SoC driving it low clears bit 0."""
        m = self.start()
        m.writel(GPIOB + DR, 0)
        m.writel(GPIOB + DDR, 1 << 27)
        self.assertEqual(read_reg(m, 0x28) >> 8, 0x6A)

    def test_negative_undriven_pin_ignores_function(self):
        """Negative: a function selected on a pin whose output is disabled drives nothing."""
        m = self.start()
        write_reg(m, 0x25, 0xFF00)
        write_reg(m, 0x26, 0x0000)
        self.assertEqual(self.pad(m, 26), 1)


class Fd6818BusTest(DM32TestCase):
    def test_write_frame_is_24_bits_msb_first(self):
        """A 24-bit write frame is clocked MSB first on the rising edge (docs/devices.md, "Frame").
        Writing REG_38H and REG_39H with a value whose bit 15 is set confirms the order."""
        m = self.start()
        write_reg(m, 0x38, 0x8000)
        write_reg(m, 0x39, 0x0001)
        self.assertEqual(read_reg(m, 0x38), 0x8000)
        self.assertEqual(read_reg(m, 0x39), 0x0001)

    def test_cs_deasserted_ignores_clocks(self):
        """Negative: with CS deasserted (bit 24 high) rising edges on CLK are ignored (docs/
        devices.md: "CS low, then 24 bits"). A full write frame clocked with CS high leaves REG_38H
        at its reset value."""
        m = self.start()
        m.writel(GPIOA + DDR, DAT)
        frame = (0x38 << 16) | 0x1111
        clock_out(m, GPIOA, [(frame >> i) & 1 for i in range(23, -1, -1)], asserted=False)
        self.assertEqual(read_reg(m, 0x38), 0x3A98)

    def test_partial_frame_writes_nothing(self):
        """Negative: a frame cut short by CS deassertion writes nothing (docs/devices.md: the
        transfer is 24 bits). Ten bits of a write to REG_30H leave it at 0, and the next full
        frame still works."""
        m = self.start()
        m.writel(GPIOA + DDR, DAT)
        m.writel(GPIOA + DR, pins(False, 0, 0))
        partial = (0x30 << 16) | 0x1234
        clock_out(m, GPIOA, [(partial >> i) & 1 for i in range(23, 13, -1)])
        m.writel(GPIOA + DR, pins(False, 0, 0))
        self.assertEqual(read_reg(m, 0x30), 0x0000)
        write_reg(m, 0x30, 0xBFF1)
        self.assertEqual(read_reg(m, 0x30), 0xBFF1)

    def test_data_not_seen_when_soc_releases_data(self):
        """Negative: with SWPORTA_DDR bit 8 clear the SoC does not drive DATA (docs/devices.md,
        "FD6818"), so the chip sees zeros. A write frame of REG_38H is then seen as a write of 0
        to read-only REG_00H, and REG_38H is unchanged."""
        m = self.start()
        m.writel(GPIOA + DDR, 0)
        frame = (0x38 << 16) | 0x1111
        clock_out(m, GPIOA, [(frame >> i) & 1 for i in range(23, -1, -1)])
        m.writel(GPIOA + DR, pins(False, 0, 0))
        self.assertEqual(read_reg(m, 0x38), 0x3A98)

    def test_bank_b_does_not_reach_chip(self):
        """Negative: the bus is on GPIOA only (docs/devices.md, "Bus pins, all on GPIO bank A"). A
        write frame clocked on bank B's DR bits 7, 8 and 24 leaves REG_38H unchanged."""
        m = self.start()
        write_reg(m, 0x38, 0x1111, base=GPIOB)
        self.assertEqual(read_reg(m, 0x38), 0x3A98)


if __name__ == "__main__":
    unittest.main()
