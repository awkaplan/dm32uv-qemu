# Devices

Reference for each device model in the `dm32` board (`hw/csky/dm32.c`): its address window, source
file, interrupt wiring, the registers and behaviour it models, what it does not model, the
environment variables it reads, and what stock firmware was observed to do against it. Environment
variables are named here and explained in [environment.md](environment.md). Subsystem grades and
open gaps are in [coverage.md](coverage.md).

## Memory map

Taken from `hw/csky/dm32.c`. Sizes are the window the board maps.

| Address range | Size | Contents | Model or source |
|---|---|---|---|
| `0x00000000`-`0x0000FFFF` | 64 KB | Low RAM (CK803S vector area) | RAM window, [dm32.c](../hw/csky/dm32.c) |
| `0x00010000`-`0x00057FFF` | 288 KB | IRAM, loaded from an IRAM image | RAM window, [dm32.c](../hw/csky/dm32.c) |
| `0x03000000`-`0x030FFFFF` | 1 MB | Program flash (execute in place) | ROM device, [dm32.c](../hw/csky/dm32.c) |
| `0x03100000`-`0x03FFFFFF` | 15 MB | Rest of the LCSFC window | unimplemented, logged (`dm32.lcsfc`) |
| `0x04000000`-`0x04000FFF` | 4 KB | SSI instance; by structural evidence a raw SPI controller for the program flash | [dm32_ssi.c](../hw/misc/dm32_ssi.c) |
| `0x11000000`-`0x1100FFFF` | 64 KB | Clock, pin-mux, DMR Layer-2 registers | [dm32_sysreg.c](../hw/misc/dm32_sysreg.c) |
| `0x12000000`-`0x12000FFF` | 4 KB | LCD controller and 8080 panel bus | [dm32_lcd.c](../hw/display/dm32_lcd.c) |
| `0x13000000`-`0x1303FFFF` | 256 KB | USB | unimplemented, logged |
| `0x14000000`-`0x14000FFF` | 4 KB | Timer block (6 channels) | [dm32_timer.c](../hw/timer/dm32_timer.c) |
| `0x14010000`-`0x14010FFF` | 4 KB | Watchdog | [dm32_wdt.c](../hw/misc/dm32_wdt.c) |
| `0x14020000`-`0x14020FFF` | 4 KB | GPIO bank A | [dm32_gpio.c](../hw/gpio/dm32_gpio.c) |
| `0x14030000` | 64 KB slot | UART0 | `csky_uart`, see [UARTs](#uarts-and-unimplemented-windows) |
| `0x14040000` | 64 KB slot | UART1 (GPS port) | `csky_uart` |
| `0x14050000` | 64 KB slot | UART2 | `csky_uart` |
| `0x14060000` | 256 B | I2C0, not used by stock, nothing on the bus | [dm32_i2c.c](../hw/misc/dm32_i2c.c) |
| `0x14070000` | 256 B | I2C1, not used by stock, nothing on the bus | [dm32_i2c.c](../hw/misc/dm32_i2c.c) |
| `0x14080000` | 256 B | I2C2 controller; RTC slave at address `0x70` | [dm32_i2c.c](../hw/misc/dm32_i2c.c) |
| `0x14090000` | 64 KB slot | UART3 | `csky_uart` |
| `0x140A0000` | 4 KB | SSI0, W25Q128 data chip | [dm32_ssi.c](../hw/misc/dm32_ssi.c) |
| `0x140B0000` | 4 KB | SSI1 | [dm32_ssi.c](../hw/misc/dm32_ssi.c) |
| `0x140C0000` | 64 KB | PWM | [dm32_pwm.c](../hw/misc/dm32_pwm.c) |
| `0x140D0000` | 64 KB | ADC | [dm32_adc.c](../hw/misc/dm32_adc.c) |
| `0x140E0000` | 4 KB | SSI2 | [dm32_ssi.c](../hw/misc/dm32_ssi.c) |
| `0x140F0000` | 64 KB | DAC (CPU side) | [dm32_dac.c](../hw/misc/dm32_dac.c) |
| `0x14100000` | 4 KB | GPIO bank B | [dm32_gpio.c](../hw/gpio/dm32_gpio.c) |
| `0x14110000` | 4 KB | GPIO bank C | [dm32_gpio.c](../hw/gpio/dm32_gpio.c) |
| `0x14120000` | 256 B | SPI slave 0 | [dm32_spi_slave.c](../hw/misc/dm32_spi_slave.c) |
| `0x14130000` | 256 B | SPI slave 1 | [dm32_spi_slave.c](../hw/misc/dm32_spi_slave.c) |
| `0x14140000` | 256 B | SPI slave 2 | [dm32_spi_slave.c](../hw/misc/dm32_spi_slave.c) |
| `0x14150000` | 4 KB | SSI3 (SPI master 3) | [dm32_ssi.c](../hw/misc/dm32_ssi.c) |
| `0x15000000` | 64 KB | SDIO | unimplemented, logged |
| `0x16000000` | 64 KB | DMR baseband RAM | [dm32_dmrram.c](../hw/misc/dm32_dmrram.c) |
| `0x160009C0` | 64 B | Audio codec registers, over the DMR RAM window | [dm32_codec.c](../hw/misc/dm32_codec.c) |
| `0x17000000` | 4 KB | Interrupt controller (PIC) | [dm32_pic.c](../hw/intc/dm32_pic.c) |
| `0x18000000` | 32 KB | SRAM, loaded from an SRAM image | RAM window, [dm32.c](../hw/csky/dm32.c) |
| `0xE000F000` | 4 KB | CK803 cache controller | [dm32_cache.c](../hw/misc/dm32_cache.c) |

Every peripheral block in the datasheet's address map (HR_C7000 v2.7 section 4.5.3, Table 26) also has
an unimplemented window under its model, so the part of a block that no model covers is logged under
`-d unimp` (see [Unimplemented windows](#unimplemented-windows)). Addresses outside every block read 0,
drop writes and are logged under `-d invalid_mem` (see [Unassigned addresses](#unassigned-addresses)).

## Board and memory

Source: [dm32.c](../hw/csky/dm32.c). Core and memories are created in `dm32_init()`.

Modelled:

- CPU is `ck803` with the DSP feature bits `ABIV2_DSP` and `ABIV2_DSP2` added. Stock executes DSP
  instructions such as `muls`. Default CPU frequency is 192 MHz.
- Reset entry. At every reset `vbr` is set to `0x03000004` and `pc` to `0x03001000`. The word at
  `0x03000000` is the `DH_FLAG` header, not code, so the reset vector is read from `+4`. The mask
  boot ROM is not executed; see [boot-rom.md](boot-rom.md).
- Program flash, `0x03000000`, 1 MB. It is a ROM device: reads come from the backing store, and
  guest stores are dropped. The `-kernel` image is loaded into the backing store. Each dropped store
  is logged with its address, size, value and an approximate PC. The first 20 are printed to stderr,
  and a total is printed at exit if more than 20 were dropped. The 1 MB image is a GD25Q80 executed
  in place through LCSFC.
- IRAM, `0x00010000`, 288 KB, plain RAM. Loaded with `-device loader,addr=0x00010000`.
- SRAM, `0x18000000`, 32 KB, plain RAM. Loaded with `-device loader,addr=0x18000000`. Bytes the
  loader does not set read 0. On hardware, SRAM the firmware never initialises holds power-on noise
  (dm32uv-re `docs/live-ram-capture.md`).
- Power-on fill. With `DM32_RAM_FILL=noise`, IRAM and SRAM are filled with seeded pseudo-random
  bytes when the board is created, before `-device loader` images are written at reset, so only
  the bytes no image covers keep the noise. A system reset does not refill RAM. The low 64 KB and
  the cache window are not filled.
- Low RAM, `0x00000000`, 64 KB, plain RAM. The mask boot ROM is not mapped.
- Cache controller, `0xE000F000`, 4 KB: see [Cache controller](#cache-controller).

Not modelled:

- The 2 KB mask boot ROM, and the boot-mode decision it makes.
- The LCSFC command engine. Stores into the flash window are not forwarded to any engine.
- The LCSFC interrupt. The datasheet lists PIC source 24; no line is wired.
- Program flash above `0x030FFFFF`. Stock's writes at `0x03800000` are the page-program data port of
  the LCSFC store interface, by structural evidence (see below).

Environment: `DM32_FLASH_WRITABLE` (see [environment.md](environment.md#board-and-flash)).

Evidence and open items:

- A stock boot to standby and menu runs with the flash read-only. The run in the source's
  verification log dropped 2 stores.
- Open: stock code at flash `0x030DB8C4` and `0x03003D5x` stores `0x06` (WREN), `0x04` (WRDI), and a
  word on the path that uses `0x20`, all to `0x03000000`, then reads the address back. This looks
  like a SPI-NOR command path through LCSFC. With `DM32_FLASH_WRITABLE=1` the readback returns the
  command byte; with the default ROM it returns flash contents, so the two modes take different
  branches. Whether LCSFC forwards window stores on silicon is not established by any source here.
  dm32uv-re's `specs/rdl/gd25q80.rdl` decodes the encoding structurally: the dispatcher is IRAM
  `0x0002bca4` (copied to flash `0x030DB8C4`), sector erase is the word `(0x20 << 24) | addr` stored to
  `0x03000000`, and page program is the word `(0x02 << 24) | addr` stored to `0x03800000`, then the
  data bytes to `0x03800000` and the last byte to `0x03000000`. Completion is polled by re-reading
  the DH_FLAG word through XIP. See [coverage.md](coverage.md#program-flash-window-used-as-a-command-path).

## PIC

Base `0x17000000`, window `0x1000`. Source: [dm32_pic.c](../hw/intc/dm32_pic.c).

Interrupt wiring. The PIC has 64 inputs (`qdev` GPIO inputs 0-63) and one output to the CPU
interrupt input (`cpu_intc[0]`), in csky-qemu's `csky_vic_v1` encoding: the vector in bits 7:0 ORed
with bit 10. The output's value is also the read-only QOM property `cpu-line` (0 when nothing is in
service), which the qtests read.

| PIC source | Driver |
|---|---|
| 1 | Timer channel 0, the RTOS tick |
| 2-6 | Timer channels 1-5. Channel 5 is stock's UI tick |
| 10 | UART1 |
| 11 | UART2 |
| 12 | UART0 |
| 13 | UART3 |
| 14 | GPIO bank B |
| 15 | GPIO bank A |
| 17 | GPIO bank C |
| 30 | `dm32_sysreg` TX-slot interrupt, held until `INTERP_CLEAR` |
| 34, 35, 36 | SPI slaves 0, 1, 2 |

Registers (offsets from `0x17000000`):

| Offset | Name | Reset | Model |
|---|---|---|---|
| `0x00`, `0x60` | PIC_MODE, PIC_MODE_1 | `0xFFFFFFFF` | Trigger: 1 = edge, 0 = level |
| `0x04`, `0x64` | PIC_PO, PIC_PO_1 | `0xFFFFFFFF` | Polarity: 1 = rising or high, 0 = falling or low |
| `0x08`, `0x68` | PIC_MASK, PIC_MASK_1 | `0xFFFFFFFF` | 1 = masked. A masked source detects no request |
| `0x0C` | PIC_VECTOR | `0x20` | vector0 [6:0]: source `x` is vector `x + vector0` |
| `0x10` | PIC_COW1 | - | Write-only. `eoi` [2] ends the source in service; `feoi` [3] has no effect |
| `0x14`-`0x30`, `0x6C`-`0x88` | PIC_PRIORn_m | byte `n` = `n` | One priority byte per source, smaller wins, used when COW2.priority is 1 |
| `0x34` | PIC_COW2 | `0x2` | `avec` [1]: 0 delivers the core autovector 10. `priority` [0]: 1 uses PIC_PRIOR |
| `0x38` | PIC_SYNC | `0` | min_interval [7:0], stored with no effect |
| `0x3C`, `0x8C` | PIC_FFLAG_L, PIC_FFLAG_H | `0` | Stored with no effect: the C7000 has no fast interrupts |
| `0x40` | PIC_RECORD_SEL | `0` | [6:0], the source PIC_INT_CNT counts; 64-127 select none |
| `0x44`, `0x48` | PIC_INT_ST, PIC_INT_ST_1 | `0` | One record per request, write 1 to clear. Status only |
| `0x4C` | PIC_INT_CNT | `0` | Read-only count of the selected source's requests |

Accesses are 32-bit; byte and halfword accesses are rejected. Datasheet: HR_C7000 v2.7 section 4.7,
p.166-183. `PIC_PRIOR32_35` resets to `0x23222120` as the SVD gives it; the datasheet prints
`0x27262524`, a copy of the next row.

Requests. A line is active at the level `PIC_PO` selects. A request starts when an unmasked line
becomes active, or when an active line is unmasked. A level source requests while its line stays
active and unmasked. An edge source's request is latched until the source enters service. Each
request start sets the source's `PIC_INT_ST` bit (once, until cleared) and counts in `PIC_INT_CNT`
when `PIC_RECORD_SEL` names the source; clearing that source's bit clears the count. The records
are status: clearing them does not withdraw a request.

Delivery. One source is in service at a time. When none is, arbitration picks a requesting source:
the lowest number when `PIC_COW2.priority` is 0, the smallest `PIC_PRIOR` byte when it is 1. The
PIC drives that source's vector until `PIC_COW1.eoi`. A source in service is never preempted; a
higher-priority request waits for the EOI, and the EOI ends the one source in service. A level
source also leaves service when its request ends before the EOI.

Assumptions (the datasheet does not say):

- Equal `PIC_PRIOR` bytes go to the lower source number. The bytes are static priorities; the
  datasheet calls them "priority counter initial values" but describes no counting.
- A level source leaves service when its line goes inactive or it is masked. Stock writes the EOI
  before calling the ISR, while the line is still active. The source re-enters service then and
  leaves it when the ISR clears the device, so the core sees one interrupt per event.
- Unmasking an active line is a request in either mode, and a latched edge survives a later mask.
- A `PIC_INT_ST` write clears `PIC_INT_CNT` only when it clears the selected source's bit.

Environment: none.

Evidence:

- The source comments in `dm32.c` record that the RTOS tick must be on source 1 (vector 33). On
  hardware, unmasking source 33 instead gave an expired timer holding its line with no ISR. Source 1
  gave an ISR.
- Stock's IRQ dispatcher at IRAM `0x1064A` calls `OSIntEnter`, writes `PIC_COW1 = 4` (the EOI),
  reads the vector from PSR, then calls the ISR from the table. It does not re-enable interrupts
  before `rte`, so delivery never nests.
- Stock programs the PIC once at boot (`pic_init_core` at `0x03001CA8`, duplicated at `0x0306CFC8`):
  `PIC_MODE` and `PIC_MODE_1` 0 (level), `PIC_PO` and `PIC_PO_1` all ones (high), both masks all
  ones, and both `PIC_INT_ST` registers cleared. That matches Table 30, which lists every source as
  level-triggered, active high. It never accesses `PIC_VECTOR`, the priority registers, `PIC_COW2`,
  `PIC_SYNC`, `PIC_FFLAG`, `PIC_RECORD_SEL` or `PIC_INT_CNT`. It unmasks sources 1-6, 10, 12, 14,
  15, 17 and 27-33 through `irq_controller_unmask_and_register_handler` at `0x0306CF70`. Static
  analysis of IRAM and program flash.
- Under emulation, stock boot takes exactly one vector-33 interrupt per timer channel 0 expiry
  (3,500 of each in a 25 s run): no re-entry after the early EOI, and no lost ticks.
- `tests/qtest/test_pic.py` covers every register, both trigger modes and polarities, arbitration
  in both priority modes, the vector base and autovector, records and counts, and the negative
  cases. `tests/qtest/test_timer.py` replays the stock tick sequence.

## Timer

Base `0x14000000`, window `0x1000`, six channels at a `0x14` stride. Source:
[dm32_timer.c](../hw/timer/dm32_timer.c).

Interrupt wiring: channel `n` drives PIC source `n + 1`, so channel 0 is source 1 and channel 5 is
source 6.

Clock: 84 MHz. The LoadCount values read from hardware (840000, 336000 and 1680000) give exact 10
ms, 4 ms and 20 ms periods at 84 MHz and non-round periods at other candidate clocks. That is the
basis for the clock.

Per-channel registers (channel `n` at `0x14 * n`):

| Offset | Name | Model |
|---|---|---|
| `+0x00` | LoadCount | Loaded at each enable, and at each reload in mode 1. A write to a running channel takes effect at the next reload. Zero is treated as `0xFFFFFFFF` |
| `+0x04` | CurrentValue | Live count, counting down. Read-only (writes ignored); a disabled channel keeps its count |
| `+0x08` | ControlReg | Bit 0 enable (0-to-1 loads LoadCount). Bit 1 mode: 1 user-defined (reloads LoadCount at 0), 0 free-running (reloads `0xFFFFFFFF` at 0). Bit 2 interrupt mask, 1 = masked |
| `+0x0C` | EOI | Read or write clears this channel's interrupt |
| `+0x10` | IntStatus | 1 when pending and unmasked |

Global registers:

| Offset | Name | Model |
|---|---|---|
| `0xA0` | TimersIntStatus | Bit `n` set when channel `n` is pending and unmasked |
| `0xA4` | TimersEOI | Read clears all channels |
| `0xA8` | TimersRawIntStatus | Bit `n` set when channel `n` is pending, regardless of mask |
| `0xAC` | TIMERS_COMP_VERSION | `0x3230322A` (HR_C7000 v2.7 section 4.8.5.34, p.200) |

At reset every channel is disabled, unmasked, with LoadCount 0 and nothing pending (SVD reset
values); a machine reset, such as a watchdog bite, returns them there. Stock programs channel 0
itself (`timer_ch0_configure_periodic` at `0x0306CEF4`).

Modes. The datasheet lists "free running and user-defined counting" (section 4.8.3, p.188) but not
which TimerMode value is which, what each reloads, or when a LoadCount write takes effect. The model
follows the DesignWare DW_apb_timers convention the block is built from, and the SVD's "1 =
user/periodic". Stock writes mode 1 for every channel it uses and re-periods running channels
without disabling them (`mmio_14000000_timer_config` at `0x0301D900`). The datasheet documents no
prescaler and no one-shot mode.

The clock is a constant, not taken from the clock manager; that waits for the clock tree
([coverage plan, item 26](coverage-plan.md#26-clock-manager-and-clock-tree-l)).

Environment: none.

Evidence:

- Channel 0 on source 1 is confirmed on hardware (see the PIC evidence above).
- Channel 5 is stock's UI tick. Its enable path, `timer_ch4_enable` at `0x0301D784`, is what the
  board's channel-5 wiring was added for. Confirmed under emulation.
- Stock uses all six channels, every one in mode 1, and acknowledges each in its ISR by reading the
  channel's EOI (channel 0: the RTOS tick, IRQ 0x21; channels 1-5: ISRs at IRAM `0x0002CA78`-
  `0x0002CAB8`, registered for IRQ 0x22-0x26). Static analysis of IRAM and program flash.
- `tests/qtest/test_timer.py` verifies every channel's period and PIC source, both modes, the
  deferred LoadCount write, EOI, and reset.

## GPIO

Banks: A at `0x14020000`, B at `0x14100000`, C at `0x14110000`. Each window is `0x1000`. Source:
[dm32_gpio.c](../hw/gpio/dm32_gpio.c).

Interrupt wiring: bank A to PIC 15, bank B to PIC 14, bank C to PIC 17. Each bank has one IRQ line,
high while `INTSTATUS` is non-zero. The banks must be created in order A, B, C, because the code
indexes them by creation order.

Registers (Synopsys DesignWare port-A layout, per bank):

| Offset | Name | Model |
|---|---|---|
| `+0x00` | SWPORTA_DR | Output data. Bank A bits 7, 8 and 24 drive the FD6818 bus; see [FD6818](#fd6818) |
| `+0x04` | SWPORTA_DDR | 1 = output. Output pins are excluded from interrupts. A pin reaches its pad only while IO_DIPLEX selects GPIO |
| `+0x30` | INTEN | Per-bit enable. Effective only on input pins whose pad is on GPIO |
| `+0x34` | INTMASK | Masks `INTSTATUS`, not `RAW` |
| `+0x38` | INTTYPE_LEVEL | 0 = level, 1 = edge. Changing a bit's type clears its raw state |
| `+0x3C` | INT_POLARITY | 0 = active low or falling, 1 = active high or rising |
| `+0x40` | INTSTATUS | Read-only, `RAW & ~INTMASK` |
| `+0x44` | RAW_INTSTATUS | Read-only. Level bits follow the pin; edge bits latch until EOI |
| `+0x48` | DEBOUNCE | Per bit: the interrupt block sees a change only after it has held for 61 us; a shorter pulse is lost. `EXT_PORTA` is not debounced |
| `+0x4C` | PORTA_EOI | Write 1 to clear edge bits. Reads 0 |
| `+0x50` | EXT_PORTA | Read-only pad levels (see below). Each read re-evaluates the interrupt block |
| `+0x78` | INTR_BOTH_EDGE | Per bit: 1 = the edge bit triggers on either edge, 0 = on the edge INT_POLARITY selects |

Input levels. `EXT_PORTA` reads each pad's level, built in this order:

1. An undriven pad reads high, or its SoC pull when `IOMGR_REN` enables one (pull-up 1, pull-down 0;
   the direction is fixed per pad, sections 4.4.4.7-4.4.4.9).
2. The idle-low list (`DM32_GPIO_IDLE_LOW`, default bank A bit 16, the power-sense input) forces
   board-level pulls the SoC does not know about to 0, over any SoC pull.
3. Injected levels from `DM32_PINSEQ`, `DM32_KNOB` and `DM32_PTT`, in virtual time.
4. A pin GPIO drives (`SWPORTA_DDR` set and the pad on GPIO) reads its `SWPORTA_DR` bit.
5. Attached devices pull their pads low (wired-AND).
6. Keypad rows held by `DM32_KEY`/`DM32_KEYSEQ`: a row reads low only while its key's column pad is
   driven low, sampled at the read. The injection is clocked by `EXT_PORTA` reads, and the rows it
   holds persist until the next read.
7. A pad whose `IOMGR_IE` bit is 0, or which IO_DIPLEX gives to another block, reads 0 and cannot
   interrupt. At reset that is GPIOA 0-4, whose pads start on JTAG.

`EXT_PORTA` is read-only; writes to `+0x50` are ignored.

Assumptions (the datasheet's GPIO chapter, section 5.7, is brief):

- A pad with no pull reads high: the keys and boot-mode pins have board pull-ups.
- GPIO reads 0 from a pad given to another block.
- Debounce follows the DesignWare APB GPIO: it filters the interrupt inputs only, and a change must
  hold for two cycles of the debounce clock. The datasheet says only that the block "supports
  de-jitter" with a configurable sampling time and names no clock; the model uses the 32.768 kHz
  RTC clock, so the window is 61 us. Stock sets `DEBOUNCE = 0xFFFFFFFF` on every bank.

Attached devices. A device model can attach to a pad (`dm32_gpio_attach()` in
[dm32.h](../include/hw/csky/dm32.h); pads are numbered `bank * 32 + bit`). It is told the SoC side
of the pad (driven or not, and the `SWPORTA_DR` bit) whenever a `SWPORTA_DR` or `SWPORTA_DDR` write
changes it, or IO_DIPLEX moves the pad, and it puts its own level on the pad. An attached pad reads
wired-AND: the SoC's `SWPORTA_DR` bit while GPIO drives it (otherwise its idle or injected level),
and low if any attached device pulls it low. The stock bit-bang drivers drive both levels, so no open-drain
configuration is needed. Unattached pads read as described above. Attached today:

| Pads | Device |
|---|---|
| GPIOA 7, 8, 24 | FD6818 CLK, DATA, CS ([FD6818](#fd6818)) |
| GPIOB 27, 26 | FD6818 GPIO0, GPIO1 ([FD6818](#fd6818)) |
| GPIOA 5 (SCL), 6 (SDA) | Bit-banged I2C bus `alpu` (ALPU-MP, not modelled) |
| GPIOA 26 (SCL), 25 (SDA) | Bit-banged I2C bus `fm`, with the RDA5807M tuner ([RDA5807M](#rda5807m-fm-tuner)) |

The two I2C buses bridge the pads to QEMU's `bitbang_i2c`, so any QEMU I2C slave can be put on them
with `-device <type>,bus=alpu` or `bus=fm`. A line the SoC does not drive is high (the bus
pull-up), and a slave pulling SDA low (an ACK or a 0 bit) pulls the pad low. SCL is not stretched.
`alpu` is empty, so every address NAKs; `fm` has the RDA5807M at 0x10. The board build includes the AT24C EEPROM
model for the bus tests (`tests/qtest/test_gpio_i2c.py`).

The stock bit-bang routine for the ALPU-MP (`i2c_read_block_0x7a` at IRAM `0x000144FC`) takes a bus
index from IRAM `0x33C54` and uses GPIOA `2 * idx + 5` / `2 * idx + 6`, so index 1 would drive the
FD6818's CLK and DATA pads. The only store to the index (IRAM `0x000141C4`, called from flash
`0x03021B46`) writes 0, so stock does not run that routine on the FD6818 pads.

Not modelled: synchronisation to the bank clock, and interrupt timing below QEMU's scheduling. The
debounce clock is an assumption (the 32.768 kHz RTC clock), as is the level of a pad with no pull.

Environment: `DM32_KEY`, `DM32_KEY_HOLD`, `DM32_KEYSEQ`, `DM32_KEYLOG`, `DM32_PINSEQ`, `DM32_KNOB`,
`DM32_KNOB_EDGE_MS`, `DM32_PTT`, `DM32_GPIO_IDLE_LOW`, `DM32_GPIOLOG`. See
[environment.md](environment.md#keypad-and-pin-injection). The keypad matrix pins are GPIOB 17-20
(logical 49-52) and GPIOC 7-11 (logical 71-75). Key grammar is in
[key-injection.md](key-injection.md).

Evidence:

- Stock's boot-mode check reads pins 43 and 51 (GPIOB bits 11 and 19). With idle-high inputs it
  takes the normal-boot branch instead of the serial recovery console at IRAM `0x12614`.
- Stock enables bank B bits 6 and 7 at boot with polarity 0. During Snake, keypad keys forced on
  `EXT_PORTA` reads raise bank B interrupts on bits 17, 18 and 20.
- Pin 43 (`0x2B`) is configured as an edge interrupt on both edges. The PTT active-low polarity is
  an assumption, based on `bitwise_not_of_boolean_result` at `0x03012F48`.
- With `DM32_GPIO_IDLE_LOW=none`, the power-off task parks on a "Low battery voltage" dialog. The
  idle-low default for bank A bit 16 prevents that park. The dialog is caused by this GPIO, not by
  the ADC.

## LCD

Base `0x12000000`, window `0x1000`. Source: [dm32_lcd.c](../hw/display/dm32_lcd.c).

Interrupt wiring: none.

Registers:

| Offset | Name | Model |
|---|---|---|
| `0x00` | Command | Write a MIPI-DCS command byte. Writes are dropped until `+0x28` bit 0 is set |
| `0x04` | Data | Write a command argument or pixel byte |
| `0x10` | Config | Stored and read back. Written by panel init |
| `0x28` | Status | Write bit 0 enables the controller. Read returns `0x3` (bits 0 and 1) once enabled, else `0x0` |

Commands decoded: CASET `0x2A`, RASET `0x2B`, RAMWR `0x2C`, MADCTL `0x36`. Other commands take their
arguments and have no effect. CASET and RASET latch each coordinate as its bytes arrive, so a
two-byte cursor set (the firmware's `display_set_cursor_pos` at `0x03017978`) is applied. RAMWR
takes RGB565 pixels, high byte first, and advances inside the window.

Framebuffer: GRAM is 256 by 320 pixels in controller coordinates. The displayed panel is 240 by 320
at offset `(off_x, off_y)`. MADCTL is decoded and logged, but not applied to the view: the
controller has already mapped pixels into GRAM by the time they are stored.

Panel data path: while `IO_DIPLEX2` bits 9-15 hand DB0-DB4 to the keypad (see [system
registers](#system-registers)), panel data bytes are replaced with `0xFF`. The first such byte
prints a warning.

Not modelled: bus timing, reads from the panel path (reads return 0 except `+0x10` and `+0x28`), and
any effect of MADCTL on the displayed image.

Environment: `DM32_LCD_W`, `DM32_LCD_H`, `DM32_LCD_OFFX`, `DM32_LCD_OFFY`, `DM32_GRAMDUMP`,
`DM32_GRAMQUIET`, `DM32_GRAMSEQ`, `DM32_LCDLOG`. See
[environment.md](environment.md#lcd-and-frame-capture). The frame-capture procedure is in
[running.md](running.md).

Evidence:

- In a `DM32_LCDLOG` run of stock, its only window is CASET 0-239 and RASET 0-319 with MADCTL
  `0xC0`. That is a 240 by 320 portrait panel with no transposition.
- On hardware, a port that skipped the controller enable handshake (write `+0x10`, set `+0x28` bit
  0, wait for `+0x28` bit 1) booted with the backlight on and a black panel. The enable gate here
  reproduces that requirement.

## System registers

Base `0x11000000`, window `0x10000`. Source: [dm32_sysreg.c](../hw/misc/dm32_sysreg.c).

Interrupt wiring: TX-slot interrupt to PIC source 30.

The window is storage, except for the following registers.

| Offset | Name | Model |
|---|---|---|
| `0x00` | SYS_SOFT_RSTN | Per-module soft reset; reads `0x1FF`. See [Resets](#resets) |
| `0x04` | CLK_MGR_REG0x04 | Reads force bits 31:30 set. This makes stock's PLL-lock wait return at once |
| `0x34` | IO_DIPLEX0 | Stored. Reset `0x800FFFE0` (JTAG on its pads, the I2C, UART, PWM and PTB7 pads on GPIO) |
| `0x38` | IO_DIPLEX1 | Stored. Reset `0xFFFFFFFF`. Bits 8:3 select the SPI0 pads |
| `0x3C` | IO_DIPLEX2 | Stored. Reset `0x3FFFFFFB` (every pad on GPIO). Bits 19-26 select the ADC pads, bits 3-18 the LCD bus (and SPI2 on PTC3, 4, 7, 8) |
| `0x40`-`0x48` | IOMGR_REN_REG0-2 | Stored. Reset all ones (`0x7FFFFFFF` for REG2): every pull resistor off |
| `0x4C`-`0x54` | IOMGR_IE_IE0-2 | Stored. Reset all ones (`0x7FFFFFFF` for IE2): every input enabled |
| `0x88` | LINEOUT_CTRL | Bits 1:0 stored. Bit 31 standby_lo (read-only) is the codec's DAC standby; see [Codec](#codec) |
| `0x8C` | CODEC_I2C_MUX | Stored. Bit 7 set (I2C) cuts the codec's register window; see [Codec](#codec) |
| `0x100` | WORK_MODE | Stored. Used by `DM32_MODEM_LEADER` |
| `0x3B0` | INTERP_CLEAR | Bit 0 write clears the TX-slot interrupt |
| `0x400` | LAYER2_CONTROL | Bit 1 = tx_master_mode |
| `0x404` | LAYER2_SLOTON | Bit 0 = dll_tx_slot_on. Starts the slot counter when tx_master_mode is set |
| `0x408` | LAYER2_TXRX_CTRL | Stored. Writes of `0x40` or `0x41` while the counter runs are logged as TX-ORIGINATE |
| `0x41C` | LAYER2_STATUS | Low 9 bits read the live slot counter |

The slot timing (30 ms period, counter 0-287, TX-slot interrupt on wrap) is described in
[modem-timing.md](modem-timing.md).

Pad control. The model decodes the pad registers per pad, a pad being numbered as its GPIO line
(`bank * 32 + bit`, as `DM32_PINSEQ` numbers pins). A table in `dm32_sysreg.c` gives each of the 87
GPIO pads that has one its `IO_DIPLEX` select field (from the SVD, with the guide's `LCD_DB7` typo
corrected to PTC14), its pull-resistor bit and direction, and its input-enable bit (HR_C7000 v2.7
sections 4.4.4.4-4.4.4.12; the English and Chinese editions agree on every pull direction). Other
devices query it through `include/hw/csky/dm32.h`:

| Function | Returns |
|---|---|
| `dm32_pad_function(pad)` | The pad's select value, or -1 if it has no select field |
| `dm32_pad_is_gpio(pad)` | Whether the pad is on its GPIO line |
| `dm32_pad_pull(pad)` | `DM32_PULL_NONE`, `_UP` or `_DOWN`; none when the resistor is off |
| `dm32_pad_input_enabled(pad)` | Whether the input buffer is on; true for a pad with no enable bit |
| `dm32_pad_add_notifier(n)` | Registers a notifier called after any pad-register write |

Users, every block with pads (HR_C7000 v2.7 sections 4.4.4.4-4.4.4.6 for the select values):

| Block | Pads and select | Without its pads |
|---|---|---|
| GPIO | Each pin's pad, on its GPIO value | The output does not reach the pad (attached devices see it undriven). Driving it, by setting the `SWPORTA_DDR` bit or changing `SWPORTA_DR`, counts `pad_conflict`, with one stderr warning per pad. A pad whose `IOMGR_IE` bit is 0 reads 0 |
| SPI0 | PTA20-22, value 0 | DR write dropped (`ssi_discard`) |
| SPI1 | PTA25-27, value 0 | DR write dropped (`ssi_discard`) |
| SPI2 | PTC4, PTC8, PTC7 (the LCD RS, DB1 and DB0 pads), value 1 | DR write dropped (`ssi_discard`) |
| SPI3, `0x04000000` | Dedicated (SPI3 is "EFUSE only"; the program flash has its own NFC pads) | Always connected |
| I2C0, I2C1 | PTA5/6, PTA7/8, value 0 | The address phase NAKs (`ABRT_7B_ADDR_NOACK`) without touching the bus; `pad_unclaimed` |
| I2C2 | None, internal to the RTC | Always connected |
| PWM0-2 | PTA15-17, value 0 | Channel start counts `pad_unclaimed` (the output is not modelled) |
| UART1-3 | TXD on PTA10, PTA12, PTA14, value 0 | A transmitted byte is dropped and counts `pad_unclaimed`. Receive is not gated. UART0 has dedicated pads |
| ADC | PTC(15 + n), value 0 | Conversion reads 0 (`adc_pad`) |
| LCD | DB0-DB4 (PTC7-11), value 0 | Data byte forced to `0xFF` (`lcd_mux`) |

The UART check is a 16-byte window over each csky_uart's THR and LCR in `hw/csky/dm32.c`, which
forwards every access and drops only a transmitted byte (LCR bit 7, the divisor latch, is tracked so
that divisor writes pass). The CLK_OUT pad (PTC0, `clk_out_sel`, IO_DIPLEX2 bits 1:0) is covered by
the GPIO rule: once stock selects the PLLA clock output there, GPIO no longer drives the pad.
Assumptions: the datasheet does not say what a block sees on pads it does not own, or whether the
GPIO input reads a pad given to a peripheral; the model reads the pad. No signal can be routed to two
pads (each has one field), so the only way two blocks claim one pad is GPIO driving a peripheral's
pad. The SVD names `+0x40` and `+0x48` reserved; the datasheet's register
table is followed. LCD_NRESET's pull and enable bits are taken to be pad PTC2, which the board DTS
names LCD_RESET_N (assumption). The UART0 and program-flash pads have pull and enable bits but no
GPIO line, and are not in the table.

Not modelled: PLL state, invalid divider detection, clock-tree effects, and every other clock
register beyond storage.

Environment: `DM32_MODEM_LOG`, `DM32_MODEM_LEADER`. See
[environment.md](environment.md#system-and-modem).

Evidence:

- `IO_DIPLEX2` resets to `0x3FFFFFFB` (datasheet field defaults and SVD). Stock claims the LCD bus
  with `IO_DIPLEX2 &= 0xFFF80107` (`lcd_controller_power_on_init_sequence` at `0x03017D86`) and
  selects CLK_OUT (bits 1:0 = 0, `hr_c7000_modem_core_reset_and_poll_ready` at `0x03004468`), which
  gives `0x3FF80100`, the value read on hardware after boot.
- Stock's pad writes (static analysis of IRAM and program flash): at boot, LCD bus, CLK_OUT, ADC1
  and ADC2, DAC_VOUT_MCUB and MCUC, PWM_0, the SPI0 master's SCLK/MOSI/MISO (CSN0 stays GPIO; GPIOA 18
  is the data chip's select), PTB2-4 to GPIO, UART1 RXD only (TXD stays GPIO), and two pull bits in
  `IOMGR_REN_REG0`. At run time only the keypad scan's `IO_DIPLEX2 |= 0xFE00` and its restore. It
  never writes `IOMGR_IE`. SPI1's master pads are claimed only on the SD-card probe path.
- Stock drives GPIOC 13 (LCD_DB6) as a keypad column while the LCD owns the pad
  (`menu_power_on_display_and_keypad_poll_loop` at `0x0305F128`, and the runtime scan): its scan
  hands only DB0-DB4 to GPIO. A stock boot therefore counts `pad_conflict` (4 at boot, more while
  keys are pressed). On silicon that level cannot reach the pad; the model's keypad injection does
  not depend on it, so keys still work here. Whether this is a stock quirk or the datasheet's DB5-DB7
  pad assignment is wrong is open.
- Stock's keypad scan sets `IO_DIPLEX2 |= 0xFE00`, and the scan's restore clears it. A patch that
  scans the keypad and omits the restore leaves the panel unwritable here, as it does on hardware.

## SSI and SPI-NOR

Five instances are created by `dm32.c`, each with a 4 KB window: `0x140A0000` (SSI0), `0x140B0000`
(SSI1), `0x140E0000` (SSI2), `0x14150000` (SSI3) and `0x04000000`. Source:
[dm32_ssi.c](../hw/misc/dm32_ssi.c).

Interrupt wiring: SSI0 to PIC source 16, SSI1 to 22, SSI2 to 23 (Table 30). SSI3 and `0x04000000`
have none.

The stock SSI driver indexes a base table at flash `0x030ADA00`. The table lists `0x140A0000`,
`0x140B0000`, `0x140E0000`, `0x14150000` and `0x04000000`.

The `0x04000000` instance is, by structural evidence, a raw SPI controller for the program flash, not
for the data chip. Its only user, IRAM `0x0002bb98`, stores the base, initialises the controller
through `0x0002bb28`, and sends the GD25Q80 quad-enable sequence `35h`, `06h`, `01h 00h 02h`, `05h`,
then sets `QUAD_ENABLE` (`0x1100005C`). Its chip select is the controller's own SER bit 0, not the
data chip's GPIOA 18. Source: dm32uv-re `specs/rdl/gd25q80.rdl`. It has the program flash's
dedicated pads, so it is not pad-gated, and the sequence reaches the instance's SPI-NOR model, which
is the data chip's (W25Q128) until the program flash has its own.

No stock use of SSI1 or SSI2 was found. SSI3 reads the eFuse bytes described below.

Registers (HR_C7000 v2.7 section 5.2, Register Table 11; SVD SPI_Master0):

| Offset | Name | Reset | Model |
|---|---|---|---|
| `0x00` | CTRLR0 | `0x7` | `[9:8]` TMOD, `[7]` SCPOL, `[6]` SCPH, `[5]` bit order, `[4:0]` DFS (frame size - 1). Locked while enabled |
| `0x04` | CTRLR1 | `0` | `[15:0]` NDF: frames to receive - 1 in TMOD 2 and 3. Locked while enabled |
| `0x08` | SSIENR | `0` | `[0]`. Writing 0 empties both FIFOs and clears the interrupt status |
| `0x10` | SER | `0` | `[1:0]`, the controller's own slave selects; a frame starts only while it is non-zero |
| `0x14` | BAUDR | `0` | `[15:0]` SCKDV: SCLK = 84 MHz / SCKDV, even; 0 stops the clock. Locked while enabled |
| `0x18`, `0x1C` | TXFTLR, RXFTLR | `0` | FIFO thresholds. Locked while enabled |
| `0x20`, `0x24` | TXFLR, RXFLR | `0` | FIFO levels, 0-128 |
| `0x28` | SR | `0x6` | RFF, RFNE, TFE, TFNF, BUSY (set while a frame shifts) |
| `0x2C`, `0x30`, `0x34` | IMR, ISR, RISR | `0x3F`, `0`, `0` | RXF, RXO, RXU, TXO, TXE; ISR = RISR & IMR |
| `0x38`, `0x3C`, `0x40`, `0x48` | TXOICR, RXOICR, RXUICR, ICR | `0` | Read to clear TXO, RXO, RXU, or all three |
| `0x4C` | DMACR | `0` | Storage (not in the datasheet; stock writes 0 on SSI3 and `0x04000000`) |
| `0x58`, `0x5C` | IDR, SSI_COMP_VERSION | `0x11223344`, `0x3331312A` | Read-only |
| `0x60` | DR | - | Write pushes a frame to the TX FIFO; read pops the RX FIFO (`0xFF` when empty) |

Transfers. Both FIFOs hold 128 frames (section 5.2.2.1). While SSIENR is 1, SER is non-zero and
BAUDR is non-zero, the controller shifts one frame at a time in virtual time, each taking
`(DFS + 1) * SCKDV` cycles of the 84 MHz APB clock: 190 ns for stock's 8-bit frames at SCKDV 2. A
frame leaves the TX FIFO when it starts (TFE sets at once) and its received frame enters the RX
FIFO when it ends; BUSY is set in between. Progress is computed whenever the guest touches the
controller, so stock's per-byte polling (TFNF, write DR, TFE, BUSY clear, read DR, at IRAM
`0x2BF90`) sees real timing without a timer per byte. TMOD 0 transmits and receives; 1 drops what it
receives; 2 starts NDF + 1 received frames on a DR write, sending the written value each time; 3
sends the TX FIFO, then receives NDF + 1 frames. A frame shifted on pads that do not select the
controller (PTA20-22 for SPI0, PTA25-27 for SPI1, PTC4/8/7 for SPI2; SSI3 and `0x04000000` are
dedicated) is lost and counted as `ssi_discard`, and so is a DR write while SSIENR is 0.

Interrupts: RXF while the RX FIFO holds more than RXFTLR frames, TXE while the TX FIFO holds no more
than TXFTLR (enabled), TXO on a write to a full TX FIFO, RXO on a frame received into a full RX FIFO,
RXU on a read of an empty one. The line is ISR != 0; a timer runs only while an interrupt is enabled.

Chip selects. The data chip on SSI0 is selected by GPIOA 18 driven low, the line stock drives around
every operation (pin `0x12`), not by SER: the pad is attached to the GPIO model. The other instances
select their device with SER. Releasing the select ends the SPI-NOR command. With nothing selected
a frame reads `0xFF`.

Assumptions, from the DesignWare APB SSI the block is built from (the datasheet does not say): the
configuration lock list; the TXE, TXO, RXO and RXU rules; IMR bit 1 enabling; MISO idling high, so an
unselected chip or an empty FIFO reads `0xFF`; with DFS below 3 the frame size comes from bits
`[20:16]`, the DesignWare 32-bit layout that the `0x04000000` instance's `CTRLR0 = 0x70000` uses.
Devices see the low 8 bits of a frame.

SPI-NOR commands implemented:

| Command | Behaviour |
|---|---|
| `0x03`, `0x0B` | READ, 24-bit address, streams bytes |
| `0x05` | RDSR. Bit 1 WEL is live. Bit 0 WIP is set for the next N reads after a completed program or erase |
| `0x9F` | RDID returns `EF 40 18` (Winbond, SPI-NOR, 128 Mbit) |
| `0x06`, `0x04` | WREN, WRDI |
| `0x02` | Page program. Programming only clears bits (AND) |
| `0x20` | Sector erase, 4 KB |
| `0xD8` | Block erase, 64 KB |
| `0xC7`, `0x60` | Chip erase |

WRSR (`0x01`) is accepted with no effect. Programs and erases are ignored unless WEL is set.

Store: each instance owns a 16 MB store (`NOR_SIZE`), filled with `0xFF`. A file named by
`DM32_SPINOR` is read over the fill; a missing file is ignored. The store is the W25Q128 data chip
that holds the codeplug.

Persistence: `DM32_SPINOR_OUT` writes each instance changed by a program or erase at exit, to
`<path>.tmp`, then renames it into place. The first changed instance goes to `<path>`; later ones go
to `<path>.<idx>`. `DM32_SPINOR_SYNC=1` writes after each completed program or erase.

Not modelled: program and erase timing (WIP is a fixed count of RDSR reads), WRSR protection bits,
suspend and resume, OTP security registers, unique ID (`0x4B`), and the other opcodes.
Multi-instance output is implemented but was not exercised by any run.

Environment: `DM32_SPINOR`, `DM32_SPINOR_OUT`, `DM32_SPINOR_SYNC`, `DM32_SPINOR_WIP_READS`,
`DM32_SPINOR_LOG`. See [environment.md](environment.md#spi-nor-data-chip).

Evidence:

- In a stock boot to standby, the SPI-NOR is read 13,986 times, one tag-byte read at `+0xFFF` per 4
  KB page. Chip select is GPIOA bit 18 (pin `0x12`).
- Stock's writes in one run were 14 page-program pages in `0x001000`-`0x00E000`, and three pages at
  `0x6DC000`, `0x6DF000` and `0x6E0000`. The erase count differs between two recorded runs: nine 4 KB erases in one, and 27
  `ERASE` lines in the other. The two counts are not reconciled.
- Stock polls RDSR after each erase. With the default of 2 WIP reads, the poll completes on the
  third read.
- Stock derives its hardware-binding record from eFuse bytes read over SPI_Master3 (`0x14150000`).
  This model answers that read from its own store. The record is patched by
  [compose_qemu_image.py](https://github.com/awkaplan/dm32uv-re/blob/main/tools/src/dm32_patch_tools/compose_qemu_image.py)
  in dm32uv-re for that reason.

## ADC

Base `0x140D0000`, window `0x10000`. Source: [dm32_adc.c](../hw/misc/dm32_adc.c). Behaviour follows
HR_C7000 datasheet v2.7 section 5.9.

Interrupt wiring: none. `ADC_INTR_STA` latches but no line is driven. The datasheet lists PIC source
26.

Registers:

| Offset | Name | Model |
|---|---|---|
| `0x00` | ADC_CTRL | Bit 3 PD_FORCE_HIGH holds the ADC in reset. Bit 2 continuous mode |
| `0x04` | INTR_DELTA | Stored |
| `0x08` | ADC_INTR | Bit 16 master enable, bits 8:0 per-channel enable |
| `0x0C` | ADC_INTR_STA | Latched channel flags. Write 1 to clear |
| `0x10` | SCAN_TIME | Stored |
| `0x14` | ADC_START | Write bit 0 starts one conversion. Reads 0 |
| `0x18` | CTRL_STATE | Reads 0 (idle) |
| `0x1C` | INTR_THRES | Stored |
| `0x20` | CTRL_STOP | Write bit 1 resets the controller |
| `0x24` | CH_VLD | Bits 7:0 select channels |
| `0x28` | PD_SEOC_TIME | Reset `0x0A20` |
| `0x2C` | P2S_EN | Stored |
| `0x30`-`0x3C` | DATA_AB, DATA_CD, DATA_EF, DATA_GH | Each pair: even channel in bits 9:0, odd channel in bits 25:16. Reads 0 for a channel that has not converted |
| `0x40` | VERID | `0x20151216` |
| `0x44` | DIR_DAT_OP | Last conversion result |

A conversion is immediate. In single-scan mode only the lowest selected channel converts. A channel
whose IO_DIPLEX2 bit (`19 + ch`) still selects GPIO reads 0. The completion flag sets only if the
channel is enabled in ADC_INTR.

Input levels come from the environment, 10-bit values, one per channel. An unset channel reads 0.

Not modelled: settling time, noise, conversion timing, and any interrupt.

Environment: `DM32_ADC_CH0` to `DM32_ADC_CH7`, `DM32_ADC_VBAT`, `DM32_ADC_VBAT_CH`,
`DM32_ADC_VERBOSE`. See [environment.md](environment.md#adc).

Evidence:

- Stock's battery reader, `adc_read_channel_dispatch(2)`, reads channel C (index 2), not channel F.
  Feed stock's battery reading with `DM32_ADC_CH2`.

## DAC

Base `0x140F0000`, window `0x10000`. Source: [dm32_dac.c](../hw/misc/dm32_dac.c). This is the
CPU-side 12-bit DAC. The baseband TX DAC at `SystemControl.DAC_CONTROL` is a separate block and is
not modelled.

Interrupt wiring: none. The datasheet lists PIC source 25; no line is wired.

Registers:

| Offset | Name | Reset | Model |
|---|---|---|---|
| `0x00` | DAC_PD_CTRL | `0x7` | Bits 2:0 PD_C, PD_B, PD_A. 1 = powered off |
| `0x04` | DAC_PD_MODE_EN | `0x7` | Stored, no effect |
| `0x08` | DAC_DATA_A | 0 | 12-bit, firmware argument 1 |
| `0x0C` | DAC_DATA_B | 0 | 12-bit, firmware argument 2 |
| `0x10` | DAC_DATA_C | 0 | 12-bit, firmware argument 4 |
| `0x14` | DAC_CTRL_VERID | `0x20160229` | Read-write, stored |

A DATA write while that channel's PD bit is 1 is stored, and a warning is printed once per channel.
Accesses are 4-byte only. Other offsets are storage, with a one-time note.

Not modelled: the analogue output, PD_MODE_EN, conversion timing, and the interrupt.

Environment: `DM32_DAC_LOG`, `DM32_DAC_TRACE`. See [environment.md](environment.md#dac).

Evidence:

- Stock's init at `0x0301CC30` writes PD_MODE_EN=1, DATA_A=0, DATA_B=0, DATA_C=0, PD_CTRL=1, then
  DATA_C=`0x6E2`. The three data writes before PD_CTRL=1 are to powered-down channels, so the model
  prints three warnings in that boot.
- The boot-to-menu run makes no DAC reads.
- No beep or alert path in the corpus reaches the DAC. The TX-power ramp (`FUN_0002B364`) is not
  exercised by a boot-to-menu run. Its use of DATA_B is not verified.

## PWM

Base `0x140C0000`, window `0x10000`. Source: [dm32_pwm.c](../hw/misc/dm32_pwm.c).

Interrupt wiring: none. PIC source 18 is not wired, because the SVD does not state which waveform
event sets `pwmN_intr`.

Channels are at `0x20 * n` for `n` = 0, 1, 2:

| Offset | Name | Model |
|---|---|---|
| `+0x00` | CFG | Bit 0 start, 1 soft_rst, 2 cfg_update, 3 inten, 5:4 mode (1 one-shot, 2 continuous; 0 and 3 reserved), 7:6 evtrig (stored), 8 fpout, 9 inactout |
| `+0x04` | RPT | Bits 15:0, stored |
| `+0x08` | PER | Bits 31:0, stored. Period in system-clock cycles |
| `+0x0C` | FP | Bits 31:0, stored. First-phase length in cycles |
| `+0x10` | STATUS | Bits 7:0 and 9 stored. Bit 8 busy is derived from CFG |
| `+0xFF0` | PWM_INTR_STA | Reads 0, writes ignored |

A channel is running when start is set, soft_rst is clear, and the mode is 1 or 2. The output uses
latched PER and FP. Those latch on the start edge. In continuous mode they latch again on a
cfg_update 0-to-1 write. While not running, PER and FP writes take effect at once.

Not modelled: the waveform, the output pin level, the pulse counter, the evtrig external modes, and
the interrupt.

Environment: `DM32_PWM_LOG`, `DM32_PWM_TRACE`. See [environment.md](environment.md#pwm).

Evidence:

- Changing the Back Light menu level rewrites PWM0's FP register from 23990 to 3427, matching the
  stock level table `floor(24000 * 1428 / 10000)`. PWM0 therefore drives the backlight. The
  pad-to-LED connection is not measured on hardware. The board map labels GPIOA 15, the PWM0 pad, as
  the backlight.
- At boot the stock firmware writes PER 24000 and FP 23990, a duty of 0.9996.
- The Light Time (idle timeout) menu item was not characterised.

## Watchdog

Base `0x14010000`, window `0x1000`. Source: [dm32_wdt.c](../hw/misc/dm32_wdt.c).

Interrupt wiring: none. Expiry resets the machine after the reset pulse. The datasheet's interrupt
table lists no watchdog source.

Registers:

| Offset | Name | Reset | Model |
|---|---|---|---|
| `0x00` | WDG_LOCK | Locked | Write `0x5ADA7200` to unlock. Any other value locks. Reads 0 unlocked, 1 locked |
| `0x04` | WDG_OT_LOAD_H | `0x7` | Bits 2:0. Latched at EN 0-to-1 and at each feed |
| `0x08` | WDG_OT_LOAD_L | `0xC0000000` | Read-only in the model. Writes are dropped with a warning |
| `0x0C` | WDG_RST_LOAD | `0x03F80000` | Start of the reset-pulse count. Writes keep bits 24:0 |
| `0x10` | WDG_EN | 0 | Bit 0. 0-to-1 starts the count from the current H:L. 1-to-0 stops it |
| `0x14` | WDG_KNOCK | 0 | Write `0x55AADD22` while enabled to restart the count. Other values are stored |

While locked, writes to every register except LOCK are dropped with a warning. Offsets
`0x18`-`0xFFF` are storage.

Timeout. The counter counts up from the 35-bit `OT_LOAD_H:OT_LOAD_L` to its overflow (HR_C7000 v2.7
section 4.10.3, p.215; OT_LOAD descriptions, p.216-217), so the timeout is `2^35 - H:L` ticks. The
reset values give 2^30 ticks, which the datasheet calls "about 60s". A larger load is a shorter
timeout. EN 0-to-1 and a feed both start a full period from the current H:L. A disable leaves the
load and control values unchanged (p.214), and the next enable counts from the load again (p.215).

Clock. The datasheet names only "a system clock", and its two figures disagree: 2^30 ticks in 60 s
is about 17.9 MHz, and the reset pulse below (2^19 ticks in "about 20ms") is about 26 MHz. The model
uses 17,895,697 Hz, so that the reset-value timeout is the datasheet's 60 s (an assumption).
`DM32_WDT_HZ` overrides it until the clock is measured on a radio.

Reset pulse. On expiry the machine is held in reset while a second counter runs from `RST_LOAD` to
`0x03FFFFFF` (p.218), `0x04000000 - RST_LOAD` ticks: 2^19 ticks with the reset value, 29 ms at the
default clock. The vCPUs are paused for the pulse, from a bottom half, so a few instructions may run
first; with `-icount` they are not paused. At the end of the pulse the whole machine is reset
(assumption: the datasheet does not say which blocks the reset reaches). The datasheet gives the
field as bits 24:0 although its reset value and end value need bit 25, so a written value is masked
to 25 bits and always gives a pulse of at least 2^25 ticks.

Not modelled: the reset-output line, and the 510-clock reset extension of an external reset (p.213).

Environment: `DM32_WDT_LOG`, `DM32_WDT_NORESET`, `DM32_WDT_HZ`. See
[environment.md](environment.md#watchdog).

Evidence:

- Stock DM32.00.01.048 never uses the watchdog. A static sweep of IRAM and program flash, which
  tracks constant chains as well as literals, finds no access to `0x14010000` and neither key
  (`0x5ADA7200`, `0x55AADD22`) anywhere; the same sweep finds the timer and I2C2 accesses. An
  11-minute stock standby soak with `DM32_WDT_LOG` made no watchdog access.
- With the watchdog enabled from the gdb stub on a stock run (`DM32_WDT_HZ` set for a 1 s timeout),
  the bite came 1 s after EN, the pulse ended about 1 ms later, and stock booted again to standby.
- `tests/qtest/test_wdt.py` checks the timeout formula, the 60 s default, the pulse and its length,
  feeding, disabling, locking and `DM32_WDT_HZ`.

## I2C and RTC

All three controllers use the same model: I2C0 at `0x14060000`, I2C1 at `0x14070000` and I2C2 at
`0x14080000`. Source: [dm32_i2c.c](../hw/misc/dm32_i2c.c) and [dm32_i2c_rtc.c](../hw/misc/dm32_i2c_rtc.c).
Each controller drives a QEMU I2C bus named `i2c0`, `i2c1` or `i2c2`, so any QEMU `I2CSlave` model
attaches with `-device <type>,bus=i2c<n>,address=<a>`. The board puts the RTC on `i2c2`; `i2c0` and
`i2c1` are empty, so every address NAKs on them. I2C0 and I2C1 reach their bus only while
`IO_DIPLEX0` gives them PTA5/6 and PTA7/8 (select 0); otherwise the address phase NAKs and counts
`pad_unclaimed` (see [system registers](#system-registers)). Stock uses neither I2C0 nor I2C1. The model covers
the 256-byte register file; the rest of each 64 KB block is the block's unimplemented window.
The I2C0 pads carry the bit-banged ALPU-MP bus (GPIOA 5 and 6), which the firmware drives through
GPIOA, not through the controller. See [FD6818](#fd6818) for the other bit-banged bus on GPIOA.

Interrupt wiring: I2C0 to PIC source 8, I2C1 to 7, I2C2 to 9 (Table 30). The RTC's source 19 is not
wired.

The block is the HR_C7000's cut-down DesignWare APB I2C (datasheet section 5.1, p.218-243): master
only, no STOP or RESTART bits in `IC_DATA_CMD`, no standard-mode count registers, and an added
`IC_START` register.

| Offset | Name | Reset | Model |
|---|---|---|---|
| `0x00` | IC_CON | `0x7D` | [6:0]: SLAVE_DISABLE, RESTART_EN, 10BITADDR_MASTER/SLAVE, SPEED, MASTER_MODE |
| `0x04` | IC_TAR | `0x55` | [11:0]; bits 6:0 address the slave |
| `0x10` | IC_DATA_CMD | - | Write: bit 8 = read command, bits 7:0 = data, into the TX FIFO. Read: pops the RX FIFO |
| `0x1C`, `0x20` | IC_FS_SCL_HCNT, IC_FS_SCL_LCNT | `0x5A`, `0xA0` | SCL high and low counts; set the transfer timing |
| `0x2C` | IC_INTR_STAT | `0` | IC_RAW_INTR_STAT & IC_INTR_MASK |
| `0x30` | IC_INTR_MASK | `0x8FF` | [11:0], 1 = enabled |
| `0x34` | IC_RAW_INTR_STAT | `0` | See "Interrupts" below |
| `0x38`, `0x3C` | IC_RX_TL, IC_TX_TL | `6` | 0-7; larger values read back 7 |
| `0x40` | IC_CLR_INTR | `0` | Read clears every latched interrupt and IC_TX_ABRT_SOURCE |
| `0x44`-`0x64` | IC_CLR_RX_UNDER ... IC_CLR_START_DET | `0` | Read clears that interrupt (IC_CLR_TX_ABRT also clears IC_TX_ABRT_SOURCE) |
| `0x6C` | IC_ENABLE | `0` | [0] |
| `0x70` | IC_STATUS | `0x6` | ACTIVITY, TFNF, TFE, RFNE, RFF, MST_ACTIVITY from the controller |
| `0x74`, `0x78` | IC_TXFLR, IC_RXFLR | `0` | FIFO levels, 0-8 |
| `0x80` | IC_TX_ABRT_SOURCE | `0` | Why the last transfer aborted |
| `0x9C` | IC_ENABLE_STATUS | `0` | IC_EN: 1 from enable until the controller is idle after a disable |
| `0xA0` | IC_START | `0` | [0]: with data queued, starts the transfer |

Transfers. While IC_ENABLE and IC_START are 1 and the TX FIFO holds a command, the controller sends
START and the address, then executes the commands in order on the QEMU bus. A change of direction
sends RESTART and the address again, or STOP and START with IC_CON.RESTART_EN clear. When the byte
that empties the TX FIFO ends, a STOP follows (datasheet p.222), and the last byte read before it is
NACKed. Each step takes virtual time: one SCL period is `(HCNT + 8) + (LCNT + 1)` cycles of the 84
MHz APB clock (p.220), with the counts held at their minimums of 6 and 8. A byte is nine periods;
START, RESTART and STOP one each. Stock's counts (97 and 104) give 2.5 us, so a one-byte register
read takes 97.5 us.

Interrupts. RX_UNDER, RX_OVER, TX_OVER, TX_ABRT, ACTIVITY (the end of a transfer), STOP_DET and
START_DET latch until their clear register is read. TX_EMPTY is set while the TX FIFO holds at most
IC_TX_TL entries (while disabled: while a transfer is still finishing). RX_FULL is set while the RX
FIFO holds more than IC_RX_TL entries, or 7 when IC_RX_TL is 7 (IC_RX_TL's description, p.233; the
IC_RAW_INTR_STAT text says "greater than or equal"). RD_REQ, RX_DONE and GEN_CALL are slave or
general-call events and stay 0.

Aborts set IC_TX_ABRT_SOURCE and TX_ABRT, flush the TX FIFO and end the transfer with STOP:
ABRT_7B_ADDR_NOACK (no slave at the address), ABRT_TXDATA_NOACK (a written byte NAKed),
ABRT_10ADDR1_NOACK (10-bit addressing), ABRT_MASTER_DIS (IC_CON.MASTER_MODE 0).

Assumptions (the datasheet does not say; they follow the DesignWare block):

- FIFO depth 8, from the 4-bit level registers and the 0-7 thresholds.
- IC_CON, IC_TAR and the SCL counts are refused, and logged, while IC_ENABLE is 1.
- IC_DATA_CMD writes are dropped while disabled and while TX_ABRT is set.
- Clearing IC_ENABLE lets the byte in flight finish, then sends STOP; both FIFOs are flushed when
  IC_ENABLE_STATUS drops.
- With 10-bit addressing no slave answers: QEMU buses carry 7-bit addresses only.
- START, RESTART and STOP take one SCL period; IC_CON.SPEED does not change the timing.
- IC_START is not self-clearing; the IC_CLR_* registers read 0.

Not modelled: slave mode, general call and START byte, high-speed mode, SDA hold and setup timing.

RTC, a slave at 7-bit address `0x70` on I2C2 (HR_C7000 v2.7 section 4.9, Register Table 7). Its
alarm drives PIC source 19 (Table 30, level, high). Registers, all resetting to 0 except
SOFT_RST_N:

| Register | Name | Model |
|---|---|---|
| `0x00`-`0x04` | VAL_S [5:0], VAL_M [5:0], VAL_H [4:0], VAL_D_L, VAL_D_H | Read-only count. Binary; rolls over at 60 s, 60 min, 24 h and 65536 days |
| `0x05`-`0x09` | MATCH_S, _M, _H, _D_L, _D_H | Alarm time, same widths |
| `0x0A` | MATCH_VEN | Writing 1 copies MATCH_* into the internal match register; reads 0 |
| `0x0B` | MATCH_VSTAT | Software's "match configured" flag, bit 0. No effect |
| `0x0C`-`0x10` | LOAD_S, _M, _H, _D_L, _D_H | Initial time, same widths |
| `0x11` | CCR | Bit 2 load (count from LOAD, clears itself), bit 1 enable (count), bit 0 interrupt enable |
| `0x12` | LOAD_VSTAT | Software's "load configured" flag, bit 0. No effect; stock reloads its default time when it is 0 |
| `0x13` | RSTAT | Bit 0, interrupt occurred. Read clears it and drops the line |
| `0x14`, `0x15`, `0x16` | CLKDIV_NUMER_L, _H, _VLD | With VLD set, the count runs at 32768 / t Hz instead of 1 Hz |
| `0x17` | SOFT_RST_N | Reset 1. Writing 0 resets every other register and the count and holds them until 1 |

Counting. The count advances on QEMU's RTC clock: host time by default, virtual time with
`-rtc clock=vm`, which makes the RTC reproducible between runs. A stop, a load or a divider change
starts a new tick. At power-up the count is 0 and stopped, as with a fresh coin cell; stock then
finds LOAD_VSTAT clear, soft-resets the RTC, starts it and loads its default of 2016-09-02
16:45:20 (day 17046 counted from 1970-01-01, `rtc_validate_and_set_default_datetime` at
`0x0301C608`).

Alarm. When the count reaches the internal match value by counting and CCR bit 0 is set, RSTAT is 1
and the line stays high until RSTAT is read. Stock never sets an alarm or unmasks PIC 19.

Assumptions (the datasheet does not say): a load onto the match value does not fire it; a LOAD field
beyond its range carries into the next field; a divider of 0 does not count; the soft reset clears
every register; the pointer auto-increments and VAL_* are latched when a read starts. Bus protocol:
the first byte of a write sets the register pointer, and each further byte writes the register it
points at. A read past `0x17` returns `0xFF`.

Not modelled: the 25 ms soft-reset release time, and power failure (the VSTAT guard flags have no
effect).

Environment: `DM32_I2C_LOG`. See [environment.md](environment.md#i2c-and-rtc).

Evidence:

- Stock's RTC read, `rtc_register_read` at `0x0301C2D4`, sends `IC_TAR = 0xE0 >> 1 = 0x70` with one
  write byte and one read command. The RTC is an I2C part, not a memory-mapped block.
- Stock touches only I2C2 (`i2c2_master_init_400khz` at `0x0301DACC`, `i2c2_master_write_reg` at
  `0x0301DB40`, `i2c2_master_read_reg` at `0x0301DC1C`). Init: IC_ENABLE 0, IC_CON `0x65`,
  IC_INTR_MASK `0x40`, SCL counts for 400 kHz from its 84 MHz clock variable (IRAM `0x3A60C`),
  thresholds 6. Each transfer disables, reads IC_CLR_INTR, sets IC_TAR, enables, queues the index,
  data or read commands, sets IC_START, then polls IC_STATUS: TFNF to refill, RFNE per byte (up to
  `0xFFFFF` loads), and TFE with ACTIVITY clear to finish a write (up to `0xFFFFE` delays). It never
  reads IC_TX_ABRT_SOURCE and never unmasks PIC source 9. Static analysis of IRAM and program flash.
- Under emulation, stock boot makes 75 RTC reads in 20 s with no abort, and its set-default-date
  write lands.
- `tests/qtest/test_i2c.py` covers every register, the transfer timeline at stock's timing, the
  FIFOs and thresholds, every interrupt the master raises, the aborts, the enabled-write lock, the
  RTC, and an AT24C EEPROM on `i2c1`.
- Before the RTC slave existed, I2C2 was a RAM window. Every stock RTC read then ran into the
  driver's per-byte timeout. The source records 58% of emulated translation blocks in standby, a
  frozen clock, and a starved keypad poll.
- Stock's ALPU-MP anti-clone check addresses `0x7A` on the bit-banged GPIOA 5/6 bus, not on any
  I2C controller (`i2c_read_block_0x7a` at IRAM `0x000144FC`; dm32uv-re `specs/dts/dm32uv.dts`).
  Nothing answers there, so the check fails and halts at IRAM `0x169D4`. The boot runs with the ALPU-bypass sector spliced in, which is a QEMU-only patch
  ([sector_030c6000_alpubypass.bin](https://github.com/awkaplan/dm32uv-re/blob/main/data/firmware/patching/sector_030c6000_alpubypass.bin)
  in dm32uv-re).

## FD6818

RF transceiver, bit-banged on GPIOA. Source: [dm32_fd6818.c](../hw/misc/dm32_fd6818.c). Register
addresses, fields and reset values come from the FD6818 programming guide (REV 1.0.0, cited by page
in the source header), which is not included in this repository. Where the populated FD6818B
differs from the guide, the model follows the hardware reads recorded in dm32uv-re
`specs/rdl/fd6818.rdl`. The AT1846S, from which the part derives, has a different register map and
is not used.

Bus pins, all on GPIO bank A: CLK = bit 7, DATA = bit 8, CS = bit 24. DATA is driven by the SoC only
while `SWPORTA_DDR` bit 8 is set. Otherwise the chip owns DATA, and reads of bit 8 return its level.
The chip is attached to the three pads ([GPIO](#gpio), "Attached devices") and receives each change of
CLK, DATA or CS; while it drives DATA, it pulls pad GPIOA 8 to its data bit. Its GPIO0 and GPIO1 pins
are attached to GPIOB 27 and 26 (from the dm32uv-re pin map; assumption). It has no interrupt line to
the PIC; its interrupt output reaches the SoC only through a GPIO pin set to INT.

Frame: CS low, then 24 bits clocked MSB first, sampled on the CLK rising edge. Bit 23 is R/W (0 =
write, 1 = read). Bits 22-16 are the 7-bit address. Bits 15-0 are data. On a read, the chip drives
bit 15 right after the address byte, then one bit per following rising edge.

Pages (guide p.15). `WRITE(0x7F, 1)` selects page 1, where wire address `n` is register `0x80 + n`;
`WRITE(0x7F, 0)` returns to page 0, the default. Wire address `0x7F` is the page command on both
pages and reads 0, as on hardware. Stock never leaves page 0.

Registers with behaviour. Every other register is storage with the guide's reset value.

| Register | Access | Reset | Model |
|---|---|---|---|
| REG_00H | Read; write bit 15 | `0x4819` | Chip ID (the guide's is `0x6818`; `DM32_FD6818_ID` sets it). A write with bit 15 set is the soft reset (p.16): every register on both pages, the detectors and the interrupt return to reset, and the PLL stops. What the guide's reset covers is not stated; the model resets all of it (assumption) |
| REG_02H | Read; any write clears | 0 | Interrupt vector (p.51): `[11:10]` CDCSS, `[7:6]` CTCSS, `[4]` SQ, `[3:0]` source. A write clears `int_out` and leaves the vector |
| REG_07H | Write | 0 | CTCSS word, `f * 2^27 / 6500000` in `[12:0]`; `[13]` picks slot 0 or 1, each kept separately |
| REG_08H | Write | 0 | CDCSS code, `[11:0]`; `[15]` picks the high or low 12 bits, each kept separately |
| REG_0CH | Read | `0x0080` | Status (p.58): `[9]` pll_lock, `[7]` weak_rssi (SNR under 10 dB, assumption), `[1]` sq_out, `[0]` int_out. `[8]` afc_rail, `[3]` tot_out and `[2]` vox_out read 0, as do the reserved `[15:10]`, which stock reads. Stock sees `0x0280` in standby, the radio's value with the PLL locked and no signal |
| REG_25H | Read-write | `0xFF00` | GPIO output enables `[15:8]` (0 drives) and values `[7:0]`. The guide's reset is `0x0000` (all driven); see GPIO below |
| REG_26H, REG_27H | Read-write | `0x0005`, `0x3200` | GPIO functions, four bits a pin: 0 out_val, 1 INT, 2 SQ, 3 VOX (0), 4 sub-audio compare; the others drive 0 (assumption) |
| REG_28H | `[15:8]` read, `[7:0]` read-write | `0x6B3F` | `[15:8]` gpio_in_val, the pins' live levels. `[7:0]` storage (the guide's `0x3F`; one radio read `0x38`) |
| REG_30H | Read-write | 0 | Enable word, the guide's REG_03H layout (stock writes `0xBFF1` for RX, `0xC1FE` for TX, and never REG_03H). The PLL runs while `[8:4]` pll_en is 11111 and txon `[1]` or rxon `[0]` is set |
| REG_38H, REG_39H | Read-write | `0x3A98`, `0x0271` | Frequency `(REG_39H << 16 \| REG_38H) * 10` Hz, 409.75 MHz at reset. A REG_38H write unlocks the PLL until REG_39H is written |
| REG_3FH | Read-write | 0 | Interrupt mask: bit n enables source n |
| REG_4FH | Read-write | `0x7E34` | Squelch noise thresholds: `[14:8]` close, `[6:0]` open |
| REG_51H | Read-write | `0x1050` | `[15]` sub-audio detection on, `[12]` 1 = CTCSS, 0 = CDCSS |
| REG_52H | Read-write | `0x028F` | CTCSS thresholds: `[11:6]` detect, `[5:0]` lose, in 0.1 Hz, or 0.1 % of the tone with `[12]` set |
| REG_60H | Read | 0 | `[11:10]` CDCSS positive, negative; `[1:0]` CTCSS slot 1, slot 0 |
| REG_61H | Read | 0 | snr_out `[15:8]` |
| REG_62H | Read | jitter | agc_rssi `[15:8]` and lna_peak_rssi `[7:0]`; without `DM32_RF_ENV`, the stand-in below |
| REG_65H | Read | 0 | rssi_rel `[15:8]`, noise_sq `[7:0]` |
| REG_67H | Read | 0 | rssi_sq `[8:0]`, 0.5 dB a step, RSSI = rssi_sq / 2 - 160 dB (p.47) |
| REG_78H | Read-write | `0x44FF` | Squelch RSSI thresholds, 0.5 dB a step: `[15:8]` open, `[7:0]` close |
| REG_0BH, REG_64H, REG_87H | Read | 0 | DTMF, VOX amplitude and compander level: not modelled |

Writes to the read-only registers (`0x01`, `0x0B`, `0x0C`, `0x60`-`0x67`, `0x87`) are dropped.

PLL. Lock comes `DM32_FD6818_LOCK_US` (default 40 us, the upper bound measured on hardware) after a
REG_39H write or the PLL switch-on. Stock writes REG_38H, then REG_39H, then the enable word. With the
high word first, a radio stayed unlocked (REG_0CH `0x0080`), and so does the model. A REG_38H write
while locked raises "PLL lock lost" (source 7).

Receiver. While rxon is set and the PLL is locked, the receiver measures the strongest carrier within
6.25 kHz of the tuned frequency over the noise floor. Carriers come from `DM32_RF_ENV`, a script of
timed carriers ([environment.md](environment.md#fd6818)). From the in-channel power P in dBm:

| Quantity | Value | Source |
|---|---|---|
| rssi_sq | `2 * (P + 160)` | guide p.47 |
| agc_rssi | `P + 160` | 1 dB a step per the guide; the offset is an assumption |
| lna_peak_rssi | the same over all carriers within 5 MHz | as agc_rssi; the span is an assumption |
| snr_out, rssi_rel | carrier minus noise floor, dB | assumption |
| noise_sq | `60 - snr_out`, at least 0 | assumption |

With rxon clear or the PLL unlocked, every measurement reads 0 and the squelch is closed. Without
`DM32_RF_ENV` the band is empty: the floor alone (rssi_sq 66), except REG_62H, which keeps a
deterministic jitter (base `0x30`, 12 dB peak to peak), because on hardware it wanders and that wander
drives the UI's redraw rate. `DM32_FD6818_RSSI` pins it and `DM32_FD6818_RSSI_JITTER` sizes it.

Squelch (guide p.47):

```
if (rssi_sq > rssi_sq_th_in && noise_sq < noise_sq_th_in)        SQ = 1;
else if (rssi_sq < rssi_sq_th_out || noise_sq > noise_sq_th_out) SQ = 0;
```

Sub-audio (p.42-45), with REG_51H `[15]` set and a carrier at least 10 dB above the floor
(assumption). CTCSS: slot k is detected when the carrier's tone is within the detect threshold of the
slot's word, and lost beyond the lose threshold. CDCSS: the carrier's code is taken as its 23-bit
Golay word (data bits 11:9 = 100, generator `0xC75`), complemented for an inverted code. REG_08H's
word is compared with every cyclic rotation of it in both bit orders, because the guide does not give
the bit order (assumption). A match is a positive code, a match of the complement a negative one.

Interrupts (p.50-52). A change of SQ (source 0), CTCSS (5) or CDCSS (6), or a PLL lock loss (7),
whose REG_3FH bit is set latches REG_02H and sets `int_out`. Source numbers follow the interrupt
section and the mask bits; the guide's summary table gives 8 for the PLL and 4 for DTMF.

GPIO (p.53-54). A pin whose REG_25H enable bit is 0 drives its function. An undriven pin reads the
board: GPIO0 and GPIO1 the GPIOB 27 and 26 pad (the SoC's driven level, otherwise its pull, high
without one), GPIO2-7 the levels of one radio's read (`0x68`, assumption). The guide's REG_25H reset
of `0x0000` would drive every pin, and no combination of the reset functions gives the `0x6B` a radio
read, so the model resets REG_25H to `0xFF00`.

SoC resets do not reset the chip, which is a separate part (see [Resets](#resets)).

Not modelled: transmit, audio, DTMF, VOX, TOT, AFC, FSK, and the meaning of REG_0CH `[15:10]`. The
ALPU-MP bit-bang bus is on GPIOA 5 and 6 and does not share a pad with the chip (see [GPIO](#gpio)).

Environment: `DM32_RF_ENV`, `DM32_FD6818_ID`, `DM32_FD6818_LOCK_US`, `DM32_FD6818_LOG`,
`DM32_FD6818_BITS`, `DM32_FD6818_RSSI`, `DM32_FD6818_RSSI_JITTER`. See
[environment.md](environment.md#fd6818).

Evidence:

- Stock's default init issues the soft reset (`0x8000`, then `0x0000`), then writes about 74
  registers, including `REG_30H = 0xBFF1` for RX, `REG_78H = 0xFFFE` and `REG_4FH = 0x302F`. With
  the blank emulated codeplug it tunes 0 Hz. In standby it polls REG_0CH, which reads `0x0280`.
- With a `*` carrier at -20 dBm (rssi_sq 280, above stock's open threshold of 255), REG_0CH reads
  `0x0202`, stock starts reading REG_67H and draws the RSSI bar on the standby screen. It writes no
  FD6818 register while the carrier is on. When the carrier goes, REG_0CH returns to `0x0280`. Without `DM32_RF_ENV`, stock's frames
  are byte-identical to the model before the RF environment existed.
- The OpenRTX port, booted under this model, reports chip ID `0x4819` and passes a register bus
  test, as the hardware does.

## RDA5807M FM tuner

An I2C slave at 7-bit `0x10` on the bit-banged `fm` bus (GPIOA 26 SCL, 25 SDA). Source:
[dm32_rda5807m.c](../hw/misc/dm32_rda5807m.c). RDA5807M datasheet v1.0 sections 2.4-2.5 and 6;
dm32uv-re `specs/rdl/rda5807m.rdl`. No interrupt line. Audio is not produced.

Bus protocol: no register address on the wire. A write transfer starts at `02H`, a read transfer at
`0AH`; each register goes high byte first, then the counter moves on, wrapping from `3AH` to `00H`.
Every byte is acknowledged; there is no second (random-access) address.

| Register | Reset | Model |
|---|---|---|
| `00H` CHIPID | `0x5800` | Read-only; bits 15:8 `0x58` (the low byte is undocumented; 0 assumed) |
| `02H` | `0x0000` | ENABLE, SOFT_RESET (0 -> 1 edge restores every reset value), SEEK, SEEKUP, SKMODE, MONO, the rest stored |
| `03H` | `0x0000` | CHAN, TUNE, BAND, SPACE |
| `04H`, `05H`, `07H` | `0x0200`, `0x880F`, `0x4202` | Stored (de-emphasis, SEEKTH, VOLUME, blend) |
| `06H` | `0x0000` | OPEN_MODE: `11` makes the hidden registers `10H`-`3AH` writable; otherwise they ignore writes |
| `0AH` | - | Read-only: STC, SF, ST, READCHAN |
| `0BH` | - | Read-only: RSSI, FM_TRUE, FM_READY |
| `0CH`-`0FH` | `0x5803`, `0x5804`, `0x5808`, `0x5804` | RDS blocks, read-only; RDS is not modelled |

Tune and seek. A channel is `spacing * CHAN` above the band base (BAND 00 87-108 MHz, 01 76-91, 10
76-108, 11 65-76 or 50-76; SPACE 100, 200, 50 or 25 kHz). With ENABLE set, TUNE tunes to CHAN; SEEK
searches up or down for the next station, wrapping at the band edge (SKMODE 0) or stopping there
(SKMODE 1) with SF set when nothing is found. On completion TUNE or SEEK clears and STC sets.
ENABLE = 0 cancels an operation and reads both status registers as 0. The RF input comes from
`DM32_FM_STATIONS` (stations and their RSSI) and `DM32_FM_RSSI` (every other channel): FM_TRUE is
set on a station, ST on a station while MONO is clear.

Assumptions (the datasheet gives no times or protocol detail): a tune takes 50 ms and a seek 10 ms
per channel; a seek starts from CHAN at its first step, so stock's transfer, which writes `02H` (SEEK)
before `03H` (CHAN), seeks from the new channel; SEEKTH is not compared (any listed station stops a
seek); a register takes a written value when its low byte arrives. SoC resets do not reach the tuner.

Environment: `DM32_FM_STATIONS`, `DM32_FM_RSSI`, `DM32_FM_LOG`. See
[environment.md](environment.md#rda5807m-fm-tuner).

Evidence:

- Stock's driver (`0x03065444`-`0x030658EB`): push-pull SDA, driven high in the ACK slot, ACK never
  sampled. At boot it writes its 61-word table from `0x030B790C` (init `0x0306565C`), powers the tuner
  down (`02H = 0x8000`, `0x030657C0`), then writes `02H`-`05H` for the volume (`0x03065694`). Tune
  (`0x030656EC`) writes `02H = 0xC001` and `03H` with TUNE, BAND 10 or 11, SPACE 50 kHz; seek
  (`0x03065860`) writes `02H = 0xC301`; the seek poll (`0x03065BB8`) reads `0AH` (2 bytes) until
  STC = 1 and SF = 0 and takes the frequency as `READCHAN * 5 + 7600` (or `+ 6500`) in 10 kHz units.
  A successful seek makes stock write the frequency to flash block 4 `+0x702`.
- Under emulation, stock's boot reaches the model as the RDL describes: the table's status and hidden
  registers ignore their writes (`06H` OPEN_MODE is `10`), and the counter's wrap after `3AH` lands
  the table's last four words on `00H`-`03H`; the power-down then rewrites `02H`.
- `tests/qtest/test_rda5807m.py` replays stock's transfers on the GPIO pads: the init table, tune,
  seek in both directions with wrap and limit, power-down, soft reset, OPEN_MODE.
- No key or menu path that turns the FM radio on was found under emulation (the side keys do not,
  with a blank codeplug), so stock's tune and seek are verified by replaying its transfers, not by
  driving its FM screen.

## DMR RAM

Base `0x16000000`, 64 KB. Source: [dm32_dmrram.c](../hw/misc/dm32_dmrram.c). Capture only.

Interrupt wiring: none. The datasheet lists PIC sources 27 and 28 (built-in vocoder PCM read and
write), 29 (system), 30 and 31 (TX and RX slot), 32 and 33 (RF TX and RX), and 37 for the codec.
Source 30 is driven by `dm32_sysreg`, not by this device; see [System registers](#system-registers).

Layout, named from the datasheet and SVD. Names are used in logs and do not change behaviour:

| Offset | Size | Name |
|---|---|---|
| `0x0000`-`0x04AF` | 1200 B | TX baseband RAM. DMR physical SEND_DATA0 at `+0x00`, SEND_DATA1 at `+0x24`. Link header at `+0x00`. FM voice at `+0x030` and `+0x230` |
| `0x04B0`-`0x095F` | 1200 B | RX baseband RAM. RECV_INFO at `+0x30` |
| `0x0960`-`0x097F` | 32 B | MODEM_RAM (named in PSPEC only) |
| `0x0980`-`0x09BF` | 64 B | SOUND (named in PSPEC only) |
| `0x09C0`-`0x09FF` | 64 B | CODEC registers: [Codec](#codec) is mapped over them, so they never reach this device |
| `0x0A00`-`0xFFFF` | | Unnamed RAM |

Behaviour: plain RAM, zeroed at reset (the datasheet gives no reset value). No RX injection, no
signalling, no busy or underrun state, no interrupts. The model stores bytes and does not decode
them.

Reads of the status bytes `0x32`, `0x33`, `0x232` and `0x233` are logged
when `DM32_DMR_LOG` is set. Those four status bytes are read by stock's DMR routines.

Not modelled: frame decoding, mode-dependent layouts, and any RF or vocoder path.

Environment: `DM32_DMR_LOG`, `DM32_DMR_TRACE`, `DM32_DMR_DUMP`. See
[environment.md](environment.md#dmr-baseband-ram).

Evidence:

- In a stock boot to standby: 672 reads each of the
  four status bytes. No write reaches the TX or RX RAM.
- Stock writes outbound frames in the DMR timer path. The standby boot does not reach it.

## Codec

Audio codec registers: 64 bytes at `0x160009C0`, mapped over the [DMR RAM](#dmr-ram) window. Source:
[dm32_codec.c](../hw/misc/dm32_codec.c). Registers are from HR_C7000 v2.7 ("DS") Register Table 26
and the 37 descriptions in sections 6.5.2.1-6.5.2.37; the modes, sequences and soft mute from
section 6.4. The SVD lists the registers up to `0x26` with the DS reset values; `0x27`-`0x30` come
from the DS alone.

Interrupt wiring: PIC source 37 (Table 30, level, active high). SYS_SOFT_RSTN bit 4 resets the codec.

Access: one byte per register, at `0x160009C0 + address`; wider accesses are split into bytes. With
SystemControl `CODEC_I2C_MUX` bit 7 set (I2C, section 4.6.5.6) the window reads 0 and drops writes
(assumption: the DS names no bus for the I2C interface). `AUDIO_CONTROL` selects audio data paths, not
register access. Addresses the DS does not define read 0 and drop writes, logged under
`-d guest_errors`.

| Address | Register | Model |
|---|---|---|
| `0x00` | SR1 | Read-only. `[7]` pon_ack: the codec is in working mode (below). `[6]` irq_ack: an interrupt is pending. `[4]` dac_locked reads 0 (ADAS unused) |
| `0x01` | SR2 | Read-only, 0 (ADAS unused) |
| `0x07` | MR | Read-only. `[4:3]` ADC and `[1:0]` DAC soft-mute state: 00 not muted, 01 muting, 10 ending the mute, 11 muted |
| `0x08`, `0x09` | AICR_DAC, AICR_ADC | Reset `0xD3`. `[4]` powers down the audio interface |
| `0x0B`, `0x0C` | CR_MIC1, CR_MIC2 | Reset `0x30`. Stored |
| `0x0D` | CR_DAC | Reset `0xB0`. `[7]` soft mute, `[4]` sb_dac (power-down) |
| `0x0F` | CR_ADC | Reset `0xB0`. `[7]` soft mute, `[4]` sb_adc (power-down) |
| `0x10`, `0x11` | CR_MIX, DR_MIX | Indirect access to MIX_0-3 (below) |
| `0x12` | CR_VIC | Reset `0x0F`. `[1]` sb_sleep, `[0]` sb (analog power-down) |
| `0x13` | CR_CK | Reset `0x40`. `[4]` shutdown_clock stops the timer; `[3:0]` crystal, 24 or 26 MHz |
| `0x14`-`0x17` | FCR_DAC, SFCCR_DAC, SFFCR_DAC, FCR_ADC | Stored |
| `0x18`, `0x19` | CR_TIMER_h, CR_TIMER_l | Timer count; a write to CR_TIMER_l starts it (below) |
| `0x1A` | ICR | `[7:6]` int_form: the output's shape (below) |
| `0x1B`, `0x1D` | IMR, IMR2 | Reset `0xFF`: every source masked |
| `0x1C`, `0x1E` | IFR, IFR2 | Event flags: IFR `[0]` DAC and `[2]` ADC mute ramp done, IFR2 `[4]` timer end. A 1 written clears a flag (assumption: the DS does not say) |
| `0x1F`-`0x2A` | GCR_DACL/R, GCR_MIC1, GCR_ADCL/R, GCR_MIXDACL/R, GCR_MIXADCL/R | Gains, stored |
| `0x2B`, `0x2C` | CR_DAC_AGC, DR_DAC_AGC | Indirect access to DAC_AGC_0-3 |
| `0x2F`, `0x30` | CR_ADC_AGC, DR_ADC_AGC | Indirect access to ADC_AGC_0-4 (reset `0x34`, `0x07`, `0x44`, `0x1F`, `0x00`) |

Power modes (DS 6.4.1, Table 40). Power-down: sb = 1, the reset state. Sleep: sb = 0 and sb_sleep =
1. Playback: sb = 0 with the DAC path on (AICR_DAC `[4]` = 0, CR_DAC `[4]` = 0). Record: sb = 0,
sb_sleep = 0 and CR_ADC `[4]` = 0. SR1 pon_ack sets Tstbon (12 ms, DS 6.4.2.4) after the codec
reaches playback, or Tsbyu (50 ms, the low end of DS 6.4.2.3's 50-100 ms) after it wakes with sb_sleep
= 0, and clears as soon as neither holds. SystemControl `LINEOUT_CTRL` standby_lo (bit 31, read-only,
reset 1; the DAC standby the DS 6.4.2.4 sequence polls) reads 0 from Tstbon after the playback path
comes on, and 1 otherwise. Assumption: the DS gives these times as bounds.

Soft mute (DS 6.4.4). A change of a converter's soft-mute bit ramps for 24 ms (the DS's upper
bound), with MR showing 01 or 10, then settles at 11 or 00 and sets the IFR event flag. The ramp runs
only while the converter is powered (DAC: sb = 0, sb_dac = 0; ADC: sb = 0, sb_sleep = 0, sb_adc = 0);
one that powers up takes its requested state at once, and one that is off reads 00, the MR reset value
(assumption).

Timer (DS 6.5.2.18-19). A write to CR_TIMER_l starts a one-shot count of `CR_TIMER_h << 8 |
CR_TIMER_l`, ending after `(3 + count)` slow clocks of 512 MCLK, the DS's minimum period, and
setting IFR2 `[4]` (assumption: the DS does not say what starts it or how the halves combine).

Interrupt. An event is pending while `IFR & ~IMR` (bits 7, 2 and 0) or `IFR2 & ~IMR2` (bit 4) is non-zero.
ICR int_form shapes the line to PIC source 37: 00 high while pending, 01 low while pending, 11 a low
pulse of 8 MC_CLK (9.8304 MHz, about 0.8 us) on each new event and high otherwise. 10 is undocumented
and is taken as 00, logged under `-d unimp`.

Indirect registers. Writing a control register with load (`[6]`) = 1 stores its data register's byte
at index `[5:0]`, as in the DS 6.4.5 example (data first, then the control write). With load = 0 the
data register reads the indexed register, so at reset DR_ADC_AGC reads ADC_AGC_0, `0x34`. An index
past the bank's last register stores nothing and reads 0 (assumption).

Not modelled: audio samples, and so the gains, mixer, AGC and DRC have no effect; the ADAS lock; the
sample-rate calibration.

Environment: `DM32_CODEC_LOG`. See [environment.md](environment.md#codec).

Evidence: stock's boot follows the DS sequences: CR_VIC `0x0F` to `0x0E` to `0x0C`, the ADC path with
soft mute off, then AICR_DAC `0xC0`, CR_DAC `0xA0` then `0x20`, and the DAC and ADC gains. Snake
cycles CR_VIC through `0x0C`, `0x0F` and `0x0E` about 120 times. Stock never reads SR1, MR or IFR, and
never unmasks a codec interrupt. Its read-modify-writes now start from the DS reset values instead of
0; its frames are unchanged.

## SPI slaves

Bases `0x14120000`, `0x14130000` and `0x14140000`, window `0x100` each; the rest of each 64 KB block is
its unimplemented window. Source: [dm32_spi_slave.c](../hw/misc/dm32_spi_slave.c).

Interrupt wiring: PIC sources 34, 35 and 36 (Table 30, level, active high), high while
`RISR & IMR` is non-zero.

The HR_C7000's SSI works "as a Master or Slave" with one register set (section 5.2, p.243-263), so
the three slave blocks have the SPI master's registers (the SVD derives them from SPI_Master0), with
slave-only fields in CTRLR0 and master-only CTRLR1, SER and BAUDR. Their FIFOs are 8 entries deep
(p.244). Registers and reset values come from the SVD through `include/hw/csky/dm32_regfile.h`:
CTRLR0 `0x7`, SR `0x6`, IMR `0x3F`, IDR `0x11223344` and SSI_COMP_VERSION `0x3331312A`, the last two
read-only.

A slave transfers only when an external master clocks it, and nothing on the board can. The model
does what a slave does on its own, following the DesignWare SSI the block is built from (the
datasheet does not describe it):

| Register | Model |
|---|---|
| DR write | Queued in the TX FIFO while SSIENR is set, up to 8 entries; a write to a full FIFO is lost and raises TXOI. Dropped while SSIENR is clear |
| DR read | The RX FIFO is always empty: returns 0 and raises RXUI |
| TXFLR, RXFLR | TX FIFO level; 0 |
| SR | TFNF while the TX FIFO has room, TFE while it is empty; BUSY, RFNE and RFF 0 |
| RISR, ISR | TXEI while enabled with no more entries than TXFTLR; TXOI and RXUI latched. ISR = RISR & IMR |
| TXOICR, RXOICR, RXUICR, ICR | Read returns the interrupt (ICR: any of them) and clears it |
| SSIENR | Clearing it empties the TX FIFO and clears the latched interrupts |

Configuration writes while enabled are accepted (assumption: the datasheet does not refuse them).
The IO_DIPLEX "Slave SPIx" pad selects are not consulted. Accesses are 32-bit only.

Environment: none.

Evidence:

- Stock never uses the slaves: a static sweep finds no access to the three bases, no handler
  registered for PIC 34-36 (vectors 66-68), and no pad select of a slave function.
- `tests/qtest/test_spi_slave.py` checks reset values, read-only registers, TXEI on each slave's own
  PIC source, the TX FIFO and its overflow, RX underflow, the clear registers and the disable rule.

## Cache controller

Base `0xE000F000`, window `0x1000`. Source: [dm32_cache.c](../hw/misc/dm32_cache.c). The CK803 core's
own cache controller, in its tightly coupled IP space; the HR_C7000 datasheet does not describe it.
The layout follows XuanTie's CSI core header (`core_803.h`, `CACHE_Type`, in alibaba/AliOS-Things
and XUANTIE-RV/open-yoc `components/csi`), the only source found.

Interrupt wiring: none.

| Offset | Name | Model |
|---|---|---|
| `0x000` | CER | EN [0], CFIG [1], WB [2], DCW [4], WA [5] kept; other bits read 0 |
| `0x004` | CIR | INV_ALL [0], INV_ONE [1], CLR_ALL [2], CLR_ONE [3], line address [31:4]. The operation completes at once; reads 0 |
| `0x008`-`0x014` | CRCR0-3 | Cacheable regions: EN [0], SIZE [5:1] (code n = 2^(n+1) bytes), base [31:10]; bits 9:6 read 0 |
| `0xFF4` | CPFCR | PFEN [0] kept; PFRST [1] resets the counters and reads 0 |
| `0xFF8`, `0xFFC` | CPFATR, CPFMTR | Access and miss counts. Read 0: no accesses are counted |

Every register resets to 0 (assumption: the header gives no reset values). The reserved space
`0x018`-`0xFF3` reads 0 and ignores writes. Accesses are 32-bit only.

QEMU does not model caches, so the model keeps and decodes the configuration but cannot detect a
stale line or a missing invalidate, and the profiling counters do not count.

Environment: `DM32_CACHE_LOG`. See [environment.md](environment.md#cache-controller).

Evidence:

- Stock configures the cache twice at boot, with the same code at flash `0x03001010` (boot entry)
  and `0x0300C010` (application start): CIR = 1 (invalidate all), CER = 4 (write-back),
  CRCR0 = `0x0300002D` (region 0: `0x03000000`, 8 MB, the program-flash window), then CER |= 1.
  Observed with `DM32_CACHE_LOG`, and confirmed by a static sweep of IRAM and program flash.
- Stock never writes the cache controller after boot, and uses no cache control registers (cr17,
  cr18, cr22). In particular nothing invalidates the cache after the program flash is written at
  run time (the settings sector at `0x0300B000`) or by the YMODEM updater. Whether that can leave a
  stale line on the radio depends on how the LCSFC write path interacts with the cache, which is
  open ([coverage plan, item 7](coverage-plan.md#7-cache-controller-s) and
  [item 29](coverage-plan.md#29-program-flash-xip-tail-and-lcsfc-command-path-l-hw-for-a)).
- `tests/qtest/test_cache.py` checks each register's fields, CIR and PFRST completion, the reserved
  space, reset and the log.

## Resets

Machine reset. QEMU's `system_reset`, the watchdog bite and `SYS_SOFT_RSTN` bit 7 reset the CPU and
every device through its reset handler. The datasheet's global reset sources (power-on, NRESET, the
watchdog) reset the whole chip (HR_C7000 v2.7 sections 4.1.2 and 4.10). What each model resets:

| Model | On reset |
|---|---|
| PIC, timer, watchdog, I2C, SPI slaves, cache, ADC, DAC, PWM | Every register to its reset value; FIFOs, timers and interrupt lines cleared |
| GPIO | Every register to 0, so every pin is an input; attached devices see the pads released. Injected pin levels persist |
| SSI | Controller registers to 0, RX FIFO empty, any NOR command ended as chip select is released |
| LCD | The bus interface's `+0x10` and `+0x28`, so writes are dropped until the enable handshake |
| System registers | Every register to its reset value; slot counter stopped, TX-slot interrupt dropped |
| CPU | Restarts at the reset entry `0x03001000` |

Not reset, because they are separate parts or memory: the FD6818 registers, the LCD panel's GRAM,
window and command state, the NOR chip's contents and write-enable latch, the RTC, which keeps
counting (assumption: it runs from a coin cell while the chip is off, section 4.9.2, and nothing says
a chip reset reaches it),
and RAM, including the DMR RAM window. QEMU's loader images are copied again into IRAM and SRAM
on every machine reset.

`SYS_SOFT_RSTN` (`0x11000000`, section 4.1.4.1) has nine active-low bits, each a reset pulse: a
write of 0 resets the module and the bit "automatically" returns to 1 (section 4.1.2.2), so the
register always reads `0x1FF`. Assumption: the release is immediate; the datasheet gives no
duration and stock never polls.

| Bit | Module | Model |
|---|---|---|
| 8 | CPU | The CPU alone restarts at the reset entry; devices and memory are untouched |
| 7 | System global | A machine reset, except that the system registers keep their values: the datasheet exempts "the register configuration module", whose registers have reset mode H |
| 6 | CPU-side ADC | Resets `dm32_adc` |
| 5 | Baseband ADC | No model; logged under `-d unimp` |
| 4 | Codec | Resets `dm32_codec` |
| 3 | Audio interface | No model; logged |
| 2 | FM | No model; logged |
| 1 | Physical layer | No model; logged |
| 0 | Protocol layer | No model; logged |

Evidence:

- Stock writes `SYS_SOFT_RSTN` at boot (`0x190`, pulsing bits 0-3, 5 and 6, in
  `hr_c7000_modem_core_register_init` at `0x0302361A`), pulses audio then codec by
  read-modify-write (`hr_c7000_modem_reset_sequence` at `0x03021B96`), pulses bits 0 and 1 at
  radio mode changes (`radio_mode_transition_sequencer` at `0x0302167C`,
  `radio_mode_reset_to_idle` at `0x030217D2`, `modem_queue_drain_and_stop` at `0x0302359E`), and
  reboots with `0xFE7F` (`flash_quad_enable_and_system_reset` at `0x0305EF30`, called from the CPS
  command dispatcher, the upgrade entry and the factory reset). It never reads the register back to
  poll. Static analysis of IRAM and program flash.
- Under emulation, writing `0xFE7F` to the register at the standby screen reboots stock: its boot
  pulse runs again and the RTC reads resume.
- `tests/qtest/test_reset.py` covers the register, each modelled bit, the system bit's exemption,
  and every model's machine reset.

## UARTs and unimplemented windows

### UARTs

UART0 at `0x14030000`, UART1 at `0x14040000`, UART2 at `0x14050000` and UART3 at `0x14090000`. Each
is a `csky_uart` from the csky-qemu tree, with the same register model as UART0. Source for the
board wiring: [dm32.c](../hw/csky/dm32.c).

Interrupt wiring: UART0 to PIC 12, UART1 to PIC 10, UART2 to PIC 11, UART3 to PIC 13. Vectors are
source plus 32.

Backends: UART `n` takes `serial_hd(n)`, the `n`-th `-serial` chardev. With no chardev for an index,
the UART drops transmitted bytes.

Roles on stock: UART0 is the CPS and console port. UART1 is the optional GPS port:
`gps_uart1_init_9600` (`0x0301D96C`) sets 9600 baud and muxes only the receive pad, and runs only
when option byte bit 2 is set. No stock use of UART2 or UART3 was found.

Evidence:

- UART0 transmitted zero bytes in a boot to menu. UART1 transmitted zero bytes; its init code
  programs the baud and FIFO registers and does not transmit. UART1 to 3 are not verified on
  hardware.

### Unimplemented windows

Reads return 0, writes are dropped, and every access is logged under `-d unimp`, in the same format
as QEMU's `create_unimplemented_device`. Every guest access is also an `unimp` permissive event (see
[Permissive events](#permissive-events-and-dm32_strict)).

One window is created by `dm32_add_unimp()` in [dm32.c](../hw/csky/dm32.c) for every peripheral block
in the datasheet's address map (HR_C7000 v2.7 section 4.5.3, Table 26), from the
`DM32_SVD_PERIPHERALS` list in `dm32_svd.h`, which has the same blocks. Each is named
`dm32.<SVD name>` in lower case (`dm32.timer`, `dm32.spi_slave0`, ...) and mapped at priority -1000,
below the block's model at priority 0. So:

- A block with no model reads as unimplemented throughout: USB (`0x13000000`, 256 KB, PIC 21) and
  SDIO (`0x15000000`, PIC 20).
- A block whose model maps less than the block, such as the timer, watchdog, GPIO, LCD, SSI and PIC
  models (4 KB of 64 KB each), reads as unimplemented past the model's window.
- `dm32.lcsfc` covers the 16 MB LCSFC window `0x03000000`-`0x03FFFFFF`, below the 1 MB program-flash
  image.

Table 26 lists no reserved ranges, so an address between blocks is unassigned, not unimplemented.

Evidence: the boot-to-menu and Snake runs made no unimplemented or unassigned access.

Environment: none.

### Unassigned addresses

A background region, `dm32.unassigned`, lies under the whole 32-bit address space at a lower
priority than every other region. It stands in for QEMU's unassigned-memory handling, which board
code cannot hook. It behaves the same way: a read returns 0, a write is dropped, and the access ends
in a decode error, which the C-SKY target ignores. Each access is logged under `-d invalid_mem` as
`Invalid read|write at addr ..., region 'dm32.unassigned', reason: rejected`, and is an
`unassigned` permissive event. Debugger accesses are not logged and still fail.

## Permissive events and DM32_STRICT

Source: [dm32.c](../hw/csky/dm32.c) (`dm32_event()`), declared in
[dm32.h](../include/hw/csky/dm32.h).

A permissive event is a guest access that the model lets through or quietly drops where the silicon
would refuse it or behave differently, or where no model exists. Each device reports its events
through `dm32_event()`. Every event is counted, whatever the environment, and one line with every
count is printed to stderr at exit:

```
dm32-events: unimp=0 unassigned=0 flash_store=2 dac_pd=3 lcd_mux=0 lcd_off=0 ssi_discard=0 nor_wel=0 adc_pad=0 wdt_locked=0 pad_conflict=4 pad_unclaimed=0 i2c_nak=504
```

That is a stock DM32.00.01.048 boot to the menu. Stock raises `flash_store`, `dac_pd`,
`pad_conflict` and `i2c_nak` itself (the last because no ALPU-MP answers on the `alpu` bus and stock's
anti-clone check clocks on through the NAKs); see [environment.md](environment.md#board-and-flash) for a strict list that it
passes.

With `DM32_STRICT` set, ` strict_stop=<class>` (or `none`) is appended.

| Class | Device | Event |
|---|---|---|
| `unimp` | board | Guest access to an unimplemented-device window |
| `unassigned` | board | Guest access to an address no region covers |
| `flash_store` | board | Guest store to the read-only program flash (`FLASH WRITE DROPPED`) |
| `dac_pd` | DAC | DATA write to a channel whose PD_CTRL bit is set |
| `lcd_mux` | LCD | DATA write while `IO_DIPLEX2` hands DB0-DB4 to the keypad (the byte is forced to `0xff`) |
| `lcd_off` | LCD | Write before the controller enable at `+0x28` (the write is dropped) |
| `ssi_discard` | SSI | DR write dropped because SSIENR or SER is clear or the instance's pads are not muxed |
| `nor_wel` | SSI | SPI-NOR page program or erase without WREN (dropped) |
| `adc_pad` | ADC | Conversion of a channel whose pad is still muxed to GPIO (reads 0) |
| `wdt_locked` | Watchdog | Register write while `WDG_LOCK` is locked (dropped) |
| `pad_conflict` | GPIO | GPIO drives a pad that IO_DIPLEX gives to a peripheral (the level does not reach the pad; keypad injection still works) |
| `pad_unclaimed` | I2C0/1, PWM, UART1-3 | A transfer, channel start or transmitted byte while the block's pads are not selected |
| `i2c_nak` | GPIO I2C buses | A bit-banged master clocks on after a slave's NAK instead of sending STOP or START (the rest of the transfer is lost); one stderr warning per bus |

`DM32_STRICT` selects classes that stop the guest on their first event. The message names the class,
the detail and an approximate PC, for example
`dm32: strict: unimp: dm32.usb write, offset 0x0, size 4, value 0x13000000 (pc~0x03001004)`. By
default QEMU then exits with status 3. `DM32_STRICT_ACTION=pause` stops the VM instead, so that
the gdb stub can inspect the state. See [environment.md](environment.md#board-and-flash).

The existing per-device messages (the first 20 `FLASH WRITE DROPPED` lines, the LCD and DAC
warnings, the watchdog warning) are unchanged.
