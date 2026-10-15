# Coverage plan

This is the item-by-item plan behind the roadmap in [coverage.md](coverage.md). Checked boxes are
done and unchecked boxes are open. Contributions that pick up an open item are welcome.

A checklist for raising every row of the [coverage table](coverage.md#coverage-table) to the highest
grade the available evidence supports. Items are sorted by effort, smallest first. Shared foundations
come first because several items depend on them.

Each item gives the rows it covers, the current grade, the target grade, and, where the target is
below A, what stands between the target and A. Grades use the scale in
[coverage.md](coverage.md#fidelity-scale).

## Conventions

Effort:

- **XS**: documentation only.
- **S**: up to a day.
- **M**: two to five days.
- **L**: one to three weeks.
- **XL**: more than three weeks, or dependent on research that has not been done.

Gates, which mark what a grade above the target needs:

- **[HW]**: a measurement on a physical radio, which cannot be done in this repository or in CI.
- **[EXT]**: an input that cannot be committed here (a boot-ROM image, per-unit bytes, key
  material). The model reads it at run time from a path the user supplies.
- **[DS]**: a datasheet section that has not been transcribed or checked yet.

Sources cited below:

- **DS**: HR_C7000 v2.7 datasheet (English, 734 pages). Not redistributed.
- **SVD**: `data/registers/HR_C7000-DM32.svd` in dm32uv-re.
- **DTS**: `specs/dts/hr_c7000.dtsi` and `specs/dts/dm32uv.dts` in dm32uv-re.
- **RDL**: `specs/rdl/*.rdl` in dm32uv-re.
- **FW**: the stock DM32.00.01.048 image.

## Definition of done (every item)

- [ ] Reinstall (`./install.sh <qemu>`) and build `qemu-system-cskyv2` with no warnings.
- [ ] `tests/run-smoke.sh` passes. `tests/checkpatch.sh` is clean.
- [ ] Register-level tests for the device pass under the qtest harness (P0.3). They include at least
      one negative case, such as misuse the model now rejects. For grade A, a negative case is
      required.
- [ ] dm32uv-re stock regression: boot to menu (`steps=3 crash=0 muxwarn=0 unimp=0 invalid=0`) and
      the Snake walkthrough (`steps=11 ...`, `9472` and `1175` frames present).
- [ ] Every new register behaviour cites DS, SVD, RDL or a stock address. Anything else is marked
      `Assumption:`.
- [ ] The row in `coverage.md` (grade and note), the device section in `devices.md`, and any new
      variable in `environment.md` are updated. Items in "Where emulation is more permissive" that
      the work closes are removed.
- [ ] No firmware, ROM, datasheet, capture, calibration, key or per-unit data is added.

---

## Phase 0: shared foundations

### P0.1 Corrections to coverage.md and devices.md (XS)

These are facts already established in [dm32uv-re](https://github.com/awkaplan/dm32uv-re) that the
current docs contradict or omit. Apply
them before grading anything.

- [x] ALPU-MP is on a **bit-banged I2C bus on GPIOA 5 (SCL) and 6 (SDA)**, not the I2C0 controller.
      Stock never touches the I2C0 controller. Source: DTS `i2c_alpu`; FW `i2c_read_block_0x7a`
      @0x000144fc (bus index at `0x00033c54` is constant 0; pads muxed to GPIO at IRAM `0x000141c8`).
      Fix the ALPU-MP and I2C0 rows and the ALPU-MP open question.
- [x] **The DM-32 has no HR_V3000.** DM32.00.01.048 contains no HR_V3000 driver. The radio uses the
      HR_C7000's built-in vocoder (DS 7.1.1, p.473). Source: dm32uv-re `specs/include/hr_v3000.h`.
      Forum posts mention separate "HR_Vocoder" builds (00.01.034, 00.01.046) for a hardware variant
      that is unresolved. The row is now N/A; the rest is item 1.
- [x] The **LCSFC store interface** is documented structurally in RDL `gd25q80.rdl`. A byte `06h`/`04h`
      to `0x03000000`. A word `(20h << 24 | addr)` to `0x03000000` erases a sector. A word
      `(02h << 24 | addr)` to `0x03800000`, then data bytes to `0x03800000`, then the last byte to
      `0x03000000`, programs a page. Completion is polled by re-reading the DH_FLAG word through
      XIP. This identifies the "six stock writes to `0x03800000`".
- [x] **`0x04000000` is a raw SSI to the program flash.** Only `0x0002bb98` uses it, for the
      quad-enable sequence `35h / 06h / 01h 00h 02h / 05h`, followed by `QUAD_ENABLE = 1`. Source:
      RDL `gd25q80.rdl`.
- [x] **UART1 is the optional ATGM336H GPS** at 9600 baud, receive pad only, initialised only when
      option byte bit 2 is set (`0x0305f310`–`0x0305f31e`). Source: DTS `&uart1`.
- [x] Record the PIC sources of blocks that currently list "Interrupt wiring: none". From DTS
      (DS Table 30): I2C0 8, I2C1 7, I2C2 9, RTC 19, SPI0 16, SPI1 22, SPI2 23, PWM 18, ADC 26,
      DAC 25, codec 37, LCSFC 24, baseband 27–33.
- [x] The comment in `hw/gpio/dm32_gpio.c` that says "Up/Down are GPIOB 20/19 read before the
      matrix" is stale. GPIOB 19/20 are side keys SK1/SK2, and Up/Down are matrix keys on GPIOC 11
      (DTS reconciliation table).
- [x] SRAM is not zero at power-on on hardware. A live capture shows `0x18004c00`–`0x18007fff` as
      power-on noise (dm32uv-re `docs/patch-patterns.md`, item 12).
- [x] DTS marks USB, SDIO, UART2/3, I2C0/1, SPI1/2 and the SPI slaves as "no DM-32 use found". For
      the watchdog it says only that stock use is not established. Say so in each row's note.

### P0.2 Strict mode (S)

Needed by: item 3, and by any regression that wants a permissive path to fail loudly.

- [x] Add `DM32_STRICT=1`. Each permissive event counts and, in strict mode, stops the guest with a
      named reason: an unimplemented-window access, an unassigned access, a dropped flash store, a
      powered-down DAC write, an LCD write while muxed, or an SSI byte discarded for a missing pad
      mux. Also covered: LCD writes before the controller enable, SPI-NOR program or erase without
      WREN, ADC conversion of a GPIO pad, and watchdog writes while locked. `DM32_STRICT` takes `all`
      or a list of classes, and `DM32_STRICT_ACTION=pause` stops for gdb instead of exiting (status 3).
- [x] Report the counts at exit in the same token style the regression parses: one
      `dm32-events: <class>=<n> ...` line on stderr, with no key that the harness's existing patterns
      match.
- [x] Do it from the board and device files only. csky-qemu installs no transaction-failed hook, and
      its files are not edited here. The unimplemented windows are now board-created
      (`dm32_add_unimp`), and a background region (`dm32.unassigned`) stands in for unassigned memory.
      Both keep the log formats that `qemu_run.py` counts.
- Found by strict mode: stock raises `flash_store` (2), `dac_pd` (3) and `ssi_discard` (2) on every
  boot. The two `ssi_discard` events are stock's program-flash quad-enable sequence on `0x04000000`,
  dropped because every SSI instance is gated on SPI0's pads (item 17). A stock-clean strict list is
  in `environment.md`.

### P0.3 Register-level test harness (M)

Needed by: every item, for its negative cases.

- [x] Add `tests/qtest/`: a small Python client for QEMU's `-qtest` protocol (`readl`, `writel`,
      `clock_step`, `irq_intercept_in`). It starts `-M dm32 -accel qtest` with no firmware.
      `dm32qtest.py`, standard library only; also QMP device lookup, `peek_output()` and parsing
      of the `dm32-events:` line. Conventions in `tests/qtest/README.md`.
- [x] Add one script per device. Each asserts reset values, register semantics and IRQ edges against
      the citations in the device file. 14 modules, 203 tests, about 10 s; every module has negative
      cases. Seven `expectedFailure` tests record model gaps found while writing them:
      - PIC: `PIC_VECTOR` resets to 0 (SVD 0x20); `PIC_MODE`/`PIC_PO`/`_1` reset to 0 (SVD
        0xffffffff); `PIC_PRIOR0_3` resets to 0 (SVD 0x03020100). Item 9.
      - Timer: `TIMERS_COMP_VERSION` reads 0x3230352A; the datasheet (4.8.5.34) gives 0x3230322A.
        Item 6.
      - I2C: a write to an address with no slave sets no `TX_ABRT`, while a read does. Item 10.
      - PWM: a STATUS write stores bit 8 (busy), which is meant to be derived from CFG. Item 21.
      - FD6818: `REG_0CH` reads 0x0000 at reset; `devices.md` lists 0x0200 while also saying bit 9
        follows `REG_30H` bit 0 (0 at reset). The doc and model need reconciling. Item 31: resolved,
        `REG_0CH` reads 0x0080 at reset (unlocked, weak signal) and the test now passes.
- [x] Add `tests/run-qtest.sh` and a row in the checking table in `AGENTS.md` and `CONTRIBUTING.md`.
      Also added to `README.md` and as a CI step in `.github/workflows/build.yml`.

### P0.4 SVD-derived reset and access tables (S)

Needed by: items 8, 22, 23 and 32.

- [x] Add `scripts/gen_svd_regs.py`. It reads the SVD from a path argument and writes
      `include/hw/csky/dm32_svd.h`: per peripheral, each register's offset, reset value, and read-only,
      write-only and write-1-to-clear masks. Record the SVD's sha256 in the header. Also
      write-1-to-set and clear-on-read masks, base, size and PIC source, a flag on the 113 registers
      whose reset value is only inherited, `derivedFrom`, and the TIMER channel cluster. The lists are
      X-macros, so the header holds no data until a device uses it.
- [x] Add a small register-file helper. It serves reset values and enforces those masks, so a stub
      returns silicon reset values, not 0. `include/hw/csky/dm32_regfile.h`; no device uses it yet
      (items 8, 22, 23 and 32 will). `hw/csky/dm32.c` checks its addresses and PIC sources against
      the header at compile time.
- [x] Add a `--check` mode so a stale header fails the build checks. `tests/run-svd-tests.sh SVD`
      runs it, plus a host test of the register file and generator tests on a synthetic SVD. CI runs
      the script without the SVD, which lives in dm32uv-re, so CI cannot detect a stale header.

### P0.5 Pin-control core (M)

Needed by: items 13, 14, 15, 17, 19, 21, 27 and 28.

- [x] In `dm32_sysreg`, decode IO_DIPLEX0/1/2 from the DS pin-mux tables (IO_DIPLEX0 is not handled
      today), and add IOMGR_REN (pull-up) and IOMGR_IE (input enable). A per-pad table covers the 87
      GPIO pads that have a select field, pull bit or enable bit. IO_DIPLEX0, REN and IE now reset to
      their datasheet values; before, they read 0. The datasheet's pulls are up or down per pad, not
      only up. The guide's LCD_DB7 typo (PTC24) is corrected to PTC14. LCD_NRESET is taken to be PTC2
      (assumption, from the board DTS).
- [x] Export, through `include/hw/csky/dm32.h`: `dm32_pad_function(pad)`, `dm32_pad_pull(pad)`,
      `dm32_pad_input_enabled(pad)`, and a change notifier. Also `dm32_pad_is_gpio(pad)` and
      `DM32_PTA/PTB/PTC(n)` pad numbers, matching `DM32_PINSEQ`. The notifier has no user yet (items
      13 and 14).
- [x] Replace the three ad hoc checks (SPI0 pads from IO_DIPLEX1 bits 8:3, ADC pads from IO_DIPLEX2
      bit `19 + ch`, LCD DB pads from `IO_DIPLEX2 & 0xFE00`) with the API. The behaviour must not
      change (`tests/objhash.py` may differ; the stock regression must not). The old functions are
      removed. The stock and Snake regressions give the same counts and frames as before, all 203
      earlier qtests pass, and `tests/qtest/test_pinmux.py` (12 tests) covers the reset values and
      each user's per-pad decision.

### P0.6 GPIO pin attach points and bit-banged I2C (M)

Needed by: items 14, 24 and 30.

- [x] Generalise bank A's FD6818 forwarding into a per-pin attach API in `dm32.h`. An attached device
      gets the pad's output level and direction on change, and contributes an input level.
      `dm32_gpio_attach()`, `dm32_gpio_output()` and `dm32_gpio_input_changed()`. The FD6818 now
      attaches to GPIOA 7, 8 and 24 like any other device, and its three bus functions are private
      to `dm32_fd6818.c`.
- [x] Resolve shared lines wired-AND: a pad reads low if the SoC drives it low or any attached device
      pulls it low. Stock drives both levels on these buses (DTS), so do not require open drain.
      This applies to attached pads only; unattached pads read exactly as before.
- [x] Bridge two pin pairs to QEMU's core `bitbang_i2c` helper (`include/hw/i2c/bitbang_i2c.h`) so
      ordinary `I2CSlave` models attach: `alpu` on GPIOA 5 (SCL) and 6 (SDA), `fm` on GPIOA 26 (SCL)
      and 25 (SDA). `dm32_gpio_i2c_create()`. When one write moves both lines, a falling clock is
      applied first and a rising clock last. The board selects `BITBANG_I2C` and implies `AT24C`,
      which `tests/qtest/test_gpio_i2c.py` (6 tests) uses as a slave on both buses.
- [x] Check whether ALPU SDA (A6) shares anything with FD6818 DATA (A8). `devices.md` lists an
      unmodelled "interaction with the ALPU bit-bang on DATA". They share no pad. The stock
      bit-bang routine would use GPIOA 7/8 (the FD6818's CLK and DATA) with bus index 1, but the only
      store to the index (IRAM `0x000141C4`, from flash `0x03021B46`) writes 0. The note in
      `devices.md` is replaced with this finding.

---

## Subsystems, sorted by effort

### 1. HR_V3000 vocoder: retire the row (XS)

Rows: HR_V3000 vocoder. Current F. Target: **N/A** (not fitted).

- [ ] Replace the row (now N/A after P0.1) with "HR_C7000 built-in vocoder", folded into item 34. Cite
      `specs/include/hr_v3000.h` (no driver, no frame template, UART1 is the GPS) and DS 7.1.1.
- [x] Remove the HR_V3000 mentions from the roadmap and from "RF, audio and vocoder signal path".
- [ ] Keep "AMBE++ is absent by choice" against the built-in vocoder instead.

### 2. IRAM and SRAM: power-on contents (S)

Rows: IRAM / SRAM. Current A. Target: **A**, with a hardware-like power-on option.

- [x] Add `DM32_RAM_FILL=zero|noise[:seed]`, default `zero` so regressions do not change. `noise`
      fills untouched SRAM and IRAM with seeded pseudo-random bytes before `-device loader` runs.
      splitmix64 per region, reproducible per seed; a reset does not refill. `tests/qtest/test_ram_fill.py`
      (9 tests). Full-size loader images overwrite the fill, so a hardware-like run loads IRAM only to
      `0x4a7af` and SRAM only to `0x18004bff` (documented in `running.md`).
- [x] Verify the overlay trampoline's magic-value guard and Snake under `noise`. That reproduces the
      condition that broke the original zero-guard on hardware. With noise and the cut-down images,
      the overlay's last frame is byte-identical to the zero-fill run (banner drawn), and stock and
      Snake give the same steps, events and frame sequences as the zero-fill baseline.

### 3. Unassigned peripheral space (S)

Rows: Unassigned peripheral space. Current F (logged). Target: **F (logged), strict-capable**. An
address with no device stays F by definition. The goal is that it fails loudly on request and that
the space it covers shrinks as other items map their blocks.

Needs: P0.2.

- [x] Route the catch-all's accesses through P0.2's counter, so `DM32_STRICT=1` stops on the first
      one.
- [x] Cover `0x16010000`–`0x16FFFFFF`, `0x17001000`–`0x17FFFFFF` and every other gap. Done by P0.2's
      `dm32.unassigned` background region: such accesses are now `unassigned` events (logged under
      `-d invalid_mem`), not silent.
- [x] Decide whether gaps inside the peripheral range past `0x15FFFFFF` should log as `unimp` (a
      known peripheral window) rather than `unassigned`, once the SVD tables from P0.4 are available.
      Decided by the datasheet's address map (HR_C7000 v2.7 section 4.5.3, Table 26, the same blocks
      as the SVD): an address inside a block is `unimp`, named after the block; anything else is
      `unassigned`. Every block now has a `dm32.<name>` window under its model (from the generator's
      new `DM32_SVD_PERIPHERALS` list), plus `dm32.lcsfc` over the 16 MB LCSFC window. The 80 MB
      catch-all is gone, so a gap between blocks is now `unassigned`. The PIC's tail
      (`0x17001000`-`0x1700FFFF`) is now `unimp`.
- [x] [HW] What the silicon does on an unmapped access (a bus error or a 0 read) is unknown. Note it
      as open. Table 26 lists no reserved ranges and the datasheet does not describe the case. Noted
      in `coverage.md` (open items and permissive item 5), and as an `Assumption:` in `dm32.c`.

### 4. I2C0 and I2C1 controllers (S)

Rows: I2C0, I2C1. Current D and F. Target: **A** once item 10 lands, **B** before then. Nothing is
attached to either controller on this board: the I2C0 pads carry the ALPU bit-bang (P0.1). An
accurate model is therefore the controller with no slaves, so every address NAKs.

- [x] Instantiate `dm32_i2c` at `0x14060000` (PIC 8) and `0x14070000` (PIC 7) in place of the RAM
      window and the unimplemented device. Set the window to the SVD size. Done, now B. The slave
      binding, which was one file-scope hook shared by every instance (so the RTC would have answered
      on all three buses), is now per controller, and the RTC is bound to I2C2 only. A controller
      with no slave now NAKs a read instead of discarding it silently. The model keeps its 256-byte
      register window; since item 3, the rest of each 64 KB block is the block's unimplemented
      window, which logs instead of storing. PIC 8 and 7 stay unwired until item 10 gives the
      model an interrupt. Tests: `I2c01Test` in `tests/qtest/test_i2c.py` (4 tests).
- [x] Confirm by sweep that stock never touches either controller. The SVD records no base literal
      for I2C0. Also check for arithmetically built bases, which the PWM sweep missed. No access
      found: no `movih 0x1406`/`0x1407`, no exact base literal, and no constant chain that builds
      either base, in IRAM or flash. The positive control found all twelve I2C2 sites (`movih 0x1408`,
      flash `0x0301db06`-`0x0301dd04`).

### 5. Watchdog (S, then [HW])

Rows: Watchdog. Current B. Target: **B+** now, **A** after [HW].

- [x] Decide whether stock ever enables the watchdog. Sweep IRAM and flash for `0x14010000` built
      arithmetically as well as literally. Then run a long stock soak (menu and standby, more than
      10 minutes virtual) with `DM32_WDT_LOG`. It never does: the sweep (constant-chain tracking,
      checked against the timer and I2C2 as positive controls) finds no access and neither key
      anywhere, and an 11-minute standby soak made no access. A menu soak was not run separately;
      the Snake and menu regressions also make no access.
- [x] Model `WDG_RST_LOAD` as the reset pulse width when expiry fires. Take the clock from the clock
      tree once item 26 lands. The machine is held (vCPUs paused, from a bottom half: pausing from
      the expiry timer deadlocks) for `0x04000000 - RST_LOAD` ticks, then reset. The clock is still a
      constant plus `DM32_WDT_HZ`; the clock tree is item 26.
- [x] Settle the disabled-count rule (freeze or reload on re-enable) from DS 4.10, p.212–218, or mark
      it `Assumption:`. Settled: a disable leaves the load unchanged (p.214) and an enable counts from
      the load (p.215), so reload-on-enable is the datasheet's. Also corrected from the datasheet: the
      timeout is `2^35 - H:L` ticks (the counter counts up to the overflow, p.215-217), not `H:L`, so
      the reset values give 2^30 ticks, the datasheet's "about 60s". The default clock now makes
      that 60 s; the datasheet's 20 ms pulse would imply a different clock.
- [ ] [HW] Measure the timeout with reset values (the datasheet says about 60 s; the model gives 60 s
      at its assumed clock) and the clock. This is the A gate. `DM32_WDT_HZ` takes the result.

### 6. Timer (S)

Rows: Timer. Current B. Target: **A**.

- [x] Model ControlReg bit 1 as the DesignWare APB timer mode. Free-running (0) reloads from
      `0xFFFFFFFF`, and user-defined (1) reloads from LoadCount. Today every channel reloads from
      LoadCount. Confirm the bit meaning in the DS timer chapter first. The DS (4.8.3, p.188) names
      both modes but not the bit mapping or reload values, so the model follows the DesignWare
      convention and the SVD ("1 = user/periodic"), and says so. Stock uses mode 1 everywhere.
- [x] Check the DS for a prescaler or one-shot mode. The coverage note lists both as unmodelled, but
      the DesignWare timer has neither. Drop the note, or model what the DS shows. The DS documents
      neither; the note is dropped.
- [x] Model writes to LoadCount while enabled (reload at next expiry) and CurrentValue while disabled.
      A running channel's LoadCount write now takes effect at the next reload (it restarted the count
      at once before); stock re-periods running channels this way. CurrentValue is read-only (SVD)
      and a disabled channel keeps its count. Also: TIMERS_COMP_VERSION is now the DS's `0x3230322a`;
      the timer has a reset handler (a watchdog reset used to leave every channel running); and the
      board no longer starts channel 0 itself, which silicon does not do and stock does not need.
- [x] Verify channels 1–4 under qtest: period, IRQ to PIC sources 2–5, and the global
      `TimersIntStatus`, `TimersEOI` and `TimersRawIntStatus` registers. Done (`tests/qtest/test_timer.py`,
      22 tests). Static analysis shows stock uses all six channels.
- [ ] Take the 84 MHz clock from the clock tree once item 26 lands.

### 7. Cache controller (S)

Rows: Cache controller. Current D. Target: **B**, stretch **A**.

- [x] Model the CK803 cache registers at `0xE000F000` (CER, CIR, CRCR and the profiling
      registers, as laid out in the CK803 CSI core header in XuanTie open-yoc). Invalidate and clean
      operations self-clear. Enable bits are stored and logged. Done, now B: new device
      `hw/misc/dm32_cache.c` (registered in `integration/top-level.diff`), `DM32_CACHE_LOG`, and
      `tests/qtest/test_cache.py` (9 tests). Field layout verified against `core_803.h` in
      alibaba/AliOS-Things. Stock caches region 0 = `0x03000000`, 8 MB, write-back.
- [ ] Stretch, to A: track XIP lines read while the cache is enabled. After a program or erase of the
      program flash (item 29), flag an XIP read of the changed range that is not preceded by an
      invalidate. That is the hardware-only stale-code bug this block can cause. Blocked on item 29.
      Relevant finding: stock caches the whole program-flash window and never invalidates after
      boot, including after its run-time settings-sector writes (`0x0300B000`) and the YMODEM
      updater. Either the LCSFC write path keeps the cache coherent on silicon, or stock reads
      that sector only after a reset; which one is open and would make this check meaningful.

### 8. SPI slaves 0/1/2 (S)

Rows: SPI slaves 0/1/2. Current F (logged). Target: **C**, or **B** if the DS has a register table.
DTS marks them disabled and stock does not touch them.

Needs: P0.4.

- [x] [DS] Find a register table. The SVD has none for these blocks, and the DTS calls them
      `dahua,hr-c7000-spi-slave`. Found: the SPI chapter (section 5.2, p.243-263) documents one SSI
      that works "as a Master or Slave" with one register set and slave-only CTRLR0 fields, and gives
      the slave FIFO depth (8, p.244). The SVD in fact derives SPI_Slave0-2 from SPI_Master0.
- [x] If a table exists, use the P0.4 register file with its reset values and IRQs 34, 35 and 36, and
      no attached master. Otherwise keep the logged window and record that the DS gives nothing.
      Done, now B: `hw/misc/dm32_spi_slave.c` (the register file's first user), three instances on
      PIC 34-36. It models what a slave does without a master: TX FIFO and overflow, RX underflow,
      TXEI, the clear registers. `tests/qtest/test_spi_slave.py` (9 tests). A static sweep confirms
      stock never uses the slaves. Side finding for item 17: stock switches SPI1's pads to master
      at flash `0x03066750` (`IO_DIPLEX1 &= ~0x7E000`).

### 9. PIC (M)

Rows: PIC. Current B. Target: **A**.

- [x] Use the priority registers (`PIC_PRIOR0_3` to `PIC_PRIOR60_63`) for arbitration, with the DS
      rule for ties. The DS gives no tie rule: with `PIC_COW2.priority` 0 the order is fixed
      (IRQ0 first, p.179); with 1 the smallest byte wins, and equal bytes go to the lower source
      number (assumption).
- [x] Model `PIC_MODE`/`PIC_MODE_1` (trigger type) and `PIC_PO`/`PIC_PO_1` (polarity). Latch edge
      sources, and leave level sources following the line. Masked sources detect nothing (p.170).
- [x] Model `PIC_VECTOR`, `PIC_COW2`, `PIC_SYNC`, `PIC_FFLAG_L`/`_H`, `PIC_RECORD_SEL` and
      `PIC_INT_CNT` from the DS. `PIC_COW2.avec` 0 delivers the core autovector (table offset
      0x28, vector 10). `PIC_FFLAG` and `PIC_SYNC` are stored with no effect: the C7000 has no fast
      interrupts and the inputs are synchronous. All SVD reset values, so the three PIC
      expected-failure tests now pass.
- [x] Define EOI for nested delivery: what `PIC_COW1` clears when a higher-priority source preempted.
      One in-service register, no acknowledge from the core: nothing preempts, and `eoi` [2] ends
      the one source in service. Stock's dispatcher never nests. A level source also leaves service
      when its request ends (assumption), which lets stock's EOI-before-ISR order give one
      interrupt per tick; the board's PIC-EOI-to-timer coupling (`dm32_timer_eoi_source`) is
      removed. Measured: 3,500 timer expiries, 3,500 taken vector-33 interrupts.
- [x] Negative tests: wrong polarity loses an edge, an unacknowledged level source re-enters, and a
      priority inversion is visible. `tests/qtest/test_pic.py` (34 tests), read through the PIC's
      `cpu-line` property; `test_timer.py` replays the stock tick sequence.

### 10. I2C2 controller (M)

Rows: I2C2 controller (and I2C0/I2C1 through item 4). Current B. Target: **A**.

- [x] Model the DesignWare APB I2C interrupts on PIC 9: `IC_INTR_STAT`, `IC_INTR_MASK` and
      `IC_RAW_INTR_STAT`, the `IC_CLR_*` read-to-clear registers, and the `IC_RX_TL`/`IC_TX_TL`
      thresholds (DS 5.1, p.218–243). Also on PIC 8 and 7 for I2C0 and I2C1. The DS's two RX_FULL
      rules disagree (p.232 "≥", p.233 ">"); the model follows `IC_RX_TL`'s own text.
- [x] Model `IC_TX_ABRT_SOURCE`, `IC_ENABLE_STATUS`, STOP_DET, START_DET and ACTIVITY. Found: the
      HR_C7000 block is a cut-down DW master. `IC_DATA_CMD` has no STOP or RESTART bits, a
      transfer starts on the extra `IC_START` register (`0xa0`), and STOP follows the byte that
      empties the TX FIFO (p.222). No standard-mode count or COMP registers exist.
- [x] Time transfers from `IC_FS_SCL_HCNT`/`LCNT` and the clock tree, in virtual time, with real TX
      and RX FIFO levels. Period `(HCNT + 8) + (LCNT + 1)` APB cycles (p.220), 84 MHz until item 26
      (stock's clock variable at IRAM `0x3a60c` holds 84000000; its counts give 2.5 us). FIFOs 8
      deep (assumption, from the 4-bit levels). The controller now drives a QEMU `I2CBus` per
      instance (`i2c0`-`i2c2`), and the RTC is a QEMU `I2CSlave`; the old block-callback slave API
      is gone. Any QEMU I2C device attaches with `-device ...,bus=i2c<n>`.
- [x] Reject a write to `IC_TAR` or `IC_CON` while enabled, as the DS says. The DS does not say it;
      its init sequence only disables first. Refused and logged as an assumption, with the SCL
      counts too. Stock always disables first.
- [x] Verify stock's RTC reads still complete and that the UI tick still starts. Stock boot: 75
      RTC reads in 20 s, no abort, the default-date write lands; stock frames byte-identical to
      the previous model's, Snake walkthrough complete. `tests/qtest/test_i2c.py` now 37 tests (the
      write-NAK expected failure passes); a mutation (8-period bytes, no lock) fails the timing and
      lock tests.

### 11. RTC (M)

Rows: RTC. Current B. Target: **A**.

- [x] Add the MATCH comparison and the alarm interrupt on PIC 19 (DTS), gated by CCR bit 0. DS 4.9.3:
      MATCH_VEN copies MATCH_* into an internal match register and clears itself; a match with
      CCR[0] set sets RSTAT (`0x13`, read-clear) and holds PIC 19 high (Table 30, level). Found
      registers the model lacked: RSTAT, CLKDIV_NUMER_L/H/VLD (count at 32768 / t Hz) and
      SOFT_RST_N (`0x17`, reset 1), all now modelled. Stock never sets an alarm or unmasks PIC 19.
- [x] Define `LOAD_VSTAT` from the DS. It is software's power-fail guard ("pull this bit high after
      configuring the LOAD related registers", 4.9.5.19), RW bit 0, no hardware effect, reset 0;
      MATCH_VSTAT is the same for MATCH. Stock reloads its default time whenever LOAD_VSTAT reads 0
      (`rtc_validate_and_set_default_datetime` @0x0301c608), and pulses SOFT_RST_N first.
- [x] Count from QEMU's RTC clock (`-rtc base=...,clock=vm`) instead of host `time()`, so runs are
      reproducible. Keep host time as the default if the stock regression relies on it. Counts on
      `rtc_clock`, which is host time by default, so the default is unchanged; the tests run with
      `-rtc clock=vm` and assert exact times. Power-up is now the DS reset (count 0, stopped)
      instead of counting from 00:00:00; stock starts it and loads its default either way.
- [x] Confirm from the DS that values are binary, not BCD, and cite the section. The DS never says
      BCD: 6-bit second and minute, 5-bit hour and 16-bit day registers that count to 60 (4.9.2).
      Stock reads the five bytes unmasked as binary numbers and counts days from 1970-01-01
      (`rtc_decode_calendar_date` @0x0301c2f0; its sign-extended day limits it to 2059-09-18).
      Tests: `RtcTest` in `test_i2c.py`, 14 tests on the virtual clock; disabling the CCR[0] gate
      or the MATCH_VEN copy each fails one.

### 12. Reset and system control (M)

Rows: Reset / system control. Current C. Target: **A**.

- [x] Model `SYS_SOFT_RSTN` (`0x11000000 + 0x00`) as the DS's 9-bit, self-clearing, per-module soft
      reset. Map each bit to its module from the DS and call that device's reset. DS 4.1.4.1: [8]
      CPU, [7] system global, [6] CPU-side ADC, [5] baseband ADC, [4] codec, [3] audio, [2] FM,
      [1] PHY, [0] protocol. [8] resets the CPU alone (`async_run_on_cpu`), [7] is a machine reset
      that spares the system registers (DS: "except the register configuration module"), [6]
      resets `dm32_adc`; [5:0] have no model and are logged under `-d unimp`. Found: stock uses the
      register at every boot (`0x190`), at radio mode changes (bits 0-1), and to reboot (`0xFE7F`
      from CPS, upgrade and factory reset), which the old storage-only model ignored. Verified:
      `0xFE7F` written at standby reboots stock under emulation.
- [x] Each device model must have a correct `reset` handler. Check every `dm32_*` file, starting
      with FD6818, which today initialises once at board creation. Added to sysreg, ADC, DAC, PWM,
      GPIO, SSI and the LCD interface. Not reset, as separate parts or memory, documented in each
      file and in devices.md "Resets": FD6818 (assumption: no SoC reset line), the LCD panel, the
      NOR chip, the RTC (assumption: own power domain), DMR RAM.
- [x] Test that a module held in reset ignores register writes, if the DS says so. It does not: the
      bits self-release, so no module is ever held in reset. `tests/qtest/test_reset.py` (9 tests)
      covers the bits and every machine reset instead; a mutation (system registers wiped, ADC
      not registered) fails two of them.

### 13. Pin mux IO_DIPLEX (M)

Rows: Pin mux IO_DIPLEX1/2. Current B. Target: **A**.

Needs: P0.5.

- [x] Every block with pads consults P0.5: GPIO (item 14), SSI per instance (items 15 and 17), UARTs
      (item 27), PWM output (item 21), ADC (item 19), LCD (item 28), and the CLKOUT_PLLA pad on
      GPIOC 0. GPIO: a pin reaches its pad only on the GPIO select, and IE 0 reads 0. SSI: SPI0
      PTA20-22, SPI1 PTA25-27, SPI2 PTC4/8/7 at select 1; SSI3 ("EFUSE only") and `0x04000000`
      (NFC pads) are dedicated, so stock's `ssi_discard` is now 0. I2C0/1 NAK without PTA5-8. PWM
      start without PTA15-17. UART1-3 TX through a forwarding window over THR/LCR in `dm32.c`
      (csky_uart cannot be edited); receive is not gated. ADC and LCD unchanged. CLK_OUT is the GPIO
      rule on PTC0 (stock selects PLLA there at boot).
- [x] Warn when two blocks claim one pad. Warn when a block transfers on an unclaimed pad, in the
      same style as `muxwarn`. The DS routes each signal to one pad, so a double claim is GPIO driving
      a peripheral's pad: `pad_conflict`, one stderr line per pad. Transfers without pads:
      `pad_unclaimed` (I2C0/1, PWM, UART1-3) beside the existing `ssi_discard`, `adc_pad`,
      `lcd_mux`. Found: stock drives GPIOC 13 (LCD_DB6) as a keypad column while the LCD owns it
      (`pad_conflict` 4 per stock boot); open question in coverage.md.
- [x] Seed IOMGR_REN and IOMGR_IE reset values from the DS. Already done by P0.5, and checked against
      the DS here. Also IO_DIPLEX2 now resets to the DS `0x3FFFFFFB`; the old `0x3FF80100` was the
      post-boot value, which stock's LCD claim and CLK_OUT select reproduce. Tests: 11 new in
      `test_pinmux.py` and one in `test_gpio_i2c.py`; removing the GPIO gate fails the latter.

### 14. GPIO A/B/C (M)

Rows: GPIO A/B/C. Current C+ (IRQ block B). Target: **A**.

Needs: P0.5, P0.6.

- [x] A pad muxed away from GPIO reads the alternate function's level, or the DS default, not the GPIO
      input word. GPIO interrupts on it do not fire. The DS gives neither, so it reads 0
      (assumption) and is excluded from INTEN; at reset that is GPIOA 0-4 (JTAG). No stock input
      read of a muxed-away pad was found (IRAM swept fully, flash partly). Also: `EXT_PORTA` is now the pad level, so a driven pin reads its
      `SWPORTA_DR` bit, and writes to `+0x50` are ignored (read-only per DS and SVD; the old echo
      had no stock user).
- [x] Derive idle levels from IOMGR_REN pulls instead of the "all high except an idle-low list" rule.
      Keep `DM32_GPIO_IDLE_LOW` as an override for board-level pulls the SoC does not know about, such
      as power-sense on A16. An enabled pull-up reads 1 and pull-down 0; a pad with no pull reads high
      (assumption: board pull-ups; the DS gives no level). Pulls reset off (DS); stock enables one,
      PTA24's pull-up (REN_REG0 bit 24, the FD6818 CS line), which reads high either way.
- [x] Model `DEBOUNCE` in virtual time on the debounce clock, from the DS. The DS names no clock and
      no cycle count (section 5.7.2: "de-jitter ... sampling time is configurable"). Modelled as the
      DesignWare block does it: interrupt inputs only, two cycles, on the 32.768 kHz RTC clock
      (assumption), 61 us; shorter pulses are lost. `DM32_PINSEQ` now takes fractional milliseconds
      for the tests. Found and fixed on the way: `INTR_BOTH_EDGE` and `INT_POLARITY` were applied as
      whole-word booleans, not per bit.
- [x] Fix the stale Up/Down comment (P0.1). Already fixed by P0.1 (the comment names the side keys
      SK1/SK2 on GPIOB 19/20 and calls Up and Down matrix keys); checked against the DTS table.
- [x] Negative test: a keypad scan with the wrong drive sense reads nothing plausible. The injector
      now samples the column pad at the row read (output, data 0, pad on GPIO) instead of latching
      the data write; a column driven high, left an input, or on the LCD bus gives no key. Stock and
      Snake regressions unchanged (stock frames byte-identical). Tests: 11 new in `test_gpio.py`;
      bypassing the debounce or the per-bit edge fix fails three.

### 15. SPI0 controller (M)

Rows: SPI0. Current B+. Target: **A**.

- [x] Model the full DesignWare APB SSI: `CTRLR0` (frame size, TMOD transmit-only, receive-only and
      EEPROM-read modes), `CTRLR1` (NDF), the `TXFTLR`/`RXFTLR` thresholds, real TX FIFO levels,
      `SR.BUSY` in virtual time from `BAUDR` and the clock tree, and `IMR`/`ISR`/`RISR` with the
      `*ICR` clear registers on PIC 16. Found: the HR_C7000 block is a DesignWare variant, with
      `CTRLR0` `[9:8]` TMOD, `[5]` bit order and `[4:0]` DFS, 128-entry FIFOs (p.244), no DMACR,
      MSTI or RX_SAMPLE_DLY, and IDR/version registers. Frames take `(DFS + 1) * SCKDV` cycles of
      the 84 MHz APB clock (p.247), computed lazily at each register access, so stock's per-byte
      polling costs no timer; a timer runs only while an interrupt is enabled. Also wired PIC 22
      and 23 for SPI1/SPI2 (item 17).
- [x] Model RX FIFO overflow (RXOI) when firmware does not drain, instead of silently keeping 8
      entries. 128 entries; RXO, TXO and RXU latch until their clear register is read. Stock reads
      every byte back, so it never overflows.
- [x] Reject `CTRLR0` and `BAUDR` writes while `SSIENR` is set, as the DS says. The DS says only
      "when 1, disable configuration register" (p.256); the model locks CTRLR0, CTRLR1, BAUDR, TXFTLR
      and RXFTLR (assumption, DesignWare) and logs refused writes. Stock writes only SER and SSIENR
      while enabled.
- [x] Keep the chip select on GPIOA 18 for the data chip. Use `SER` only for the controller's own
      chip-select lines. SSI0's chip is attached to pad PTA18 and selected while it is driven low;
      the old hook that reset every instance's NOR state on any GPIO data write in any bank is gone.
      The other instances select with SER. Verified: stock's SPI-NOR command log over a 15 s boot
      (1230 RDSR, 372 programs, 38 erases) is byte-identical to the previous model's, stock frames
      unchanged. Tests: `test_ssi.py` rewritten around stock's set-up (42 tests); removing the timing
      or the GPIOA 18 select fails four.

### 16. Data flash W25Q128 (M)

Rows: Data flash (W25Q128). Current B+. Target: **A**.

- [ ] Time WIP in virtual time from the W25Q128JV Rev F typical figures for page program, 4 KB, 32 KB
      and 64 KB erase, and chip erase. Keep `DM32_SPINOR_WIP_READS` as an optional override.
- [ ] Reject every command except RDSR (and suspend) while busy. Clear WEL at the end of a program,
      erase or WRSR.
- [ ] Model status registers 1–3 (`05h`/`35h`/`15h`, written by `01h`/`31h`/`11h`, plus `50h`
      volatile writes). Enforce BP, TB, SEC, CMP and the SRP lock on program and erase.
- [ ] Add `52h` (32 KB erase), `4Bh` unique ID (from `DM32_SPINOR_UID`, default a fixed synthetic
      value), `48h`/`42h`/`44h` security registers (default `0xff`, matching what the radio
      returned), `75h`/`7Ah` suspend and resume, `66h`/`99h` reset, and `90h`/`ABh` IDs. Use RDL
      `w25q128jv.rdl` for the opcode table.
- [ ] Handle page-program wrap within a 256-byte page.
- [ ] Reconcile the two recorded erase counts (nine 4 KB erases against 27 `ERASE` lines) with a
      logged run.

### 17. SPI1, SPI2, SPI3 and 0x04000000 (M)

Rows: SPI1/2/3 and 0x04000000. Current C. Target: **A** for the SPI1 and SPI2 controllers, **B+**
for SSI3 and 0x04000000.

Needs: item 15. The SSI3 half needs item 18, and the 0x04000000 half needs item 29.

- [x] Gate each instance on its own pads (P0.5), not SPI0's. Today stock's quad-enable sequence on
      `0x04000000` runs before the SPI0 pads are muxed and is dropped (two `ssi_discard` events per
      boot, P0.2). When this lands, `ssi_discard` should read 0 on a stock boot. Done with item 13;
      `ssi_discard` reads 0.
- [ ] Give each instance its own attached device instead of a copy of the data-chip image.
      - SPI1 and SPI2: nothing attached; MISO reads idle (`0xFF`). SPI1's pads carry the RDA5807M
        bit-bang (DTS), so the pad-conflict warning from item 13 applies.
      - SSI3: the eFuse slave (item 18).
      - `0x04000000`: the GD25Q80 program flash on the same store as XIP (item 29), with `35h`, `06h`,
        `01h` and `05h` and the QE bit, so stock's quad-enable sequence at `0x0002bb98` runs against
        real status bits.
- [x] Wire IRQs from DTS: SPI1 22, SPI2 23. SSI3 and `0x04000000` have none listed. Done with item 15.
- [ ] Drop the multi-instance `DM32_SPINOR_OUT` path, which is now unnecessary because only SPI0 holds
      the data chip.
- [ ] [HW] Confirm `0x04000000` is the program flash's raw controller. The evidence is structural (RDL
      `gd25q80.rdl`).

### 18. eFuse ID over SPI_Master3 (M)

Rows: eFuse ID. Current F. Target: **B**. A needs [HW]: the eFuse protocol on silicon is not
documented in any DS edition (DTS).

Needs: item 15. Item 17 attaches the slave to SSI3.

- [ ] Trace the stock eFuse read on `0x14150000` (fourth SSI table entry, `0x030ada0c`): opcode
      bytes, chip select, and which eight bytes it keeps.
- [ ] Add an eFuse slave on SSI3 that answers that sequence from `DM32_EFUSE=<hex or file>` [EXT].
      Never commit per-unit bytes.
- [ ] Choose a default synthetic eFuse value whose derived record equals what
      `compose_qemu_image.py` patches at `0xB10C` (`ff 18 ff ff`). Document the derivation from
      `scrub_unit_sector.py`. Then the hardware-binding patch can be dropped from the QEMU image.
- [ ] Verify stock boots without the HWBIND patch.

### 19. ADC (M)

Rows: ADC. Current B. Target: **A** for the digital block. Inputs stay injected values by design.

- [ ] Time conversions from `SCAN_TIME`, `PD_SEOC_TIME` and the clock tree. `CTRL_STATE` reads busy
      while converting.
- [ ] Model continuous mode (`ADC_CTRL` bit 2) and multi-channel scan, from DS 5.9.
- [ ] Drive the interrupt on PIC 26 from `ADC_INTR`, with the `INTR_THRES` and `INTR_DELTA` rules
      from the DS.
- [ ] Model `P2S_EN` and the first-sample behaviour, if the DS defines them.
- [ ] Add `DM32_ADC_SEQ` for time-varying inputs in virtual time, in the `DM32_PINSEQ` style, and
      optional seeded noise.
- [ ] Document that emulated noise says nothing about hardware entropy. Channels 1 and 2 seed the
      `mi_rng` patch in dm32uv-re, whose entropy is unmeasured.
- [ ] [HW] Battery scaling on channel 2 is unknown (DTS). This does not block A for the block itself.

### 20. DAC (M)

Rows: DAC. Current B. Target: **A** for the digital block.

- [ ] Model `DAC_PD_MODE_EN` from the DS. Today it is stored with no effect.
- [ ] Drive the interrupt on PIC 25, from the DS rule.
- [ ] Publish each channel's code as an output (a `qemu_irq` level or a named property) that other
      models can read. The TX-power ramp (`FUN_0002B364`, dm32uv-re `docs/tx-power-control.md`)
      feeds the RF model through it.
- [ ] Find DATA_C's consumer (`0x6E2` at boot) and DATA_B's role in the power ramp. The DTS says the
      consumer is unknown.

### 21. PWM (M)

Rows: PWM. Current B. Target: **B+**. A needs [DS] or [HW] for the interrupt event rule, which the
SVD does not state.

- [ ] Generate the waveform in virtual time from the latched PER and FP. Model `RPT` pulse counting
      and one-shot completion (`busy` clears).
- [ ] Drive the output pad (GPIOA 15 for PWM0) through P0.5, honouring `fpout` and `inactout`.
- [ ] Carry the backlight duty into `DM32_GRAMDUMP` output. For example, scale the dumped frame, or
      record the duty in the dump's metadata, so a dark-screen bug is visible in emulation.
- [ ] Characterise the Light Time (idle timeout) path: which writes stock makes when the backlight
      times out, and when.
- [ ] [DS] Find which event sets `pwmN_intr`, then wire PIC 18 and make `PWM_INTR_STA` live. That is
      the A gate.

### 22. SDIO (M)

Rows: SDIO. Current F (logged). Target: **B**, stretch **A**. The DM-32 has no card slot (DTS), so an
accurate model is a DesignWare Mobile Storage host with no card.

Needs: P0.4.

- [ ] Serve SVD reset values (DS 5.5, p.285–325). `CDETECT` reports no card.
- [ ] `CTRL` reset bits self-clear. `CMD.start_cmd` self-clears, and a command completes with a
      response timeout (`RINTSTS.RTO`) and command done. Clock-update-only commands complete without
      a response. Wire PIC 20.
- [ ] Stretch: make `HLE` and the FIFO threshold behaviour match the DS for misuse cases.

### 23. USB (M)

Rows: USB. Current F (logged). Target: **C+**, stretch **B**. Stock does not use USB in any run, and
a device-mode DWC2 with a host-side backend is XL with no consumer. See "Not planned".

Needs: P0.4.

- [ ] Serve SVD reset values for the DWC2 OTG core (DS 5.6, p.325–377): `GSNPSID = 0x4f54270a`, and
      `GHWCFG1`–`4` and `GUSBCFG.PHYSel = 1` from the DS.
- [ ] `GRSTCTL.CSftRst` self-clears, and `AHBIdle` reads set. `GINTSTS` is write-1-to-clear, and a
      no-session (cable absent) state is reported. Wire PIC 21.
- [ ] Stretch, to B: a USB-reset and enumeration-start sequence without data transfer, if a firmware
      port needs it.

### 24. RDA5807M tuner (M)

Rows: RDA5807M tuner. Current F. Target: **A** at the register level. Audio stays absent.

Needs: P0.6.

- [x] Add an `I2CSlave` at 7-bit address `0x10` on the `fm` bit-bang bus (GPIOA 26 SCL, 25 SDA).
      `hw/misc/dm32_rda5807m.c`, attached by the board.
- [x] Implement sequential access as RDL `rda5807m.rdl` describes it. Writes start at register `02H`,
      reads at `0AH`. The counter auto-increments, high byte first, and wraps from `3AH` to `00H`.
      There is no register address on the wire. Reset values from DS section 6; status and RDS
      registers read-only; hidden `10H`-`3AH` writable only with `06H` OPEN_MODE = 11. With the DS
      wrap, stock's table (which writes `06H = 0x4000`) has its hidden-register words ignored and its
      last four words land on `00H`-`03H`, which the power-down and tune paths rewrite anyway.
- [x] Model tune and seek: `STC` sets after a virtual-time delay, then `READCHAN`, `SF`, `ST`,
      `FM_TRUE` and `RSSI` (from `DM32_FM_RSSI`, or from the RF environment in item 31). Model
      power-down (`02H = 0x8000`) and soft reset. Stations come from `DM32_FM_STATIONS`
      (`kHz[:rssi]`). The DS gives no times: tune 50 ms and seek 10 ms per channel are assumptions,
      as is a seek starting from CHAN at its first step (stock writes `02H` SEEK before `03H` CHAN).
- [x] Stock samples no ACK on this bus (`0x030654d8`). The slave must ACK correctly anyway, and the
      model warns when the master ignores a NAK. The tuner ACKs every byte; the GPIO bridge counts
      a master that clocks on after a slave's NAK as the new event `i2c_nak`, with one stderr
      warning per bus. Found: stock raises it 504 times per boot on the `alpu` bus, where no ALPU-MP
      is modelled (item 30); the `fm` bus is clean.
- [x] Verify against the stock driver: the init image of `0x7a` bytes from `0x030b790c`, volume, tune,
      seek and status read, with the FM radio screen driven by key injection. Init, power-down and
      volume run at every stock boot and reach the model as the RDL describes (logged). No key path
      to the FM screen was found under emulation (the side keys do nothing with a blank codeplug),
      so tune, seek and the status poll are verified by replaying stock's transfers on the GPIO
      pads in `tests/qtest/test_rda5807m.py` (17 tests), including its poll's acceptance test and
      frequency formula. Removing the 3AH wrap fails four. Driving the FM screen stays open.

### 25. Boot ROM (M; [EXT] for A)

Rows: Boot ROM. Current D. Target: **A** with a user-supplied ROM image [EXT], **C** without one.

- [ ] Without an image, map `0x00000000`–`0x0000FFFF` as read-only instead of RAM, so guest stores are
      dropped and counted (strict-capable). Check that stock never writes there.
- [ ] Add `DM32_BOOTROM=<file>` (2 KB). Mirror it 32 times across the 64 KB window.
- [ ] When an image is given, stop forcing `vbr = 0x03000004` and `pc = 0x03001000`. Reset to the
      ROM's entry instead, and let the ROM check DH_FLAG (`0x0c646874`) and jump to the image.
- [ ] Verify the `02 24 00 03` announcement on UART0. It depends on the UART's LSR bit 5 polarity
      (item 27), which `boot-rom.md` describes.
- [ ] Exercise the ROM's boot-mode decision with injected pins, and record which pins it reads.
- [ ] Update `boot-rom.md`, including the untested loader path it describes.

### 26. Clock manager and clock tree (L)

Rows: Clock manager. Current C. Target: **A**. Several items above take absolute timing from this.

- [ ] Model the clock-manager registers at `+0x04`–`+0x2c` and `LCSFC_BAUDR` from the DS register map.
- [ ] Model PLL state: `pll_ld` and `clk_rdy` assert after a lock delay from the DS, or an
      `Assumption:` value, not on every read. Stock's busy-wait at IRAM `0x12480` must still pass.
- [ ] Detect invalid divider and PLL settings and log them as guest errors. Strict mode stops on
      them.
- [ ] Model the hardware hand-over state the roadmap names.
- [ ] Build a QEMU Clock tree (`qdev_init_clock_in/out`) from the sysreg to every timed consumer:
      timer, watchdog, UARTs, SSI, I2C, PWM and ADC. Each consumer derives its rate from the tree, not
      from a constant.
- [ ] Negative test: reprogramming the PLL changes the timer period, as it does on silicon.

### 27. UART0 and UART1–3 (L)

Rows: UART0, UART1/UART2/UART3. Current B and B (unverified). Target: **A** for UART0, **B+** for
UART1–3. A for UART1–3 needs [HW] to confirm their pins and PIC lines.

- [ ] Replace `csky_uart` on this board with a `dm32_uart` model in this repository (csky-qemu files
      are not edited). Use QEMU's core 16550 (`serial-mm`) for the standard registers, plus the
      DesignWare extensions from the SVD: `USR`, `TFL` and `RFL`.
- [ ] Model FIFO depth from the DS, `IIR` interrupt causes and priorities, PTIME and the LSR bit 5
      meaning (transmit FIFO full) as the boot ROM uses it, and character timing from `DLL`/`DLH`
      and the clock tree.
- [ ] Attach a counting sink when no `-serial` chardev is given for an index, so "no bytes sent" is
      reported as a count, not inferred from an empty capture.
- [ ] UART0: verify the CPS protocol end to end. Drive a codeplug read from the host with dm32uv-re's
      `specs/ksy/cps_serial.ksy` and `hdboot_serial.ksy` fixtures, and compare the bytes.
- [ ] UART1: add `DM32_GPS_NMEA=<file>` to replay NMEA at 9600 baud into the receive side. Document
      how to set option byte bit 2 so `gps_uart1_init_9600` runs, and verify `$GPRMC` parsing on the
      GPS screen.
- [ ] Gate each UART on its pads (P0.5).

### 28. I80 / LCD (L)

Rows: I80 / LCD. Current B. Target: **A**.

- [ ] Map the controller registers by their SVD names: `INDEX` (`+0x00`), `DATA` (`+0x04`), `WCFG`,
      `RCFG`, `SCFG`, `UNDOC_0x28` (the enable handshake) and `AC_MODE`. Today the model knows
      `+0x00`, `+0x04`, `+0x10` and `+0x28` only.
- [ ] Charge each bus write a virtual-time cost from `WCFG` and the clock tree, so frame render time
      is measurable.
- [ ] Implement the panel reads that `RCFG` enables: `04h` (RDDID), `09h` (RDDST), `0Ah` and `0Ch`,
      from the ST7789V datasheet.
- [ ] Implement the ST7789V state stock's init uses (dm32uv-re `specs/lcd/st7789v_stock_init.txt`):
      `SLPOUT` and its 120 ms rule, `DISPON`/`DISPOFF` gating the visible image, `INVON` (`0x21`,
      which stock sends) inverting displayed colours, `COLMOD 0x3A 05`, and `TEON 0x35`.
- [ ] Apply MADCTL MY, MX, MV, ML and RGB to the GRAM address mapping, per the ST7789V datasheet.
      This replaces the current "decoded but not applied" note. Check the existing stock frames still
      match.
- [ ] Reset the panel on GPIOC 2 (`LCD_RESET_N`) and honour chip select on GPIOC 3.

### 29. Program flash: XIP, tail and LCSFC command path (L; [HW] for A)

Rows: Program flash (XIP), Program flash tail. Current D+ and F. Target: **B** for both. A needs [HW]
confirmation that the silicon forwards window stores as commands. The firmware evidence is strong:
two independent drivers rely on it, the application's and the YMODEM upgrade loader's
(`0x03003d30`–`0x03004010`).

Needs: P0.2. Item 7's stretch goal builds on this.

- [ ] Implement the store interface from RDL `gd25q80.rdl`. A byte `06h`/`04h` to `0x03000000` sets or
      clears WEL. A word `(20h << 24 | addr)` to `0x03000000` erases a 4 KB sector. A word
      `(02h << 24 | addr)` to `0x03800000`, then data bytes to `0x03800000`, then the last byte to
      `0x03000000`, programs a page (AND semantics, 256-byte wrap).
- [ ] While a program or erase is busy, XIP reads of `0x03000000` must not return DH_FLAG, so stock's
      completion poll at `0x0002bc78` waits a realistic time (GD25Q80C datasheet timing). Switch the
      ROM device to I/O mode for the busy window.
- [ ] Map the full 16 MB LCSFC window. Reads above 1 MB alias the 1 MB part (it ignores the upper
      address bits; confirm from the GD25Q80C datasheet), and `0x03800000` is the data port.
- [ ] Persist to `DM32_FLASH_OUT` (`.tmp` then rename, as the data chip does). Never modify the
      `-kernel` file. Retire `DM32_FLASH_WRITABLE`.
- [ ] Share this store with the `0x04000000` raw controller (item 17).
- [ ] Wire the LCSFC interrupt on PIC 24. Model `QUAD_ENABLE` (`+0x5c`) and `LCSFC_BAUDR` (`+0x30`).
- [ ] Refuse, with a loud log, any erase of `0x000000`–`0x00BFFF` unless `DM32_FLASH_ALLOW_BOOT=1`.
      This mirrors the RDL warning and keeps an emulated upgrade from silently wiping the boot sector.
- [ ] Verify with the stock YMODEM upgrade loader: drive an upgrade over UART0 under emulation and
      compare the result with the update file.

### 30. ALPU-MP (L; [EXT] for A)

Rows: ALPU-MP. Current F. Target: **B** (stock passes the check with no bypass sector), **A** with
key material supplied at run time [EXT], so that a wrong response fails as it does on the chip.

Needs: P0.6.

- [ ] Trace the parts of the handshake not yet traced: the write loop in `alpu_handshake_step_sequence`
      @0x0001f96c, and the steps after the challenge-response (`0x1e8bc`, `0x1f798`). Record what the
      chip must return at each step.
- [ ] Read the chip-side protocol in the ALPU-MP datasheet v2.16 (dm32 `hardware/ALPU-MP-datasheet/`):
      register map, challenge generation, and how it reports pass or fail.
- [ ] Add an `I2CSlave` at 7-bit `0x3d` (wire byte `0x7a`) on the `alpu` bit-bang bus. Register
      `0x30` returns a 16-byte challenge and accepts the 16-byte response.
- [ ] Add mode `DM32_ALPU=accept`, which answers every step as a genuine chip would and accepts any
      response. That is the B target.
- [ ] Add mode `DM32_ALPU=verify:<keyfile>`. Reimplement `alpu_block_cipher` @0x0001f028 (including the
      five S-op routines `0x1eafc`–`0x1ef00`) in C. Load the per-round tables (SRAM `0x18000bd4` and
      `0x18000be4`) and the static vectors from the file, never from this repository. Compute the
      expected response, and fail the handshake on a mismatch. First establish whether those tables
      come from the generic SRAM init image or are built at run time. That decides whether the key
      file can be derived from the stock image alone.
- [ ] Verify: stock boots to standby and runs the Snake walkthrough without
      `sector_030c6000_alpubypass.bin`, including the seven in-operation call sites of
      `alpu_auth_gate_3retry`. In `verify` mode, a corrupted response reaches the halt at IRAM
      `0x169d4`.
- [ ] Update `compose_qemu_image.py` usage notes in dm32uv-re once the bypass is optional. That is a
      dm32uv-re change, tracked there.

### 31. FD6818 RF chip and the RF environment (L)

Rows: FD6818 RF chip. Current B. Target: **A** at the register level. The RF signal itself is
functional (B), driven by an injected RF environment.

Needs: item 20 for the power-ramp input. Items 24, 33 and 34 consume the RF environment.

- [x] Model the `REG_00H` soft reset (`0x8000`, then `0x0000`): restore reset values. A write with
      bit 15 set restores every register, both pages, and the detection state, and turns the PLL off.
      What the guide's soft reset restores is not stated; the model restores everything (assumption).
- [x] Implement page select `REG_7FH` properly, addressing the paged registers from the FD6818
      programming guide, instead of storing the bit. Page 1 holds `0x80`–`0xFF` at wire address
      register minus `0x80`; wire address `0x7F` is the command on both pages and reads 0.
- [x] Implement the remaining `REG_0CH` bits (10–11 and 14–15) and the other read-only status
      registers (`0x01`, `0x02`, `0x0B`, `0x60`–`0x67`, `0x87`) from the guide. The guide marks
      `REG_0CH` bits 15:10 reserved, so they read 0; their FD6818B meaning is still open. `REG_0CH`
      has pll_lock, weak_rssi, sq_out and int_out. `0x02` is the interrupt vector, `0x60` the CTCSS
      and CDCSS detection, and `0x61`, `0x62`, `0x65` and `0x67` the measurements. `0x0B` (DTMF), `0x64`
      (VOX) and `0x87` (compander) read 0 because DTMF, VOX and audio are not modelled.
- [x] Make `REG_28H` reflect the chip's live GPIO state, which reads `0x6B38` on hardware. Drive
      FD6818_GPIO0 and GPIO1 into GPIOB 27 and 26. `gpio_in_val` reads `0x6B` at reset. That needs
      no pin driven at reset, so `REG_25H` resets to `0xFF00`, not the guide's `0x0000` (assumption).
      A driven pin carries its `REG_26H`/`27H` function (out_val, INT, SQ, VOX, sub-audio compare).
      An undriven GPIO0/1 reads the GPIOB pad. `[5:0]` reset to the guide's `0x3F`, not the `0x38`
      one radio read.
- [x] Add a PLL lock delay after a frequency change (`REG_38H`/`39H` or `REG_30H`), so `pll_lock`
      in `REG_0CH` is not instantaneous. Lock comes `DM32_FD6818_LOCK_US` (40) after the `REG_39H`
      write or the PLL switch-on. A `REG_38H` write holds it unlocked until `REG_39H` follows, as
      hardware did with the high word first. While locked, a `REG_38H` write is a lock-lost
      interrupt.
- [x] Add an RF environment input, `DM32_RF_ENV=<script>`, with entries for virtual time, frequency,
      level, and modulation (none, FM with a CTCSS or DCS code, or DMR). It drives RSSI in `REG_62H`
      (both taps), and noise for the squelch. It also drives `rssi_sq`, SNR and noise in `0x61`,
      `0x65` and `0x67`. `*` is the tuned frequency, which stock's blank codeplug sets to 0 Hz. A
      `floor` line sets the noise floor.
- [x] Compute `sq_out` from RSSI and noise against the `REG_78H` thresholds (0.5 dB per step), per
      the guide, and drop the jitter stand-in when an environment is given. Verify the stock
      squelch opens and the RSSI bar moves on a scripted carrier. Verified: stock (REG_78H
      `0xFFFE`, REG_4FH `0x302F`) reads REG_0CH `0x0202` on a -20 dBm carrier, then reads REG_67H
      and draws the RSSI bar on the standby screen. Without the environment, stock's frames are
      byte-identical to before.
- [x] Use only first-party FD6818 addresses. The AT1846S documents are for concepts only (CLAUDE.md
      in dm32). Every address and field is from the FD6818 programming guide or from dm32uv-re
      `specs/rdl/fd6818.rdl`'s hardware reads.

### 32. Codec (L)

Rows: Codec. Current D. Target: **A** at the register level. Audio output is B, through QEMU's audio
backend.

Needs: P0.4.

- [x] Model the 37 registers of DS 6.5, Register Table 26 (p.443–472), starting from `CR_VIC`
      (`0x12`), which is hot in Snake. Model the `SR1`/`SR2` ready bits after power-up sequences,
      `IFR`/`IFR2` write-1-to-clear flags with `IMR`/`IMR2`, and soft-mute ramp completion.
      `hw/misc/dm32_codec.c`, mapped over the DMR RAM window. All 37 registers carry the DS reset
      values; `0x27`–`0x30` are not in the SVD. Table 26's overview lists only 31; sections
      6.5.2.1–37 have all 37. SR1 pon_ack sets after Tstbon (12 ms) for playback or Tsbyu (50 ms)
      on waking. SR2 is ADAS status only and reads 0. MR shows the 24 ms soft-mute ramps, and
      their end sets IFR `[0]`/`[2]`. The DS does not say how flags clear; write-1-to-clear is an
      assumption. Also modelled: the codec timer (IFR2 `[4]`), the indirect MIX, DAC_AGC and
      ADC_AGC banks, and LINEOUT_CTRL standby_lo. Stock's boot follows the DS power-up
      sequences. It never reads SR1, MR or IFR, and its frames are unchanged.
- [x] Wire the codec interrupt on PIC 37. Level per ICR int_form: high or low while pending, or
      an 8 MC_CLK low pulse.
- [x] Find what `CODEC_I2C_MUX` and `AUDIO_CONTROL` in SystemControl do to the codec's access path,
      from the DS. `CODEC_I2C_MUX` bit 7 selects the codec's register interface: 0 is MC (this
      memory-mapped window), 1 is I2C at the address in `[6:0]` (4.6.5.6). The DS names no bus
      for the I2C interface, so the model cuts the window while bit 7 is set (assumption).
      `AUDIO_CONTROL` (4.6.5.3) selects audio data paths, not register access: prompt-tone and
      FM playback, AHB voice read/write, sidetone, I2S, and the TX voice source (codec/vocoder or
      MCU RAM). It stays storage until samples flow.
