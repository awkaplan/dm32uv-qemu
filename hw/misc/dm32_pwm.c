/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Baofeng DM-32UV PWM controller: three channels
 *
 * Copyright (c) 2026 Alex Kaplan
 *
 * Models the PWM controller of the HR_C7000 SoC, with three channels, mapped
 * at 0x140C0000 in a 64 KB window. The register layout and field names come
 * from dm32uv-re data/registers/HR_C7000-DM32.h, PWM_Type (DS section 5.8,
 * p.385-400), and dm32uv-re data/registers/HR_C7000-DM32.svd, peripheral PWM
 * (field descriptions quoted below).
 *
 * Per channel N (stride 0x20; N = 0, 1, 2), at base 0x20*N:
 *   +0x00 PWMn_CFG    [0] start  [1] soft_rst  [2] cfg_update  [3] inten
 *                     [5:4] mode  [7:6] evtrig  [8] fpout  [9] inactout.
 *                     Reset 0x0. RW.
 *                     SVD: start "Start encoding, rising edge is valid (ie
 *                     from 0 Change to 1)"; soft_rst "Software reset enable,
 *                     used to turn off PWM at any time"; mode "1: ONE_SHOT
 *                     model 2: CONTINUOUS model 0,3: Reserved, no PWM
 *                     Waveform output"; fpout "Output level of the first
 *                     phase in one cycle"; inactout "The output level when
 *                     not enabled"; inten "Interrupt enable"; cfg_update "In
 *                     CONTINUOUS mode, changes to PWM_PER and PWM_FP take
 *                     effect mid-stream: after writing them, write cfg_update
 *                     0->1 so both registers update simultaneously"; evtrig
 *                     "In ONE_SHOT Event trigger mode in mode 1: rising edge
 *                     trigger 2: Falling edge trigger 0,3: Normal mode" (the
 *                     SVD's own text is garbled here; evtrig is stored, not
 *                     modelled).
 *   +0x04 PWMn_RPT    [15:0] pwm_rpt: "ONE_SHOT mode: number of times the
 *                     waveform is repeated". Reset 0x0. RW.
 *   +0x08 PWMn_PER    [31:0] pwm_per: "Length of one waveform period, in
 *                     system-clock cycles". Reset 0x0. RW.
 *   +0x0c PWMn_FP     [31:0] pwm_fp: "Length of the first phase of each
 *                     period, in system-clock cycles; if pwm_fp > pwm_per the
 *                     whole period is at fpout". Reset 0x0. RW.
 *   +0x10 PWMn_STATUS [7:0] pwm_cycle ("In CONTINUOUS mode, number of cycles
 *                     output with the same pwm_per/pwm_fp parameters"),
 *                     [8] pwm_busy ("PWM state machine: 0 idle; 1 working"),
 *                     [9] pwm_out ("Current value of the waveform"). Reset
 *                     0x0. RW.
 *   +0xff0 PWM_INTR_STA  [0] pwm0_intr, [1] pwm1_intr, [2] pwm2_intr
 *                     ("output interrupt status"). Read-only in the SVD.
 *                     See "Interrupt" below.
 *
 * Other offsets in the window (0x14-0x1f, 0x34-0x3f, 0x54-0xfef, ...) are
 * reserved in the header and are plain storage here, with a one-time note on
 * first access.
 *
 * Model:
 * - Storage: every register is stored and read back as written, masked to its
 *   real bits.
 * - pwm_busy is derived, not stored: it is 1 while the channel is "running"
 *   (see below) and 0 otherwise, updated on each CFG write. The SVD says only
 *   "0 idle; 1 working". No timing is modelled, so busy follows start and
 *   soft_rst immediately. This is an assumption, stated here.
 * - pwm_out and pwm_cycle are plain storage: the waveform is not generated,
 *   so pwm_out never changes and pwm_cycle never counts.
 * - "Running" (effective enable) means start == 1, soft_rst == 0, and mode in
 *   {1 (ONE_SHOT), 2 (CONTINUOUS)}. Modes 0 and 3 are "Reserved, no PWM
 *   Waveform output" per the SVD.
 * - Latched period and phase: the output uses act_per and act_fp, not the raw
 *   PER and FP registers. act_per and act_fp are loaded from PER and FP when
 *   the channel starts (start 0->1) and, while running in CONTINUOUS mode,
 *   only on a cfg_update 0->1 write (SVD text above). While not running they
 *   track PER and FP writes immediately. The SVD states the cfg_update rule
 *   for CONTINUOUS only; the same latch is applied to start, and nothing else
 *   is assumed for ONE_SHOT mid-run writes. Hardware behaviour in the
 *   unstated cases is unverified.
 * - The effective state reported to the trace is (enable, act_per, act_fp,
 *   fpout, inactout).
 *
 * Interrupt: PWM_INTR_STA reads 0 in this model and writes are ignored. The
 * SVD describes an interrupt pair (CFG.inten, PWM_INTR_STA.pwmN_intr) and the
 * PIC source 18 (PWM), but it does not say which waveform event sets
 * pwmN_intr (period end, one-shot completion, ...). With no waveform
 * generation there is no event to raise, so no IRQ is wired to the PIC
 * (intc[18]). This is unverified.
 *
 * Environment variables (DM32_PWM_LOG is read at instance init, DM32_PWM_TRACE
 * at the first change; see docs/environment.md):
 *   DM32_PWM_LOG=1        print every register access by name, decoded.
 *                         Writes to PER and FP also print the duty ratio
 *                         fp/per when both are non-zero (clipped to 1.0 when
 *                         fp > per, per the SVD's "whole period at fpout"
 *                         rule).
 *   DM32_PWM_TRACE=<path> append one line per change of a channel's
 *                         effective state: "<vtime_ns> <ch> <act_per>
 *                         <act_fp> <cfg>", where <cfg> is the raw CFG
 *                         register at that moment (decimal). The file is
 *                         flushed on every line.
 *
 * Not modelled: waveform generation (no output level, no counter, no cycle
 * count); the evtrig external-signal modes; the output pin (GPIOA 15 for
 * channel 0, IO_DIPLEX0 bit 17; see dm32uv-re specs/dts/hr_c7000.dtsi); any
 * interrupt; reset of the latch on soft_rst.
 */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "hw/csky/dm32.h"
#include <stdio.h>
#include <stdlib.h>

#define TYPE_DM32_PWM "dm32_pwm"
OBJECT_DECLARE_SIMPLE_TYPE(DM32PwmState, DM32_PWM)

#define WIN 0x10000
#define NCH 3
#define CH_STRIDE 0x20

#define R_CFG         0x00
#define R_RPT         0x04
#define R_PER         0x08
#define R_FP          0x0c
#define R_STATUS      0x10
#define R_INTR_STA    0xff0

#define CFG_START      (1u << 0)
#define CFG_SOFT_RST   (1u << 1)
#define CFG_CFG_UPDATE (1u << 2)
#define CFG_INTEN      (1u << 3)
#define CFG_MODE_SH    4
#define CFG_EVTRIG_SH  6
#define CFG_FPOUT      (1u << 8)
#define CFG_INACTOUT   (1u << 9)
#define CFG_MASK       0x3ffu
#define MODE_ONESHOT    1u
#define MODE_CONTINUOUS 2u

#define RPT_MASK      0xffffu
#define STATUS_BUSY   (1u << 8)
#define STATUS_MASK   0x3ffu

typedef struct PwmChan {
    uint32_t cfg;
    uint32_t rpt;
    /* register (what software wrote, read back) */
    uint32_t per;
    uint32_t fp;
    uint32_t status;
    /* latched values that drive the (modelled) output */
    uint32_t act_per;
    uint32_t act_fp;
    int      last_valid;
    /* last traced effective state */
    int      last_en;
    uint32_t last_per, last_fp;
    int      last_pol;
} PwmChan;

struct DM32PwmState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    PwmChan ch[NCH];
    /* always 0: no event source is modelled */
    uint32_t intr_sta;
    /* storage for reserved offsets */
    uint32_t reg[WIN / 4];
    /* one-time note for an unmodelled offset */
    uint8_t  noted[WIN / 4];
    FILE    *trace;
    int      trace_tried;
    int      log;
};

