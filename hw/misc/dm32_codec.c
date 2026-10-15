/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Baofeng DM-32UV HR_C7000 audio codec: register-level model
 *
 * Copyright (c) 2026 Alex Kaplan
 *
 * Source: HR_C7000 v2.7 datasheet ("DS"), section 6.4 (working modes and
 * sequences, soft mute) and section 6.5 (Register Table 26 and the 37
 * register descriptions 6.5.2.1-6.5.2.37); section 4.6.5.3-4.6.5.6 for the
 * SystemControl audio registers; Table 30 for the interrupt. The SVD lists
 * the registers up to 0x26 with the same reset values; 0x27-0x30 are taken
 * from the DS.
 *
 * Access. The codec's registers are bytes at 0x160009c0 + address, inside
 * the Modem_Buffer window, which dm32_dmrram models; this window sits over
 * it. CODEC_I2C_MUX (SystemControl 0x8c) bit 7 selects the codec's register
 * interface: 0 the MC (memory-mapped) interface, 1 I2C. Assumption: with I2C
 * selected, the window does not reach the registers, so reads return 0 and
 * writes are dropped (logged as a guest error); the DS does not name the I2C
 * bus. AUDIO_CONTROL (0x80) selects audio data paths, not register access.
 *
 * Registers (DS 6.5.2): SR1 0x00, SR2 0x01 and MR 0x07 are read-only; the
 * rest are read-write with the DS reset values. Addresses the DS does not
 * define read 0 and drop writes, logged as guest errors.
 *
 * Power modes (DS 6.4.1, Table 40), from CR_VIC sb [0] and sb_sleep [1]:
 * power-down (sb = 1, the reset state), sleep (sb = 0, sb_sleep = 1),
 * playback (sb = 0 with the DAC path on: AICR_DAC sb_aicr_dac [4] = 0 and
 * CR_DAC sb_dac [4] = 0) and record (sb = 0, sb_sleep = 0, CR_ADC sb_adc
 * [4] = 0). SR1 pon_ack [7] ("codec is in working mode") sets when the codec
 * reaches playback, after Tstbon (12 ms, DS 6.4.2.4), or leaves sleep with
 * sb_sleep = 0, after Tsbyu (50 ms, the lower end of DS 6.4.2.3's 50-100 ms),
 * and clears as soon as neither holds. LINEOUT_CTRL standby_lo (SystemControl
 * 0x88 [31], "standby of the mercury DAC", which the DS 6.4.2.4 sequence
 * polls) reads 0 Tstbon after the playback path comes on, and 1 otherwise.
 * Assumption: the DS gives these times as bounds, not as the moment the bits
 * change.
 *
 * Soft mute (DS 6.4.4). CR_DAC dac_soft_mute [7] and CR_ADC adc_soft_mute
 * [7] request mute; MR [1:0] and [4:3] report it: 00 not muted, 01 being
 * muted, 10 ending the mute, 11 muted. A change ramps for 24 ms (the DS's
 * upper bound), then the state settles and IFR dac_mute_event [0] or
 * adc_mute_event [2] sets. Assumption: the ramp runs only while that
 * converter is powered (the DAC with sb = 0 and sb_dac = 0, the ADC with sb
 * = 0, sb_sleep = 0 and sb_adc = 0); a converter that powers up takes its
 * requested state at once, and one that is off reads 00, the MR reset value.
 *
 * Timer (DS 6.5.2.18-19). Assumption: writing CR_TIMER_l starts a one-shot
 * count of (CR_TIMER_h << 8 | CR_TIMER_l), which ends after (3 + count) slow
 * clocks of 512 MCLK, the DS minimum, MCLK being 24 MHz (CR_CK crystal 0000)
 * or 26 MHz (0010), and sets IFR2 TIMER_END [4]. CR_CK shutdown_clock [4]
 * stops it.
 *
 * Interrupt (DS 6.5.2.20-24; Table 30: source 37, level, active high). An
 * event is pending while (IFR & ~IMR) or (IFR2 & ~IMR2) has a defined flag
 * set (IFR [7], [2], [0]; IFR2 [4]); SR1 irq_ack [6] reads it. ICR int_form
 * [7:6] shapes the output: 00 high while pending, 01 low while pending, 11
 * a low pulse of 8 MC_CLK (9.8304 MHz) per new event, otherwise high; 10 is
 * undocumented and is taken as 00. Assumption: a 1 written to an IFR or IFR2
 * flag clears it (the DS types them read-write and does not say how they
 * clear).
 *
 * Indirect registers (DS 6.5.2.10-11, 6.5.2.34-37). CR_MIX/DR_MIX reach
 * MIX_0-3, CR_DAC_AGC/DR_DAC_AGC reach DAC_AGC_0-3, and CR_ADC_AGC/DR_ADC_AGC
 * reach ADC_AGC_0-4. Writing the control register with load [6] = 1 stores
 * the data register's byte at the index [5:0], as in the DS 6.4.5 example
 * (data first, then the control write that latches it). With load = 0 the
 * data register reads the indexed register. Assumption: an index past the
 * last register is ignored, and logged.
 *
 * Not modelled: the audio itself (no samples flow), the gains' effect, AGC,
 * DRC, the mixer's effect, the ADAS lock and the sample-rate calibration.
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/csky/dm32.h"
#include "hw/csky/dm32_regfile.h"

#define TYPE_DM32_CODEC "dm32_codec"
OBJECT_DECLARE_SIMPLE_TYPE(DM32CodecState, DM32_CODEC)

#define WIN 0x40

#define R_SR1         0x00
#define R_MR          0x07
#define R_AICR_DAC    0x08
#define R_CR_DAC      0x0d
#define R_CR_ADC      0x0f
#define R_CR_MIX      0x10
#define R_DR_MIX      0x11
#define R_CR_VIC      0x12
#define R_CR_CK       0x13
#define R_CR_TIMER_H  0x18
#define R_CR_TIMER_L  0x19
#define R_ICR         0x1a
#define R_IMR         0x1b
#define R_IFR         0x1c
#define R_IMR2        0x1d
#define R_IFR2        0x1e
#define R_CR_DAC_AGC  0x2b
#define R_DR_DAC_AGC  0x2c
#define R_CR_ADC_AGC  0x2f
#define R_DR_ADC_AGC  0x30

#define SR1_PON_ACK   0x80
#define SR1_IRQ_ACK   0x40
#define VIC_SB        0x01
#define VIC_SB_SLEEP  0x02
#define SB_BLOCK      0x10      /* sb_aicr_*, sb_dac, sb_adc */
#define SOFT_MUTE     0x80
#define CK_SHUTDOWN   0x10
#define IFR_DAC_MUTE  0x01
#define IFR_ADC_MUTE  0x04
#define IFR_DEFINED   0x85
#define IFR2_TIMER    0x10
#define IND_LOAD      0x40
#define IND_ADDR      0x3f

#define T_STBON_NS    (12 * SCALE_MS)
#define T_SBYU_NS     (50 * SCALE_MS)
#define T_MUTE_NS     (24 * SCALE_MS)
#define MC_CLK_HZ     9830400
#define PULSE_NS      (8 * NANOSECONDS_PER_SECOND / MC_CLK_HZ)

/* MR states */
#define MUTE_OFF      0
#define MUTE_RAMP_ON  1
#define MUTE_RAMP_OFF 2
#define MUTE_ON       3

/* The 30 registers the SVD lists, then the 7 it does not (DS 6.5.2.30-37). */
static const DM32RegDesc dm32_codec_regs[] = {
    DM32_SVD_CODEC_REGS(DM32_REGDESC)
    { "GCR_MIXDACL", 0x27, 8, 0, 0, 0, 0, 0, 0, 0 },
    { "GCR_MIXDACR", 0x28, 8, 0, 0, 0, 0, 0, 0, 0 },
    { "GCR_MIXADCL", 0x29, 8, 0, 0, 0, 0, 0, 0, 0 },
    { "GCR_MIXADCR", 0x2a, 8, 0, 0, 0, 0, 0, 0, 0 },
    { "CR_DAC_AGC",  0x2b, 8, 0, 0, 0, 0, 0, 0, 0 },
    { "DR_DAC_AGC",  0x2c, 8, 0, 0, 0, 0, 0, 0, 0 },
    { "CR_ADC_AGC",  0x2f, 8, 0, 0, 0, 0, 0, 0, 0 },
    { "DR_ADC_AGC",  0x30, 8, 0, 0, 0, 0, 0, 0, 0 },
};
#define NREGS ARRAY_SIZE(dm32_codec_regs)

/* Indirect register banks: control and data offsets, size, reset values. */
typedef struct CodecBank {
    uint8_t cr, dr, n;
    uint8_t reset[5];
    const char *name;
} CodecBank;

static const CodecBank dm32_codec_banks[] = {
    { R_CR_MIX, R_DR_MIX, 4, { 0, 0, 0, 0 }, "MIX" },
    { R_CR_DAC_AGC, R_DR_DAC_AGC, 4, { 0, 0, 0, 0 }, "DAC_AGC" },
    /* ADC_AGC_0 target 0xd, _1 hold 7, _2 atk 4 dcy 4, _3 agc_max 0x1f */
    { R_CR_ADC_AGC, R_DR_ADC_AGC, 5, { 0x34, 0x07, 0x44, 0x1f, 0x00 },
      "ADC_AGC" },
};
#define NBANKS ARRAY_SIZE(dm32_codec_banks)

typedef struct CodecMute {
    bool on;                /* the converter is powered */
    uint8_t state;          /* MR encoding */
    int64_t done;           /* end of the ramp, or 0 */
} CodecMute;

struct DM32CodecState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer *timer;
    uint32_t val[NREGS];
    uint8_t ind[NBANKS][5];

    /* power: when the working state and the DAC path were reached */
    bool working, playback;
    int64_t pon_at, stbon_at;

    CodecMute dac, adc;
    int64_t timer_end;      /* codec timer, or 0 */
    int64_t pulse_end;      /* ICR int_form 11 pulse, or 0 */
    bool pending;
    bool log;
};

