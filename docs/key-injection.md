# Key injection

This page covers how the firmware reads the keypad through the GPIO model, the environment
variables that inject key presses and pin levels, the key positions measured so far, and the
open questions. The grammar below is taken from the header comment of
[`hw/gpio/dm32_gpio.c`](../hw/gpio/dm32_gpio.c).

## How the firmware reads the keypad

- The keypad is scanned as a matrix. The firmware drives one column low at a time and reads the row
  inputs. A closed key pulls its row low while its column is driven low.
- The key matrix pins are GPIOB bits 17-20 and GPIOC bits 7-11. The logical pin numbers are
  `bank*32 + bit`, with A=0, B=1, C=2. Columns in this model's matrix steps are GPIOB 17-20, and rows
  are GPIOC 7-11. The orientation the stock firmware uses is disputed. See
  [Open questions](#open-questions).
- The port data register is read at offset `+0x50` (`EXT_PORTA`). `DATA_OFF` in `dm32_gpio.c` is
  `0x50`. The bank's input word is returned from that read.
- Inputs idle high, as pull-ups with the switch to ground. Input bits that idle low are listed in
  `DM32_GPIO_IDLE_LOW`, with GPIOA bit 16 low by default. An enabled SoC pull-down reads low.
- Key injection is clocked by `+0x50` reads. The rows it holds stay low until the next read, so the
  interrupt block sees them too.

A key's row reads low only while its column pad is driven low when the row is read: the column bit
must be an output (`SWPORTA_DDR`), its `SWPORTA_DR` bit 0, and its pad on GPIO (stock's scan hands
GPIOC 7-11 to GPIO with `IO_DIPLEX2 |= 0xFE00` first). A scan that drives the column high, leaves it
an input, or drives it while the LCD owns the pad, sees no key.

## Grammar

### `DM32_KEY`

```
DM32_KEY="<colbank>:<colbit>:<rowbank>:<rowbit>"
```

Banks are `0`=A, `1`=B, `2`=C, and bits are 0-31. A malformed value stops QEMU with an error. While the named column bit is driven low, the named row bit is forced
low in the row bank's data read. This is what a closed switch does. `DM32_KEY_HOLD=N` releases the
key after `N` row-bank data reads. The default `0` holds the key forever.

```sh
DM32_KEY="1:17:2:7" DM32_KEY_HOLD=3000 ./build/qemu-system-cskyv2 ...
```

A permanently held key produces one event in OpenRTX's input layer and then no more, so a bounded
hold is needed for a UI that reacts to a press.

### `DM32_KEYSEQ`

```
DM32_KEYSEQ="cb:cbit:rb:rbit:hold:gap;cb:cbit:rb:rbit:hold:gap;..."
```

A scripted sequence of up to 32 steps. Each step forces its key for `hold` row-bank data reads, then
releases it for `gap` reads, then advances to the next step. `DM32_KEYSEQ` is ignored when
`DM32_KEY` is set. A malformed step stops QEMU with an error that names the variable and the step.

- `cb >= 0` is a matrix step, which behaves like `DM32_KEY`. The row bit is forced low only while the
  named column is driven low.
- `cb = -1` is a dedicated input pin. The row bit is forced low on every read while the step is
  active, with no column condition.

`hold` and `gap` count row-bank data reads, not milliseconds. The rate of those reads depends on the
firmware's scan loop.

```sh
# Press KEY_1 (1:17:2:7), release, then press KEY_2 (1:18:2:7)
DM32_KEYSEQ="1:17:2:7:2000:500;1:18:2:7:2000:500" DM32_KEYLOG=1 ./build/qemu-system-cskyv2 ...
```

`DM32_KEYLOG=1` traces each step to stderr as `KEYSEQ step N` and `KEYSEQ -> step N`.

### Virtual-time pin injectors

These three variables force logical pin levels at virtual times. The forced level goes through the
same interrupt evaluation as a real pin change. Times are in virtual milliseconds from start. Each
injected event is logged under `DM32_KEYLOG=1` as `PINSEQ`, `KNOB` or `PTT` with `t=` and `pin=`.

- **`DM32_PINSEQ="t:pin:level;..."`** forces a pin to `0` or `1` at time `t`. A level of `-` or `x`
  releases the pin. Pins are numbered 0-95.

  ```sh
  DM32_PINSEQ="5000:0x2b:0;8000:0x2b:-"   # pin 0x2b low at 5 s, released at 8 s
  ```

  Driving a keypad matrix pin (49-52 or 71-75) prints `DM32 WARNING: DM32_PINSEQ drives keypad
  matrix pin N`.

- **`DM32_KNOB="t:+N"`** or **`"t:-N"`** emits `N` quadrature detents on pins `0x26` and `0x27`
  (bank B bits 6 and 7). One detent is four edges, spaced by `DM32_KNOB_EDGE_MS` (default `5`,
  minimum `1`).

  The phase convention for `+` is that A falls, B falls, A rises, B rises, so A leads B. `-` is the
  reverse. Which direction the firmware calls `+` is the firmware's decision and is not established
  here.

  ```sh
  DM32_KNOB=20000:+3 DM32_KNOB_EDGE_MS=5
  ```

- **`DM32_PTT="t:hold_ms"`** forces pin `0x2b` (bank B bit 11) low at `t` and releases it at
  `t + hold_ms`.

  ```sh
  DM32_PTT=20000:1500
  ```

Assumptions:

- The pin identities for the knob (`0x26`/`0x27`), PTT (`0x2b`) and power (`0x11`) are hypotheses
  taken from the project's pin map. They are not measured on the model.
- PTT active-low polarity is an assumption. The source for it is one decompiled runtime routine,
  `bitwise_not_of_boolean_result_03012f48`, whose caller has not been traced.
- Under host load, timer callbacks can bunch. Two edges can then land at the same virtual instant,
  and the latched-edge model shows one raise for both.
- The injectors do not touch the matrix pins unless a `DM32_PINSEQ` step names one.

## Verified key positions

The five positions below were pressed in OpenRTX. Each press put OpenRTX into frequency-entry mode
and displayed the pressed digit. The predicted key comes from the OpenRTX port's `keyMap`.

| `DM32_KEY` | Displayed | Predicted | Result |
|---|---|---|---|
| `1:17:2:7` | `Rx: 11_` | KEY_1 | match |
| `1:18:2:7` | `Rx: 22_` | KEY_2 | match |
| `1:19:2:7` | `Rx: 33_` | KEY_3 | match |
| `1:18:2:9` | `Rx: 88_` | KEY_8 | match |
| `1:19:2:9` | `Rx: 99_` | KEY_9 | match |

These positions were measured against the OpenRTX port's keypad driver, `keyboard_DM32.c`. They
were not measured against stock firmware. See [the orientation note](#open-questions).

The result shows the matrix scan, the row read, the keycode lookup, and OpenRTX's input layer
working end to end.

## Open questions

### Fifteen positions produce no UI reaction

Of the 20 positions in the matrix (column bits 17-20 times row bits 7-11), 5 are verified above.
The other 15 are injected but do not change the UI. Injection does fire: the `KEY_4` injection was
logged 19,715 times. The key is pressed, and OpenRTX does not act on it.

The observed pattern:

- Row bit 7 works with column bits 17, 18 and 19 (KEY_1, KEY_2, KEY_3).
- Row bit 9 works with column bits 18 and 19 (KEY_8, KEY_9) but not with column bit 17 (KEY_7).
- Rows bits 8, 10 and 11 never work on any tested column.

Ruled out by testing:

- The hold duration. `DM32_KEY_HOLD` of 2,000 and 20,000 both fail on KEY_4.
- `KEY_0` stuck at bit 0. KEY_0 itself fails, so it is not a bit-0 artefact.
- A pull-up asymmetry in `IOMGR_REN_REG1`, with rows 0-2 pull-down and rows 3-4 pull-up. Rows 0 and
  2 work and row 1 does not, which the split does not predict.

To settle this, the source suggests instrumenting `kbd_getKeys()` to report its returned bitmap for
each injected position. That separates "the driver never saw the key" from "the driver saw it and
the UI discarded it". The source's remark that the untested entries follow from the same geometry
is an extrapolation and is not measured.

### The Up/Down dedicated-pin claim and the table orientation

Two statements in the sources conflict.

1. **Up and Down as dedicated pins.** An earlier `DM32_KEYSEQ` comment in `dm32_gpio.c` said that Up
   and Down are dedicated input pins, GPIOB bits 20 and 19, read before the matrix scan. That is
   wrong. dm32uv-re's `specs/dts/dm32uv.dts` puts the side keys SK1 and SK2 on GPIOB 19 and 20, and
   Up and Down in the matrix on GPIOC 11. The comment now names the side keys. The measured key table
   in dm32uv-re's
   [`tools/src/dm32_patch_tools/qemu_run.py`](https://github.com/awkaplan/dm32uv-re/blob/main/tools/src/dm32_patch_tools/qemu_run.py)
   (`KEYS`) has Up = `2:11:1:17` and Down = `2:11:1:18`. Those are matrix steps: GPIOC bit 11
   driven, GPIOB bits 17 and 18 sensed. The matrix form works in the Snake walkthrough. The only
   dedicated-pin step measured is MENU from standby, `-1:0:1:17`.

2. **Column and row orientation.** The examples in this page drive GPIOB bits 17-20 as columns and
   sense GPIOC bits 7-11 as rows. For example, `DM32_KEY="1:17:2:7"` is KEY_1. The measured `KEYS`
   table has the opposite orientation: GPIOC is the driven column (`cb=2`, bits 7-11) and GPIOB is
   the sensed row (`rb=1`, bits 17-20). For example, `'1'` is `2:7:1:20`. The two tables give
   different bit pairs for the same keys.

The source gives one explanation for the second conflict. The table in this page was measured
against the OpenRTX port's driver, which drives GPIOB and senses GPIOC. The `KEYS` table was measured
against stock firmware, whose scan drives GPIOC 7-11 and senses GPIOB 17-20. The injector forces a
row bit only while the named column is driven low, so each table is valid only for the firmware whose
scan direction it assumes.

This explanation is not verified on hardware and was not re-measured. Use the `KEYS` table for stock
firmware and the table above for the OpenRTX port.