static const char *mode_name(uint32_t mode)
{
    switch (mode) {
    case 0:
        return "reserved0";
    case MODE_ONESHOT:
        return "oneshot";
    case MODE_CONTINUOUS:
        return "continuous";
    default:
        return "reserved3";
    }
}

static int chan_running(uint32_t cfg)
{
    uint32_t mode = (cfg >> CFG_MODE_SH) & 3u;

    return (cfg & CFG_START) && !(cfg & CFG_SOFT_RST) &&
           (mode == MODE_ONESHOT || mode == MODE_CONTINUOUS);
}

/*
 * Decode a channel-relative offset: 0 CFG, 1 RPT, 2 PER, 3 FP, 4 STATUS,
 * or -1.
 */
static int chan_reg(uint32_t off)
{
    uint32_t r;

    if (off >= NCH * CH_STRIDE) {
        return -1;
    }
    r = off % CH_STRIDE;
    switch (r) {
    case R_CFG:
        return 0;
    case R_RPT:
        return 1;
    case R_PER:
        return 2;
    case R_FP:
        return 3;
    case R_STATUS:
        return 4;
    default:
        return -1;
    }
}

static const char *latch_note(uint32_t cfg)
{
    return chan_running(cfg) ?
           "pending: applies at cfg_update 0->1 (CONTINUOUS) or next start" :
           "applies now";
}

