/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Baofeng DM-32UV 12-bit DAC controller: CPU-side DAC, channels A-C
 *
 * Copyright (c) 2026 Alex Kaplan
 *
 * Models the 12-bit DAC controller of the HR_C7000 SoC, with three channels
 * (A-C), mapped at 0x140F0000 in a 64 KB window. The register map is
 * transcribed from:
 *   - dm32uv-re data/registers/HR_C7000-DM32.h, DAC_Type (DS section 5.10,
 *     p.423-426), and
 *   - dm32uv-re data/registers/HR_C7000-DM32.svd, peripheral DAC (field
 *     descriptions quoted below).
 * This is not the baseband TX DAC (SystemControl.DAC_CONTROL, DS 4.6.5.1);
 * that one is not modelled.
 *
 *   0x00 DAC_PD_CTRL     [2] PD_C [1] PD_B [0] PD_A. SVD: "1 : Power off;
 *                        0 : Power on". Reset 0x7 (all three channels
 *                        powered off at reset). RW.
 *   0x04 DAC_PD_MODE_EN  [2] MODE_CEN [1] MODE_BEN [0] MODE_AEN. "1 : Enable;
 *                        0 : Shutdown" (C, B) and "1 : Enable; 0 : Disable
 *                        Break" (A). Reset 0x7. RW. No modelled effect.
 *   0x08 DAC_DATA_A      [11:0] channel A data. Reset 0. RW (SVD: "FW: 0
 *                        reads, 2 writes").
 *   0x0c DAC_DATA_B      [11:0] channel B data. Reset 0. RW.
 *   0x10 DAC_DATA_C      [11:0] channel C data. Reset 0. RW.
 *   0x14 DAC_CTRL_VERID  Controller version, reset 0x20160229. SVD marks it
 *                        read-write ("for debugging only"), so writes are
 *                        stored; the header comment does not say otherwise.
 *
 * Firmware channel numbering. The stock function dac_channel_write_12bit
 * (IRAM 0x0002c388, 42 bytes; dm32uv-re
 * src/unclassified/named-unmatched/dac_channel_write_12bit_0002c388.c)
 * selects the register by its first argument:
 *   arg 1 -> DAC_DATA_A (+0x08)  (channel A, PD_A = PD_CTRL bit 0)
 *   arg 2 -> DAC_DATA_B (+0x0c)  (channel B, PD_B = PD_CTRL bit 1)
 *   arg 4 -> DAC_DATA_C (+0x10)  (channel C, PD_C = PD_CTRL bit 2)
 * The decompile of 0x0002c388 also shows the routine re-storing the other
 * DATA registers with their current values. A stock boot-to-menu
 * run accesses only DATA_C on the arg-4 path, with no
 * A or B reads or writes. No recorded run exercises the arg-2 path, so its
 * A/B behaviour is unverified. The model follows the observed accesses and
 * adds no re-stores. The caller dac_channel2_write_from_byte (0x0002c3b4)
 * sends arg 2 with value (byte << 4).
 *
 * Behaviour:
 * - Stored values are masked to the register's real bits (PD: 3 bits;
 *   DATA: 12 bits; VERID: 32).
 * - A DATA write while that channel's PD bit is 1 (powered down) is stored,
 *   not dropped. The datasheet text here does not say whether a powered-down
 *   channel latches data, so the model does not invent that behaviour. It
 *   warns instead (once per channel, to stderr).
 * - Registers in the window other than the ones above are plain storage. The
 *   first access to each such offset prints a one-time note to stderr, so an
 *   unmodelled register is never silent.
 * - Accesses are word-sized only (min = max = 4). Anything else is rejected
 *   by QEMU's memory core and shows up under -d guest_errors, rather than
 *   being silently widened.
 *
 * Environment variables (DM32_DAC_LOG is read at instance init, DM32_DAC_TRACE
 * at the first DATA write; see docs/environment.md):
 *   DM32_DAC_LOG=1        print every access by register name: writes, and
 *                         reads too.
 *   DM32_DAC_TRACE=<path> append one line per DATA write:
 *                         "<vtime_ns> <A|B|C> <value>" (value is the stored
 *                         12-bit decimal). The file is flushed on every line.
 *
 * Not modelled: PD_MODE_EN (low-power mode) has no effect on DATA or PD; the
 * output (analogue) value of each channel is not computed; no interrupt (the
 * SVD lists PIC source 25, but this model raises none); no timing of the
 * conversion.
 */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "hw/csky/dm32.h"
