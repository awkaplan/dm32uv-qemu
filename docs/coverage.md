# Subsystem coverage

This page grades how closely each part of the DM-32UV's HR_C7000 SoC is modelled in this board
model, lists where the model is more permissive than the silicon, and gives the open questions and
the remaining work in order. It is the page to read before trusting a result from the emulator.

## How coverage is assessed

"Coverage" here means, for each SoC block the firmware touches, whether the model implements the
block's register semantics, state machine and side effects, and how far the result can be trusted
in emulation. Three sources set the expectations:

- The HR_C7000 v2.7 datasheet, its register-table directory (29 tables, the SoC's peripheral
  inventory). The datasheet is not redistributed with this repository; register names and offsets
  cited here come from it.
- The SVD file [`HR_C7000-DM32.svd`](https://github.com/awkaplan/dm32uv-re/blob/main/data/registers/HR_C7000-DM32.svd)
  in dm32uv-re, which supplies reset values, access types and interrupt numbers.
- The register traffic of the stock DM32.00.01.048 firmware in the boot-to-menu and Snake runs.
  Stock's accesses are the evidence for whether a register is exercised at all.

Anything the board file does not map is absent, whatever a device file says. The board file is
[`hw/csky/dm32.c`](../hw/csky/dm32.c); the device files are under `hw/`. Each row below names the file
that implements it. The per-device register reference is in [devices](devices.md).

## Fidelity scale

- **A, behavioural.** Register semantics, state machine and side effects are modelled. Wrong firmware
  behaviour is detected.
- **B, functional.** Firmware proceeds correctly. Interlocks and timing are absent.
- **C, permissive stub.** Reads return a value chosen to let firmware continue. Misuse is not detected.
- **D, RAM window.** Storage only. Reads return what was written. No device behaviour.
- **F, absent.** Not mapped, or mapped to a logging catch-all.

An F address does not fault. QEMU's unassigned-memory handler returns 0 for reads and drops writes,
and the C-SKY target installs no transaction-failed hook, so the decode error is discarded. The only
trace is a `LOG_INVALID_MEM` line, visible with `-d invalid_mem`. The unimplemented-device windows
described below log under `-d unimp`. An F row therefore reads as zeros and swallows writes, which is
the most permissive behaviour available.

## Coverage table

Grades are as of the current board model. "Note" gives the main behaviour or gap; the linked file has
the detail.

| Subsystem | Base | Model file | Grade | Note |
|---|---|---|---|---|
| Reset / system control | 0x11000000 | [dm32_sysreg.c](../hw/misc/dm32_sysreg.c) | A | `SYS_SOFT_RSTN` as the datasheet's self-clearing per-module reset: the CPU, system (the system registers spared), and ADC bits act; the six bits of unmodelled modules are logged. Every device has a reset handler; separate parts (FD6818, panel, NOR, RTC) and RAM persist. Stock's reboot (`0xFE7F`) reboots |
| Clock manager | 0x11000000 | [dm32_sysreg.c](../hw/misc/dm32_sysreg.c) | C | `+0x04` reads forced locked and ready (bits 31:30); other registers are storage; no PLL state, no invalid-divider check |
| Baseband system control | 0x11000000 | [dm32_sysreg.c](../hw/misc/dm32_sysreg.c) | C | DMR Layer-2 slot timing (30 ms tick, slots 0..287, TX-slot IRQ to PIC source 30); see [modem-timing](modem-timing.md) |
| Pin mux IO_DIPLEX0/1/2, pulls and input enables | 0x11000034-0x11000054 | [dm32_sysreg.c](../hw/misc/dm32_sysreg.c) | A | Decoded per pad for all 87 GPIO pads (select, pull, input enable) with datasheet reset values, IO_DIPLEX2 included (`0x3FFFFFFB`). Every block with pads consults it: GPIO, SPI0-2 per instance, I2C0/1, PWM, UART1-3 TX, ADC, LCD. `pad_conflict` when GPIO drives a peripheral's pad, `pad_unclaimed` when a block transfers without its pads. UART receive is not gated |
| FM subsystem | 0x11000500 | [dm32_sysreg.c](../hw/misc/dm32_sysreg.c) | D | Inside the sysreg window; FM_SQL is ordinary storage |
| PIC | 0x17000000 | [dm32_pic.c](../hw/intc/dm32_pic.c) | A | Every register per the datasheet: edge and level triggering with polarity, fixed or PIC_PRIOR arbitration, vector base and autovector, one in-service source until EOI, request records and count. The tie rule and level-source withdrawal before the EOI are assumptions |
| Timer | 0x14000000 | [dm32_timer.c](../hw/timer/dm32_timer.c) | A | Six channels at 84 MHz, channel n to PIC source n+1 (`hw/csky/dm32.c`). Free-running and user-defined modes, LoadCount writes deferred to the next reload, reset to disabled. All six channels verified by qtest; stock uses all six in mode 1. The clock is a constant until the clock tree lands |
| RTC | I2C2 slave 0x70 | [dm32_i2c_rtc.c](../hw/misc/dm32_i2c_rtc.c) | A | A QEMU `I2CSlave` on `i2c2`, not MMIO, with all 24 registers of Register Table 7: binary count with rollovers, load, divider, soft reset, MATCH alarm with RSTAT on PIC 19. Counts on QEMU's RTC clock (`-rtc clock=vm` is reproducible). Pointer auto-increment and the VAL latch are assumptions |
| I2C2 controller | 0x14080000 | [dm32_i2c.c](../hw/misc/dm32_i2c.c) | A | The HR_C7000's DesignWare master with IC_START, on QEMU bus `i2c2`. Transfers timed from the SCL counts in virtual time, 8-entry FIFOs, every master interrupt on PIC 9, abort sources, enable status, enabled-write lock (assumption) |
| Watchdog | 0x14010000 | [dm32_wdt.c](../hw/misc/dm32_wdt.c) | B+ | Counting watchdog per the datasheet: counts up from OT_LOAD_H:L to the 2^35 overflow, holds the machine for the RST_LOAD reset pulse, then resets it. The clock is assumed (see the list below). Stock never uses it |
| I2C0 | 0x14060000 | [dm32_i2c.c](../hw/misc/dm32_i2c.c) | A | The I2C2 controller model on PIC 8, with nothing on bus `i2c0`, so every address NAKs. Stock never uses the controller: its pads carry the bit-banged ALPU-MP bus (see ALPU-MP) |
| I2C1 | 0x14070000 | [dm32_i2c.c](../hw/misc/dm32_i2c.c) | A | The I2C2 controller model on PIC 7, with nothing on bus `i2c1`, so every address NAKs. No stock access found; its pads carry the FD6818's CLK and DATA |
| SPI0 | 0x140A0000 | [dm32_ssi.c](../hw/misc/dm32_ssi.c) | A | The datasheet's SSI master: 128-entry FIFOs, frames timed from BAUDR and DFS in virtual time, TMOD 0-3 with NDF, thresholds and all five interrupts on PIC 16, configuration locked while enabled. The data chip is selected by GPIOA 18 driven low. RDID `ef 40 18`; PROGRAM and ERASE gated by WREN ([coverage plan, item 16](coverage-plan.md#16-data-flash-w25q128-m)) |
| SPI1/2/3 and 0x04000000 | 0x140B0000, 0x140E0000, 0x14150000, 0x04000000 | [dm32_ssi.c](../hw/misc/dm32_ssi.c) ×4 | C | Same model as SPI0, each instance gated on its own pads (SPI1 PTA25-27, SPI2 the LCD pads at select 1; SSI3 and 0x04000000 have dedicated pads and are not gated), but each holds its own copy of the data-chip image. No separate attached device. 0x04000000 is, by structural evidence, a raw SSI to the program flash, used only for its quad-enable sequence (see open questions). The controller is the SPI0 model (A-grade); SPI1 interrupts on PIC 22 and SPI2 on PIC 23. SSI1 carries stock's SD-card probe; no stock use of SSI2 was found |
| I80 / LCD | 0x12000000 | [dm32_lcd.c](../hw/display/dm32_lcd.c) | B | 240x320 GRAM and MADCTL. Enable handshake enforced. Data writes while IO_DIPLEX2 hands DB0..DB4 to the keypad are forced to 0xff. No bus timing |
| UART0 | 0x14030000 | `csky_uart` ([csky_uart.c](https://github.com/awkaplan/csky-qemu/blob/main/hw/char/csky_uart.c)) | B | PIC source 12. DW PTIME LSR[5] polarity honoured |
| UART1 / UART2 / UART3 | 0x14040000 / 0x14050000 / 0x14090000 | `csky_uart` ×3 | B (UART0 model, unverified) | PIC 10 / 11 / 13. UART1 is the optional GPS port (see open questions); no stock use of UART2 or UART3 was found. A NULL chardev (no `-serial` for that index) drops TX silently |
| SDIO | 0x15000000 | unimplemented device (`hw/csky/dm32.c`) | F (logged) | PIC 20; card-detect and similar read 0. The radio has no card slot, and no stock access was found |
| USB | 0x13000000 | unimplemented device (`hw/csky/dm32.c`) | F (logged) | 256 KB; PIC 21. A DesignWare USB 2.0 OTG core, device-only (SVD). No stock access found |
| SPI slaves 0/1/2 | 0x14120000 / 0x14130000 / 0x14140000 | [dm32_spi_slave.c](../hw/misc/dm32_spi_slave.c) | B | The SSI register set in slave mode, SVD reset values through the register file, 8-entry TX FIFO, TXEI/TXOI/RXUI on PIC 34-36. No master exists on the board, so no transfer happens. Stock never uses them |
| GPIO A/B/C | 0x14020000 / 0x14100000 / 0x14110000 | [dm32_gpio.c](../hw/gpio/dm32_gpio.c) | A | DesignWare port-A interrupt block on all banks, PIC 15/14/17, with per-bit both-edge and a 61 us debounce on interrupt inputs (clock assumed). EXT_PORTA is the pad level: SoC pulls from IOMGR_REN, board idle-low overrides, driven pins read SWPORTA_DR, muxed-away and input-disabled pads read 0 and cannot interrupt. Keypad rows follow a column pad driven low at the read |
| PWM | 0x140C0000 | [dm32_pwm.c](../hw/misc/dm32_pwm.c) | B | Three channels; CFG, RPT, PER, FP and STATUS decoded. No waveform and no interrupt. PWM0 is the LCD backlight (see [running](running.md) for the trace variables) |
| ADC | 0x140D0000 | [dm32_adc.c](../hw/misc/dm32_adc.c) | B | Datasheet state machine: PD_FORCE_HIGH reset, ADC_START, CH_VLD, single-scan lowest channel, VERID `0x20151216`. Refuses to convert a pad still muxed to GPIO. Inputs are constants |
| DAC | 0x140F0000 | [dm32_dac.c](../hw/misc/dm32_dac.c) | B | PD_CTRL resets to 0x7 (all powered down); 12-bit DATA masking; DATA_A/B/C at +0x08/+0x0c/+0x10. No analogue output |
| Codec | 0x160009C0 | [dm32_codec.c](../hw/misc/dm32_codec.c), over the DMR RAM window | A | All 37 registers of DS Register Table 26 with their reset values, read-only status, power modes with pon_ack after the DS's Tsbyu/Tstbon, soft-mute ramps in MR with the IFR events, the codec timer, IFR/IMR and IFR2/IMR2 to PIC 37 shaped by ICR, the indirect mixer and AGC banks, LINEOUT_CTRL standby_lo, and the CODEC_I2C_MUX cut-off. Timings are the DS's bounds; how flags clear and what starts the timer are assumptions. No audio |
| DMR phy/link RAMs | 0x16000000, 64 KB | [dm32_dmrram.c](../hw/misc/dm32_dmrram.c) | D | Plain RAM with capture. Zero at reset. No RX injection, no signal, no interrupts, no busy or underrun behaviour |
| Program flash (XIP) | 0x03000000, 1 MB image | `hw/csky/dm32.c` (ROM device) | D+ | Reads served from the backing store. Guest stores dropped and counted (`FLASH WRITE DROPPED`). The LCSFC command path that stock uses through this window is not modelled; see open questions |
| Program flash tail | 0x03100000-0x03FFFFFF | unimplemented window `dm32.lcsfc` (`hw/csky/dm32.c`) | F (logged) | Reads 0, writes dropped, logged under `-d unimp`. Stock writes to 0x03800000 are, by structural evidence, the LCSFC page-program data port (see open questions) |
| Data flash (W25Q128) | SPI0, CS = GPIOA bit 18 | [dm32_ssi.c](../hw/misc/dm32_ssi.c) | B+ | 16 MB store. PP, sector, block and chip erase WREN-gated. Writes persist to `DM32_SPINOR_OUT`. WIP is a fixed count of RDSR reads |
| IRAM / SRAM | 0x00010000 / 0x18000000 | RAM (`hw/csky/dm32.c`) | A | Loaded with `-device loader`. Bytes the loader does not cover start at 0, or with `DM32_RAM_FILL=noise` hold seeded noise as untouched SRAM does on hardware (see the list below) |
| Boot ROM | 0x00000000 | plain RAM, zero-filled (`hw/csky/dm32.c`) | D | The 2 KB ROM is not loaded and does not run; see [boot-rom](boot-rom.md) |
| Cache controller | 0xE000F000 | [dm32_cache.c](../hw/misc/dm32_cache.c) | B | Registers per the XuanTie CSI header: CER and CRCR fields kept, CIR operations complete at once, profiling counters read 0. QEMU has no cache, so stale lines are not detected. Stock caches the 8 MB program-flash window, write-back |
| FD6818 RF chip | 3-wire on GPIOA bits 7 / 8 / 24 | [dm32_fd6818.c](../hw/misc/dm32_fd6818.c) | A | Registers per the FD6818 programming guide: soft reset, page select, REG_0CH status (stock reads `0x0280`, the radio's value), PLL lock 40 us after a retune or enable, squelch with the guide's RSSI and noise hysteresis, CTCSS and CDCSS detection, interrupt vector and mask, GPIO0/1 on GPIOB 27/26 and live `gpio_in_val`. The RF signal is a scripted environment (`DM32_RF_ENV`, grade B): stock's squelch opens and its RSSI bar moves on a scripted carrier. Transmit, audio, DTMF, VOX and FSK absent |
| ALPU-MP | bit-banged I2C on GPIOA 5 (SCL) / 6 (SDA), address 0x7a | none (bus `alpu` in [dm32_gpio.c](../hw/gpio/dm32_gpio.c)) | F | The pads are an I2C bus that QEMU slaves can join, but no ALPU-MP slave exists, so the address NAKs; stock's check clocks on through the NAKs (504 `i2c_nak` events per boot). The bypass sector in dm32uv-re is needed to boot stock |
| RDA5807M tuner | bit-banged I2C on GPIOA 26 (SCL) / 25 (SDA), address 0x10 | [dm32_rda5807m.c](../hw/misc/dm32_rda5807m.c) on bus `fm` | A | Register level per datasheet section 6 and the RDL: sequential access from 02H/0AH with the 3AH wrap, hidden registers behind OPEN_MODE, tune and seek in virtual time with STC/SF/READCHAN, RSSI, FM_TRUE and ST from `DM32_FM_STATIONS`, power-down and soft reset. Tune and seek times are assumptions; audio and RDS absent |
| HR_V3000 vocoder | not fitted | none | N/A | The DM32.00.01.048 image has no HR_V3000 driver; the radio uses the HR_C7000's built-in vocoder (see open questions). AMBE++ is absent by choice |
| eFuse ID | read over SPI_Master3 | [dm32_ssi.c](../hw/misc/dm32_ssi.c) (SSI3) | F | SSI3 answers with data-chip bytes, so the hardware-binding record is patched rather than read |
| Unassigned peripheral space | everything outside the Table 26 blocks | `dm32.unassigned` background region (`hw/csky/dm32.c`) | F (logged) | Each datasheet address-map block (HR_C7000 v2.7, Table 26) has a `dm32.<name>` unimplemented window under its model, logged under `-d unimp`. Every other address, including the gaps between blocks, is unassigned: reads 0, drops writes, logs under `-d invalid_mem`. Both are permissive events, so `DM32_STRICT` can stop on them. What the silicon does on an unassigned access is not documented (see open questions) |

Grade counts across the 38 rows: A 14, B or B+ 10, C or C+ 3, D or D+ 4, F 6, N/A 1.

## What the model is verified to do

The stock DM32.00.01.048 image boots in this model with three emulator-only patches applied by
dm32uv-re's
[`compose_qemu_image.py`](https://github.com/awkaplan/dm32uv-re/blob/main/tools/src/dm32_patch_tools/compose_qemu_image.py):
the ALPU-bypass sector (`data/firmware/patching/sector_030c6000_alpubypass.bin`), auto-power-off NOPs,
and hardware-binding bytes. Those patches are QEMU-only.

- Stock boots to the standby screen.
- The menu system works, and scripted key presses (`DM32_KEYSEQ`) register.
- Quiescent frames are dumped (`DM32_GRAMDUMP`).
- The Snake example patch from dm32uv-re
  ([`tools/examples/snake`](https://github.com/awkaplan/dm32uv-re/tree/main/tools/examples/snake))
  runs through the menu, the sub-list, the game, direction changes and Back, with no LCD or keypad
  pin-mux warnings and no CPU-model crash.
- The regression runs report zero unimplemented and zero invalid accesses. Stock boot to menu:
  `steps=3 crash=0 muxwarn=0 unimp=0 invalid=0`. Snake walkthrough: `steps=11 crash=0 muxwarn=0
  unimp=0 invalid=0`, with the `9472` and `1175` frames present. The regression compares the step
  count, the crash and muxwarn counts, and the key pixel-count fingerprints. It does not compare the
  whole token line, whose order changes with host load.

The results that depend on specific rows: the RTC slave on I2C2 (without it the UI tick never starts
and no key registers), timer channel 5 (stock's UI tick), EXT_PORT reads in `dm32_gpio` (stock reads
inputs at `+0x50`), and the watchdog and DAC devices. Stock was not observed to enable the watchdog,
so the watchdog's bite and feed behaviour were exercised by the qtests and a gdb-stub driver, not by
stock (stock never uses the watchdog).

## Where emulation is more permissive than hardware

These are the places where the model accepts something the silicon would refuse, or returns a value the
silicon would not. A firmware bug in these areas can pass in emulation and fail on the radio.

The model counts the accesses it can detect in these areas as permissive events and prints the counts
at exit (`dm32-events:`). `DM32_STRICT` stops the guest at the first event of the selected classes
([devices](devices.md#permissive-events-and-dm32_strict)). A clean strict run rules out only the
detected cases; the gaps listed here without an event class (timing, constant inputs, the absent
signal paths) stay undetected.

1. **GPIO debounce clock is assumed, and edges can bunch under host load.** Interrupt inputs use the
   DesignWare debounce with the 32.768 kHz RTC clock assumed, so two cycles are 61 us. Interrupt
   timing follows QEMU's scheduler, so closely spaced edges can bunch. The board's idle-low pads
   (GPIOA bit 16 by default, the power-sense line) are a board assumption that `DM32_GPIO_IDLE_LOW`
   replaces. Keypad changes are verifiable only on hardware. See [key-injection](key-injection.md).
2. **LCD bus timing is absent.** `+0x10` is stored and read back with no effect. A frame costs the same
   in emulation whatever the firmware's bus configuration, so render time cannot be measured.
3. **ADC inputs are constants.** `DM32_ADC_CH0..7` supply fixed values, and an unset channel reads 0.
   There is no settling, noise, or first-sample behaviour, so battery-threshold logic is untested. The
   state machine and the refusal to convert a GPIO-muxed pad are modelled.
4. **Clock manager is permissive.** `+0x04` always reads locked and ready. Other clock registers are
   storage, so an invalid divider or a reprogrammed PLL has no consequence. `+0x10` reads 0 until
   written.
5. **Unimplemented and unassigned addresses read 0 and drop writes.** SDIO, USB, the parts of other datasheet blocks that no model covers, and every address outside those blocks
   return 0. Each access is logged (`-d unimp` or `-d invalid_mem`) and counted, but firmware that
   polls one of these windows sees a permanent 0. On silicon an unassigned access might instead raise
   a bus error or hang; the datasheet does not say.
6. **The FD6818 receives only a scripted RF environment.** Without `DM32_RF_ENV` the band is empty
   (noise floor only), and REG_62H keeps a stand-in jitter (`DM32_FD6818_RSSI`,
   `DM32_FD6818_RSSI_JITTER`). With a script, levels, SNR and noise follow the formulas in
   [devices](devices.md#fd6818), several of whose constants are assumptions: the REG_62H and REG_65H
   offsets, the 6.25 kHz channel, the noise curve, the weak-signal and sub-audio SNR limits, and the
   CDCSS bit order (any rotation in either order matches). REG_0CH bits 10-11 and 14-15, which stock
   reads, are reserved in the guide and read 0. Transmit, audio, DTMF, VOX, TOT, AFC and FSK are
   absent.
7. **Watchdog clock is assumed.** The timeout formula is the datasheet's (count up from H:L to the
   2^35 overflow), but the datasheet does not name the clock and its two figures imply different ones
   (17.9 MHz from the 60 s timeout, 26 MHz from the 20 ms reset pulse). The model takes the clock that
   gives 60 s; `DM32_WDT_HZ` changes it. The pulse pauses the vCPUs from a bottom half, so a few
   instructions may run after the bite, and with `-icount` the guest keeps running for the pulse.
   `DM32_WDT_NORESET=1` logs a bite without resetting the guest.
8. **ALPU-MP is bypassed.** Stock's anti-clone check never reaches an ALPU-MP. With the bypass sector
   spliced in, the boot proceeds, and the hardware-binding record is patched.
9. **Boot ROM does not run.** The board forces `vbr=0x03000004` and `pc=0x03001000` at reset. The
   DH_FLAG check, the boot-mode pin decision and the `02 24 00 03` UART announcement are never exercised.
10. **DMR RAM has no modem state.** Status bytes that stock polls at `+0x32`, `+0x33`, `+0x232` and
    `+0x233` return whatever was last written. Zero at reset is a model choice, since the datasheet gives
    no reset value.
11. **DAC has no output.** Writes to a powered-down channel are stored, with one warning per channel.
    A power-ramp path sees the code, not a voltage.
12. **SPI instances copy the image.** Each `dm32_ssi` instance is gated on its own pads, but each
    holds its own copy of the data-chip file. The eFuse read over SPI_Master3 returns data-chip bytes,
    and stock's quad-enable sequence on `0x04000000` runs against the data chip's model. Only the instance stores changed by a PROGRAM or ERASE are written back
    with `DM32_SPINOR_OUT`, and the multi-instance output path has not been exercised.
13. **SPI-NOR protection and timing are simplified.** WRSR is accepted and ignored, so the status-register
    protection bits are not enforced. WIP is a poll count (`DM32_SPINOR_WIP_READS`, default 2), not
    elapsed time. A second command during a PROGRAM or ERASE is not rejected. Suspend, OTP and unique ID
    (0x4B) are absent.
14. **UART1-3 use the UART0 model unverified.** A NULL chardev drops TX bytes silently, so an empty
    capture does not prove that the firmware did not transmit.
15. **SRAM starts at zero by default.** Bytes that the `-device loader` image does not set read 0. On
    hardware, SRAM that the firmware never initialises holds power-on noise: a live capture shows
    13,277 of the 13,312 bytes in `0x18004c00`-`0x18007fff` non-zero (dm32uv-re
    `docs/live-ram-capture.md`). Code that relies on zero-initialised SRAM passes here and fails on
    the radio. `DM32_RAM_FILL=noise` closes the gap, but only for bytes no loader image covers: the
    usual dm32uv-re run loads a full 288 KB IRAM image and a 32 KB zero SRAM image, which overwrite
    the noise. A hardware-like run loads IRAM only to `0x4a7af` and SRAM only to `0x18004bff`, the
    ranges stock initialises itself (dm32uv-re `docs/rtos-and-memory.md`). The noise is uniform
    random bytes; its real distribution and dependence on the unit are not modelled.

Modelled as on hardware, and therefore not a risk: all PIC sources masked at reset,
and the RTOS tick on PIC source 1 (timer channel 0).

## Not modelled and open questions

### Program-flash window used as a command path

Stock code at flash `0x030db8c4` and `0x03003d5x` stores the SPI-NOR opcodes `0x06` (WREN) and `0x04`
(WRDI) to `0x03000000`, and a word store on the `0x20` path (the sector-erase opcode, by standard NOR
convention; not confirmed for this part). It then reads `0x03000000` back with `ld16.w` at `0x030db94a`
and branches on the value. The model drops these stores by default, so the readback returns flash
contents. With `DM32_FLASH_WRITABLE=1` the readback returns the command byte, so the two modes take
different branches. Both modes reach the same screens in the regression runs.

The public HR_C7000 datasheet documents only `LCSFC_BAUDR` and `QUAD_ENABLE` (section 4.4.4). Whether
the LCSFC forwards window stores as commands on the silicon is open. Resolving it needs a source beyond
the public datasheet or a measurement on hardware.

The stock code's own encoding is decoded structurally in dm32uv-re's
[`specs/rdl/gd25q80.rdl`](https://github.com/awkaplan/dm32uv-re/blob/main/specs/rdl/gd25q80.rdl)
(confidence STRUCTURAL). The dispatcher is IRAM `0x0002bca4`; `0x030db8c4` is its copy in flash. It
issues each command as a store, with the opcode in the top byte of the stored word:

- A byte `0x06` (WREN) or `0x04` (WRDI) to `0x03000000`.
- Sector erase: a word `(0x20 << 24) | addr` to `0x03000000`.
- Page program: a word `(0x02 << 24) | addr` to `0x03800000`, then the data bytes to `0x03800000`,
  then the last byte to `0x03000000`.

Completion is not read from a status register. The code at `0x0002bc78` re-reads the DH_FLAG word
(`0x0c646874`) at `0x03000000` through XIP. A twin of this driver sits in the flash-resident YMODEM
upgrade loader (`0x03003d30`-`0x03004010`). The stock writes to `0x03800000`, inside the 16 MB LCSFC
window and past the 1 MB image, are therefore page-program data. None of this is confirmed on silicon.

### ALPU-MP

The check talks to the device at `0x7a` (7-bit `0x3d`) over a bit-banged I2C bus on GPIOA, not
through the I2C0 controller. `i2c_read_block_0x7a` (IRAM `0x000144fc`), `i2c_write_block_0x7a`
(`0x00014424`) and `i2c_send_byte_wait_ack` (`0x00014304`) load the GPIOA base `0x14020000` and drive
SCL on bit `2 * idx + 5` and SDA on bit `2 * idx + 6`. The bus index is 0, giving SCL on GPIOA 5 and
SDA on GPIOA 6. The pads are muxed to GPIO at IRAM `0x000141c8` (`IO_DIPLEX0 |= 0x60`). Sources:
dm32uv-re `specs/dts/dm32uv.dts` (`i2c_alpu`) and the disassembly of those functions.

The model bridges those pins to an I2C bus (`alpu`), but no slave answers at `0x3d`, so every ACK slot
reads high (a NAK) and the handshake fails. Without the bypass sector, stock fails the check and halts at IRAM
`0x169d4` (`br16 .`), reached from IRAM `0x16c8c`. The ALPU-MP protocol is not modelled. The bypass
sector is for QEMU only and must never be flashed.

### RF, audio and vocoder signal path

- RF reaches the firmware only as a scripted environment (risk 6): carriers with a level and a CTCSS
  or CDCSS code. There is no modulation content: no received audio, DMR bursts or data.
- The codec has registers but no audio: no samples flow through it. The DAC has no output. The DMR RAM has no RF or vocoder path.
- The RDA5807M tuner has no audio or RDS. AMBE++ is absent by choice.
- The radio has no HR_V3000. DM32.00.01.048 contains no HR_V3000 driver, frame template or frame
  builder (dm32uv-re `specs/include/hr_v3000.h`). The DM-32 uses the HR_C7000's built-in vocoder
  (HR_C7000 v2.7, 7.1.1, p.473), whose coded frames sit in the baseband RAM. Forum posts mention
  separate "HR_Vocoder" firmware builds (DM32.00.01.034 and .046) for a hardware variant; what that
  variant contains is unresolved, and this model does not target those builds.

### Other open items

- 0x04000000 (`hw/csky/dm32.c`) is attributed to the program flash by structural evidence only. Its
  only user, IRAM `0x0002bb98`, stores the base, initialises the controller through `0x0002bb28`, and
  sends the GD25Q80 quad-enable sequence (`35h`, `06h`, `01h 00h 02h`, `05h`), then sets
  `QUAD_ENABLE` (`0x1100005c`). The instance has dedicated pads and is not pad-gated, so in the model
  the sequence reaches the data chip's SPI-NOR model, until the program flash has its own
  ([coverage plan, item 29](coverage-plan.md#29-program-flash-xip-tail-and-lcsfc-command-path-l-hw-for-a)).
- Stock drives GPIOC 13 (LCD_DB6) as a keypad column while the LCD owns the pad: its keypad scans
  hand only DB0-DB4 to GPIO (`menu_power_on_display_and_keypad_poll_loop` at `0x0305f128`). The
  model counts `pad_conflict` and keeps keys working, because its keypad injection does not depend on
  the pad. Either stock relies on something the model does not know, or the datasheet's DB5-DB7 pad
  assignment is wrong. A hardware check of PTC13 during a scan would settle it.
- The source of the eFuse ID over SPI_Master3 is not modelled.
- The watchdog clock, and so its absolute timeout and pulse length, needs a measurement on a radio.
- What the silicon does on an access outside every address-map block (a bus error, a 0 read or a
  hang) is not documented: Table 26 (HR_C7000 v2.7 section 4.5.3) lists no reserved ranges and no
  section describes the case. The model returns 0 and drops writes. Settling it needs a measurement
  on a radio.
- UART1 is the GPS port. `gps_uart1_init_9600` (`0x0301d96c`) sets 9600 baud and muxes only the
  receive pad, and `radio_boot_full_init_sequence` calls it only when option byte bit 2 is set. The
  module is reported as an ATGM336H (CASIC `$PCAS` sentences in flash; identity from a board photo).
  Stock queues those sentences through `uart_tx_chunked_mutex_send` (`0x0301d9d4`) although the
  transmit pad is not muxed; whether they reach the module is open. No byte was seen on UART1 in the
  boot runs, which does not show that the init did not run.

## Roadmap

Ordered by the risk of a hardware-only bug times how much current work depends on it.

### Tier 1: blocks current work or hides destructive errors

1. **LCSFC program-flash path.** Establish whether stores to the window are commands on the silicon.
2. **PWM waveform, interrupt and Light Time.** The register model is done and PWM0 as backlight is
   confirmed. The idle-timeout path and the interrupt event rule are not modelled.
3. **ALPU-MP handshake.** Port the handshake framing on the bus stock uses (bit-banged on GPIOA 5 and
   6), so the bypass sector can be dropped for at least a unit whose ALPU-MP answers the handshake.

### Tier 2: needed for the next features

4. **Clock manager state machine.** PLL lock (`pll_ld`) and clock-ready (`clk_rdy`), divider validity,
   and the hardware hand-over state.
5. **Boot ROM execution.** Load the 2 KB ROM image mirrored across 64 KB and stop forcing the entry point.
   The ROM is described in [boot-rom](boot-rom.md).
6. **Codec audio.** The registers are modelled; samples do not flow. A path from the built-in
   vocoder's buffers to a QEMU audio backend, through the DAC gain and mute, is the next step.
7. **Watchdog timing.** Confirm the clock on hardware (the timeout formula and stock's non-use are
   settled).
8. **RTC alarms.** The MATCH alarm is modelled. The open assumptions are the register pointer
   auto-increment and the VAL latch.
9. **Map the remaining windows that only log.** USB and SDIO, each with a device model where one is
   known.

### Tier 3: completeness

10. **LCD bus timing.** Needed for any performance question.
11. **Cache coherency checks** (stale XIP lines after a program-flash write) once the LCSFC
    programming path from item 1 exists.
12. **FM subsystem, RDA5807M audio and RDS, the built-in vocoder interface**, and an eFuse ID source
    for SPI_Master3.
13. **RF and vocoder paths**: received audio and DMR bursts from the RF environment, transmit, and the
    meaning of the FD6818's REG_0CH bits 10-11 and 14-15, for RF bring-up.

### Hardware verification list

These items are modelled or assumed but have not been confirmed on a radio:

- UART1-3: pins and PIC lines.
- Watchdog: the counter clock (`DM32_WDT_HZ`), and which blocks its reset reaches.
- Pin identities: the knob on 0x26/0x27, PTT on 0x2b, and power on 0x11. The injectors use these as
  hypotheses from the project pin map.
- PTT polarity. Active-low is an assumption.
- PWM register meanings. Codec: how IFR flags clear, what starts its timer, when pon_ack and
  standby_lo change within the DS's bounds, and whether CODEC_I2C_MUX cuts the register window.
- FD6818: the PLL lock time (40 us, an upper bound measured once), the GPIO0/1 wiring to GPIOB 27/26
  (pin map only), the REG_25H reset value (the model takes "no pin driven" to match `gpio_in_val`
  0x6B; the guide gives 0x0000), the board levels of GPIO2-7, and the RSSI offsets of REG_62H.
