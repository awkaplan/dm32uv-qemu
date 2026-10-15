# DM-32 helper scripts

Python helpers for the DM-32UV board model. Standard library only, Python 3.8 or newer. Each
script runs directly as `python3 scripts/<name>.py`. `dm32fb.py` is a shared module that sits
next to them and is not run on its own.

- `gram2png.py`: a `DM32_GRAMDUMP` raw capture to PNG. `--view panel` (default) gives the visible
  240x320 panel, upright. `gram` is the same area with a different colour expansion, and
  `gram-full` is all 256 columns of GRAM.
  `python3 scripts/gram2png.py --view panel --scale 3 gram.raw panel.png`
- `ppm2png.py`: a QEMU `screendump` PPM to PNG, with `--crop X,Y,W,H` or `--panel` (requires the
  240x320 panel, which a screendump already is; it is upright and passed through unchanged).
  `python3 scripts/ppm2png.py --panel screen.ppm panel.png`
- `gramdiff.py`: reports what changed between two `DM32_GRAMDUMP` captures. `--png` also writes a
  highlight image.
  `python3 scripts/gramdiff.py stock.raw patched.raw --png changes.png`
- `build_datachip.py`: builds the 16 MiB emulated data-chip image from your own radio's
  calibration page. The output goes to the current directory unless a path is given. Calibration
  is per unit, and the image is for the emulator only; never write it to a radio. Keep generated
  images out of the repository.
  `python3 scripts/build_datachip.py --cal-addr 0xA7000 cal_page.bin config.bin dm32_datachip_16m.bin`
- `dm32fb.py`: shared helpers (RGB565 decoding, PNG writing, panel geometry).

Each script takes `-h` for its full option list.
