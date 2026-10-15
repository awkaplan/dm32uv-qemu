/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Baofeng DM-32UV timer (DesignWare APB timer): six-channel dw_apb_timer
 * model
 *
 * Copyright (c) 2026 Alex Kaplan
 *
 * Layout confirmed by live probing of a DM-32UV and by HR_C7000 v2.7
 * section 4.8:
 *   per channel, 0x14-byte stride: LoadCount(+0), CurrentValue(+4),
 *     ControlReg(+8), EOI(+0xc, read-to-clear), IntStatus(+0x10)
 *   ControlReg bits: [0]=enable, [1]=mode (1=user/periodic),
 *     [2]=interrupt MASK (1=masked)
 *   global: TimersIntStatus(0xa0), TimersEOI(0xa4, read-to-clear-all),
 *     TimersRawIntStatus(0xa8)
 *
 * Channel n drives PIC source n+1. The RTOS tick is channel 0
 * (enable, periodic, unmasked). The stock UI tick is channel 5. The
 * counter counts DOWN (v2.7 EN 4.8.3, p.188); on reaching 0 it raises the
 * channel interrupt, held until an EOI, and reloads.
 *
 * Modes. The datasheet lists "free running and user-defined counting"
 * (p.188) but does not say which TimerMode value is which or what each
 * reloads. The model follows the DesignWare DW_apb_timers convention the
 * block is built from, and the SVD's "1 = user/periodic": an enable (0 to 1)
 * loads LoadCount in either mode; at 0, mode 1 (user-defined) reloads
 * LoadCount and mode 0 (free-running) reloads 0xffffffff. Stock writes mode
 * 1 for every channel it uses (timer_ch0_configure_periodic @0x0306cef4,
 * hw_timer_counter_to_time_components @0x0301d860).
 *
 * LoadCount written while a channel runs takes effect at the next reload,
 * not at once (DesignWare convention; the datasheet only says the counter
 * reloads on enable and on reaching 0). Stock relies on this when it
 * re-periods a running channel without disabling it first
 * (mmio_14000000_timer_config @0x0301d900). A LoadCount of 0 is treated as
 * 0xffffffff (assumption; the datasheet is silent). CurrentValue is read-only
 * (SVD; the datasheet prints RW); stock's writes of 0 to it before an enable
 * are ignored. A disabled channel keeps its last count.
 *
 * Reset: every channel disabled, unmasked, LoadCount 0, nothing pending
 * (SVD reset values). The datasheet documents no prescaler and no one-shot
 * mode. TIMERS_COMP_VERSION reads 0x3230322a (p.200).
 *
 * Port note: this model uses the modern ptimer API. ptimer_init() takes
 * the tick callback directly, with no QEMUBH wrapper. Every state-changing
 * ptimer call (set_limit, set_freq, run, stop, set_count) must be inside
 * ptimer_transaction_begin() and ptimer_transaction_commit(), and
 * ptimer_init() asserts this at runtime. The model uses PTIMER_POLICY_LEGACY,
 * the default policy: reaching zero triggers the tick and reloads the counter
 * with the limit, so each channel counts down and reloads. The rough edges
 * that ptimer.h lists for this policy apply to the model. Bit-exact timing of
 * DW-APB silicon is not a goal. Matching what the stock firmware observes is.
 * See docs/port-notes.md.
 *
 * Not modelled: bit-exact DW-APB timing (see above); the clock tree, since the
 * 84 MHz clock is a constant rather than taken from the clock manager; and a
 * prescaler or a one-shot mode, which the datasheet does not document.
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/ptimer.h"
#include "qemu/main-loop.h"
#include "qemu/log.h"
#include "hw/csky/dm32.h"

/* Six channels, Timer1..6 at a 0x14 stride (HR_C7000 v2.7 EN 4.8). */
#define DM32_TIMER_NCH 6
#define CH_STRIDE     0x14

/* Per-channel register offsets, named as in the DesignWare APB timer. */
#define CH_LOAD_COUNT     0x0
#define CH_CURRENT_VALUE  0x4
#define CH_CONTROL_REG    0x8
#define CH_EOI            0xc         /* read or write clears the channel */
#define CH_INT_STATUS     0x10

