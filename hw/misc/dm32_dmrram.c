/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Baofeng DM-32UV DMR baseband RAM window (capture only)
 *
 * Copyright (c) 2026 Alex Kaplan
 *
 * A plain byte-addressable RAM mapped at 0x16000000 (64 KB). Stock firmware
 * writes outbound DMR frames into it and reads a few status bytes back from
 * it. Its only purpose in the model is observability: every access is logged
 * or recorded, so that what the firmware transmits (including the DMR privacy
 * PI header) can be seen under emulation. Without this device the window is
 * unmapped: reads return 0 and writes vanish with no fault (visible with
 * "-d invalid_mem").
 *
 * What the device does not do:
 * - No RX injection path. Nothing the model writes here is ever fed back as
 *   a received frame.
 * - No signal modelling. No RF, no vocoder, no AMBE, no codec data path. The
 *   window holds bytes; it does not act on them.
 * - No device behaviour. No busy flags, no FIFO or double-buffer state, no
 *   underrun detection, no interrupts. The firmware's own handshakes (if any)
 *   therefore complete only as far as RAM does.
 *
 * Layout, from the datasheet and the SVD (dm32uv-re
 * data/registers/HR_C7000-DM32.svd, peripherals Modem_Buffer and CODEC; the
 * header dm32uv-re data/registers/HR_C7000-DM32.h only has the codec struct):
 *
 *   0x0000-0x04AF  TX baseband RAM, 1200 bytes (Modem_Buffer addressBlock
 *                  0x0, SVD)
 *                    DMR phy: SEND_DATA0 +0x00-0x23, SEND_DATA1 +0x24-0x47
 *                    (alternating frames)
 *                    DMR link: PI/LC header, terminator or CSBK at
 *                    +0x00-0x0b (datasheet Table 52)
 *                    FM: FM_VOICE_1 +0x030-0x22f, FM_VOICE_2 +0x230-0x42f
 *                    (16-bit PCM, high byte first)
 *   0x04B0-0x095F  RX baseband RAM, 1200 bytes (Modem_Buffer addressBlock
 *                  0x4b0, SVD)
 *                    DMR phy: RECV_INFO +0x30-0x53 (relative to the RX RAM
 *                    base, SVD)
 *   0x0960-0x097F  "MODEM_RAM": named by PSPEC only, not in the datasheet
 *                  (SVD description)
 *   0x0980-0x09BF  "SOUND": named by PSPEC only, not in the datasheet (SVD
 *                  description)
 *   0x09C0-0x09FF  CODEC control registers, byte-wide (SVD peripheral CODEC,
 *                  size 0x40). dm32_codec is mapped over them, so accesses
 *                  there never reach this device.
 *   0x0A00-0xFFFF  no datasheet description; RAM here.
 *
 * Firmware touchpoints (dm32uv-re src/, decompiled corpus):
 *   sram_16000000_block_write @0x0300d3ec: byte-store loop into
 *     0x16000000+off, length n. Called by dmr_layer2_frame_build_and_send
 *     @0x03029d0c with (off 0, len 0x18) and (off 0 / off 0x24, len 0x24),
 *     and by radio_mode_transition_sequencer @0x03021670 with len 0x24
 *     (offsets per the dm32uv-re call sites).
 *   FUN_030c6780 reads 0x16000032/33 and 0x16000232/233; FUN_00016b60 reads
 *     the same four bytes.
 *
 * Reset state: the datasheet gives no reset value for the baseband RAM
 * contents. The RAM is zeroed when the device is created; that is a model
 * choice, not a hardware fact. The device has no reset handler, so a reset
 * leaves the contents as they were.
 *
 * Environment variables (all optional; all read once at instance init):
 *   DM32_DMR_LOG=1        stderr log. Every write: "DMRRAM W +0xOFF size=N
 *                         val=0x... (REGION)". Every read of the four known
 *                         status bytes (0x32, 0x33, 0x232, 0x233): "DMRRAM R
 *                         ...". Writes are grouped into BURST lines: a burst
 *                         is closed when more than 1 ms of virtual time
 *                         passes since the previous write, or when a write
 *                         lands in a different 0x100-aligned region. The
 *                         burst line is "DMRRAM BURST +0xLO..+0xHI n=COUNT
 *                         t=T0ns: HEX", where HEX has one byte per offset in
 *                         [LO, HI] ("--" = not written in that burst). The
 *                         final open burst is flushed at QEMU exit.
 *   DM32_DMR_TRACE=PATH   Binary record per write (see Trace format). Written
 *                         as writes happen, flushed at exit. Truncates PATH
 *                         when the device is created.
 *   DM32_DMR_DUMP=PATH    At QEMU exit, the whole 64 KB window is written to
 *                         PATH as raw bytes.
 *
 * Trace format (little-endian, 17 bytes per write, no padding, no header):
 *   u64  virtual time, ns (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) at the
 *        access)
 *   u32  offset within the window (0x0000-0xFFFF)
 *   u8   access size in bytes (1..4)
 *   u32  value written (the low `size` bytes are significant; upper bytes
 *        zero)
 */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qemu/notify.h"
