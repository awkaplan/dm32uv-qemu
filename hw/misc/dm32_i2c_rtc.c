/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Baofeng DM-32UV real-time clock (I2C slave at 0x70): behavioural model.
 *
 * Copyright (c) 2026 Alex Kaplan
 *
 * HR_C7000 v2.7 EN section 4.9 (p.200-212). The RTC sits on the internal
 * I2C2 bus at 7-bit address 0x70 ("Device address 7'b1110000", 4.9.3.1); it
 * is not memory-mapped. It counts a 32.768 kHz crystal divided down to 1 Hz
 * and keeps running on a coin cell while the chip is off (4.9.2). The stock
 * read path rtc_register_read @0x0301c2d4 sends IC_TAR 0xe0 >> 1 = 0x70, one
 * index byte and one read command.
 *
 * Registers (Register Table 7, 4.9.5), one byte each, reset 0 unless noted:
 *   0x00-0x04 VAL_S [5:0], VAL_M [5:0], VAL_H [4:0], VAL_D_L, VAL_D_H: the
 *             count, read-only
 *   0x05-0x09 MATCH_S/M/H/D_L/D_H, the same widths: alarm time
 *   0x0a      MATCH_VEN [0]: 1 copies MATCH_* into the internal match
 *             register, then clears itself (4.9.3.1 step 6)
 *   0x0b      MATCH_VSTAT [0]: software's "match configured" flag
 *   0x0c-0x10 LOAD_S/M/H/D_L/D_H, the same widths: initial time
 *   0x11      CCR: [2] load (count <- LOAD_*, clears itself), [1] enable
 *             (count), [0] intr_en
 *   0x12      LOAD_VSTAT [0]: software's "load configured" flag
 *   0x13      RSTAT [0]: interrupt occurred; read clears
 *   0x14-0x15 CLKDIV_NUMER_L/H: divider t; the count advances at
 *             32768 / t Hz
 *   0x16      CLKDIV_NUMER_VLD [0]: 1 uses t, 0 the default 32768 (1 Hz)
 *   0x17      SOFT_RST_N [0], reset 1: 0 holds the RTC in soft reset
 *
 * Values are binary. The datasheet gives 6-bit second and minute registers,
 * a 5-bit hour register and a 16-bit day register that count "when it
 * reaches 60 ... the second register is reset to zero" (4.9.2), and never
 * BCD; stock uses the five bytes it reads as plain binary numbers and counts
 * the day from 1970-01-01 (rtc_decode_calendar_date @0x0301c2f0). The count
 * rolls over at 60 seconds, 60 minutes, 24 hours and
 * 65536 days. Assumption: a LOAD field beyond its range carries into the next
 * field (LOAD_S 70 is 1 minute 10 seconds); the datasheet does not say.
 *
 * Alarm. When the count reaches the internal match value by counting, and
 * CCR[0] is set, RSTAT becomes 1 (4.9.3.1 step 10) and the interrupt line,
 * PIC source 19 (Table 30, level, high), stays high until RSTAT is read
 * (4.9.3.2 step 1). Assumptions: "reaches" is equality on a count step, so a
 * LOAD onto the match value does not fire, and the line follows RSTAT.
 *
 * Time base. The count advances on QEMU's RTC clock (-rtc clock=...): host
 * time by default, virtual time with -rtc clock=vm, which makes runs
 * reproducible. A divider change, a stop and a load restart the current
 * tick. Assumption: t = 0 does not count.
 *
 * Soft reset. SOFT_RST_N = 0 returns every other register and the count to
 * their reset values and holds them there: writes are ignored until
 * SOFT_RST_N = 1 (4.9.3.1 steps 3-5; assumption: the datasheet does not list
 * what the soft reset clears). The 25 ms release wait is not modelled.
 *
 * Power-up. The registers start at their reset values: count 0 (00:00:00,
 * day 0), stopped, as on a first power-up with a fresh coin cell. Stock then
 * finds LOAD_VSTAT clear, soft-resets the RTC, starts it and loads its
 * default time (rtc_validate_and_set_default_datetime @0x0301c608). SoC resets
 * do not reach the RTC, which keeps counting (assumption, from the coin-cell
 * domain of 4.9.2; RTC_REQ_HOLD at system control +0x58 is its output clamp).
 *
 * Bus protocol: the first byte of a write sets the register pointer, and
 * each further byte writes the register it points at. A read returns the
 * register at the pointer. The pointer advances after every data byte.
 * Assumption: the pointer auto-increments, and VAL_* are latched when a read
 * starts, so a multi-byte read is one consistent instant. The datasheet
 * gives the register map but not the bus protocol; stock always writes the
 * pointer before a read.
 *
 * Not modelled: the 25 ms soft-reset release time, and power failure (the
 * VSTAT guard flags have no effect).
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/i2c/i2c.h"
#include "system/system.h"
#include "hw/csky/dm32.h"

