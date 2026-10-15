# Contributing

Issues and pull requests are welcome. This page covers how the repository is put together, how to
test a change, and the conventions the code follows.

## How the overlay works

The repository holds only the DM-32UV board files. `install.sh` copies the `hw/` and `include/`
trees into a QEMU checkout at the same paths and applies `integration/top-level.diff`, which adds
one Kconfig entry and one `meson.build` line per device file to QEMU's `hw/display`, `hw/gpio`,
`hw/intc`, `hw/misc` and `hw/timer` directories.

It installs on top of [csky-qemu](https://github.com/awkaplan/csky-qemu), which provides the CPU
target, `hw/csky/boot.c` (`csky_load_kernel`), the UART (`csky_uart`) and the C-SKY interrupt
controller. Two consequences:
- `hw/csky/Kconfig` and `hw/csky/meson.build` here are csky-qemu's files with the `dm32` machine
  added, and they replace csky-qemu's copies. When csky-qemu changes either file, the change has to
  be carried over here.
- `QEMU_COMMIT` and `CSKY_QEMU_COMMIT` pin the pair of commits this repository is tested against.
  CI builds exactly that pair.

The rule that keeps the overlay workable: **no changes to QEMU core files.** The only edits to
QEMU's own files are the build registrations in `integration/top-level.diff`.

## Layout

```
hw/csky/dm32.c          the -M dm32 machine: memory map, device wiring, reset entry
hw/<subsystem>/dm32_*.c one file per device model (see docs/devices.md)
include/hw/csky/dm32.h  functions the devices call in each other
include/hw/csky/dm32_svd.h      register tables generated from the SVD (do not edit)
include/hw/csky/dm32_regfile.h  register file that applies those tables
integration/            the patch to QEMU's build files
tests/                  smoke test, qtests, SVD tests, style check, object-hash tool
scripts/                framebuffer, data-chip and SVD-table helpers (Python, standard library only)
docs/                   reference documentation
```

Devices that depend on each other's state (the FD6818 and SPI chip select are driven through GPIO;
pad multiplexing lives in the system registers) call each other through the functions declared in
`include/hw/csky/dm32.h`. Add a new cross-device hook there, not as an `extern` in a `.c` file.

## Building and testing

Set up a QEMU checkout at `QEMU_COMMIT` with csky-qemu at `CSKY_QEMU_COMMIT` installed, as the
[README](README.md#quick-start) shows. After a change, reinstall and rebuild:

```sh
./install.sh ../qemu && ninja -C ../qemu/build qemu-system-cskyv2
tests/run-smoke.sh ../qemu/build/qemu-system-cskyv2
tests/run-qtest.sh ../qemu/build/qemu-system-cskyv2
tests/run-svd-tests.sh
tests/checkpatch.sh ../qemu
```

QEMU builds with warnings as errors by default; keep it that way.

The smoke test ([`tests/smoke/smoke.S`](tests/smoke/smoke.S)) boots a small raw image on `-M dm32`
and checks that the board's memory and a few device registers (ADC and DAC version registers,
watchdog lock) answer as modelled. The prebuilt `smoke.bin` is committed so no C-SKY toolchain is
needed; `tests/smoke/build.sh` rebuilds it.

The register-level tests ([`tests/qtest/`](tests/qtest/README.md)) start the board under
`-accel qtest` with no firmware, one QEMU per test, and drive each device model through MMIO, the
virtual clock and its interrupt lines. There is one module per device, and every expectation cites
the datasheet, the SVD, a stock-firmware address or the device file's header. A change to a device
model's behaviour comes with tests for it, including at least one negative case (misuse the model
rejects or reports). Python 3 standard library only.

Neither the smoke test nor the qtests run firmware. For any change to a device model, also boot the
stock firmware with [dm32uv-re](https://github.com/awkaplan/dm32uv-re)'s tooling (`qemu_run.py`;
see its `docs/qemu-workflow.md`): the stock boot to standby and the Snake example walkthrough are
the real regression tests. Compare the step, crash and `muxwarn` counts and the frame fingerprints,
not the whole token line, because the frame order varies with host load.

For a change that should not alter behaviour (comments, formatting), prove it with
`tests/objhash.py`: run it on the built checkout before and after the change and diff the output.
Identical hashes mean identical machine code.

## Code style

- QEMU's [coding style](https://www.qemu.org/docs/master/devel/style.html): 4-space indents, braces
  on every `if`/`else`/`for`/`while`, `/* */` comments with the opening and closing markers on their
  own lines, 80 columns. `tests/checkpatch.sh` runs QEMU's checker; a finding that has to stay goes
  in `tests/checkpatch-allow.txt` with a comment saying why.
- MMIO regions are `DEVICE_LITTLE_ENDIAN`; the SoC is little-endian. A register block takes 32-bit
  accesses only (`.valid.min_access_size = 4`), unless its handlers act on the addressed bytes of a
  narrow access: split them into whole-register `_read_reg`/`_write_reg` functions and wrap them
  with `dm32_reg_lane_read()`/`dm32_reg_lane_write()` from `include/hw/csky/dm32.h`, as
  `dm32_sysreg.c` and `dm32_gpio.c` do.
