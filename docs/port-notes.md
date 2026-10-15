# Porting notes: QEMU API changes

This page lists the QEMU API changes this overlay depends on. Each entry gives the old call, the
current call, and the file in the current QEMU tree that was checked. The board file
([hw/csky/dm32.c](../hw/csky/dm32.c)) and the first device models were written against QEMU 3.0 and
ported to current QEMU. Later device models target current QEMU directly. Both use the forms below. The general API catalogue for the csky-qemu target is
in the csky-qemu port notes ([awkaplan/csky-qemu](https://github.com/awkaplan/csky-qemu),
`docs/PORT-NOTES.md`).

## Timers: `ptimer_init()` takes the callback

Old:

```c
QEMUBH *bh = qemu_bh_new(dm32_timer_tick, &s->ch[i]);
s->ch[i].ptimer = ptimer_init(bh, PTIMER_POLICY_DEFAULT);
```

Current, in [hw/timer/dm32_timer.c](../hw/timer/dm32_timer.c):

```c
s->ch[i].ptimer = ptimer_init(dm32_timer_tick, &s->ch[i],
                              PTIMER_POLICY_LEGACY);
```

- `ptimer_init()` takes the callback and its opaque pointer. The bottom-half wrapper is gone.
- `PTIMER_POLICY_DEFAULT` is not a QEMU symbol. Compose the policy from the `PTIMER_POLICY_*` bits
  in `hw/core/ptimer.h`. The model uses `PTIMER_POLICY_LEGACY`, which keeps the countdown-reload
  behaviour the model was written against. Checked against `hw/timer/allwinner-a10-pit.c`, which has
  the same per-channel ptimer array.
- Every state-changing call must run inside a transaction, and `ptimer_init()` asserts this. From
  the LoadCount write in [hw/timer/dm32_timer.c](../hw/timer/dm32_timer.c):

```c
ptimer_transaction_begin(c->ptimer);
ptimer_set_limit(c->ptimer, val ? val : 0xffffffff, 1);
ptimer_transaction_commit(c->ptimer);
```

  The state-changing calls are `ptimer_set_freq()`, `ptimer_set_limit()`, `ptimer_set_count()`,
  `ptimer_run()` and `ptimer_stop()`. `ptimer_get_count()` is a read and needs no transaction.

## Graphics console: creation, update callback and flushing

Old:

```c
static void dm32_lcd_gfx_update(void *opaque) { /* ... */ dpy_gfx_update(s->con, 0, 0, w, h); }
static const GraphicHwOps dm32_lcd_gfx_ops = { .gfx_update = dm32_lcd_gfx_update };
s->con = graphic_console_init(dev, 0, &dm32_lcd_gfx_ops, s);
```

Current, in [hw/display/dm32_lcd.c](../hw/display/dm32_lcd.c):

```c
static bool dm32_lcd_gfx_update(void *opaque)
{
    /* ... decode into the surface ... */
    qemu_console_update(s->con, 0, 0, s->pw, s->ph);
    return true;
}
s->con = qemu_graphic_console_create(dev, 0, &dm32_lcd_gfx_ops, s);
qemu_console_resize(s->con, s->pw, s->ph);
```

- `graphic_console_init()` is now `qemu_graphic_console_create()`, with the same arguments.
- `GraphicHwOps.gfx_update` returns `bool`, which reports whether anything was drawn.
- The free function `dpy_gfx_update()` is gone. The callback flushes each dirty rectangle with
  `qemu_console_update()`.
- Checked against `hw/display/tcx.c`, whose `tcx_update_display()` follows the same shape.
- `qemu_console_resize()` and `qemu_console_surface()` are unchanged.

## Interrupt helpers need `hw/core/irq.h`

`qemu_set_irq()` is declared in `hw/core/irq.h`. The `qemu_irq` type arrives through
`hw/core/sysbus.h`, but the function does not. Modern QEMU treats the implicit declaration as an
error. Any device that calls `qemu_set_irq()` includes the header:

```c
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
```

This applies to [dm32_pic.c](../hw/intc/dm32_pic.c), [dm32_timer.c](../hw/timer/dm32_timer.c) and
[dm32_sysreg.c](../hw/misc/dm32_sysreg.c). The board file only passes `qemu_irq` values and does
not need it.

## Object properties: argument order

Old:

```c
object_property_set_bool(cpuobj, true, "realized", &error_fatal);
```

Current, in [hw/csky/dm32.c](../hw/csky/dm32.c):

```c
object_property_set_bool(cpuobj, "realized", true, &error_fatal);
```

The name now comes before the value.

## `class_init` takes a const data pointer

Old:

```c
static void dm32_class_init(ObjectClass *oc, void *data)
```

Current, in [hw/csky/dm32.c](../hw/csky/dm32.c) and [hw/display/dm32_lcd.c](../hw/display/dm32_lcd.c):

```c
static void dm32_class_init(ObjectClass *oc, const void *data)
```

## Header paths

Headers moved under `hw/core/` and `system/`, for example `hw/sysbus.h` to `hw/core/sysbus.h`.
The board file includes `system/system.h`, `system/reset.h`, `hw/core/boards.h` and
`system/address-spaces.h`. The csky-qemu port notes list the full set.

## Shared hooks: `include/hw/csky/dm32.h`

Functions that one device calls in another are declared once, in
[include/hw/csky/dm32.h](../include/hw/csky/dm32.h). Files include that header and do not declare
the function themselves.

Old, in the board file:

```c
DeviceState *dm32_ssi_create(hwaddr base);       /* hw/misc/dm32_ssi.c */
void dm32_i2c_set_instance(void *dev, int n);    /* hw/misc/dm32_i2c.c */
```

Current, in [hw/csky/dm32.c](../hw/csky/dm32.c):

```c
#include "hw/csky/dm32.h"
```

The header declares the SSI, SYSREG, timer, FD6818, I2C and I2C RTC hooks. The I2C controllers
drive QEMU `I2CBus` objects, and the RTC is an ordinary `I2CSlave`.

## MMIO regions: little-endian

Every MMIO region sets its endianness explicitly. The SoC is little-endian.

Old:

```c
.endianness = DEVICE_NATIVE_ENDIAN,
```

Current, for example in [hw/misc/dm32_adc.c](../hw/misc/dm32_adc.c):

```c
.endianness = DEVICE_LITTLE_ENDIAN,
```

`tests/checkpatch.sh` rejects `DEVICE_NATIVE_ENDIAN`. The behaviour is the same on a little-endian
host, because the SoC is little-endian.

## Verifying a port

Use the repository's own checks. Each one runs against a built QEMU checkout with the overlay
installed (`./install.sh <qemu>`).

| Step | Command | Passes when |
|---|---|---|
| Build | `ninja -C <qemu>/build qemu-system-cskyv2` | no warnings. Warnings are errors by default. |
| Style | `tests/checkpatch.sh <qemu>` | prints `checkpatch: clean` |
| Smoke | `tests/run-smoke.sh <qemu>/build/qemu-system-cskyv2` | prints `dm32uv-qemu smoke test: PASS`, exit 0 |
| Behaviour | `tests/objhash.py <qemu>`, before and after the change | identical output, for changes meant to be behaviour-neutral |
| Firmware | the stock boot and Snake regression in dm32uv-re | see below |

`tests/checkpatch.sh` runs QEMU's `scripts/checkpatch.pl` on every C file in the repository. It
fails on any finding that is not listed in `tests/checkpatch-allow.txt`, which has a comment for
each entry. The smoke test does not run firmware.

The firmware check is the stock boot and the Snake walkthrough from
[dm32uv-re](https://github.com/awkaplan/dm32uv-re) (`regress.sh stock` and `regress.sh snake`).
Both runs must end with `crash=0 muxwarn=0 invalid=0` in their token line. The step count and the
pixel fingerprints show whether the screens still render. Do not compare the whole token line,
because the order of transient frames depends on host load.