- [ ] Stretch, for audio output (B): take PCM from the built-in vocoder's SRAM buffers
      (`0x18000000`–`0x1800009F` and `0x180000A0`–`0x18000140`) to a QEMU audio backend or a WAV
      file, through the codec's DAC gain and mute state. Not done: nothing in the model produces
      samples yet. The DS (7.1.2.2–7.1.2.3) places the codec ADC's PCM for encoding at
      `0x18000000`–`0x1800009F`, with interrupt 28, which is the transmit and record side. Decoded
      voice for playback needs the built-in vocoder (no model), and analogue FM receive audio goes
      from the modem to the DAC without a RAM buffer. Item 31's RF environment carries no audio.

### 33. FM subsystem (L)

Rows: FM subsystem. Current D. Target: **B**. A needs [DS] detail on decoder timing and flag
semantics that the SVD descriptions do not give.

Needs: item 31 for the RF environment.

- [ ] Split the FM registers out of plain sysreg storage: `FM_BANDWIDTH`, `FM_DEV_COEF`,
      `SUBVOICE_COEF`, `FM_MOD`, `CTCSS_ADDR`, `SELCALL_TONE`, `TONE_SEND_CTRL`, `FM_TONE1`–`4`,
      `FM_PTT`, `FM_DECODE_FLAG2` and `FM_SQL`, with DS semantics.
