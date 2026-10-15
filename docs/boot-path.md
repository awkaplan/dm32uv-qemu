# Boot path: what stock firmware needs from the board

This page lists, in boot order, what the stock DM32.00.01.048 firmware reads from and writes to the
board, the address of each step, what the board model must provide for it to pass, and the model
that does so. The board is defined in [hw/csky/dm32.c](../hw/csky/dm32.c); the shared hooks it and
the devices use are declared in [include/hw/csky/dm32.h](../include/hw/csky/dm32.h). Firmware
addresses are from the stock image unless stated otherwise. The firmware is not in this
repository; see [dm32uv-re](https://github.com/awkaplan/dm32uv-re).

The stages below are the ones the firmware passes through in order. Where the sources disagree,
both statements are kept and marked as unverified.

## Board memory map

| Range | Contents | Model |
|---|---|---|
| `0x00000000`-`0x0000FFFF` | Low RAM: CK803S vectors, GPIO shadow | `dm32.lowram`, RAM. The boot ROM is not mapped. |
| `0x00010000`-`0x00057FFF` | IRAM, 288 KB | `dm32.iram`, RAM |
| `0x03000000`-`0x030FFFFF` | Program flash, GD25Q80 (1 MB), executed in place | `dm32.flash`, read-only ROM device |
| `0x04000000` | SSI controller; by structural evidence the program flash's raw SPI port | `dm32_ssi` |
| `0xe000f000`, 4 KB | CK803 cache controller | RAM stub |
| `0x11000000` | Clock, system and DMR modem registers | `dm32_sysreg` |
| `0x12000000` | LCD controller: command `+0x00`, data `+0x04`, control `+0x10`, status `+0x28` | `dm32_lcd` |
| `0x13000000` | USB | unimplemented window |
| `0x14000000` | DesignWare APB timer, 6 channels | `dm32_timer` |
| `0x14010000` | Watchdog | `dm32_wdt` |
| `0x14020000`, `0x14100000`, `0x14110000` | GPIO banks A, B, C | `dm32_gpio` |
| `0x14030000`, `0x14040000`, `0x14050000`, `0x14090000` | UART0, UART1, UART2, UART3 | `csky_uart` |
| `0x14060000` | I2C0, not used by stock | `dm32_i2c`, nothing on the bus |
| `0x14070000` | I2C1, not used by stock | `dm32_i2c`, nothing on the bus |
| `0x14080000` | I2C2, the RTC bus | `dm32_i2c` with the RTC slave `dm32_i2c_rtc` |
| `0x140a0000`, `0x140b0000`, `0x140e0000`, `0x14150000` | SSI0 to SSI3 | `dm32_ssi` |
| `0x140c0000` | PWM | `dm32_pwm` |
| `0x140d0000` | ADC | `dm32_adc` |
| `0x140f0000` | DAC | `dm32_dac` |
| `0x15000000` | SDIO | unimplemented window |
| `0x16000000` | DMR baseband RAM | `dm32_dmrram`, capture-only |
| `0x160009c0` | Codec registers | `dm32_codec` |
| `0x17000000` | Interrupt controller (PIC) | `dm32_pic` |
| `0x18000000`, 32 KB | SRAM | `dm32.sram`, RAM |

The core is a CK803S with the DSP extension. The board sets `ABIV2_DSP | ABIV2_DSP2` on the CPU
because the firmware executes `muls` and the stock core model lacks it.

## Reset entry and vector base

Firmware:

- The flash image starts with the DH_FLAG header word `0x0c646874`. The entry word is at flash
  `+0x04`, which is `0x03001000`.
- On reset the CK803S loads `pc` from `*(vbr + 0)`.

The board must provide:

- `vbr = 0x03000004` and `pc = 0x03001000` at every reset.

Model:

- `dm32_cpu_reset()` in [hw/csky/dm32.c](../hw/csky/dm32.c) sets both values. It is registered
  after the loader's own reset hook, so it runs last.
- Without it, `vbr` points at the DH_FLAG word and the reset jumps into data.

## Early init and the boot-mode selector

Firmware:

- The entry at `0x03001000` runs early init: `bsr 0x3001600`, `bsr 0x12190`, `bsr 0x120e8`, then
  `delay(100)` through `bsr 0x1239c`.
- The boot-mode selector at `0x030017bc` calls `gpio_read_pin(43)` and then `gpio_read_pin(51)`
  through `0x030015b0`. Either pin reading set selects a normal boot, which jumps to `0x0300c000`
  with `r3 = 49200 rotli 10 = 0x0300c000; jsr r3`.
- Otherwise the firmware enters a serial recovery console at `0x12614`, which blocks in a UART0
  `getchar` at `0x12378`.
- `gpio_read_pin(pin)` reads bank A at `0x14020000 + 0x50` for pins below 32, bank B at
  `0x14100000 + 0x50` for pins 32 to 63 (bit `pin - 32`), and bank C at `0x14110000 + 0x50` for
  pins 64 and up. It returns 1 when the bit is set. Pins 43 and 51 are bank B bits 11 and 19.

The board must provide:

- Bank B data reads that idle high. The front-panel keys are active low with pull-ups, so an idle
  high level selects normal boot.

Model:

- `dm32_gpio` returns `0xffffffff` for `+0x50` when no pin is injected. The power-sense line is the
  exception. It must idle low, which is set with `DM32_GPIO_IDLE_LOW` (see
  [key-injection](key-injection.md)).

## Clock init and PLL lock

Firmware:

- The clock init at IRAM `0x12480` busy-waits for PLL lock on `CLK_MGR_REG0x04` at
  `0x11000004`, bits 30 and 31.

The board must provide:

- A readable `+0x04` that reports locked (bits 30 and 31 set) while keeping the low configuration
  bits.

Model:

- `dm32_sysreg` returns the locked value on `+0x04`. The rest of the register window is storage.
- The core frequency is 192 MHz (`dm32_binfo.freq`), overridable by the CPU's `cpu_freq` property.

## crt0 and the second-stage copy

Firmware, from the normal-boot target `0x0300c000`:

- Programs the cache controller at `0xe000f000`: `+0x4 = 1`, `+0x0 = 4`, `+0x8 = 0x300002d`,
  then `+0x0 |= 1` to enable the instruction and data caches.
- Sets `r14 = 0x50000` (stack pointer at the top of IRAM) and `mtcr 0x10000, cr1` (VBR = 0x10000).
- Copies `.data` from flash `0x03072000` to SRAM `0x18000800`-`0x18004bfc`, and from flash
  `0x030bfc20` to IRAM `0x10000`-`0x2f098`.
- Zeroes `.bss` in IRAM `0x2f098`-`0x4a7b0`.
- Calls `bsr 0x2e158`, which is `main()`. A `br .` follows if it returns.

Unverified: an earlier trace describes a `.data` copy from flash `0x03001f48` into IRAM
`0x10000`-`0x12a60` and from `0x03001d00` into SRAM, done by the entry code before the selector. The
two descriptions give different source ranges. The `0x0300c000` path is the one traced to `main()`.

The board must provide:

- Cache controller reads and writes that do not fault. It is a RAM stub.
- The flash image as readable program memory. The flash window is a read-only ROM device, and
  stores into it are dropped. `DM32_FLASH_WRITABLE=1` restores plain RAM for experiments.
- The CK803S DSP extension (see above). Without it the first `muls` raises an undefined-instruction
  exception and the boot halts in the exception stub at `0x0300c086`.

Model: the flash image is loaded by `csky_load_kernel()` with the entry set in the board file.

## Operating system start: main, tick, PIC and interrupt delivery

Firmware, from `main()` at `0x2e158`:

- `bsr 0x10d5c` is `OSInit`.
- `bsr 0x12dd8` is `OSTaskCreate`, with name `0x40da0`, entry `0x3068688`, priority 30.
- `bsr 0x10a04` is `OSStart`, which dispatches through `OSStartHighRdy` at `0x10620`.
- The startup task at `0x3068688` runs its init calls and then loops. It creates the display and
  key tasks (`ScanDisp`, `TmrKey`, `CallDisp`).

Interrupt dispatcher at IRAM `0x1064a`: saves context, calls `OSIntEnter`, writes the PIC end of
interrupt at `0x17000010`, takes the vector from `PSR >> 16`, calls the ISR at
`table[0x45514 + (vector - 32) * 4]`, calls `OSIntExit`, and returns with `rte`.

The board must provide:

- A periodic tick. The stock tick is timer channel 0 on PIC source 1, which the firmware installs
  as logical vector 33. The UI tick is timer channel 5, enabled by `timer_ch4_enable` at
  `0x0301d784`.
- A PIC that delivers a source as CPU vector `source + 32`.
- The end-of-interrupt write at `+0x10` (`PIC_COW1`, value 4) to end the source in service.
- The RTC on I2C2 at `0x14080000`. The stock UI tick does not run and no key registers without it.

Model:

- `dm32_timer` holds its channel line asserted while a count is pending. Channel n drives PIC
  source n + 1, so channels 0 to 5 drive sources 1 to 6.
- `dm32_pic` holds one source in service and drives its vector until the end-of-interrupt write.
  Stock makes every source level-triggered, so the tick re-enters service at the EOI the dispatcher
  writes before the ISR, and leaves it when the ISR reads the timer channel's EOI. The PIC masks
  every source at reset.
- PIC sources used by the board: UART0 12, UART1 10, UART2 11, UART3 13, GPIOA 15, GPIOB 14,
  GPIOC 17, the DMR TX slot 30 (vector 62, from `dm32_sysreg`).
- `dm32_i2c_rtc` answers as the RTC slave on I2C2.

Pitfalls, from the same work:

- Timer channel 0 goes to PIC source 1, not 33. Source 33 gives CPU vector 65, which has no entry in
  the firmware's table, so the tick ISR is never taken. The SVD's note that the tick is source 33
  is wrong on this point.
- A line that is asserted and deasserted between two translation-block boundaries is never taken,
  because the CPU samples interrupts only at those boundaries. A version that pulsed the line and
  auto-deasserted it when the active source read zero lost almost every tick. The line is held
  until the end-of-interrupt write.
- The stock run needs the core-side fix in the csky-qemu base that syncs the PC before `mtcr psr`
  and `psrset`. Without it the guest can die at `pc=0` within minutes, even on stock firmware.

## SSI status polls

Firmware:

- The SSI driver's byte routine at IRAM `0x2bf90` (called from `0x2bfc4`) writes a byte and then
  polls the status register at `base + 0x28`. It waits for TFNF (bit 1), writes the data at
  `base + 0x60`, waits for TFE (bit 2), and waits for BUSY (bit 0) to clear.
- The controller base is `*(u32 *)(0x030ada00 + id * 4)`. The table at flash `0x030ada00` is
  `0x140a0000, 0x140b0000, 0x140e0000, 0x14150000, 0x04000000, ...`.

The board must provide:

- A status register with TFNF and TFE set for every base in the table. A plain RAM window returns 0,
  so the poll spins forever. A spinning poll looks like a semaphore wait in the scheduler trace.

Model:

- `dm32_ssi_create()` is called for `0x04000000`, `0x140a0000`, `0x140b0000`, `0x140e0000` and
  `0x14150000`.
- Each DR write completes at once (`TXFLR` is always 0), so TFNF and TFE stay set. BUSY is never
  set.
- The SPI-NOR chip is attached to the SSI block; see the next section.

## Memory-pool starvation and the SPI-NOR store

Observed: after the SSI fix the boot reached about 13,650 executed blocks and then stopped
advancing. The scheduler spun in its ready-priority update, and a memory-pool allocator at flash
`0x030695b4` returned empty with count 0. The pend cluster at IRAM `0x12340` retried it.

Two explanations are possible: a blank codeplug leaving about 13 subsystems pending, or missing
tick delivery. The plateau is gone once the tick and the SPI-NOR store are both modelled, and stock
boots to standby with a blank (`0xFF`) data chip. Open: whether the pool exhaustion and the tick
fault are the same problem.

The board must provide:

- A W25Q128 data chip on SSI0 (`0x140a0000`), selected by GPIOA bit 18, which holds the codeplug and
  configuration records. The firmware reads them with command `0x03` near address `0x2000`.

Model:

- `dm32_ssi` implements the chip: READ (`0x03` and `0x0b` with a 24-bit address), RDSR, RDID and
  WREN. Program and erase persist to `DM32_SPINOR_OUT`.
- The backing store is the file named by `DM32_SPINOR`, a dump of a real radio's data chip. When
  unset the store is `0xff`, which is the erased state. The README's current stock run reports
  reaching the standby screen. Unverified: that the blank store is enough for every menu screen.
- The stock firmware stores SPI-NOR opcodes into the flash window at `0x03000000`. The model drops
  them. This is an open item in the [project README](../README.md).

## ALPU-MP anti-clone check

Firmware:

- The ALPU-MP check runs on a bit-banged I2C bus on GPIOA 5 (SCL) and 6 (SDA), not on an I2C
  controller. Its failure branch reaches a fatal halt, `br16 0x169d4`,
  which is `while (1);`. Its only caller is IRAM `0x16c8c`.

The board must provide:

- A pass. The board model bridges those pins to an I2C bus, but no ALPU-MP sits on it, so the check
  is not satisfied on its own.

Model:

- The ALPU-bypass sector is spliced into the flash image at `0x030c6000`. Its file name gives the
  address. It is `data/firmware/patching/sector_030c6000_alpubypass.bin` in
  [dm32uv-re](https://github.com/awkaplan/dm32uv-re), and `compose_qemu_image.py` applies it.
- Without the bypass, boot stalls at `0x169d4`. That is firmware behaviour, not an emulator gap.
- The README's stock run also uses two other emulator-only patches from `compose_qemu_image.py`:
  auto-power-off NOPs and hardware-binding bytes.

## LCD init and first pixels

Firmware:

- The controller is enabled by writing `+0x10` (`0x303`) and `+0x28`, then polling `+0x28` bit 1.
  The panel init at flash `0x03017d66` does this.
- Panel commands in order: `0x11` SLPOUT, `0x36` MADCTL `0xc0`, `0x3a` COLMOD `0x05` (RGB565),
  `0x21` INVON, `0x29` DISPON. Then configuration commands `b2 b7 bb c0 c2 c3 c6 d0 d6 e0 e1 35`,
  and then `0x2a` CASET, `0x2b` RASET and `0x2c` RAMWR for each window.
- Cursor moves send CASET and RASET with two data bytes (the start coordinate), then RAMWR, per
  pixel. `display_set_cursor_pos` is at `0x03017978`.
- The backlight is PWM channel 0. Changing the Back Light menu level rewrote its FP register from
  23990 to 3427, the stock level table's value (`dm32_pwm`). The pad-to-LED link is not measured on
  hardware.

The board must provide:

- A status read at `+0x28` that reports ready (bit 1 set). `dm32_lcd` returns `0x3`.
- A panel that accepts commands and data on the two MMIO addresses.

Model:

- `dm32_lcd` decodes the command stream into a 256-pixel-wide GRAM (`GRAM_W`) and shows the visible
  240 by 320 window of it. The panel is 240 by 320 portrait (`DM32_LCD_W`, `DM32_LCD_H`).
- MADCTL is decoded and logged but not applied to pixel placement. The view reads GRAM straight
  through.

## Keypad and the shared LCD and keypad pads

Firmware:

- Each keypad scan sets `IO_DIPLEX2 |= 0xfe00` (`rf_modem_mute_during_keypad_scan` at
  `0x03012e14`). The restore after the scan is at `FUN_0301cad0` (`0x0301cad0`), and it clears bits
  15 to 9. The panel init clears them with mask `0xfff80107`.
- The LCD data bus DB0 to DB4 shares pads with keypad rows ROW0 to ROW4 on GPIOC 7 to 11.

The board must provide:

- Keypad reads on `EXT_PORT` at `+0x50` that idle high. Injected keys appear on those reads.
- A data bus that is not handed to GPIO when the panel writes.

Model:

- `dm32_sysreg` reports the pads as muxed to GPIO while `IO_DIPLEX2` bits 15 to 9 are set.
- `dm32_lcd` forces data writes made while muxed to white and logs one warning.
- Key injection is in `dm32_gpio` and is documented in [key-injection.md](key-injection.md).

## Diagnosing a stuck boot

Techniques that still apply:

- Block count. Count the distinct translation blocks in an `in_asm` trace (`-d in_asm -D <file>`).
  Sample at several times. A count that stays the same across 10, 30 and 60 seconds is a hard block.
  A count that keeps rising is slow progress.
- Trace tail. Read the last blocks in the `in_asm` trace. A tight loop that keeps repeating is a
  poll. Find the base register and the status bit it tests.
- Live bytes. Use the QEMU monitor. `memsave <addr> <size> <file>` dumps the running code. The
  static IRAM image is stale for the self-rewritten region. Disassemble the dump with
  [dm32uv-re](https://github.com/awkaplan/dm32uv-re)'s `tools/src/dm32_patch_tools/cs_disasm.py`.
- Execution histogram. `tools/src/dm32_patch_tools/pc_histogram.py` in dm32uv-re maps a trace to
  function names.
- Unmodelled accesses. Run with `-d unimp,guest_errors,invalid_mem`. Accesses to windows that are
  unimplemented, or to unmapped space, are logged there.
- Debugger. Start with `-s -S` for the gdb stub. dm32uv-re's
  `tools/src/dm32_patch_tools/gdb_rsp.py` reads registers and memory and sets watchpoints over the
  stub.
- Screens. See the next section for the quiescent frame dump.

### Capturing a settled screen

- `DM32_GRAMDUMP=<path>` writes the GRAM at each settled point. A frame is settled when drawing has
  been idle for `DM32_GRAMQUIET` milliseconds of virtual time (default 150).
- `DM32_GRAMSEQ=1` also writes `<path>.000`, `<path>.001` and so on, one per settled frame.
- Use `-icount shift=7,sleep=off` so the idle threshold does not trip mid-redraw under slow
  emulation.
- Render the dump with [scripts/gram2png.py](../scripts/gram2png.py) (`--view panel`, the default,
  is the visible 240 by 320 panel, upright; `gram` is the same area with a different colour
  expansion; `gram-full` is all 256 columns) or [scripts/ppm2png.py](../scripts/ppm2png.py)
  (`--panel`).

## Pitfalls

- Read the GRAM with a stride of 256 pixels (`GRAM_W`), not 240. A 240 stride shears the rows and
  looks like an orientation fault.
- The stock panel is 240 by 320, portrait. An earlier build used 160 by 128, which was an
  assumption from another controller and is not the stock panel. Unverified: whether OpenRTX drives
  a smaller window.
- Do not apply MADCTL again when reading the GRAM. The model stores pixels at the addresses the
  controller was given. Applying MADCTL a second time turns the picture 180 degrees.
- CASET and RASET can carry two data bytes (a cursor set) or four (a window). The model latches the
  start on two bytes and the end on the next two. RAMWR rewinds to the window start.
- Take screenshots only at a settled point (`DM32_GRAMDUMP`). The firmware clears a region and then
  draws into it, so a snapshot taken mid-repaint shows a half-cleared screen.
- A tick line that is pulsed or auto-deasserted loses interrupts. Hold it until the end-of-interrupt
  write (see the tick section).
- Timer channel n is PIC source n + 1. Do not use the SVD's note of source 33.
- Every SSI base in the table at `0x030ada00` needs a model. An unmodelled base returns zero and
  the poll never completes.
- Keep the boot-mode pins idle high. If they are low, the firmware goes to the recovery console
  and blocks in `getchar`.
- A keypad scan that is not followed by the restore leaves the panel unwritable on hardware, with a
  white screen until power cycle. The model warns about it.
- The power-sense line must idle low, not high.
- Stores into the flash window are dropped. A boot that depends on a flash write will not match
  hardware.
- Measuring with `-d cpu,nochain` starves the timer, so it shows no interrupts that are in fact
  delivered. Count device writes instead of matching a data address in `-d in_asm`, because data
  addresses never appear there.