static const char *chan_reg_name(int r)
{
    static const char *n[] = { "CFG", "RPT", "PER", "FP", "STATUS" };

    return (r >= 0 && r < 5) ? n[r] : "?";
}

static void trace_state(DM32PwmState *s, int c)
{
    PwmChan *p = &s->ch[c];
    int en = chan_running(p->cfg);
    int pol = ((p->cfg & CFG_FPOUT) ? 1 : 0) |
              ((p->cfg & CFG_INACTOUT) ? 2 : 0);

    if (p->last_valid && en == p->last_en && p->act_per == p->last_per &&
        p->act_fp == p->last_fp && pol == p->last_pol) {
        return;
    }
    p->last_valid = 1;
    p->last_en = en;
    p->last_per = p->act_per;
    p->last_fp = p->act_fp;
    p->last_pol = pol;

    if (!s->trace_tried) {
        const char *path = getenv("DM32_PWM_TRACE");

        s->trace_tried = 1;
        if (path && *path) {
            s->trace = fopen(path, "a");
            if (!s->trace) {
                fprintf(stderr, "dm32_pwm: cannot open DM32_PWM_TRACE=%s\n",
                        path);
            }
        }
    }
    if (s->trace) {
        fprintf(s->trace, "%lld %d %u %u %u\n",
                (long long) qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), c,
                (unsigned) p->act_per, (unsigned) p->act_fp,
                (unsigned) p->cfg);
        fflush(s->trace);
    }
}

static void log_duty(int c, PwmChan *p)
{
    if (p->act_per && p->act_fp) {
        double r = (double) p->act_fp / (double) p->act_per;

        if (r > 1.0) {
            r = 1.0;
        }
        fprintf(stderr, "PWM%d effective fp/per = %u/%u = %.4f\n", c,
                (unsigned) p->act_fp, (unsigned) p->act_per, r);
    } else {
        fprintf(stderr, "PWM%d effective fp/per n/a (per=%u fp=%u)\n", c,
                (unsigned) p->act_per, (unsigned) p->act_fp);
    }
}