- [ ] Set the CTCSS, DCS and selcall decode flags from the RF environment's modulation, after the DS
      detection delay, or an `Assumption:` value.
- [ ] Log tone-send and PTT activity as TX events, as the modem log does for DMR.
- [ ] Verify: a scripted carrier with the channel's CTCSS opens the stock squelch, and the wrong tone
      does not.

### 34. Baseband system control, DMR RAM and the built-in vocoder (XL)

Rows: Baseband system control, DMR phy/link RAMs, and the built-in vocoder (from item 1). Current C
and D. Target: **B**. A would need bit-exact modem behaviour that no source here describes, and [HW]
slot timing.

Needs: items 26 and 31.

- [ ] Read DS 9.4, 10.4 and 11.4 (p.520–521, 555–557, 608) and settle the injection level. Either the
      hardware hands firmware decoded, FEC-corrected payloads (`RECV_INFO`, `RX_INFO`,
      `RX_TYPE_INFO`) or raw bursts. The CRC and RS mask registers (`PI_CRC_MASK`, `RS_H_MASK`,
      `CSBK_CRC_MASK`, ...) suggest the hardware checks CRCs, and the model must then do the same
      checks.
- [ ] Model `SYS_INTERP_LIST`/`MASK`/`CLEAR` and `LAYER3_INTERP_LIST`/`MASK`/`CLEAR` and their bitmaps
      (from DS, cross-checked with `fenfir/hd2_infodump`). Wire PIC 27–33: PCM read and write,
      system, TX slot, RX slot, RF TX and RF RX.