#include <stdio.h>
#include <stdlib.h>

#define TYPE_DM32_DAC "dm32_dac"
OBJECT_DECLARE_SIMPLE_TYPE(DM32DacState, DM32_DAC)

#define WIN 0x10000

#define R_PD_CTRL     0x00
#define R_PD_MODE_EN  0x04
#define R_DATA_A      0x08
#define R_DATA_B      0x0c
#define R_DATA_C      0x10
#define R_CTRL_VERID  0x14

#define PD_MASK       0x7u
#define DATA_MASK     0xfffu
#define VERID_RESET   0x20160229u
#define NCH           3

struct DM32DacState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    /* storage for every word in the window */
    uint32_t reg[WIN / 4];
    /* one-time "unmodelled register" note, per offset */
    uint8_t  noted[WIN / 4];
    /* one-time powered-down-write warning, per channel */
    uint8_t  warned[NCH];
    /* DM32_DAC_TRACE stream, opened on first DATA write */
    FILE    *trace;
    int      trace_tried;
    int      log;               /* DM32_DAC_LOG=1 */
};

static const char *reg_name(uint32_t off)
{
    switch (off) {
    case R_PD_CTRL:
        return "PD_CTRL";
    case R_PD_MODE_EN:
        return "PD_MODE_EN";
    case R_DATA_A:
        return "DATA_A";
    case R_DATA_B:
        return "DATA_B";
    case R_DATA_C:
        return "DATA_C";
    case R_CTRL_VERID:
        return "CTRL_VERID";
    default:
        return NULL;
    }
}

/* Channel index for a DATA offset (0=A, 1=B, 2=C), or -1. */
static int data_channel(uint32_t off)
{
    switch (off) {
    case R_DATA_A:
        return 0;
    case R_DATA_B:
        return 1;
    case R_DATA_C:
        return 2;
    default:
        return -1;
    }
}

static const char chan_letter[NCH] = { 'A', 'B', 'C' };
/*
 * The firmware's channel argument for each DATA register
 * (dac_channel_write_12bit: 1/2/4).
 */
static const int chan_fw_arg[NCH] = { 1, 2, 4 };

static void trace_data_write(DM32DacState *s, int ch, uint32_t value)
{
    if (!s->trace_tried) {
        const char *path = getenv("DM32_DAC_TRACE");
        s->trace_tried = 1;
        if (path && *path) {
            s->trace = fopen(path, "a");
            if (!s->trace) {
                fprintf(stderr, "dm32_dac: cannot open DM32_DAC_TRACE=%s\n",
                        path);
            }
        }
    }
    if (s->trace) {
        fprintf(s->trace, "%lld %c %u\n",
                (long long) qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                chan_letter[ch], value);
        fflush(s->trace);
    }
}

static uint64_t dm32_dac_read(void *opaque, hwaddr off, unsigned size)
{
    DM32DacState *s = opaque;
    uint32_t v;

    if (off + 4 > WIN) {
        return 0;
    }
    v = s->reg[off / 4];
    switch (off) {
    case R_PD_CTRL:
    case R_PD_MODE_EN:
        v &= PD_MASK;
        break;
    case R_DATA_A:
    case R_DATA_B:
    case R_DATA_C:
        v &= DATA_MASK;
        break;
    default:
        break;
    }

    if (reg_name(off) == NULL && !s->noted[off / 4]) {
        s->noted[off / 4] = 1;
        fprintf(stderr, "dm32_dac: note: access to unmodelled offset "
                "0x%02x (storage only)\n", (unsigned) off);
    }
    if (s->log) {
        const char *n = reg_name(off);
        if (n) {
            fprintf(stderr, "DAC %s -> 0x%x\n", n, v);
        } else {
            fprintf(stderr, "DAC +0x%02x -> 0x%x (unmodelled)\n",
                    (unsigned) off, v);
        }
    }
    return v;
}

