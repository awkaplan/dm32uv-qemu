# SoC boot ROM

This page describes the HR_C7000's mask boot ROM: what it contains, the observations that identify
it, and why the board model does not run it. The ROM image itself is not in this repository.

## Ownership

The ROM is the SoC vendor's code, fixed in silicon, and it is not distributed here.

## What the ROM is

The CK803S core starts from a 2 KB mask ROM. The image was read from a radio's own address space
at `0x00000000`-`0x000007FF`. Of its 2048 bytes, only the first 564 (`0x000`-`0x233`) are code and
data. The rest is fill.

## Mirroring

A read of `0x00000000`-`0x0000FFFF` returns the same 2 KB block 32 times. The block is the boot ROM,
mirrored across the low 64 KB. The range holds no other code. The mirror explains the low addresses
that an earlier analysis had placed in a missing region below IRAM. Those are CK803S trap or
syscall-gate artifacts, not real code.

## Contents by offset

| Offset | Contents |
|---|---|
| `0x000` | Reset entry. Zeroes every general-purpose register, then builds `0x8000 | (1 << 8)` in `l0` (the PSR value). |
| `0x1da` | UART transmit loop: `ld16.w l0,(a1,0x14)`, `btsti32 l0,5`, `bt16 0x1da`, `st16.w a2,(a1,0)`. |
| `0x20c` | `0x0c646874`, the DH_FLAG magic, which reads as `"thd\x0c"` (T-Head). The ROM checks it at flash `+0x00`. |
| `0x218` | `0x02000003`, part of the `02 24 00 03` announcement frame that the ROM sends on UART0 before it jumps to the loaded entry. |

### UART transmit polarity

The loop at `0x1da` writes the transmit register only while LSR bit 5 is clear. Bit 5 therefore
means the transmit FIFO is full, not that the holding register is empty. Treating it as
"holding register empty" inverts the meaning of the loop.

## Board model

The board does not load or run the ROM.

- `hw/csky/dm32.c` maps `0x00000000`-`0x0000FFFF` as zero-filled RAM (`dm32.lowram`).
- The reset hook sets `vbr = 0x03000004` and `pc = 0x03001000` at every reset, which starts the
  flash image directly (see [boot-path](boot-path.md), "Reset entry and vector base").

Consequently none of the ROM's code runs in the model, including the announcement frame and the
boot-mode decision made by the firmware's own code.

## Getting the image

The image is not included in the repository. It can be read from a radio's own address space at
`0x00000000`-`0x000007FF` by a firmware patch that dumps low memory. The read is outside this
repository. Once the bytes are on disk, the image can be loaded by hand:

```sh
qemu-system-cskyv2 -M dm32 -cpu ck803 -kernel firmware.bin \
  -device loader,file=bootrom.bin,addr=0
```

This loading path has not been tested. The reset hook still forces the entry at `0x03001000`, so
the loaded ROM is not executed. Making the ROM run is a separate change to the reset path.