#define TYPE_DM32_I2C_RTC "dm32_i2c_rtc"
OBJECT_DECLARE_SIMPLE_TYPE(DM32I2cRtcState, DM32_I2C_RTC)

#define RTC_ADDR7       0x70
#define RTC_NREGS       0x18

#define R_VAL_S         0x00
#define R_MATCH_S       0x05
#define R_MATCH_VEN     0x0a
#define R_MATCH_VSTAT   0x0b
#define R_LOAD_S        0x0c
#define R_CCR           0x11
#define R_LOAD_VSTAT    0x12
#define R_RSTAT         0x13
#define R_NUMER_L       0x14
#define R_NUMER_H       0x15
#define R_NUMER_VLD     0x16
#define R_SOFT_RST_N    0x17

#define CCR_INTR_EN     (1u << 0)
#define CCR_ENABLE      (1u << 1)
#define CCR_LOAD        (1u << 2)

#define RTC_XTAL_HZ     32768
#define RTC_PERIOD_S    (65536ull * 86400)      /* the 16-bit day count */

/* Register widths (4.9.5): seconds and minutes 6 bits, hours 5. */
static const uint8_t rtc_mask[RTC_NREGS] = {
    0x3f, 0x3f, 0x1f, 0xff, 0xff,               /* VAL */
    0x3f, 0x3f, 0x1f, 0xff, 0xff, 0x01, 0x01,   /* MATCH, VEN, VSTAT */
    0x3f, 0x3f, 0x1f, 0xff, 0xff,               /* LOAD */
    0x07, 0x01, 0x01, 0xff, 0xff, 0x01, 0x01,   /* CCR .. SOFT_RST_N */
};

struct DM32I2cRtcState {
    I2CSlave parent_obj;
    qemu_irq irq;
    QEMUTimer *match_timer;

    uint8_t regs[RTC_NREGS];
    uint8_t ptr;                /* register pointer */
    bool first;                 /* next written byte is the pointer */
    uint8_t latch[5];           /* VAL_* at the start of a read */

    uint64_t count;             /* seconds count at ref_ns */
    int64_t ref_ns;             /* rtc_clock time of the current tick start */
    uint64_t match;             /* internal match register */
    bool log;
};

static int64_t rtc_tick_ns(DM32I2cRtcState *s)
{
    uint32_t t = RTC_XTAL_HZ;

    if (s->regs[R_NUMER_VLD] & 1) {
        t = s->regs[R_NUMER_L] | (s->regs[R_NUMER_H] << 8);
    }
    return muldiv64(t, NANOSECONDS_PER_SECOND, RTC_XTAL_HZ);
}

static bool rtc_running(DM32I2cRtcState *s)
{
    return (s->regs[R_SOFT_RST_N] & 1) && (s->regs[R_CCR] & CCR_ENABLE) &&
           rtc_tick_ns(s) > 0;
}

static uint64_t rtc_now(DM32I2cRtcState *s)
{
    int64_t tick = rtc_tick_ns(s);
    uint64_t ticks;

    if (!rtc_running(s)) {
        return s->count;
    }
    ticks = (qemu_clock_get_ns(rtc_clock) - s->ref_ns) / tick;
    return (s->count + ticks) % RTC_PERIOD_S;
}