static void log_cfg(int c, uint32_t v, const char *lead)
{
    uint32_t mode = (v >> CFG_MODE_SH) & 3u;

    fprintf(stderr, "%sPWM%d CFG <- 0x%x (EN=%d SOFT_RST=%d CFG_UPDATE=%d "
            "INTEN=%d MODE=%s EVTRIG=%u FPOUT=%d INACTOUT=%d)\n",
            lead, c, (unsigned) v,
            (v & CFG_START) ? 1 : 0, (v & CFG_SOFT_RST) ? 1 : 0,
            (v & CFG_CFG_UPDATE) ? 1 : 0, (v & CFG_INTEN) ? 1 : 0,
            mode_name(mode),
            (unsigned) ((v >> CFG_EVTRIG_SH) & 3u),
            (v & CFG_FPOUT) ? 1 : 0, (v & CFG_INACTOUT) ? 1 : 0);
}

static uint64_t dm32_pwm_read(void *opaque, hwaddr off, unsigned size)
{
    DM32PwmState *s = opaque;
    int c, r;
    uint32_t v;

    if (off + 4 > WIN) {
        return 0;
    }
    if (off == R_INTR_STA) {
        if (s->log) {
            fprintf(stderr, "PWM_INTR_STA -> 0x%x\n", (unsigned) s->intr_sta);
        }
        return s->intr_sta;
    }
    c = (int) (off / CH_STRIDE);
    r = chan_reg(off);
    if (c < NCH && r >= 0) {
        PwmChan *p = &s->ch[c];

        switch (r) {
        case 0:
            v = p->cfg;
            break;
        case 1:
            v = p->rpt;
            break;
        case 2:
            v = p->per;
            break;
        case 3:
            v = p->fp;
            break;
        default:
            v = p->status;
            break;
        }
        if (s->log) {
            fprintf(stderr, "PWM%d %s -> 0x%x\n", c, chan_reg_name(r),
                    (unsigned) v);
        }
        return v;
    }

    v = s->reg[off / 4];
    if (!s->noted[off / 4]) {
        s->noted[off / 4] = 1;
        fprintf(stderr, "dm32_pwm: note: access to reserved offset "
                "0x%03x (storage only)\n", (unsigned) off);
    }
    return v;
}