- [ ] Add the RX-slot interrupt (source 31) and passive establishment on RX-slot events, which the
      model omits today.
- [ ] Take the slot period from the DS (30 ms slot, 60 ms TDMA frame) and the clock tree, and explain
      the 9.3 s against 8.64 s wrap discrepancy in `modem-timing.md`.
- [ ] Model `LAYER2_SLOT_UPDATE`, `LAYER2_SEND_TYPE`, `LAYER2_SLOT_PRE`, the sync-pattern registers,
      `LOCAL_CC` and `SCRAMBLE_REG`.
- [ ] Add RX injection, `DM32_DMR_RX=<file>`: burst records written into RX RAM (`+0x4B0`) at slot
      times, with the status bytes (`+0x32`, `+0x33`, `+0x232`, `+0x233`) and interrupts set the way
      the DS describes. Gate it on the RF environment (carrier present, frequency matches).
- [ ] Add TX capture, `DM32_DMR_TX=<file>`: decode outbound frames from TX RAM into burst records on
      each TX slot, in the same format, so a transmit can be replayed as a receive.
- [ ] Built-in vocoder: raise the PCM read and write interrupts every 20 ms during a call. Supply
      silence, or a test pattern, as decoded PCM, and accept any coded frame at
      `0x16000960`–`0x1600097A`. AMBE stays absent by choice.