/* Fold elapsed ticks into count and start a new tick now. */
static void rtc_rebase(DM32I2cRtcState *s)
{
    s->count = rtc_now(s);
    s->ref_ns = qemu_clock_get_ns(rtc_clock);
}

static uint64_t rtc_fields(const uint8_t *r)
{
    return r[0] + 60ull * r[1] + 3600ull * r[2] +
           86400ull * (r[3] | (r[4] << 8));
}

static void rtc_update_irq(DM32I2cRtcState *s)
{
    qemu_set_irq(s->irq, s->regs[R_RSTAT] & 1);
}

/* Arm the timer for the next step onto the match value. */
static void rtc_schedule(DM32I2cRtcState *s)
{
    int64_t tick = rtc_tick_ns(s);
    int64_t now, elapsed;
    uint64_t cur, ahead;

    timer_del(s->match_timer);
    if (!rtc_running(s)) {
        return;
    }
    now = qemu_clock_get_ns(rtc_clock);
    elapsed = (now - s->ref_ns) / tick;
    cur = (s->count + elapsed) % RTC_PERIOD_S;
    ahead = (s->match + RTC_PERIOD_S - cur) % RTC_PERIOD_S;
    if (ahead == 0) {
        ahead = RTC_PERIOD_S;
    }
    /* Beyond about 146 years of virtual time the alarm is not scheduled. */
    if (ahead + elapsed > INT64_MAX / 4 / tick) {
        return;
    }
    timer_mod(s->match_timer, s->ref_ns + (elapsed + ahead) * tick);
}

static void rtc_match_fire(void *opaque)
{
    DM32I2cRtcState *s = opaque;

    if (s->regs[R_CCR] & CCR_INTR_EN) {
        s->regs[R_RSTAT] = 1;
        rtc_update_irq(s);
        if (s->log) {
            fprintf(stderr, "dm32_rtc(i2c 0x70): match at count %" PRIu64
                    ", interrupt\n", s->match);
        }
    }
    rtc_rebase(s);
    rtc_schedule(s);
}

static void rtc_fill_val(DM32I2cRtcState *s, uint8_t *v)
{
    uint64_t c = rtc_now(s);
    uint32_t secs = c % 86400;
    uint32_t day = c / 86400;

    v[0] = secs % 60;
    v[1] = (secs / 60) % 60;
    v[2] = secs / 3600;
    v[3] = day & 0xff;
    v[4] = day >> 8;
}

/* Reset values (Register Table 7): all 0 but SOFT_RST_N; count 0, stopped. */
static void rtc_reset_regs(DM32I2cRtcState *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[R_SOFT_RST_N] = 1;
    s->count = 0;
    s->match = 0;
    s->ref_ns = qemu_clock_get_ns(rtc_clock);
    timer_del(s->match_timer);
    rtc_update_irq(s);
}

static void rtc_write_reg(DM32I2cRtcState *s, uint8_t reg, uint8_t val)
{
    if (reg >= RTC_NREGS) {
        return;
    }
    val &= rtc_mask[reg];
    if (!(s->regs[R_SOFT_RST_N] & 1) && reg != R_SOFT_RST_N) {
        return;                         /* held in soft reset */
    }
    switch (reg) {
    case R_VAL_S ... R_VAL_S + 4:
    case R_RSTAT:
        return;                         /* read-only */
    case R_MATCH_VEN:
        if (val & 1) {
            s->match = rtc_fields(&s->regs[R_MATCH_S]) % RTC_PERIOD_S;
            rtc_schedule(s);
        }
        return;                         /* clears itself */
    case R_CCR:
        rtc_rebase(s);
        if (val & CCR_LOAD) {
            s->count = rtc_fields(&s->regs[R_LOAD_S]) % RTC_PERIOD_S;
            val &= ~CCR_LOAD;
        }
        s->regs[R_CCR] = val;
        if (s->log) {
            fprintf(stderr, "dm32_rtc(i2c 0x70): CCR <- 0x%x (count %"
                    PRIu64 " s)\n", val, s->count);
        }
        rtc_schedule(s);
        return;
    case R_NUMER_L:
    case R_NUMER_H:
    case R_NUMER_VLD:
        rtc_rebase(s);
        s->regs[reg] = val;
        rtc_schedule(s);
        return;
    case R_SOFT_RST_N:
        if (!(val & 1)) {
            rtc_reset_regs(s);
            s->regs[R_SOFT_RST_N] = 0;
        } else {
            s->regs[R_SOFT_RST_N] = 1;
            s->ref_ns = qemu_clock_get_ns(rtc_clock);
        }
        return;
    default:
        s->regs[reg] = val;
        return;
    }
}