#include "qemu/log.h"
#include "system/system.h"
#include "hw/csky/dm32.h"
#include <stdio.h>
#include <string.h>

#define TYPE_DM32_DMRRAM "dm32_dmrram"
OBJECT_DECLARE_SIMPLE_TYPE(DM32DmrRamState, DM32_DMRRAM)

#define DMRRAM_SIZE   0x10000u
#define BURST_GAP_NS  1000000    /* 1 ms of virtual time */
/* a burst never spans two 0x100-aligned regions */
#define BURST_REGION  0x100u

/* Known status-byte offsets read by firmware (FUN_030c6780, FUN_00016b60). */
static bool dmrram_is_watched_read(uint32_t off)
{
    return off == 0x32 || off == 0x33 || off == 0x232 || off == 0x233;
}

static const char *dmrram_region_name(uint32_t off)
{
    if (off < 0x4b0) {
        return "TX-RAM";
    }
    if (off < 0x960) {
        return "RX-RAM";
    }
    if (off < 0x980) {
        return "MODEM_RAM(PSPEC)";
    }
    if (off < 0x9c0) {
        return "SOUND(PSPEC)";
    }
    if (off < 0xa00) {
        return "CODEC";
    }
    return "unnamed";
}

struct DM32DmrRamState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint8_t ram[DMRRAM_SIZE];

    bool log;
    const char *dump_path;      /* from DM32_DMR_DUMP, may be NULL */
    FILE *trace;                /* from DM32_DMR_TRACE, may be NULL */
    Notifier exit_notifier;

    /* burst state (only used when log is on) */
    bool burst_open;
    uint32_t burst_base;        /* 0x100-aligned base of the open burst */
    uint32_t burst_lo, burst_hi;
    uint32_t burst_count;
    int64_t burst_t0, last_write_ns;
    /* which offsets were written in this burst */
    uint8_t burst_mask[DMRRAM_SIZE / 8];
};

static void dmrram_burst_flush(DM32DmrRamState *s)
{
    /*
     * A burst is normally one 0x100 region (<=256 bytes, ~770 chars). A
     * write that straddles a region edge can widen it; the hex is then
     * truncated at the buffer end, with "..." marked.
     */
    char line[2048];
    size_t n = 0;
    uint32_t off;

    if (!s->burst_open) {
        return;
    }
    n += snprintf(line + n, sizeof(line) - n,
                  "DMRRAM BURST +0x%04x..+0x%04x n=%u t=%lldns: ",
                  s->burst_lo, s->burst_hi, s->burst_count,
                  (long long) s->burst_t0);
    for (off = s->burst_lo; off <= s->burst_hi; off++) {
        if (n > sizeof(line) - 16) {
            n += snprintf(line + n, sizeof(line) - n, "...");
            break;
        }
        if (s->burst_mask[off >> 3] & (1u << (off & 7))) {
            n += snprintf(line + n, sizeof(line) - n, "%02x", s->ram[off]);
        } else {
            n += snprintf(line + n, sizeof(line) - n, "--");
        }
        if (off != s->burst_hi) {
            n += snprintf(line + n, sizeof(line) - n, " ");
        }
    }
    fprintf(stderr, "%s\n", line);
    memset(s->burst_mask, 0, sizeof(s->burst_mask));
    s->burst_open = false;
}

static void dmrram_burst_note(DM32DmrRamState *s, uint32_t off, int size,
                              int64_t now)
{
    uint32_t base = off & ~(BURST_REGION - 1);
    int i;

    if (s->burst_open &&
        (now - s->last_write_ns > BURST_GAP_NS || base != s->burst_base)) {
        dmrram_burst_flush(s);
    }
    if (!s->burst_open) {
        s->burst_open = true;
        s->burst_base = base;
        s->burst_lo = DMRRAM_SIZE - 1;
        s->burst_hi = 0;
        s->burst_count = 0;
        s->burst_t0 = now;
    }
    for (i = 0; i < size; i++) {
        uint32_t o = (off + i) & (DMRRAM_SIZE - 1);

        s->burst_mask[o >> 3] |= (uint8_t) (1u << (o & 7));
        if (o < s->burst_lo) {
            s->burst_lo = o;
        }
        if (o > s->burst_hi) {
            s->burst_hi = o;
        }
    }
    s->burst_count++;
    s->last_write_ns = now;
}