- [ ] Verify: an injected voice LC header for a talkgroup in the test codeplug makes stock show the
      caller ID (`DM32_GRAMDUMP`). An injected data SMS shows the message. A keyed PTT on a DMR
      channel produces a TX capture whose header carries the radio's ID and talkgroup.
- [ ] Add DMR privacy coverage: with an AES key in the test codeplug, the captured PI header carries
      the MI. That makes the `mi_rng` fix in dm32uv-re testable at the protocol level.

---

## Projected grades

Targets assume every item lands. A grade in parentheses is reachable only past the item's gate.

| Subsystem | Now | Target | Item |
|---|---|---|---|
| Reset / system control | A | A | 12 |
| Clock manager | C | A | 26 |
| Baseband system control | C | B | 34 |
| Pin mux IO_DIPLEX1/2 | A | A | P0.5, 13 |
| FM subsystem | D | B (A: DS) | 33 |
| PIC | A | A | 9 |
| Timer | B | A | 6 |
| RTC | A | A | 11 |
| I2C2 controller | A | A | 10 |
| Watchdog | B | B+ (A: HW) | 5 |
| I2C0 | A | A | 4, 10 |
| I2C1 | A | A | 4, 10 |
| SPI0 | A | A | 15 |
| SPI1/2/3 and 0x04000000 | C | A for SPI1/2; B+ for SSI3 and 0x04000000 | 17 |
| I80 / LCD | B | A | 28 |
| UART0 | B | A | 27 |
| UART1 / UART2 / UART3 | B | B+ (A: HW) | 27 |
| SDIO | F | B (stretch A) | 22 |
| USB | F | C+ (stretch B) | 23 |
| SPI slaves 0/1/2 | F | C (B: DS) | 8 |
| GPIO A/B/C | A | A | 14 |
| PWM | B | B+ (A: DS or HW) | 21 |
| ADC | B | A | 19 |
| DAC | B | A | 20 |
| Codec | A (registers) | A (registers), B (audio) | 32 |
| DMR phy/link RAMs | D | B | 34 |
| Program flash (XIP) | D+ | B (A: HW) | 29 |
| Program flash tail | F | B (A: HW) | 29 |
| Data flash (W25Q128) | B+ | A | 16 |
| IRAM / SRAM | A | A | 2 |
| Boot ROM | D | A with an image (EXT); C without | 25 |
| Cache controller | D | B (stretch A) | 7 |
| FD6818 RF chip | A (registers), B (RF signal) | A (registers), B (RF signal) | 31 |
| ALPU-MP | F | B; A with keys (EXT) | 30 |
| RDA5807M tuner | A (registers) | A (registers) | 24 |
| HR_V3000 vocoder | F | N/A (not fitted) | 1 |
| eFuse ID | F | B (A: HW) | 18 |
| Unassigned peripheral space | F (logged) | F (logged), strict-capable | 3 |

