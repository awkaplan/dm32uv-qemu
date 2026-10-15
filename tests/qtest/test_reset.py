# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# Copyright (c) 2026 Alex Kaplan
"""Resets: SYS_SOFT_RSTN at 0x11000000 (hw/misc/dm32_sysreg.c; HR_C7000 v2.7 section 4.1.4.1)
and every device's machine reset (docs/devices.md, "Resets")."""

import unittest

from dm32qtest import DM32TestCase, CCR, CCR_LOAD, LOAD_S, VAL_S, rtc_read, rtc_write, setup

SOFT_RSTN = 0x11000000
IO_DIPLEX1 = 0x11000038
PIC_MASK = 0x17000008
ADC_CH_VLD, ADC_PD_SEOC = 0x140D0024, 0x140D0028
DAC_PD_CTRL = 0x140F0000
PWM_PER0 = 0x140C0008
GPIOA_DDR = 0x14020004
SSI_BAUDR = 0x04000014
LCD_STATUS = 0x12000028
ALL = 0x1FF


def pulse(*bits):
    """The SYS_SOFT_RSTN value that pulses the given bits (active low)."""
    v = ALL
    for b in bits:
        v &= ~(1 << b)
    return v


class SoftResetTest(DM32TestCase):
    def test_reads_all_released(self):
        """SYS_SOFT_RSTN resets to 0x1ff, and a 0 bit releases itself at once ("automatically
        releasing the soft reset register value to 1", section 4.1.2.2): it reads 0x1ff after
        any write."""
        m = self.start()
        self.assertEqual(m.readl(SOFT_RSTN), ALL)
        m.writel(SOFT_RSTN, 0x190)               # stock's boot pulse
        self.assertEqual(m.readl(SOFT_RSTN), ALL)

    def test_adc_bit_resets_adc(self):
        """Bit 6 (adc_ctrl_soft_rstn) resets the ADC to its reset values (section 5.9.5):
        ADC_CH_VLD 0, PD_SEOC_TIME 0x0a20."""
        m = self.start()
        m.writel(ADC_CH_VLD, 0xFF)
        m.writel(ADC_PD_SEOC, 0)
        m.writel(SOFT_RSTN, pulse(6))
        self.assertEqual(m.readl(ADC_CH_VLD), 0)
        self.assertEqual(m.readl(ADC_PD_SEOC), 0x0A20)

    def test_negative_other_bits_leave_adc(self):
        """Negative: pulsing the other module bits (stock's mode-change pulse, bits 0 and 1, and
        the baseband ADC, bit 5) does not touch the CPU-side ADC."""
        m = self.start()
        m.writel(ADC_CH_VLD, 0xFF)
        m.writel(SOFT_RSTN, pulse(0, 1, 5))
        self.assertEqual(m.readl(ADC_CH_VLD), 0xFF)

    def test_unmodelled_module_logged(self):
        """A pulse of a module with no model is logged under -d unimp."""
        m = self.start(args=["-d", "unimp"])
        m.writel(SOFT_RSTN, pulse(3))
        self.assertIn("SYS_SOFT_RSTN[3] (audio interface) reset: module not modelled", m.close())

    def test_system_bit_resets_all_but_this_block(self):
        """Bit 7 with bit 8 (stock's reboot value 0xfe7f): every module resets, the PIC masks
        every source again, but the register configuration module (this block, reset mode H)
        keeps its registers (section 4.1.2)."""
        m = self.start()
        m.writel(PIC_MASK, 0)
        m.writel(ADC_CH_VLD, 0xFF)
        m.writel(IO_DIPLEX1, 0x12345678)
        m.writel(SOFT_RSTN, 0xFE7F)
        m.clock_step(1_000_000)
        self.assertEqual(m.readl(PIC_MASK), 0xFFFFFFFF)
        self.assertEqual(m.readl(ADC_CH_VLD), 0)
        self.assertEqual(m.readl(IO_DIPLEX1), 0x12345678)

    def test_negative_cpu_bit_leaves_devices(self):
        """Negative: bit 8 alone resets the CPU, not the devices: the PIC mask stays."""
        m = self.start()
        m.writel(PIC_MASK, 0)
        m.writel(SOFT_RSTN, pulse(8))
        m.clock_step(1_000_000)
        self.assertEqual(m.readl(PIC_MASK), 0)

    def test_machine_reset_resets_this_block(self):
        """A machine reset (power-on class) returns this block to its reset values, also right
        after a soft system reset kept them."""
        m = self.start()
        m.writel(IO_DIPLEX1, 0x12345678)
        m.writel(SOFT_RSTN, pulse(7))
        m.clock_step(1_000_000)
        m.qmp("system_reset")
        m.clock_step(1_000_000)
        self.assertEqual(m.readl(IO_DIPLEX1), 0xFFFFFFFF)
        self.assertEqual(m.readl(SOFT_RSTN), ALL)


class MachineResetTest(DM32TestCase):
    def test_soc_blocks_return_to_reset_values(self):
        """A machine reset returns each SoC block with no other reset test to its reset values:
        ADC (PD_SEOC_TIME 0x0a20), DAC (all channels powered down, 0x7), PWM, GPIO (all inputs),
        SSI, and the LCD interface's enable handshake."""
        m = self.start()
        m.writel(ADC_PD_SEOC, 0)
        m.writel(DAC_PD_CTRL, 0)
        m.writel(PWM_PER0, 0x1234)
        m.writel(GPIOA_DDR, 0xFF)
        m.writel(SSI_BAUDR, 0x10)
        m.writel(LCD_STATUS, 1)
        self.assertEqual(m.readl(LCD_STATUS), 0x3)
        m.qmp("system_reset")
        m.clock_step(1_000_000)
        for addr, value in ((ADC_PD_SEOC, 0x0A20), (DAC_PD_CTRL, 0x7), (PWM_PER0, 0),
                            (GPIOA_DDR, 0), (SSI_BAUDR, 0), (LCD_STATUS, 0)):
            self.assertEqual(m.readl(addr), value, hex(addr))

    def test_rtc_keeps_time(self):
        """The RTC is not reset by a SoC reset (assumption in dm32_i2c_rtc.c): a loaded time
        survives a machine reset."""
        m = self.start()
        setup(m)
        rtc_write(m, CCR, [0])
        rtc_write(m, LOAD_S, [5, 6, 7, 8, 0])
        rtc_write(m, CCR, [CCR_LOAD])
        m.qmp("system_reset")
        m.clock_step(1_000_000)
        setup(m)
        self.assertEqual(rtc_read(m, VAL_S, 5), [5, 6, 7, 8, 0])


if __name__ == "__main__":
    unittest.main()
