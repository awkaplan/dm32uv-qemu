# Notes for AI coding agents

Read [CONTRIBUTING.md](CONTRIBUTING.md) first; it is the full guide. These are the points that are
easiest to get wrong.

## What this repository is

A QEMU board model (`-M dm32`) for the Baofeng DM-32UV radio, shipped as files that `install.sh`
copies into a QEMU checkout at `QEMU_COMMIT` that already has
[csky-qemu](https://github.com/awkaplan/csky-qemu) at `CSKY_QEMU_COMMIT` installed. It is not a
QEMU tree: nothing here builds on its own, and paths such as `hw/misc/dm32_ssi.c` mean the same
file in this repository and in the checkout.

Where to look:
- [docs/devices.md](docs/devices.md): memory map and every device model.
- [docs/coverage.md](docs/coverage.md): what is modelled, how faithfully, and the known gaps.
- [docs/boot-path.md](docs/boot-path.md): what the stock firmware needs from the board, in order.
- [docs/environment.md](docs/environment.md): every `DM32_*` environment variable.
- Firmware, patches and the test harness are in
  [dm32uv-re](https://github.com/awkaplan/dm32uv-re), not here.

## Rules

- Do not modify QEMU core files or csky-qemu's files. The only edits outside the board files are
  the build registrations in `integration/top-level.diff`.
- Edit files in this repository, then reinstall (`./install.sh <qemu>`). Edits made directly in a
  QEMU checkout are overwritten by the next install.
- `hw/csky/Kconfig` and `hw/csky/meson.build` replace csky-qemu's copies; keep csky-qemu's `smartl`
  entries in them intact.
- Cross-device calls go through `include/hw/csky/dm32.h`. No `extern` declarations in `.c` files.
- `DM32_*` environment variables are read with `dm32_env_flag()`/`dm32_env_uint()`/`dm32_env_int()`,
  once at device creation; never `getenv()` in an MMIO handler. A new variable needs a row in
  `docs/environment.md`.
- A register block either takes 32-bit accesses only or handles narrow accesses with
  `dm32_reg_lane_read()`/`dm32_reg_lane_write()` (see CONTRIBUTING.md, "Code style").
- Keep the first-line `SPDX-License-Identifier` and the copyright line of every file. New files get
  `/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */` and a copyright line;
  `hw/csky/Kconfig` and `hw/csky/meson.build` stay `LGPL-2.1-or-later` (derived from csky-qemu).
- Never add firmware images, boot ROMs, codeplugs, calibration data, datasheets or radio captures.
- Do not invent register behaviour. Cite the datasheet section, the dm32uv-re SVD, or the stock
  firmware address that shows it, and mark assumptions as `Assumption:`.
- Comments: QEMU block style, present tense, no narration of how something was found, no dates.

## Checking a change

Run all of these after reinstalling and rebuilding (`ninja -C <qemu>/build qemu-system-cskyv2`):

| Command | Passes when |
|---|---|
| `ninja -C <qemu>/build qemu-system-cskyv2` | no warnings (they are errors by default) |
| `tests/run-smoke.sh <qemu>/build/qemu-system-cskyv2` | prints `dm32uv-qemu smoke test: PASS`, exits 0 |
| `tests/run-qtest.sh <qemu>/build/qemu-system-cskyv2` | prints `dm32uv-qemu qtest: PASS`, exits 0 |
| `tests/run-svd-tests.sh [<dm32uv-re>/data/registers/HR_C7000-DM32.svd]` | prints `dm32uv-qemu svd tests: PASS`; with the SVD it also fails on a stale `include/hw/csky/dm32_svd.h` |
| `tests/checkpatch.sh <qemu>` | prints `checkpatch: clean` |
| `tests/objhash.py <qemu>` before and after | identical output, for changes meant to be behaviour-neutral |

If you change a device model's behaviour, add or update its register-level tests in `tests/qtest/`
(conventions in [tests/qtest/README.md](tests/qtest/README.md)), with at least one negative case.

The smoke test and the qtests do not run firmware. If you change a device model, say that the
stock-firmware regression in dm32uv-re (stock boot and the Snake walkthrough via `qemu_run.py`)
still needs to be run, unless you ran it yourself.