static uint64_t dm32_dmrram_read(void *opaque, hwaddr off, unsigned size)
{
    DM32DmrRamState *s = opaque;
    uint64_t v = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        uint32_t o = (uint32_t) (off + i) & (DMRRAM_SIZE - 1);

        v |= (uint64_t) s->ram[o] << (8 * i);
    }
    if (s->log && dmrram_is_watched_read((uint32_t) off)) {
        fprintf(stderr, "DMRRAM R +0x%04x size=%u val=0x%llx (%s)\n",
                (unsigned) off, size, (unsigned long long) v,
                dmrram_region_name((uint32_t) off));
    }
    return v;
}

static void dm32_dmrram_write(void *opaque, hwaddr off, uint64_t val,
                              unsigned size)
{
    DM32DmrRamState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    unsigned i;

    for (i = 0; i < size; i++) {
        uint32_t o = (uint32_t) (off + i) & (DMRRAM_SIZE - 1);

        s->ram[o] = (uint8_t) (val >> (8 * i));
    }

    if (s->log) {
        fprintf(stderr, "DMRRAM W +0x%04x size=%u val=0x%llx (%s)\n",
                (unsigned) off, size,
                (unsigned long long) (val & (((uint64_t) 1 << (8 * size)) - 1)),
                dmrram_region_name((uint32_t) off));
        dmrram_burst_note(s, (uint32_t) off, size, now);
    }

    if (s->trace) {
        uint8_t rec[17];
        uint32_t v32 = (uint32_t) val;
        uint64_t t = (uint64_t) now;
        uint32_t o32 = (uint32_t) off;

        for (i = 0; i < 8; i++) {
            rec[i] = (uint8_t) (t >> (8 * i));
        }
        for (i = 0; i < 4; i++) {
            rec[8 + i] = (uint8_t) (o32 >> (8 * i));
        }
        rec[12] = (uint8_t) size;
        for (i = 0; i < 4; i++) {
            rec[13 + i] = (uint8_t) (v32 >> (8 * i));
        }
        fwrite(rec, 1, sizeof(rec), s->trace);
    }
}

static void dm32_dmrram_exit(Notifier *n, void *data)
{
    DM32DmrRamState *s = container_of(n, DM32DmrRamState, exit_notifier);

    if (s->log) {
        dmrram_burst_flush(s);
    }
    if (s->trace) {
        fflush(s->trace);
        fclose(s->trace);
        s->trace = NULL;
    }
    if (s->dump_path) {
        FILE *f = fopen(s->dump_path, "wb");

        if (f) {
            fwrite(s->ram, 1, DMRRAM_SIZE, f);
            fclose(f);
        } else {
            fprintf(stderr, "DMRRAM: cannot write DM32_DMR_DUMP=%s\n",
                    s->dump_path);
        }
    }
}

static const MemoryRegionOps dm32_dmrram_ops = {
    .read = dm32_dmrram_read,
    .write = dm32_dmrram_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

/*
 * No reset handler: the window is RAM, and a reset leaves its contents as
 * they were.
 */
static void dm32_dmrram_init(Object *obj)
{
    DM32DmrRamState *s = DM32_DMRRAM(obj);
    const char *e;

    memory_region_init_io(&s->iomem, obj, &dm32_dmrram_ops, s,
                          TYPE_DM32_DMRRAM, DMRRAM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    memset(s->ram, 0, sizeof(s->ram));

    s->log = dm32_env_flag("DM32_DMR_LOG");

    s->dump_path = getenv("DM32_DMR_DUMP");

    e = getenv("DM32_DMR_TRACE");
    if (e && *e) {
        s->trace = fopen(e, "wb");
        if (!s->trace) {
            fprintf(stderr, "DMRRAM: cannot open DM32_DMR_TRACE=%s\n", e);
        }
    }

    s->exit_notifier.notify = dm32_dmrram_exit;
    qemu_add_exit_notifier(&s->exit_notifier);
}

static const TypeInfo dm32_dmrram_type = {
    .name = TYPE_DM32_DMRRAM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DM32DmrRamState),
    .instance_init = dm32_dmrram_init,
};

static void dm32_dmrram_register(void)
{
    type_register_static(&dm32_dmrram_type);
}

type_init(dm32_dmrram_register)