static void dm32_pwm_write(void *opaque, hwaddr off, uint64_t val,
                           unsigned size)
{
    DM32PwmState *s = opaque;
    uint32_t v = (uint32_t) val;
    int c, r;

    if (off + 4 > WIN) {
        return;
    }
    if (off == R_INTR_STA) {
        if (s->log) {
            fprintf(stderr, "PWM_INTR_STA <- 0x%x (read-only, ignored)\n", v);
        }
        return;
    }
    c = (int) (off / CH_STRIDE);
    r = chan_reg(off);
    if (c < NCH && r >= 0) {
        PwmChan *p = &s->ch[c];
        int was_running = chan_running(p->cfg);
        int now_running;
        uint32_t mode;

        switch (r) {
        case 0: { /* CFG */
            uint32_t old = p->cfg;

            p->cfg = v & CFG_MASK;
            mode = (p->cfg >> CFG_MODE_SH) & 3u;
            now_running = chan_running(p->cfg);
            if (s->log) {
                log_cfg(c, p->cfg, "");
            }
            /*
             * Channel c drives pad PTA(15 + c) only while IO_DIPLEX0
             * pwm<c>_sel (bit 17 + c) is 0 (v2.7 EN 4.4.4.4).
             */
            if (now_running && !was_running &&
                dm32_pad_function(DM32_PTA(15 + c)) != 0) {
                dm32_event(DM32_EV_PAD_UNCLAIMED, "PWM%d started while pad "
                           "PTA%d is not PWM_%d", c, 15 + c, c);
            }
            /*
             * Latch rules (see the header): start 0->1 loads PER and FP;
             * cfg_update 0->1 loads them only while running in CONTINUOUS
             * mode; while not running they track directly.
             */
            if (now_running && (!was_running ||
                                (mode == MODE_CONTINUOUS &&
                                 !(old & CFG_CFG_UPDATE) &&
                                 (p->cfg & CFG_CFG_UPDATE)))) {
                p->act_per = p->per;
                p->act_fp = p->fp;
                if (s->log) {
                    fprintf(stderr, "PWM%d latched PER/FP (%s)\n", c,
                            !was_running ? "start" : "cfg_update");
                }
            } else if (!now_running) {
                p->act_per = p->per;
                p->act_fp = p->fp;
            }
            /* STATUS.pwm_busy follows the running state (no timing). */
            p->status = (p->status & ~STATUS_BUSY) |
                        (now_running ? STATUS_BUSY : 0);
            break;
        }
        case 1:
            /* RPT */
            p->rpt = v & RPT_MASK;
            if (s->log) {
                fprintf(stderr, "PWM%d RPT <- 0x%x (%u)\n", c,
                        (unsigned) p->rpt, (unsigned) p->rpt);
            }
            break;
        case 2:
            /* PER */
            p->per = v;
            if (s->log) {
                fprintf(stderr, "PWM%d PER <- 0x%x (%u cycles; %s)\n", c, v, v,
                        latch_note(p->cfg));
            }
            if (!chan_running(p->cfg)) {
                p->act_per = p->per;
                p->act_fp = p->fp;
            }
            break;
        case 3:
            /* FP */
            p->fp = v;
            if (s->log) {
                fprintf(stderr, "PWM%d FP <- 0x%x (%u cycles; %s)\n", c, v, v,
                        latch_note(p->cfg));
            }
            if (!chan_running(p->cfg)) {
                p->act_per = p->per;
                p->act_fp = p->fp;
            }
            break;
        default:
            /*
             * STATUS: RW per SVD. pwm_busy is derived from CFG, so the
             * written bit 8 is dropped and the current busy bit is kept.
             */
            p->status = (v & STATUS_MASK & ~STATUS_BUSY) |
                        (p->status & STATUS_BUSY);
            if (s->log) {
                fprintf(stderr, "PWM%d STATUS <- 0x%x\n", c,
                        (unsigned) p->status);
            }
            break;
        }
        if (s->log && (r == 2 || r == 3 || r == 0)) {
            log_duty(c, p);
        }
        trace_state(s, c);
        return;
    }

    if (!s->noted[off / 4]) {
        s->noted[off / 4] = 1;
        fprintf(stderr, "dm32_pwm: note: write to reserved offset "
                "0x%03x (storage only)\n", (unsigned) off);
    }
    if (s->log) {
        fprintf(stderr, "PWM +0x%03x <- 0x%x (reserved)\n", (unsigned) off, v);
    }
    s->reg[off / 4] = v;
}

static const MemoryRegionOps dm32_pwm_ops = {
    .read = dm32_pwm_read,
    .write = dm32_pwm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void dm32_pwm_reset(DeviceState *dev)
{
    DM32PwmState *s = DM32_PWM(dev);

    memset(s->ch, 0, sizeof(s->ch));
    memset(s->reg, 0, sizeof(s->reg));
    s->intr_sta = 0;
    for (int c = 0; c < NCH; c++) {
        /*
         * Reset state: nothing running, all values zero. Mark it traced so
         * that the first write that changes nothing does not produce a line.
         */
        s->ch[c].last_valid = 1;
    }
}

static void dm32_pwm_init(Object *obj)
{
    DM32PwmState *s = DM32_PWM(obj);

    memory_region_init_io(&s->iomem, obj, &dm32_pwm_ops, s, TYPE_DM32_PWM,
                          WIN);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    dm32_pwm_reset(DEVICE(obj));
    s->trace = NULL;
    s->trace_tried = 0;
    s->log = dm32_env_flag("DM32_PWM_LOG");
}

static void dm32_pwm_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), dm32_pwm_reset);
}

static const TypeInfo dm32_pwm_type = {
    .name = TYPE_DM32_PWM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DM32PwmState),
    .instance_init = dm32_pwm_init,
    .class_init = dm32_pwm_class_init,
};

static void dm32_pwm_register(void)
{
    type_register_static(&dm32_pwm_type);
}

type_init(dm32_pwm_register)