Across the 38 rows: A 22 (two of them need an [EXT] input), B or B+ 12, C or C+ 2, F 1, N/A 1.
Today: A 14, B or B+ 10, C or C+ 3, D or D+ 4, F 6, N/A 1.

## Hardware gates, collected

Each of these steps needs a physical radio, so none can run in this repository or in CI. They must
follow the calibration-safety guidance in the [Safety section of README.md](../README.md#safety).
None of them writes flash.

- [ ] Watchdog timeout and clock with reset values (item 5).
- [ ] UART1–3 pins and PIC lines (item 27). PTT polarity and the pin identities from the existing
      hardware verification list in `coverage.md`.
- [ ] Whether LCSFC forwards window stores as commands, and the busy readback (item 29). Observe
      read-only, for example by timing stock's own settings-sector write; do not issue new writes.
- [ ] `0x04000000` as the program flash's raw controller (item 17).
- [ ] The eFuse read protocol over SPI_Master3 (item 18).
- [ ] The PWM interrupt event, if the DS does not give it (item 21).
- [ ] What an unmapped peripheral access does (item 3).
- [ ] DMR slot period (item 34).

## Not planned

- AMBE or AMBE++ voice coding, by choice.
- DWC2 device mode with a host-side USB backend. Stock does not use USB.
- SD card emulation behind SDIO. The radio has no card slot.
- A bit-exact 4FSK modem, or any analogue RF or audio simulation beyond the injected RF environment.