- Device types use `OBJECT_DECLARE_SIMPLE_TYPE`, not `OBJECT_CHECK`.
- Comments describe the code as it is, in present tense, with the evidence for non-obvious
  behaviour: a datasheet section (`HR_C7000 v2.7 EN section 5.9`), a register description from
  dm32uv-re's `data/registers/HR_C7000-DM32.svd`, or a stock-firmware function and address
  (`rtc_register_read @0x0301c2d4`; addresses are in the stock DM32.00.01.048 image, names come
  from dm32uv-re's decompiled corpus). Leave out how something was found and when.
- Mark what is assumed rather than known (`Assumption:`), and say in the file header what the model
  deliberately does not do.
- Port-specific explanations start with `Port note:` and stay short; longer history goes in
  [docs/port-notes.md](docs/port-notes.md).
- Files keep QEMU's layout. A new file goes at the path it will have inside QEMU, and is added to
  `install.sh`'s copy (automatic for `hw/` and `include/`), `integration/top-level.diff`, the
  `DM32` entry in `hw/csky/Kconfig`, and [docs/devices.md](docs/devices.md).

Configuration and debug knobs are environment variables (`DM32_*`, listed in
[docs/environment.md](docs/environment.md)), not qdev properties. This is a deliberate trade-off:
dm32uv-re's test harness sets them per run without changing the QEMU command line. Read a switch
with `dm32_env_flag()` and a number with `dm32_env_uint()` or `dm32_env_int()` (declared in
`include/hw/csky/dm32.h`), once, when the device is created, and keep the value in the device
state; never call `getenv()` in an MMIO handler or a timer callback. A bad value then stops QEMU
with an error naming the variable instead of being misread. A new knob gets a row in
`docs/environment.md`.

## Model fidelity

A model that is more permissive than the hardware hides firmware bugs. Prefer modelling the
documented preconditions (a disabled block reads 0, a write to a locked register is dropped) over
answering whatever the firmware asks. Record where a model is still more permissive than the
silicon in [docs/coverage.md](docs/coverage.md).

### Reset values and access rules from the SVD

`include/hw/csky/dm32_svd.h` is generated from dm32uv-re's
`data/registers/HR_C7000-DM32.svd` by `scripts/gen_svd_regs.py`, and records the SVD's sha256. For
every peripheral it gives the base, size and PIC source, and one `X()` entry per register with its
offset, size, reset value and its read-only, write-only, write-1-to-clear, write-1-to-set and
clear-on-read bits. A register without its own `resetValue` in the SVD is flagged, because its
inherited value may not be the silicon's.

`include/hw/csky/dm32_regfile.h` turns one of those lists into a register file that serves the reset
values and enforces the masks. Use it for a block that has no behavioural model yet, so that it reads
as the silicon does at reset instead of 0:

```c
static const DM32RegDesc usb_regs[] = { DM32_SVD_USB_REGS(DM32_REGDESC) };
```

`hw/csky/dm32.c` checks its addresses and PIC sources against the header at compile time. After the
SVD changes, regenerate the header and run the checks:

```sh
scripts/gen_svd_regs.py ../dm32uv-re/data/registers/HR_C7000-DM32.svd
tests/run-svd-tests.sh ../dm32uv-re/data/registers/HR_C7000-DM32.svd
```

Without the SVD argument, `tests/run-svd-tests.sh` still tests the register file and the generator,
but cannot tell whether the committed header is stale.

## Licensing

Every source file starts with an `SPDX-License-Identifier` line and a copyright line; see
[LICENSE.md](LICENSE.md). New files are `PolyForm-Noncommercial-1.0.0`, except changes to
`hw/csky/Kconfig` and `hw/csky/meson.build`, which stay `LGPL-2.1-or-later` because they are
derived from csky-qemu. Contributions are accepted under the same terms. Do not add firmware images, boot ROMs,
codeplugs, calibration data, datasheets or captures from a radio: none of these can be distributed
under this repository's license, and calibration data is specific to one radio.

## Moving to a newer QEMU or csky-qemu

1. Check out the new commits, install csky-qemu and then this overlay, and build.
2. If `integration/top-level.diff` no longer applies, add its lines by hand, then regenerate it
   with `git diff` in the QEMU checkout, limited to the files it touches.
3. Fix compile errors against the changed APIs. Record each kind of change in
   `docs/port-notes.md`, and add a `Port note:` comment where the reason is not obvious.
4. Run the tests above, including the stock-firmware boot, then update `QEMU_COMMIT` and
   `CSKY_QEMU_COMMIT`.

## Commits

Use QEMU-style subjects: `hw/misc/dm32_ssi: ...`, `hw/csky/dm32: ...`, `tests: ...`, `docs: ...`,
with a body that says what changed and why, and how it was tested.
