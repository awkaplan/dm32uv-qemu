# Environment variables

Every `DM32_*` variable the device models read, with its effect and default. The variables are debug
and test knobs for the board model. Stock firmware cannot see them. Device behaviour is described in
[devices.md](devices.md).

**How values are read.** Every variable below is read by the rules in this paragraph, so the rows
do not repeat them. A flag is on when it is set to anything other than the empty string or `0`; unset,
empty and `0` are off. A number is a C integer literal (decimal, `0x` hex or `0` octal); unset or empty
gives the default, and any other value, or one outside the range the row states, stops QEMU with an
error that names the variable (exit status 1).

Most variables are read once, when the device they belong to is created at board start-up. The
exceptions are:

- Read at the first GPIO access: `DM32_KEY`, `DM32_KEY_HOLD`, `DM32_KEYSEQ`, `DM32_GPIO_IDLE_LOW`.
  `DM32_KNOB`, `DM32_KNOB_EDGE_MS`, `DM32_PINSEQ` and `DM32_PTT` are read when the third GPIO bank
  is created.
- Read at the first DATA write or state change: the file named by `DM32_DAC_TRACE` or `DM32_PWM_TRACE`
  is opened then, not per access.

## Board and flash

| Variable | Effect | Default |
|---|---|---|
| `DM32_FLASH_WRITABLE` | Makes program flash plain writable RAM, so guest stores take effect. For experiments only. | Unset: read-only ROM; stores are dropped and counted. |
| `DM32_STRICT` | Permissive-event classes that stop the guest on their first event: `1` or `all` for every class, or a comma-separated list of class names (`unimp`, `unassigned`, `flash_store`, `dac_pd`, `lcd_mux`, `lcd_off`, `ssi_discard`, `nor_wel`, `adc_pad`, `wdt_locked`, `pad_conflict`, `pad_unclaimed`, `i2c_nak`). An unknown name is an error at start-up. Classes are described in [devices.md](devices.md#permissive-events-and-dm32_strict). | Unset or `0`: events are only counted. |
| `DM32_RAM_FILL` | Power-on contents of IRAM and SRAM. `zero` keeps QEMU's zero fill. `noise` fills both with seeded pseudo-random bytes (splitmix64, default seed `0x444d3332`), `noise:<seed>` with another seed; the same seed gives the same bytes every run. `-device loader` images are written afterwards, so only bytes they do not cover keep the noise. Anything else is an error at start-up. | `zero` |
| `DM32_STRICT_ACTION` | What a strict stop does: `exit` shuts QEMU down with exit status 3; `pause` stops the VM in the guest-panicked state, for inspection over the gdb stub. | `exit` |

Every run prints one `dm32-events:` line with each class's count to stderr at exit.

Stock DM32.00.01.048 itself raises `flash_store`, `dac_pd`, `pad_conflict` and `i2c_nak` while
booting (the program-flash command stores, its DAC init, its keypad scan driving GPIOC 13 while the
LCD owns the pad, and its ALPU-MP check clocking on through NAKs, since no ALPU-MP is modelled). A strict run that stock passes uses the other classes:

```sh
DM32_STRICT=unimp,unassigned,lcd_mux,lcd_off,ssi_discard,nor_wel,adc_pad,wdt_locked,pad_unclaimed
```

## Keypad and pin injection

Keypad grammar and examples are in [key-injection.md](key-injection.md).

| Variable | Effect | Default |
|---|---|---|
| `DM32_KEY` | Hold one matrix key, `colbank:colbit:rowbank:rowbit`, with banks 0-2 (A-C) and bits 0-31. The row reads low on `EXT_PORTA` reads while the column pad is driven low (output, data 0, pad on GPIO). | Unset: no key. |
| `DM32_KEY_HOLD` | Release the `DM32_KEY` key after this many row-bank data reads, 0 to `ULONG_MAX`. `0` holds it forever. | `0` |
| `DM32_KEYSEQ` | Scripted key sequence. Each step's `cb` is -1 (dedicated pin) or 0-2, its `rb` is 0-2, and its bits are 0-31. Ignored when `DM32_KEY` is set. | Unset: no sequence. |
| `DM32_KEYLOG` | Log key and pin injection, and GPIO interrupt events. | Unset: no log. |
| `DM32_PINSEQ` | Force logical pins to 0 or 1 at virtual times, `t_ms:pin:level;...`. `t_ms` may have a fraction (`1.03` is 1 ms 30 us). `-` or `x` releases. | Unset: no forced pins. |
| `DM32_KNOB` | `N` quadrature detents on pins `0x26` and 0x27, as `t_ms:+N` or `t_ms:-N`. | Unset: no knob. |
| `DM32_KNOB_EDGE_MS` | Spacing of the knob edges, in ms, 1 to 1000000. | `5` |
| `DM32_PTT` | Force pin `0x2B` low at `t_ms` for `hold_ms`, as `t_ms:hold_ms`. | Unset: no PTT. |
| `DM32_GPIO_IDLE_LOW` | Inputs that idle low because of board-level pulls the SoC does not know about, as `bank:bit,...` with banks 0 = A, 1 = B, 2 = C. Overrides an enabled SoC pull-up. `none` clears the list. | `0:16` (GPIOA bit 16, the power-sense input). |
| `DM32_GPIOLOG` | Log each GPIO data-register read and each GPIO write, with bank, offset and value. | Unset: no log. |

## LCD and frame capture

Frame capture is described in [running.md](running.md).

| Variable | Effect | Default |
|---|---|---|
| `DM32_LCD_W` | Width of the panel view, in pixels, 1 to 4096. | `240` |
| `DM32_LCD_H` | Height of the panel view, in pixels, 1 to 4096. | `320` |
| `DM32_LCD_OFFX` | Horizontal offset of the panel view inside the 256 by 320 GRAM, -4096 to 4096. | `0` |
| `DM32_LCD_OFFY` | Vertical offset of the panel view inside GRAM, -4096 to 4096. | `0` |
| `DM32_GRAMDUMP` | Path. Write the whole GRAM as 16-bit RGB565 (256 by 320 pixels, 163840 bytes) once drawing has been idle for `DM32_GRAMQUIET` ms. Each later settle overwrites the file. | Unset: no dump. |
| `DM32_GRAMQUIET` | Idle time before a dump, in virtual ms, 1 to 2147483647. | `150` |
| `DM32_GRAMSEQ` | Also write each settled frame to `<DM32_GRAMDUMP>.NNN`, numbered from 000. | Unset: only the single file. |
| `DM32_LCDLOG` | Log panel commands (`0x28`, `0x29`, `0x3A`, `0x20`, `0x21`, `0x10`, `0x11`), CASET and RASET windows, RAMWR windows and MADCTL. | Unset: no log. |

## System and modem

| Variable | Effect | Default |
|---|---|---|
| `DM32_MODEM_LOG` | Path. Log slot-counter start and stop, TX-slot IRQ raise and clear, and TX-ORIGINATE events. The file is truncated at start-up. An unopenable path prints an error and logging stays off. | Unset: no log. |
| `DM32_MODEM_LEADER` | The first write to WORK_MODE with bits 4:3 non-zero forces active establishment once: LAYER2_CONTROL `0xCB` and LAYER2_SLOTON `0x05`. Later SLOTON writes are ignored while it is armed. | Unset: off. |

The slot timing these variables observe is described in [modem-timing.md](modem-timing.md).

## ADC

| Variable | Effect | Default |
|---|---|---|
| `DM32_ADC_CH0` to `DM32_ADC_CH7` | Input level for channel `n`, 0 to `0x3ff`. Stock's battery reading is channel C, so use `DM32_ADC_CH2` for it. | Unset: 0. |
| `DM32_ADC_VBAT` | Level, 0 to `0x3ff`, for the channel named by `DM32_ADC_VBAT_CH`. Used only when that channel's `DM32_ADC_CHn` is unset. | Unset: no level. |
| `DM32_ADC_VBAT_CH` | Channel that `DM32_ADC_VBAT` applies to, 0 to 7. | `5` (channel F) |
| `DM32_ADC_VERBOSE` | Log conversions, and starts that are refused, to stderr. | Off. |

## DAC

| Variable | Effect | Default |
|---|---|---|
| `DM32_DAC_LOG` | Print every DAC register access by name, with `PD=off` for a powered-down channel. | Off. |
| `DM32_DAC_TRACE` | Path. Append `<vtime_ns> <A|B|C> <value>` for each DATA write, decimal value. The file opens at the first DATA write. | Unset: no trace. |

## PWM

| Variable | Effect | Default |
|---|---|---|
| `DM32_PWM_LOG` | Print every PWM register access, decoded, with the duty ratio on PER, FP and CFG writes. | Off. |
| `DM32_PWM_TRACE` | Path. Append `<vtime_ns> <ch> <act_per> <act_fp> <cfg>` on each change of a channel's effective state. The file opens at the first change. | Unset: no trace. |

## Watchdog

| Variable | Effect | Default |
|---|---|---|
| `DM32_WDT_LOG` | Log every watchdog register access, with `(locked)` on writes that are dropped. | Off. |
| `DM32_WDT_NORESET` | A bite is logged and the count re-arms. The guest is not reset. | Off: a bite resets the guest. |
| `DM32_WDT_HZ` | The watchdog counter clock, in Hz, 4 to 4294967295. The datasheet does not give it; see [devices.md](devices.md#watchdog). | `17895697`: 2^30 ticks, the reset-value timeout, in the datasheet's 60 s. |

## I2C and RTC

| Variable | Effect | Default |
|---|---|---|
| `DM32_I2C_LOG` | Log controller aborts (with the target address), RTC reads, RTC CCR writes and RTC alarm matches. | Off. |

The RTC counts on QEMU's RTC clock, chosen with the standard `-rtc clock=` option: `host` (the
default) follows host time, `vm` follows virtual time, so the RTC reads the same in every run.

## RDA5807M FM tuner

| Variable | Effect | Default |
|---|---|---|
| `DM32_FM_STATIONS` | The stations the tuner receives, `kHz[:rssi],...` (for example `94500:40,101100:70`). A seek stops on one; a station sets FM_TRUE and, unless MONO, ST. RSSI 0-127. | Unset: no stations (every seek fails). Station RSSI default 60. |
| `DM32_FM_RSSI` | RSSI of every channel that is not a listed station, 0 to 127. | `0` |
| `DM32_FM_LOG` | Log register writes and tune or seek completions. | Off. |

## FD6818

| Variable | Effect | Default |
|---|---|---|
| `DM32_FD6818_ID` | Value of REG_00H, the chip ID, at reset and at each soft reset. Range 0 to `0xffff`; `0x6818` works. | `0x4819` |
| `DM32_FD6818_LOG` | Log register writes (decoded for REG_30H, REG_38H and REG_39H), reads with their page, page selects, dropped read-only writes, RF environment entries as they apply, and interrupts. | Off. |
| `DM32_FD6818_BITS` | Log each data-phase edge and each transfer end. Verbose. | Off. |
| `DM32_FD6818_RSSI` | Fixed value, 0 to 255, for the RSSI byte of REG_62H. Disables jitter. Ignored when `DM32_RF_ENV` is set. | Unset: base `0x30`, with jitter. |
| `DM32_FD6818_RSSI_JITTER` | Peak-to-peak RSSI jitter in dB, 0 to 255. The jitter is deterministic. `0` disables it. Ignored when `DM32_RF_ENV` is set. | `12` |
| `DM32_FD6818_LOCK_US` | PLL lock time in microseconds, 0 to 1000000, after a REG_39H write or the PLL switch-on. | `40` |
| `DM32_RF_ENV` | Path of an RF environment script, read at board start-up. A malformed line, or a line longer than 254 characters, stops the board with its file and line number. Format below. | Unset: an empty band, noise floor only. |

`DM32_RF_ENV` script: one entry per line, `#` starts a comment.

```
# t_ms   freq_hz     dBm   modulation and sub-audio
floor -127                                  # noise floor in dBm (default -127)
0        438500000   -90   fm ctcss=88.5    # a carrier from 0 ms
2000     438500000   -60   fm dcs=023       # replaces the carrier on the same frequency
4000     438500000   off                    # removes it
5000     *           -20   dmr              # * = the tuned frequency, whatever it is
```

`t_ms` is virtual time since start-up and may have a fraction. The level is at the antenna, in dBm.
The modulation is `none` (the default), `fm` or `dmr`; `ctcss=<Hz>` and `dcs=<octal>` (with `i` for
an inverted code, `n` accepted for normal) need `fm`. An entry replaces any carrier on the same
frequency. At most 256 entries and 16 simultaneous carriers. What the receiver makes of the carriers
is in [devices.md](devices.md#fd6818).

## Cache controller

| Variable | Effect | Default |
|---|---|---|
| `DM32_CACHE_LOG` | Log each CER, CIR and CRCR write to stderr, decoded: enable bits, the operation, and each region's base and size. | Off. |

## DMR baseband RAM

| Variable | Effect | Default |
|---|---|---|
| `DM32_DMR_LOG` | Log writes grouped into bursts, and reads of the status bytes, with region names. The codec's registers are a separate device; see `DM32_CODEC_LOG`. | Off. |
| `DM32_DMR_TRACE` | Path. Write one 17-byte record per write: a little-endian virtual-time u64, offset u32, size u8, value u32. The file is truncated at start-up. | Unset: no trace. |
| `DM32_DMR_DUMP` | Path. Write the 64 KB window as raw bytes at exit. | Unset: no dump. |

## Codec

| Variable | Effect | Default |
|---|---|---|
| `DM32_CODEC_LOG` | Log every codec register read and write to stderr, with the register's name. Read once at start-up. | Off. |

## SPI-NOR data chip

| Variable | Effect | Default |
|---|---|---|
| `DM32_SPINOR` | Path. Each SSI instance loads this file over its `0xFF` store at start-up. A file that does not exist is ignored. | Unset: all `0xFF`. |
| `DM32_SPINOR_OUT` | Path. At exit, write each store that a program or erase changed, through `<path>.tmp` and a rename. A second changed instance goes to `<path>.<idx>`. | Unset: nothing is written. |
| `DM32_SPINOR_SYNC` | Also write `DM32_SPINOR_OUT` after each completed program or erase. | Off. |
| `DM32_SPINOR_WIP_READS` | Number of RDSR reads that report WIP after each completed program or erase. An integer from 0 to 1000. | `2` |
| `DM32_SPINOR_LOG` | Log erases, program completions, RDSR results, and refused data-register writes. | Off. |