static uint8_t rtc_read_reg(DM32I2cRtcState *s, uint8_t reg)
{
    uint8_t v;

    if (reg >= RTC_NREGS) {
        return 0xff;
    }
    if (reg <= R_VAL_S + 4) {
        return s->latch[reg];
    }
    v = s->regs[reg];
    if (reg == R_RSTAT && v) {
        s->regs[R_RSTAT] = 0;           /* read clears */
        rtc_update_irq(s);
    }
    return v;
}

static int dm32_i2c_rtc_event(I2CSlave *i2c, enum i2c_event event)
{
    DM32I2cRtcState *s = DM32_I2C_RTC(i2c);

    switch (event) {
    case I2C_START_SEND:
        s->first = true;
        break;
    case I2C_START_RECV:
        rtc_fill_val(s, s->latch);
        if (s->log) {
            fprintf(stderr, "dm32_rtc(i2c 0x70): read from reg 0x%x "
                    "(%02u:%02u:%02u day %u)\n", s->ptr, s->latch[2],
                    s->latch[1], s->latch[0], s->latch[3] | (s->latch[4] << 8));
        }
        break;
    default:
        break;
    }
    return 0;
}

static int dm32_i2c_rtc_send(I2CSlave *i2c, uint8_t data)
{
    DM32I2cRtcState *s = DM32_I2C_RTC(i2c);

    if (s->first) {
        s->ptr = data;
        s->first = false;
    } else {
        rtc_write_reg(s, s->ptr++, data);
    }
    return 0;
}

static uint8_t dm32_i2c_rtc_recv(I2CSlave *i2c)
{
    DM32I2cRtcState *s = DM32_I2C_RTC(i2c);

    return rtc_read_reg(s, s->ptr++);
}

/*
 * No reset handler: SoC resets do not reach the RTC, which keeps counting
 * (see the header).
 */
static void dm32_i2c_rtc_init(Object *obj)
{
    DM32I2cRtcState *s = DM32_I2C_RTC(obj);

    qdev_init_gpio_out(DEVICE(obj), &s->irq, 1);
    s->match_timer = timer_new_ns(rtc_clock, rtc_match_fire, s);
    s->log = dm32_env_flag("DM32_I2C_LOG");
    rtc_reset_regs(s);
}

static void dm32_i2c_rtc_class_init(ObjectClass *klass, const void *data)
{
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);

    sc->event = dm32_i2c_rtc_event;
    sc->send = dm32_i2c_rtc_send;
    sc->recv = dm32_i2c_rtc_recv;
}

static const TypeInfo dm32_i2c_rtc_type = {
    .name = TYPE_DM32_I2C_RTC,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(DM32I2cRtcState),
    .instance_init = dm32_i2c_rtc_init,
    .class_init = dm32_i2c_rtc_class_init,
};

static void dm32_i2c_rtc_register(void)
{
    type_register_static(&dm32_i2c_rtc_type);
}

type_init(dm32_i2c_rtc_register)

void dm32_i2c_rtc_attach(I2CBus *bus, qemu_irq irq)
{
    I2CSlave *dev = i2c_slave_create_simple(bus, TYPE_DM32_I2C_RTC,
                                            RTC_ADDR7);

    qdev_connect_gpio_out(DEVICE(dev), 0, irq);
}