/* Global register offsets; the global block starts at TIMERS_GLOBAL_BASE. */
#define TIMERS_GLOBAL_BASE       0xa0
#define TIMERS_INT_STATUS        0xa0
#define TIMERS_EOI               0xa4
#define TIMERS_RAW_INT_STATUS    0xa8
#define TIMERS_COMP_VERSION_REG  0xac

#define CTRL_ENABLE  BIT(0)
#define CTRL_MODE    BIT(1)     /* 1 = user-defined/periodic */
#define CTRL_MASK    BIT(2)     /* 1 = interrupt masked */

#define TYPE_DM32_TIMER "dm32_timer"
OBJECT_DECLARE_SIMPLE_TYPE(DM32TimerState, DM32_TIMER)

typedef struct DM32TimerChan {
    ptimer_state *ptimer;
    uint32_t load;
    uint32_t control;
    int int_level;            /* raw pending */
    qemu_irq irq;
} DM32TimerChan;

struct DM32TimerState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    DM32TimerChan ch[DM32_TIMER_NCH];
    uint32_t freq;
};

static void dm32_timer_update(DM32TimerChan *c)
{
    /* The IRQ is asserted when an interrupt is pending and not masked. */
    int masked = c->control & CTRL_MASK;

    qemu_set_irq(c->irq, c->int_level && !masked);
}

/* The value the counter takes when it reaches 0 (see the header). */
static uint32_t dm32_timer_reload_value(DM32TimerChan *c)
{
    if (!(c->control & CTRL_MODE)) {
        return 0xffffffff;
    }
    return c->load ? c->load : 0xffffffff;
}

static void dm32_timer_tick(void *opaque)
{
    DM32TimerChan *c = opaque;
    uint32_t reload = dm32_timer_reload_value(c);

    /*
     * The ptimer has just reloaded its limit; when the mode or LoadCount
     * says otherwise, restart this period from the right value. The ptimer
     * runs this callback inside its own transaction.
     */
    if (ptimer_get_limit(c->ptimer) != reload) {
        ptimer_set_limit(c->ptimer, reload, 1);
    }

    /*
     * Assert and hold. The level is cleared only by a channel or global
     * EOI. There is no pulse, so the CPU cannot miss a transient.
     */
    c->int_level = 1;
    dm32_timer_update(c);
}

static uint64_t dm32_timer_read(void *opaque, hwaddr offset, unsigned size)
{
    DM32TimerState *s = opaque;
    if (offset >= TIMERS_GLOBAL_BASE) {
        int i;
        uint32_t v = 0;
        switch (offset) {
        case TIMERS_INT_STATUS:
            /* TimersIntStatus (masked). */
            for (i = 0; i < DM32_TIMER_NCH; i++) {
                if (s->ch[i].int_level && !(s->ch[i].control & CTRL_MASK)) {
                    v |= (1u << i);
                }
            }
            return v;
        case TIMERS_RAW_INT_STATUS:
            /* TimersRawIntStatus (unmasked). */
            for (i = 0; i < DM32_TIMER_NCH; i++) {
                if (s->ch[i].int_level) {
                    v |= (1u << i);
                }
            }
            return v;
        case TIMERS_EOI:
            /* TimersEOI: a read clears all channels. */
            for (i = 0; i < DM32_TIMER_NCH; i++) {
                s->ch[i].int_level = 0;
                dm32_timer_update(&s->ch[i]);
            }
            return 0;
        case TIMERS_COMP_VERSION_REG:
            return 0x3230322a; /* TIMERS_COMP_VERSION, v2.7 EN p.200 */
        default:
            return 0;
        }
    }
    int n = offset / CH_STRIDE;
    if (n >= DM32_TIMER_NCH) {
        return 0;
    }
    DM32TimerChan *c = &s->ch[n];
    switch (offset % CH_STRIDE) {
    case CH_LOAD_COUNT:
        return c->load;
    case CH_CURRENT_VALUE:
        return ptimer_get_count(c->ptimer);
    case CH_CONTROL_REG:
        return c->control;
    case CH_EOI:
        /* EOI: a read clears this channel's interrupt. */
        c->int_level = 0;
        dm32_timer_update(c);
        return 0;
    case CH_INT_STATUS:
        return (c->int_level && !(c->control & CTRL_MASK)) ? 1 : 0;
    default:
        return 0;
    }
}