static void dm32_dac_write(void *opaque, hwaddr off, uint64_t val,
                           unsigned size)
{
    DM32DacState *s = opaque;
    uint32_t v = (uint32_t) val;
    const char *n = reg_name(off);
    int ch = data_channel(off);

    if (off + 4 > WIN) {
        return;
    }

    switch (off) {
    case R_PD_CTRL:
        v &= PD_MASK;
        break;
    case R_PD_MODE_EN:
        v &= PD_MASK;
        break;
    case R_DATA_A:
    case R_DATA_B:
    case R_DATA_C:
        v &= DATA_MASK;
        break;
    default:
        break;
    }

    if (ch >= 0) {
        /*
         * PD bit for this channel: PD_CTRL bit == channel index
         * (A=bit0, B=bit1, C=bit2). 1 = off.
         */
        int powered_down = (s->reg[R_PD_CTRL / 4] >> ch) & 1;

        if (s->log) {
            fprintf(stderr, "DAC %s <- 0x%x (ch%d%s)\n", n, v,
                    chan_fw_arg[ch], powered_down ? ", PD=off" : "");
        }
        if (powered_down) {
            dm32_event(DM32_EV_DAC_PD, "%s = 0x%x while channel %c is "
                       "powered down", n, v, chan_letter[ch]);
        }
        if (powered_down && !s->warned[ch]) {
            s->warned[ch] = 1;
            fprintf(stderr, "dm32_dac: WARNING: %s written while channel "
                    "%c is powered down (PD_CTRL bit %d = 1); further such "
                    "writes to %c are not reported\n",
                    n, chan_letter[ch], ch, chan_letter[ch]);
        }
        s->reg[off / 4] = v;
        trace_data_write(s, ch, v);
        return;
    }

    if (n) {
        if (s->log) {
            fprintf(stderr, "DAC %s <- 0x%x\n", n, v);
        }
        s->reg[off / 4] = v;
        return;
    }

    /* Unmodelled offset: storage, with the one-time note. */
    if (!s->noted[off / 4]) {
        s->noted[off / 4] = 1;
        fprintf(stderr, "dm32_dac: note: write to unmodelled offset "
                "0x%02x (storage only)\n", (unsigned) off);
    }
    if (s->log) {
        fprintf(stderr, "DAC +0x%02x <- 0x%x (unmodelled)\n",
                (unsigned) off, v);
    }
    s->reg[off / 4] = v;
}

static const MemoryRegionOps dm32_dac_ops = {
    .read = dm32_dac_read,
    .write = dm32_dac_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* SVD reset values: all channels powered down, everything else 0. */
static void dm32_dac_reset(DeviceState *dev)
{
    DM32DacState *s = DM32_DAC(dev);

    memset(s->reg, 0, sizeof(s->reg));
    s->reg[R_PD_CTRL / 4]    = 0x7;
    s->reg[R_PD_MODE_EN / 4] = 0x7;
    s->reg[R_CTRL_VERID / 4] = VERID_RESET;
}

static void dm32_dac_init(Object *obj)
{
    DM32DacState *s = DM32_DAC(obj);
    memory_region_init_io(&s->iomem, obj, &dm32_dac_ops, s, TYPE_DM32_DAC,
                          WIN);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    memset(s->noted, 0, sizeof(s->noted));
    memset(s->warned, 0, sizeof(s->warned));
    dm32_dac_reset(DEVICE(obj));
    s->trace = NULL;
    s->trace_tried = 0;
    s->log = dm32_env_flag("DM32_DAC_LOG");
}

static void dm32_dac_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), dm32_dac_reset);
}

static const TypeInfo dm32_dac_type = {
    .name = TYPE_DM32_DAC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DM32DacState),
    .instance_init = dm32_dac_init,
    .class_init = dm32_dac_class_init,
};
static void dm32_dac_register(void)
{
    type_register_static(&dm32_dac_type);
}
type_init(dm32_dac_register)
