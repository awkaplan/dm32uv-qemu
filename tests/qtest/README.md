# Register-level device tests

These tests drive the `dm32` board's device models directly, with no firmware. Each test starts
`qemu-system-cskyv2 -M dm32 -accel qtest`, so the CPU does not run, and talks to it over QEMU's
qtest protocol. It reads and writes MMIO, advances the virtual clock, and watches interrupt lines.
A full run takes a few seconds.

```sh
tests/run-qtest.sh ../qemu/build/qemu-system-cskyv2           # every test_*.py
tests/run-qtest.sh ../qemu/build/qemu-system-cskyv2 test_wdt  # one module
tests/run-qtest.sh ../qemu/build/qemu-system-cskyv2 -v        # verbose
```

The runner prints `dm32uv-qemu qtest: PASS` and exits 0 when every test passes. Python 3.8 or newer,
standard library only.

## Files

- `dm32qtest.py`: the client. `Machine` is one QEMU process. `DM32TestCase.start()` returns a fresh
  one that is closed after the test.
- `test_<device>.py`: one module per device model, named after its source file.

## The client

| Call | Does |
|---|---|
| `readb/readw/readl(addr)`, `writeb/writew/writel(addr, val)` | MMIO access, as a CPU access with no debug flag |
| `read(addr, size)`, `write(addr, data)` | Bulk memory access |
| `clock_step(ns)`, `clock_step()` | Advance virtual time by `ns`, or to the next timer deadline |
| `irq_intercept_in(m.pic_path())` | Observe every PIC input. The PIC still sees them. After this, `m.irq_level[src]` is the level of PIC source `src` and `m.irq_log` lists each change |
| `irq_intercept_out(path, name)` | Observe a device's output lines instead (for the PIC itself: `m.pic_path()`, name `sysbus-irq`) |
| `set_irq_in(path, n, level)` | Drive input `n` of a device, for example a PIC source |
| `find_devices(type)`, `pic_path()` | QOM paths, in creation order |
| `pic_cpu_line()` | The value the PIC drives on the core input: vector in bits 7:0 ORed with bit 10, or 0. Shows the vector, which `irq_intercept_out` cannot |
| `peek_output()` | Everything QEMU has printed so far |
| `close()`, `events()` | Quit cleanly; return the output, or the parsed `dm32-events:` counts |
| `run_plain(env, args)` | Run without qtest, for start-up checks; returns `(status, output)` |
| `DM32TestCase.make_tempdir()` | A new empty directory, removed after the test; returns its path |
| `BitBang(m, scl, sda)` | An I2C master on two GPIOA pads (`eeprom_write`, `eeprom_read`) |
| `setup`, `i2c_write`, `i2c_read`, `rtc_write`, `rtc_read` | I2C2 master and RTC helpers, with their register constants |

Only one `irq_intercept_*` call is allowed per machine. Inherited `DM32_*` variables are removed
from QEMU's environment; pass the ones a test needs with `start(env={...})`.

## Rules for a test

- **Cite the source of every expectation** in the test's docstring or a comment: the datasheet
  section (`HR_C7000 v2.7 section 5.9`), the SVD register or field, the stock-firmware address, or
  the device file's own header where it records the behaviour. A test checks the documented
  behaviour, not whatever the model happens to do.
- **At least one negative case per device**: misuse the model rejects, ignores or reports. Name it
  `Negative:` in the docstring.
- **A model that disagrees with its own citation** gets a test marked `@unittest.expectedFailure`,
  with a `Model gap:` comment saying what differs. The suite stays green and the gap stays visible.
  Do not change a test to match a model bug.
- Use `clock_step()` for anything timed. Never sleep.
- Keep each test independent: one `self.start()` per test.
- Do not edit device models to make a test pass. A fix to a model is a separate change.
