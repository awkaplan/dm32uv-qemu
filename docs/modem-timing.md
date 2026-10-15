# DMR Layer-2 slot-timing model

This page describes the DMR Layer-2 slot counter and TX-slot interrupt in
[`hw/misc/dm32_sysreg.c`](../hw/misc/dm32_sysreg.c). The model covers timing and control flow only.
It does not model RF, the air interface, or audio. The `0x11000000` window in general is described
in [devices.md](devices.md).

## What is modelled

`dm32_sysreg` maps `0x11000000`-`0x1100FFFF` as read/write storage. The registers below have
behaviour beyond storage. The offsets are from `0x11000000`.

| Offset | Register | Behaviour |
|---|---|---|
| `+0x004` | `CLK_MGR_REG0x04` | Reads return bits 31 and 30 set (PLL locked and ready). The other bits keep their stored value. Stock clock init at IRAM `0x12480` busy-waits on this. |
| `+0x100` | `WORK_MODE` | Storage. Stock writes `0x6e` at boot. |
| `+0x3b0` | `INTERP_CLEAR` | Writing bit 0 as `1` clears a pending TX-slot interrupt. |
| `+0x400` | `LAYER2_CONTROL` | Storage. Bit 1 is `tx_master_mode`. Stock wrote `0xc4` (passive) at boot. `0xcb` and `0xd3` are active-mode values recorded from stock firmware traces. |
| `+0x404` | `LAYER2_SLOTON` | Bit 0 is `dll_tx_slot_on`. See [Slot counter](#slot-counter). |
| `+0x408` | `LAYER2_TXRX_CTRL` | Storage. A write of `0x40` or `0x41` (low byte) while the counter runs logs a TX-ORIGINATE event. |
| `+0x40c` | `LAYER2_SLOT_CNT` | Storage. The counter does not read it. |
| `+0x41c` | `LAYER2_STATUS` | Reads return the live slot counter in bits 8:0. Other bits come from storage. |

The register offsets follow the HR_C7000 datasheet register table cited in the source comments. The
CONTROL values come from the firmware trace, not from the datasheet.

## Slot counter

- **Start.** A write to `LAYER2_SLOTON` with bit 0 set starts the counter when `LAYER2_CONTROL` bit 1
  is set. The counter starts at 0. A write with bit 0 set while `LAYER2_CONTROL` bit 1 is clear does
  not start it.
- **Passive mode.** With `tx_master_mode` clear, the model does not start the counter. The source
  says passive establishment begins on an RX-slot event, and the model does not drive RX-slot
  events.
- **Stop.** A write to `LAYER2_SLOTON` with bit 0 clear stops the counter and cancels its timer.
- **Period.** The counter advances once per 30 ms of QEMU virtual time, on `QEMU_CLOCK_VIRTUAL`.
  Assumption: the 30 ms period is a modelling choice that matches a DMR half-frame. The 84 MHz
  peripheral clock is not used.
- **Range.** The counter runs `0` to `287`. On the tick that would take it past 287, it wraps to `0`
  and raises the TX-slot interrupt. One wrap therefore takes 288 ticks, which is 8.64 s of virtual
  time.

## Interrupt

- The TX-slot interrupt is wired to PIC source 30. The PIC encodes a source as vector `source + 32`,
  so the vector is 62 (`dm32_pic.c`, flag encoding). The stock TX-slot handler is at `0x2c42c` in the
  firmware trace.
- The line is held high from the wrap until `INTERP_CLEAR` bit 0 is written as `1`. The PIC is
  level-triggered, so the line stays asserted until the firmware acknowledges it.
- Stock's TX-slot handler writes `INTERP_CLEAR` bit 0, then posts the semaphore `0x35f50` that wakes
  the TX-slot task `0x2d604`. The model logs the acknowledgement. It does not log the semaphore post,
  which is part of the same handler.
- The RX-slot interrupt (PIC source 31, vector 63) is not modelled.

## Logging and the self-test switch

Set `DM32_MODEM_LOG=<file>` to append one line per event. The file is flushed after each line. Logged
events are:

- the start message at device init, with the slot period,
- counter start and stop,
- writes to `WORK_MODE`, `LAYER2_CONTROL`, `LAYER2_SLOTON` and `LAYER2_TXRX_CTRL`,
- each wrap, with the virtual time in nanoseconds,
- each `INTERP_CLEAR` that clears the interrupt,
- each TX-ORIGINATE, with the command value and the slot number.

Set `DM32_MODEM_LEADER=1` to force active establishment. This is a validation aid and is not part of
the modelled hardware. On the first `WORK_MODE` write with bits 4:3 set, the model writes
`LAYER2_CONTROL = 0xcb` and `LAYER2_SLOTON = 0x05` and starts the counter. While the forced
establishment is latched, later `LAYER2_SLOTON` writes do not stop it.

```sh
DM32_MODEM_LOG=modem.log DM32_MODEM_LEADER=1 ./build/qemu-system-cskyv2 ...
grep -c "slot wrap" modem.log
```

## Not modelled

- RF, the PLL, 4FSK modulation, the vocoder, and any audio path.
- The FD6818 RF transceiver's signal path. See [coverage.md](coverage.md).
- The DMR baseband RAM at `0x16000000`. It is a separate device, `dm32_dmrram`, and it only captures
  writes. See [devices.md](devices.md).
- A TX-ORIGINATE is a log line. The model produces no signal, and it does not check that the transmit
  chain works.
- The real slot period. The 30 ms period is not compared against hardware.

## Observed with stock firmware

These results come from runs of the stock DM32.00.01.048 image with the ALPU-bypass patch applied
(see [running.md](running.md#inputs)).

- **Boot without the self-test.** Stock wrote `WORK_MODE = 0x6e`, which has bits 4:3 set. It wrote
  `LAYER2_CONTROL = 0xc4`, which is passive. It wrote `LAYER2_SLOTON = 0x05` and
  `LAYER2_TXRX_CTRL = 0x40`. The counter did not start, because `tx_master_mode` is clear. The stock
  TX-slot task reads bits 4:3 of `WORK_MODE` as `layermode`, which is the gate that the `0x6e` value
  passes. This is taken from the firmware trace, not re-verified here.
- **Boot with `DM32_MODEM_LEADER=1`.** The counter ran freely. The run logged 16 wraps over about
  150 s of virtual time. Each wrap was followed by a stock `INTERP_CLEAR` acknowledgement. The
  average interval was about 9.3 s. The nominal value is 288 x 30 ms = 8.64 s. The source does not
  account for the difference. Unverified.

These runs show the counter, the interrupt line, and the acknowledgement path working with stock
firmware. They do not show that the TX-slot task or the transmit path ran, because the model does not
log those.
