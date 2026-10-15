# Running the model

This page covers the inputs that `qemu-system-cskyv2 -M dm32` takes, the command line, the
display, frame capture, and debugging. For building QEMU and installing the overlay, see the
[quick start in README.md](../README.md#quick-start).

## Inputs

The model has no firmware of its own. Each input is a file passed on the command line.

### Program flash (`-kernel`)

`-kernel` loads a 1 MB program-flash image at `0x03000000`, and the CPU boots from it. The image
is either:

- an [OpenRTX](https://github.com/OpenRTX/OpenRTX) build for the DM-32 target, or
- a stock Baofeng firmware image (`DM32.00.01.048` is the image the model was measured with).

A stock image needs three emulator-only patches before it boots under this model. They are
applied by dm32uv-re's
[`tools/src/dm32_patch_tools/compose_qemu_image.py`](https://github.com/awkaplan/dm32uv-re/blob/main/tools/src/dm32_patch_tools/compose_qemu_image.py):

```sh
python3 tools/src/dm32_patch_tools/compose_qemu_image.py --out boot.bin
```

The three patches are:

- **The ALPU-bypass sector** (`data/firmware/patching/sector_030c6000_alpubypass.bin` in dm32uv-re).
  This is the important one. Without it the boot halts at IRAM `0x169d4`, a `br16` to itself. That
  address is the failure leg of the ALPU-MP anti-clone check. The bit-banged bus on GPIOA 5 and 6 is
  modelled, but no ALPU-MP sits on it, so the check cannot succeed.
- **A hardware-binding record check.**
- **An auto-power-off NOP.** Stock classifies the battery from an ADC reading the model does not
  source realistically, and would power itself off; the patch disables that path.

The patched image must never be flashed to a physical radio. On real hardware the three patched
behaviours are correct, and the ALPU-MP check is the anti-clone gate.

### IRAM (`-device loader,file=IRAM.bin,addr=0x00010000`)

IRAM is 288 KB at `0x00010000`-`0x00057FFF`. The firmware's boot stub normally copies initialised
data from flash into IRAM. The IRAM image pre-populates it so that boot does not depend on that
copy. The line is optional, but the model is not expected to boot without it.

The image is derived from a stock program-flash image by dm32uv-re's
[`derive_iram.py`](https://github.com/awkaplan/dm32uv-re/blob/main/tools/src/dm32_patch_tools/derive_iram.py):

```sh
python3 tools/src/dm32_patch_tools/derive_iram.py --flash <1 MB image> --out IRAM.bin
```

It contains the vendor firmware bytes plus zeroed bss. It is identical on every unit, so no live
capture from a radio is needed.

### SRAM (optional, `-device loader,file=SRAM.bin,addr=0x18000000`)

SRAM is 32 KB at `0x18000000`. The board model loads it from a file when one is given. The sources
for this page do not name a tool that derives the SRAM image.

For a run whose uninitialised RAM looks like the radio's, set `DM32_RAM_FILL=noise` and load only
what stock initialises: IRAM `0x00010000`-`0x0004a7af` (the first `0x3a7b0` bytes of the IRAM image)
and SRAM `0x18000000`-`0x18004bff` (`0x4c00` bytes). Full-size images overwrite the noise.

### Data chip (`DM32_SPINOR`)

The 16 MB Winbond W25Q128 data chip holds the codeplug and the calibration page. The model backs it
with a file named by the `DM32_SPINOR` environment variable. Without that variable it is blank
(`0xFF`). Three choices:

- **Blank (default).** No channels are configured, and calibration is absent.
- **Generic factory defaults.** dm32uv-re ships
  `data/firmware/patching/datachip_generic_factory_defaults.bin.xz`. Decompressed, the 16 MB image
  has sha256 `487b33f8940813d265db5b2db15dc5fe99c5cabaa5dadbb4de9b4c3f2788f2f2`. It is the image
  that stock's own `codeplug_factory_defaults_write` produces when run under this model, so every
  byte comes from the generic stock firmware. Its builder is dm32uv-re's
  `tools/src/dm32_patch_tools/make_generic_datachip.py`. With this image the standby screen shows
  the VFO, and channel mode shows "Zone-Ana" and "CH 1". It supplies channels only. The calibration
  page (block 2) stays blank apart from its tag byte.
- **Built from your own radio.** `scripts/build_datachip.py` combines a 4096-byte calibration page
  dump from your unit, an optional config-region dump placed at offset 0, and the page's address on
  your unit (`--cal-addr`, required) into a 16 MB image:

  ```sh
  python3 scripts/build_datachip.py cal_page.bin [cfg_region.bin] datachip.bin --cal-addr 0xA7000
  ```

  The calibration page's address differs between units (`0xA3000`, `0xA7000` and `0xAA000` have
  been seen). Find it in your own dump by its page tag or by the content signature
  `00 40 00 47 ff ff ff ff ff ff ff ff 58 02 7c 02`; never assume it.

Select the image with `DM32_SPINOR=datachip.bin`. Program and erase operations reach the file only
through `DM32_SPINOR_OUT`. See [environment.md](environment.md) for the SPI-NOR variables.

Calibration is per unit and is never shared between units. This repository ships none. Without a
calibration dump, the calibration page reads blank, firmware that checks it reports calibration
invalid, and boot is otherwise unaffected.

## Running

```sh
./build/qemu-system-cskyv2 \
  -M dm32 -cpu ck803 \
  -L <qemu>/pc-bios \
  -nographic \
  -monitor unix:/tmp/dm32.qmp,server,nowait \
  -serial none \
  -kernel boot.bin \
  -device loader,file=IRAM.bin,addr=0x00010000
```

Add `-device loader,file=SRAM.bin,addr=0x18000000` to load the optional SRAM image.

| Option | Meaning |
|---|---|
| `-M dm32` | The board model in `hw/csky/dm32.c`. `-M help` lists it. |
| `-cpu ck803` | The CK803 core that the HR_C7000 SoC uses. |
| `-L <qemu>/pc-bios` | The directory QEMU reads its keymap and firmware files from. Needed for VNC; see [Display](#display). |
| `-nographic` | No graphics window. The console is on stdio. Use `-display none` if no output is needed. |
| `-monitor unix:PATH,server,nowait` | A QEMU monitor on a Unix socket. `nowait` lets QEMU start without a client. Used for `screendump`. |
| `-serial none` | No host device for the first serial port, UART0. The firmware's UART bytes are dropped. `-serial null` drops them too. `-serial stdio` shows them in the terminal. |
| `-kernel boot.bin` | The 1 MB program-flash image, loaded at `0x03000000`. |
| `-device loader,file=...,addr=...` | Loads a raw file at an address. Used for IRAM and SRAM. |

`-serial` options are numbered in order. The first is UART0. A second `-serial` argument maps to
UART1, and so on.

## Display

The LCD model renders a 240x320 panel into a QEMU graphics console. VNC exports that console.

```sh
./build/qemu-system-cskyv2 -M dm32 -cpu ck803 -L <qemu>/pc-bios \
  -serial stdio -vnc 127.0.0.1:0 \
  -kernel boot.bin -device loader,file=IRAM.bin,addr=0x00010000
```

- `-vnc HOST:N` listens on port `5900 + N`. `127.0.0.1:0` accepts local clients only. `0.0.0.0:0`
  accepts connections from any interface.
- `-L <qemu>/pc-bios` is required with VNC. Without it, QEMU aborts when VNC starts with
  `Could not read keymap file: 'en-us'`.
- Use `-serial stdio` instead of `-nographic`. `-nographic` also disables the graphics console that
  VNC exports, and the firmware's boot output still reaches the terminal through `-serial stdio`.
- Connect with a real RFB client, such as TigerVNC, TightVNC or Remmina. macOS Screen Sharing does
  not render this model's framebuffer correctly.
- A client that connects to an idle screen receives the current framebuffer. The LCD model
  implements the graphics-console invalidate callback for this.
- The display refreshes about every 30 ms, whether or not the firmware has finished repainting. A
  clear-and-redraw cycle is visible as flicker. Use `DM32_GRAMDUMP` for captures that must not
  include half-drawn screens.
- Keyboard and mouse input from the VNC client are not wired to the keypad. Inject key presses
  with `DM32_KEYSEQ` instead. See [key-injection.md](key-injection.md).
- Under `-icount`, virtual time does not follow wall-clock time. The VNC view can run faster or
  slower than real time without changing what the firmware does.

## Capturing frames

Two methods are available.

### Quiescent dump (`DM32_GRAMDUMP`)

```sh
DM32_GRAMDUMP=frame.raw DM32_GRAMQUIET=150 ./build/qemu-system-cskyv2 \
  -M dm32 -cpu ck803 -L <qemu>/pc-bios -display none -monitor none -serial null \
  -kernel boot.bin -device loader,file=IRAM.bin,addr=0x00010000
python3 scripts/gram2png.py frame.raw frame.png
```

- `DM32_GRAMDUMP=path` writes the GRAM contents when the framebuffer has been unchanged for
  `DM32_GRAMQUIET` milliseconds. The default is 150 ms when the variable is unset or not positive.
  A slow boot is therefore not captured mid-draw.
- The dump is raw RGB565, two bytes per pixel. Its rows are **256 pixels wide**, not 240. Read it
  with a 256-pixel stride. A reader that assumes 240 produces a sheared image.
- `scripts/gram2png.py` converts the dump. `--view panel` (the default) renders the visible
  240x320 panel, upright. `--view gram` renders the same area with a different colour expansion.
  `--view gram-full` renders all 256 columns.

### Monitor screendump

With the monitor socket from the command line above, send the HMP command `screendump` to the
socket, for example with `nc -U /tmp/dm32.qmp`:

```text
screendump /tmp/screen.ppm
```

Convert the PPM with `--panel` for the upright panel view:

```sh
python3 scripts/ppm2png.py --panel /tmp/screen.ppm screen.png
```

### Comparing two captures

```sh
python3 scripts/gramdiff.py before.raw after.raw
```

`scripts/gramdiff.py` takes two `DM32_GRAMDUMP` files and reports the pixels that differ.

## Debugging

- `-s` opens QEMU's gdb stub on TCP port 1234. `-S` also halts the CPU before the first
  instruction. Connect with a gdb built for C-SKY and `target remote :1234`. dm32uv-re's
  [`tools/src/dm32_patch_tools/gdb_rsp.py`](https://github.com/awkaplan/dm32uv-re/blob/main/tools/src/dm32_patch_tools/gdb_rsp.py)
  reads registers and memory over the same stub.
- `-d unimp,guest_errors,invalid_mem` logs three classes of event to stderr, or to the file given by
  `-D`:
  - `unimp`: an access to a datasheet address-map block, or a part of one, that no model covers.
    Reads return 0 and writes are dropped. Covers SDIO, USB, the tails of
    partly modelled blocks, and the LCSFC window above the 1 MB flash image. Lines name the block,
    for example `dm32.timer: unimplemented device read`.
  - `invalid_mem`: an access outside every address-map block, for example `0x16010000` or
    `0x18008000`. Reads return 0 and writes vanish. The board's background region logs these as
    `region 'dm32.unassigned'`.
  - `guest_errors`: guest-side errors that QEMU reports.
- Every run prints one `dm32-events:` line at exit with a count of each permissive event: accesses
  the model lets through or drops where the silicon would not. `DM32_STRICT=all` (or a list of
  classes) stops the guest at the first such event instead, with exit status 3. See
  [devices.md](devices.md#permissive-events-and-dm32_strict).
- Most device models read a `DM32_*` variable to turn on logging or change behaviour, for example
  `DM32_KEYLOG=1`, `DM32_SPINOR_LOG=1` and `DM32_MODEM_LOG=path`. The full list is in
  [environment.md](environment.md).

## Driving it from dm32uv-re

dm32uv-re's
[`tools/src/dm32_patch_tools/qemu_run.py`](https://github.com/awkaplan/dm32uv-re/blob/main/tools/src/dm32_patch_tools/qemu_run.py)
builds the command line above, adds the key script, runs the emulator, and summarises the log. It
prints two lines:

```text
t=<seconds> steps=<n> crash=<n> muxwarn=<n> unimp=<n> invalid=<n>
F<index>:<nonblack> F<index>:<nonblack> S<step> ...
```

- `steps` is the last `DM32_KEYSEQ` step reached.
- `crash` counts CPU-model crashes in the log.
- `muxwarn` counts LCD writes made while the data-bus pads were muxed to the keypad GPIO. A forgotten
  mux restore in a patch shows up here. See
  [keypad-input](https://github.com/awkaplan/dm32uv-re/blob/main/docs/keypad-input.md).
- `F<index>:<nonblack>` is one `DM32_GRAMDUMP` capture and its count of non-black pixels.
- `S<step>` is one key-sequence step.

The order of the token line changes with host load, because timer callbacks can bunch. To compare
two runs, compare the step count, `crash`, `muxwarn`, and the non-black counts of the frames. Do
not compare the whole token line.

The workflow is described in dm32uv-re's
[`docs/qemu-workflow.md`](https://github.com/awkaplan/dm32uv-re/blob/main/docs/qemu-workflow.md).
