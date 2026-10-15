/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Baofeng DM-32UV watchdog: counting model
 *
 * Copyright (c) 2026 Alex Kaplan
 *
 * Models the HR_C7000 watchdog (WDG at 0x14010000, DS 4.10 p.212-218) as a
 * counter. The model counts so that firmware which stops feeding the watchdog
 * is reset, as on the radio. A plain RAM stub at this address would never
 * expire. The register layout is transcribed from
 * dm32uv-re data/registers/HR_C7000-DM32.h (WDG_Type) and the field
 * descriptions in dm32uv-re data/registers/HR_C7000-DM32.svd (peripheral WDG):
 *
 *   0x00 WDG_LOCK      RW. Write 0x5ada7200 to unlock all writable registers;
 *                      ANY other value re-locks them (SVD: "writing other
 *                      values to this register will disable write access to
 *                      all registers"). Read returns 0x0 = unlocked, 0x1 =
 *                      locked. Reset 0x1 (locked). The LOCK register itself
 *                      is always writable.
 *   0x04 WDG_OT_LOAD_H RW, 3 bits. High part of the timeout value. Reset 0x7.
 *   0x08 WDG_OT_LOAD_L RO per SVD/DS (the DS also says H is RW while L is RO;
 *                      the SVD calls this "possibly a datasheet error, not
 *                      verified"). Reset 0xc0000000. Writes are dropped with
 *                      a stderr warning (strict default).
 *   0x0c WDG_RST_LOAD  RW, 25 bits. Reset-pulse initial value; reset
 *                      0x03f80000 (the SVD field description's "[reset
 *                      0x03f80000]"; the SVD register has no resetValue).
 *   0x10 WDG_EN        RW, bit 0. 0 = off (no reset generated), 1 = counting.
 *   0x14 WDG_KNOCK     RW. Writing 0x55aadd22 feeds the dog (restarts the
 *                      count). Reset 0x0.
 *
 * Offsets 0x18..0xfff of the 4 KB window are plain storage (read/write, not
 * reset).
 *
 * Counting semantics. What the SVD and the header give, and what is assumed:
 *  - Timeout: the counter counts up "from the initial value WD_TIMER to the
 *    overflow value" (v2.7 EN 4.10.3, p.215), and "when the high counter
 *    overflows, it indicates Timeout" (OT_LOAD_H/L, p.216-217). The initial
 *    value is the 35-bit OT_LOAD_H:OT_LOAD_L, so the timeout is
 *    2^35 - (H:L) ticks. With the reset values (H = 7, L = 0xc0000000) that
 *    is 2^30 ticks, which the datasheet calls "about 60s".
 *  - Clock: the datasheet names only "a system clock" (p.214). Its two
 *    figures disagree: 2^30 ticks in about 60 s is about 17.9 MHz, and the
 *    reset pulse (below, 2^19 ticks in about 20 ms) is about 26 MHz.
 *    Assumption: the default is WDT_CLOCK_HZ, chosen so that the reset-value
 *    timeout is the datasheet's 60 s. DM32_WDT_HZ overrides it.
 *  - The count is armed when EN goes 0 -> 1 and when KNOCK receives the feed
 *    key while EN is 1 ("After knocking the door, the internal counter starts
 *    counting from WD_TIMER again", p.216). Both start a full period from the
 *    current H:L. It stops when EN goes 1 -> 0. After a disable "the initial
 *    count value of the watchdog and other control words are not changed"
 *    (p.214), and an enable starts the count from that initial value
 *    (procedure, p.215), so a re-enable starts a fresh full period.
 *  - Expiry: the device logs "WATCHDOG BITE at vtime=<ns>" and holds the
 *    machine in reset for the reset pulse: the counter counts from RST_LOAD to
 *    0x03ffffff, "about 20ms" with the reset value (p.218), so the pulse is
 *    0x04000000 - RST_LOAD ticks. The vCPUs are paused for the pulse (from a
 *    bottom half, so a few instructions may run first), then a guest reset is
 *    requested (qemu_system_reset_request) and they resume.
 *    Assumption: the whole chip is reset; the datasheet says the reset extends
 *    the physical reset (p.213) but not which blocks it reaches. With -icount
 *    the vCPUs are not paused, and the guest keeps running for the pulse.
 *  - A non-magic KNOCK value is stored and does not feed. The SVD is silent on
 *    whether a wrong key does anything else; this model does nothing else
 *    (strict default).
 *
 * Locking: when locked, every write except to WDG_LOCK is dropped, and a
 * warning is printed to stderr. A write that the hardware would ignore is
 * never applied here.
 *
 * Environment variables (both read once at instance init):
 *   DM32_WDT_LOG=1       log every register access to stderr
 *                        ("[dm32-wdt] R|W ...").
 *   DM32_WDT_NORESET=1   a bite is logged but does not reset the guest; the
 *                        count is re-armed so later bites are also logged.
 *                        For observing a bite without a reboot.
 *   DM32_WDT_HZ=<hz>     the counter clock, in Hz, from 4 to 4294967295
 *                        (default WDT_CLOCK_HZ). Anything else is a fatal
 *                        error at start-up.
 *
 * Not modelled: the reset-output line, and the 510-clock reset extension of an
 * external reset (HR_C7000 v2.7 p.213). Interrupts: none; the datasheet's
 * interrupt table lists no watchdog source.
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "system/runstate.h"
#include "system/cpus.h"
#include "exec/icount.h"
#include "qemu/main-loop.h"
#include "qemu/host-utils.h"
#include "hw/csky/dm32.h"

#define TYPE_DM32_WDT "dm32_wdt"
OBJECT_DECLARE_SIMPLE_TYPE(DM32WdtState, DM32_WDT)

#define WDT_WIN          0x1000u
/* The register block is 0x00..0x17; offsets above it are storage. */
#define WDT_REG_END      0x18u

#define R_LOCK           0x00u
#define R_OT_LOAD_H      0x04u
#define R_OT_LOAD_L      0x08u
#define R_RST_LOAD       0x0cu
#define R_EN             0x10u
#define R_KNOCK          0x14u

#define LOCK_UNLOCK_KEY  0x5ada7200u
#define KNOCK_FEED_KEY   0x55aadd22u

#define OT_LOAD_H_RESET  0x7u
#define OT_LOAD_L_RESET  0xc0000000u
#define RST_LOAD_RESET   0x03f80000u
#define RST_LOAD_MASK    0x01ffffffu    /* 25 bits */
#define OT_LOAD_H_MASK   0x7u           /* 3 bits */

/*
 * Counter clock: 2^30 ticks (the reset-value timeout) in the datasheet's
 * "about 60s". This is an assumption; see the header.
 */
#define WDT_CLOCK_HZ     17895697ull
/* The 35-bit timeout counter overflows at 2^35; the reset counter at 2^26. */
#define OT_OVERFLOW      (1ull << 35)
#define RST_OVERFLOW     0x04000000u

struct DM32WdtState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    QEMUTimer *expiry;
    /* end of the reset pulse; pending while the machine is held in reset */
    QEMUTimer *pulse;
    QEMUBH *hold_bh;
    bool vcpus_paused;
    uint64_t clock_hz;
    bool unlocked;
    bool log;
    bool noreset;
    uint32_t ot_load_h;
    uint32_t ot_load_l;
    uint32_t rst_load;
    uint32_t en;
    uint32_t knock;
    /* offsets 0x18..0xfff: plain storage */
    uint8_t store[WDT_WIN - WDT_REG_END];
};

static int64_t dm32_wdt_ticks_ns(DM32WdtState *s, uint64_t ticks)
{
    return (int64_t)muldiv64(ticks, NANOSECONDS_PER_SECOND, s->clock_hz);
}

/* Timeout: the count up from the H:L load value to the 35-bit overflow. */
static int64_t dm32_wdt_timeout_ns(DM32WdtState *s)
{
    uint64_t load = ((uint64_t)(s->ot_load_h & OT_LOAD_H_MASK) << 32) |
                    s->ot_load_l;

    return dm32_wdt_ticks_ns(s, OT_OVERFLOW - load);
}

/*
 * Reset pulse: the count up from RST_LOAD to 0x03ffffff. The datasheet gives
 * the field as [24:0] (p.218), but its reset value 0x03f80000 and its end
 * value 0x03ffffff need bit 25. The reset value is kept as documented and a
 * write is masked to 25 bits, so a written value always gives a pulse of at
 * least 2^25 ticks.
 */
static int64_t dm32_wdt_pulse_ns(DM32WdtState *s)
{
    return dm32_wdt_ticks_ns(s, RST_OVERFLOW - s->rst_load);
}

static void dm32_wdt_arm(DM32WdtState *s)
{
    timer_mod(s->expiry, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                         dm32_wdt_timeout_ns(s));
}

static void dm32_wdt_disarm(DM32WdtState *s)
{
    timer_del(s->expiry);
}

static void dm32_wdt_expire(void *opaque)
{
    DM32WdtState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    fprintf(stderr, "[dm32-wdt] WATCHDOG BITE at vtime=%" PRId64 " ns%s\n",
            now,
            s->noreset ? " (DM32_WDT_NORESET: log only, not resetting)" : "");
    if (s->noreset) {
        /* Keep counting so that later bites are logged too. */
        dm32_wdt_arm(s);
        return;
    }
    /* Hold the machine in reset for the pulse, then reset it. */
    timer_mod(s->pulse, now + dm32_wdt_pulse_ns(s));
    qemu_bh_schedule(s->hold_bh);
}

/*
 * Pause the vCPUs for the rest of the pulse. This runs as a bottom half
 * because pause_all_vcpus() waits for running virtual-clock timers to finish,
 * so it cannot be called from dm32_wdt_expire(). It also stops those timers,
 * which would keep the pulse from ever ending, so they are turned back on;
 * the clock itself keeps running while the VM does. With -icount the clock
 * advances only while a vCPU runs, so the vCPUs are not paused and the guest
 * keeps running for the pulse.
 */
static void dm32_wdt_hold(void *opaque)
{
    DM32WdtState *s = opaque;

    if (!timer_pending(s->pulse) || s->vcpus_paused || icount_enabled()) {
        return;
    }
    pause_all_vcpus();
    qemu_clock_enable(QEMU_CLOCK_VIRTUAL, true);
    s->vcpus_paused = true;
}

static void dm32_wdt_release(DM32WdtState *s)
{
    if (s->vcpus_paused) {
        s->vcpus_paused = false;
        resume_all_vcpus();
    }
}

static void dm32_wdt_pulse_end(void *opaque)
{
    DM32WdtState *s = opaque;

    if (s->log) {
        fprintf(stderr, "[dm32-wdt] reset pulse ends at vtime=%" PRId64
                " ns\n", qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
    /* The reset request is handled by the main loop before vCPUs run on. */
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    dm32_wdt_release(s);
}

static void dm32_wdt_warn_locked(const char *what, hwaddr off, uint64_t val)
{
    fprintf(stderr, "[dm32-wdt] WARNING: write to %s (off 0x%" HWADDR_PRIx
            " = 0x%" PRIx64 ") dropped: watchdog is locked "
            "(WDG_LOCK != 0x5ada7200)\n", what, off, val);
}

static uint64_t dm32_wdt_read(void *opaque, hwaddr off, unsigned size)
{
    DM32WdtState *s = opaque;
    uint64_t v = 0;

    if (off >= WDT_REG_END) {
        for (unsigned i = 0; i < size && off + i < WDT_WIN; i++) {
            v |= (uint64_t)s->store[off - WDT_REG_END + i] << (8 * i);
        }
    } else {
        switch (off) {
        case R_LOCK:
            v = s->unlocked ? 0x0 : 0x1;
            break;
        case R_OT_LOAD_H:
            v = s->ot_load_h;
            break;
        case R_OT_LOAD_L:
            v = s->ot_load_l;
            break;
        case R_RST_LOAD:
            v = s->rst_load;
            break;
        case R_EN:
            v = s->en;
            break;
        case R_KNOCK:
            v = s->knock;
            break;
        default:
            v = 0;
            break;
        }
    }
    if (s->log) {
        fprintf(stderr, "[dm32-wdt] R off=0x%03" HWADDR_PRIx
                " size=%u -> 0x%08" PRIx64 "\n", off, size, v);
    }
    return v;
}

static void dm32_wdt_write(void *opaque, hwaddr off, uint64_t val,
                           unsigned size)
{
    DM32WdtState *s = opaque;

    if (s->log) {
        fprintf(stderr, "[dm32-wdt] W off=0x%03" HWADDR_PRIx
                " size=%u val=0x%08" PRIx64 "%s\n",
                off, size, val,
                (off < WDT_REG_END && off != R_LOCK && !s->unlocked)
                ? " (locked)" : "");
    }

    if (off >= WDT_REG_END) {
        for (unsigned i = 0; i < size && off + i < WDT_WIN; i++) {
            s->store[off - WDT_REG_END + i] = (val >> (8 * i)) & 0xff;
        }
        return;
    }

    if (off == R_LOCK) {
        /* Always processed: the magic unlocks, any other value re-locks. */
        s->unlocked = ((uint32_t)val == LOCK_UNLOCK_KEY);
        return;
    }

    if (!s->unlocked) {
        dm32_event(DM32_EV_WDT_LOCKED, "write +0x%02" HWADDR_PRIx " = 0x%"
                   PRIx64 " while locked", off, val);
        dm32_wdt_warn_locked("register", off, val);
        return;
    }

    switch (off) {
    case R_OT_LOAD_H:
        /* latched; used at next arm or feed */
        s->ot_load_h = val & OT_LOAD_H_MASK;
        break;
    case R_OT_LOAD_L:
        fprintf(stderr, "[dm32-wdt] WARNING: write to read-only "
                "WDG_OT_LOAD_L (0x%" PRIx64 ") dropped (SVD: RO)\n", val);
        break;
    case R_RST_LOAD:
        s->rst_load = val & RST_LOAD_MASK;
        break;
    case R_EN: {
        uint32_t old = s->en;

        s->en = val & 0x1;
        if (s->en && !old) {
            dm32_wdt_arm(s);
        } else if (!s->en && old) {
            dm32_wdt_disarm(s);
        }
        break;
    }
    case R_KNOCK:
        s->knock = val;
        if ((uint32_t)val == KNOCK_FEED_KEY && s->en) {
            /* feed: restart the count */
            dm32_wdt_arm(s);
        }
        break;
    default:
        /* 0x18 and above are handled above; gaps are read-as-zero, ignored */
        break;
    }
}

static const MemoryRegionOps dm32_wdt_ops = {
    .read = dm32_wdt_read,
    .write = dm32_wdt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void dm32_wdt_reset(DeviceState *dev)
{
    DM32WdtState *s = DM32_WDT(dev);

    /*
     * Power-on and reset state (SVD resetValues). Storage (offsets >= 0x18)
     * is not cleared.
     */
    dm32_wdt_disarm(s);
    /* A reset from elsewhere during the pulse ends it. */
    timer_del(s->pulse);
    dm32_wdt_release(s);
    /* WDG_LOCK reset 0x1 = locked */
    s->unlocked = false;
    s->ot_load_h = OT_LOAD_H_RESET;
    s->ot_load_l = OT_LOAD_L_RESET;
    s->rst_load = RST_LOAD_RESET;
    s->en = 0;
    s->knock = 0;
}

static void dm32_wdt_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, dm32_wdt_reset);
}

static void dm32_wdt_init(Object *obj)
{
    DM32WdtState *s = DM32_WDT(obj);

    memory_region_init_io(&s->iomem, obj, &dm32_wdt_ops, s, TYPE_DM32_WDT,
                          WDT_WIN);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    s->expiry = timer_new_ns(QEMU_CLOCK_VIRTUAL, dm32_wdt_expire, s);
    s->pulse = timer_new_ns(QEMU_CLOCK_VIRTUAL, dm32_wdt_pulse_end, s);
    s->hold_bh = qemu_bh_new(dm32_wdt_hold, s);
    s->vcpus_paused = false;
    s->log = dm32_env_flag("DM32_WDT_LOG");
    s->noreset = dm32_env_flag("DM32_WDT_NORESET");
    /*
     * The count is 35 bits at the clock, so at 1..3 Hz the deadline in ns
     * overflows int64_t; 4 Hz is the lowest clock that does not. muldiv64()
     * takes the divisor as uint32_t.
     */
    s->clock_hz = dm32_env_uint("DM32_WDT_HZ", WDT_CLOCK_HZ, 4, UINT32_MAX);
    dm32_wdt_reset(DEVICE(obj));
}

static const TypeInfo dm32_wdt_type = {
    .name = TYPE_DM32_WDT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DM32WdtState),
    .instance_init = dm32_wdt_init,
    .class_init = dm32_wdt_class_init,
};

static void dm32_wdt_register(void)
{
    type_register_static(&dm32_wdt_type);
}

type_init(dm32_wdt_register)