static DM32CodecState *dm32_codec_singleton;

static int64_t now_ns(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static uint8_t reg(DM32CodecState *s, uint32_t off)
{
    uint32_t v = 0;

    dm32_regfile_read(dm32_codec_regs, NREGS, s->val, off, &v);
    return v;
}

static void set_reg(DM32CodecState *s, uint32_t off, uint8_t v)
{
    int i = dm32_regfile_index(dm32_codec_regs, NREGS, off);

    assert(i >= 0);
    s->val[i] = v;
}

static const char *reg_name(uint32_t off)
{
    int i = dm32_regfile_index(dm32_codec_regs, NREGS, off);

    return i < 0 ? "undefined" : dm32_codec_regs[i].name;
}

/* ---- power, mute and timer state ----------------------------------------- */

static bool sb_on(DM32CodecState *s)
{
    return !(reg(s, R_CR_VIC) & VIC_SB);
}

static bool dac_on(DM32CodecState *s)
{
    return sb_on(s) && !(reg(s, R_CR_DAC) & SB_BLOCK);
}

static bool adc_on(DM32CodecState *s)
{
    return sb_on(s) && !(reg(s, R_CR_VIC) & VIC_SB_SLEEP) &&
           !(reg(s, R_CR_ADC) & SB_BLOCK);
}

static bool playback_path(DM32CodecState *s)
{
    return dac_on(s) && !(reg(s, R_AICR_DAC) & SB_BLOCK);
}

static void mute_eval(CodecMute *m, bool on, bool want, int64_t now,
                      uint8_t *ifr, uint8_t event)
{
    if (!on || !m->on) {
        /* off reads 00; a converter that comes on takes its request at once */
        m->on = on;
        m->state = on && want ? MUTE_ON : MUTE_OFF;
        m->done = 0;
        return;
    }
    if (m->done && now >= m->done) {
        m->state = m->state == MUTE_RAMP_ON ? MUTE_ON : MUTE_OFF;
        m->done = 0;
        *ifr |= event;
    }
    if (m->done) {
        /* a request reversed mid-ramp turns the ramp round */
        if ((m->state == MUTE_RAMP_ON) != want) {
            m->state = want ? MUTE_RAMP_ON : MUTE_RAMP_OFF;
            m->done = now + T_MUTE_NS;
        }
        return;
    }
    if (want && m->state != MUTE_ON) {
        m->state = MUTE_RAMP_ON;
        m->done = now + T_MUTE_NS;
    } else if (!want && m->state != MUTE_OFF) {
        m->state = MUTE_RAMP_OFF;
        m->done = now + T_MUTE_NS;
    }
}

static void deadline(int64_t *next, int64_t t)
{
    if (t && t < *next) {
        *next = t;
    }
}

static void dm32_codec_update(DM32CodecState *s)
{
    int64_t now = now_ns(), next = INT64_MAX;
    uint8_t ifr = reg(s, R_IFR), ifr2 = reg(s, R_IFR2);
    bool working, playback, pending, level;
    uint8_t form;

    playback = playback_path(s);
    working = sb_on(s) && (playback || !(reg(s, R_CR_VIC) & VIC_SB_SLEEP));
    if (playback && !s->playback) {
        s->stbon_at = now + T_STBON_NS;
    }
    if (working && !s->working) {
        s->pon_at = now + (playback ? T_STBON_NS : T_SBYU_NS);
    }
    s->playback = playback;
    s->working = working;

    mute_eval(&s->dac, dac_on(s), reg(s, R_CR_DAC) & SOFT_MUTE, now, &ifr,
              IFR_DAC_MUTE);
    mute_eval(&s->adc, adc_on(s), reg(s, R_CR_ADC) & SOFT_MUTE, now, &ifr,
              IFR_ADC_MUTE);

    if (s->timer_end && now >= s->timer_end) {
        s->timer_end = 0;
        ifr2 |= IFR2_TIMER;
    }
    if (reg(s, R_CR_CK) & CK_SHUTDOWN) {
        s->timer_end = 0;
    }
    set_reg(s, R_IFR, ifr);
    set_reg(s, R_IFR2, ifr2);
    set_reg(s, R_MR, s->adc.state << 3 | s->dac.state);

    pending = (ifr & ~reg(s, R_IMR) & IFR_DEFINED) ||
              (ifr2 & ~reg(s, R_IMR2) & IFR2_TIMER);
    form = reg(s, R_ICR) >> 6;
    if (form == 3 && pending && !s->pending) {
        s->pulse_end = now + PULSE_NS;
    }
    if (s->pulse_end && now >= s->pulse_end) {
        s->pulse_end = 0;
    }
    s->pending = pending;
    set_reg(s, R_SR1, (s->working && now >= s->pon_at ? SR1_PON_ACK : 0) |
                      (pending ? SR1_IRQ_ACK : 0));

    switch (form) {
    case 1:
        level = !pending;
        break;
    case 3:
        level = !s->pulse_end;
        break;
    default:
        level = pending;
        break;
    }
    qemu_set_irq(s->irq, level);

    deadline(&next, s->dac.done);
    deadline(&next, s->adc.done);
    deadline(&next, s->timer_end);
    deadline(&next, s->pulse_end);
    if (s->working && s->pon_at > now) {
        deadline(&next, s->pon_at);
    }
    if (next == INT64_MAX) {
        timer_del(s->timer);
    } else {
        timer_mod(s->timer, next);
    }
}

static void dm32_codec_tick(void *opaque)
{
    dm32_codec_update(opaque);
}

static void dm32_codec_timer_start(DM32CodecState *s)
{
    uint32_t count = reg(s, R_CR_TIMER_H) << 8 | reg(s, R_CR_TIMER_L);
    uint8_t crystal = reg(s, R_CR_CK) & 0xf;
    uint64_t mclk = crystal == 2 ? 26000000 : 24000000;

    if (crystal != 0 && crystal != 2) {
        qemu_log_mask(LOG_UNIMP, "dm32_codec: CR_CK crystal %u undocumented, "
                      "taken as 24 MHz\n", crystal);
    }
    if (reg(s, R_CR_CK) & CK_SHUTDOWN) {
        return;
    }
    s->timer_end = now_ns() + muldiv64((3 + count) * 512ull,
                                       NANOSECONDS_PER_SECOND, mclk);
}

/* SystemControl LINEOUT_CTRL[31] */
bool dm32_codec_standby_lo(void)
{
    DM32CodecState *s = dm32_codec_singleton;

    return !s || !s->playback || now_ns() < s->stbon_at;
}

/* ---- register access ----------------------------------------------------- */

static const CodecBank *bank_for(uint32_t off, int *idx)
{
    int b;

    for (b = 0; b < NBANKS; b++) {
        if (off == dm32_codec_banks[b].cr || off == dm32_codec_banks[b].dr) {
            *idx = b;
            return &dm32_codec_banks[b];
        }
    }
    return NULL;
}

static uint64_t dm32_codec_read(void *opaque, hwaddr off, unsigned size)
{
    DM32CodecState *s = opaque;
    const CodecBank *bk;
    uint32_t v = 0;
    int b;

    if (!dm32_codec_mc_selected()) {
        qemu_log_mask(LOG_GUEST_ERROR, "dm32_codec: read at 0x%02x while "
                      "CODEC_I2C_MUX selects I2C\n", (unsigned)off);
        return 0;
    }
    dm32_codec_update(s);
    bk = bank_for(off, &b);
    if (bk && off == bk->dr && !(reg(s, bk->cr) & IND_LOAD)) {
        uint8_t i = reg(s, bk->cr) & IND_ADDR;

        v = i < bk->n ? s->ind[b][i] : 0;
    } else if (!dm32_regfile_read(dm32_codec_regs, NREGS, s->val, off, &v)) {
        qemu_log_mask(LOG_GUEST_ERROR, "dm32_codec: read of undefined "
                      "address 0x%02x\n", (unsigned)off);
    }
    if (s->log) {
        fprintf(stderr, "codec: R %s (0x%02x) -> 0x%02x\n", reg_name(off),
                (unsigned)off, v);
    }
    return v;
}

static void dm32_codec_write(void *opaque, hwaddr off, uint64_t val,
                             unsigned size)
{
    DM32CodecState *s = opaque;
    const CodecBank *bk;
    int b;

    if (!dm32_codec_mc_selected()) {
        qemu_log_mask(LOG_GUEST_ERROR, "dm32_codec: write at 0x%02x while "
                      "CODEC_I2C_MUX selects I2C\n", (unsigned)off);
        return;
    }
    if (s->log) {
        fprintf(stderr, "codec: W %s (0x%02x) <- 0x%02x\n", reg_name(off),
                (unsigned)off, (unsigned)val & 0xff);
    }
    dm32_codec_update(s);
    switch (off) {
    case R_IFR:
    case R_IFR2:
        set_reg(s, off, reg(s, off) & ~val);
        break;
    default:
        if (!dm32_regfile_write(dm32_codec_regs, NREGS, s->val, off, val)) {
            qemu_log_mask(LOG_GUEST_ERROR, "dm32_codec: write to undefined "
                          "address 0x%02x\n", (unsigned)off);
            return;
        }
        break;
    }
    bk = bank_for(off, &b);
    if (bk && off == bk->cr && (val & IND_LOAD)) {
        uint8_t i = val & IND_ADDR;

        if (i < bk->n) {
            s->ind[b][i] = reg(s, bk->dr);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "dm32_codec: %s index %u "
                          "undefined\n", bk->name, i);
        }
    }
    if (off == R_CR_TIMER_L) {
        dm32_codec_timer_start(s);
    }
    if (off == R_ICR && (val >> 6) == 2) {
        qemu_log_mask(LOG_UNIMP, "dm32_codec: ICR int_form 10 undocumented, "
                      "taken as 00\n");
    }
    dm32_codec_update(s);
}

