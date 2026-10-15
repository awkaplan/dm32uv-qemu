# README screenshots

The images in this directory are frames from the emulated LCD, not photographs of a radio. Each is
the LCD controller's frame buffer, captured by `DM32_GRAMDUMP` with `DM32_GRAMSEQ=1` (see
[environment.md](../environment.md)) and rendered by
[`scripts/gram2png.py`](../../scripts/README.md).

| File | Run |
|---|---|
| `screens.png`, panel 1 | Stock firmware DM32.00.01.048 with dm32uv-re's generic factory-default data chip (`data/firmware/patching/datachip_generic_factory_defaults.bin.xz`, written by the stock firmware itself, no per-unit data), at the standby screen |
| `screens.png`, panels 2-4, and `snake.gif` | dm32uv-re's Snake example patch on the stock image, no data chip, driven by the Snake walkthrough key script that is the stock-firmware regression test |

The runs used dm32uv-re's `qemu_run.py`. The four panels were composed side by side, and the
animation strings together the walkthrough's distinct frames, with Pillow.

The screens show the stock firmware's user interface, which belongs to its vendor; they are here to
show what the emulator runs.