static void dm32_timer_write(void *opaque, hwaddr offset, uint64_t val,
                             unsigned size)
{
    DM32TimerState *s = opaque;

    /* The global registers are read-only or read-to-clear. */
    if (offset >= TIMERS_GLOBAL_BASE) {
        return;
    }
    int n = offset / CH_STRIDE;
    if (n >= DM32_TIMER_NCH) {
        return;
    }
    DM32TimerChan *c = &s->ch[n];
    switch (offset % CH_STRIDE) {
    case CH_LOAD_COUNT:
        /*
         * LoadCount: loaded at the next enable, or, on a running channel in
         * user-defined mode, at the next reload (dm32_timer_tick).
         */
        c->load = val;
        break;
    case CH_CONTROL_REG: {
        /* ControlReg. */
        uint32_t old = c->control;
        c->control = val;
        if ((val & CTRL_ENABLE) && !(old & CTRL_ENABLE)) {
            ptimer_transaction_begin(c->ptimer);
            /* An enable loads LoadCount in either mode. */
            ptimer_set_limit(c->ptimer, c->load ? c->load : 0xffffffff, 1);
            ptimer_run(c->ptimer, 0);   /* 0 = periodic reload */
            ptimer_transaction_commit(c->ptimer);
        } else if (!(val & CTRL_ENABLE) && (old & CTRL_ENABLE)) {
            ptimer_transaction_begin(c->ptimer);
            ptimer_stop(c->ptimer);
            ptimer_transaction_commit(c->ptimer);
        }
        dm32_timer_update(c);
        break;
    }
    case CH_EOI:
        /* EOI: a write also clears the interrupt. */
        c->int_level = 0;
        dm32_timer_update(c);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps dm32_timer_ops = {
    .read = dm32_timer_read,
    .write = dm32_timer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void dm32_timer_reset(DeviceState *dev)
{
    DM32TimerState *s = DM32_TIMER(dev);
    int i;

    for (i = 0; i < DM32_TIMER_NCH; i++) {
        DM32TimerChan *c = &s->ch[i];

        ptimer_transaction_begin(c->ptimer);
        ptimer_stop(c->ptimer);
        ptimer_set_limit(c->ptimer, 0xffffffff, 1);
        ptimer_transaction_commit(c->ptimer);
        c->load = 0;
        c->control = 0;
        c->int_level = 0;
        dm32_timer_update(c);
    }
}

static void dm32_timer_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), dm32_timer_reset);
}

static void dm32_timer_init(Object *obj)
{
    DM32TimerState *s = DM32_TIMER(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    int i;


    /*
     * The APB timer clock is 84 MHz. Three channels loaded with LoadCount
     * 840000, 336000 and 1680000 give exactly 10.000, 4.000 and 20.000 ms
     * at 84 MHz, and non-round values at every other candidate clock. A
     * reload sized for 8 MHz would look right in emulation at an 8 MHz
     * clock, but would run 10.5 times too fast on hardware.
     */
    s->freq = DM32_APB_HZ;
    for (i = 0; i < DM32_TIMER_NCH; i++) {
        s->ch[i].ptimer = ptimer_init(dm32_timer_tick, &s->ch[i],
                                      PTIMER_POLICY_LEGACY);
        ptimer_transaction_begin(s->ch[i].ptimer);
        ptimer_set_freq(s->ch[i].ptimer, s->freq);
        ptimer_transaction_commit(s->ch[i].ptimer);
        sysbus_init_irq(sbd, &s->ch[i].irq);
    }
    memory_region_init_io(&s->iomem, obj, &dm32_timer_ops, s,
                          TYPE_DM32_TIMER, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);

    dm32_timer_reset(DEVICE(obj));
}

static const TypeInfo dm32_timer_type = {
    .name = TYPE_DM32_TIMER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DM32TimerState),
    .instance_init = dm32_timer_init,
    .class_init = dm32_timer_class_init,
};

static void dm32_timer_register(void)
{
    type_register_static(&dm32_timer_type);
}

type_init(dm32_timer_register)