static const MemoryRegionOps dm32_codec_ops = {
    .read = dm32_codec_read,
    .write = dm32_codec_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void dm32_codec_reset(DeviceState *dev)
{
    DM32CodecState *s = DM32_CODEC(dev);
    int b;

    dm32_regfile_reset(dm32_codec_regs, NREGS, s->val);
    for (b = 0; b < NBANKS; b++) {
        memcpy(s->ind[b], dm32_codec_banks[b].reset, sizeof(s->ind[b]));
    }
    memset(&s->dac, 0, sizeof(s->dac));
    memset(&s->adc, 0, sizeof(s->adc));
    s->working = s->playback = false;
    s->pon_at = s->stbon_at = 0;
    s->timer_end = s->pulse_end = 0;
    s->pending = false;
    timer_del(s->timer);
    qemu_set_irq(s->irq, 0);
}

static void dm32_codec_init(Object *obj)
{
    DM32CodecState *s = DM32_CODEC(obj);

    dm32_codec_singleton = s;
    memory_region_init_io(&s->iomem, obj, &dm32_codec_ops, s,
                          TYPE_DM32_CODEC, WIN);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dm32_codec_tick, s);
    s->log = dm32_env_flag("DM32_CODEC_LOG");
}

static void dm32_codec_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), dm32_codec_reset);
}

static const TypeInfo dm32_codec_type = {
    .name = TYPE_DM32_CODEC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DM32CodecState),
    .instance_init = dm32_codec_init,
    .class_init = dm32_codec_class_init,
};

static void dm32_codec_register(void)
{
    type_register_static(&dm32_codec_type);
}

type_init(dm32_codec_register)
