/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Baofeng DM-32UV system and modem registers: partial model at 0x11000000
 *
 * Copyright (c) 2026 Alex Kaplan
 *
 * The register window at 0x11000000 is backed by read/write storage, with
 * these exceptions:
 *
 *  (1) The clock-manager lock register CLK_MGR_REG0x04 (+0x04). The firmware's
 *      clock init (IRAM 0x12480) busy-waits for PLL lock. Reads of +0x04
 *      return a "locked and ready" value (bit 30 and bit 31 set) while
 *      preserving the low config bits, so that boot gets past clock init.
 *
 *  (2) The DMR Layer-2 slot-timing state machine (LAYER2 registers at +0x400,
 *      INTERP_CLEAR at +0x3b0, WORK_MODE at +0x100). It runs the FHSS
 *      beacon and hop timing chain, which was reverse-engineered statically.
 *      The model covers only timing and control flow, never RF or DSP. A
 *      "transmit" is the firmware keying an established TX slot on the right
 *      cadence, and the model logs it. There is no PLL, 4FSK or vocoder
 *      model. See docs/modem-timing.md.
 *
 *      Behaviour (HR_C7000 v2.7 EN sections 10.2.2 and 10.5.2.1, cross-checked
 *      against the traced firmware values):
 *        - LAYER2_CONTROL (+0x400) bit 1, tx_master_mode: 1 is active,
 *          free-running establishment.
 *        - LAYER2_SLOTON (+0x404) bit 0, dll_tx_slot_on: a 0-to-1 transition
 *          starts the TX slot counter, when the CONTROL mode permits (see the
 *          start condition below).
 *        - A 30 ms periodic timer advances a slot counter 0..287. On the
 *          287-to-0 wrap it raises the TX-slot interrupt (a held level) to
 *          the PIC (source 30, vector 62).
 *        - Reads of LAYER2_STATUS (+0x41c) return the live counter, so that
 *          firmware polling sees it move.
 *        - A write of 1 to bit 0 of INTERP_CLEAR (+0x3b0) acknowledges and
 *          clears the TX-slot interrupt. The firmware's ISR at 0x2c42c does
 *          this before OSSemPost. The model deasserts the PIC line on that
 *          acknowledgement.
 *        - LAYER2_TXRX_CTRL (+0x408): the firmware writes the on-air command
 *          here (0x40 or 0x41 while a TX slot is established). The model logs
 *          that as a TX-ORIGINATE event, which is the observable "it
 *          transmitted". It does not synthesise any signal.
 *
 *      The environment variable DM32_MODEM_LOG=<file> enables event logging
 *      (slot wraps, IRQ assert and clear, TX-ORIGINATE). Logging is off by
 *      default.
 *
 *  (3) SYS_SOFT_RSTN (+0x00), the per-module soft reset (v2.7 EN section
 *      4.1.4.1). Nine active-low bits: [8] CPU, [7] system global, [6] CPU-side
 *      ADC (adc_ctrl), [5] baseband ADC, [4] codec, [3] audio interface, [2]
 *      FM, [1] physical layer, [0] protocol layer. "Writing 0 ... indicates a
 *      valid reset, automatically releasing the soft reset register value to
 *      1" (section 4.1.2.2), so the register always reads 0x1ff and a 0 bit
 *      is a reset pulse:
 *        - [7] resets every module, the CPU included, except "the register
 *          configuration module" (section 4.1.2): this block keeps its
 *          registers, whose reset mode is H, hardware reset only. Stock writes
 *          0xfe7f ([8] and [7]) to reboot (flash_quad_enable_and_system_reset
 *          @0x0305ef30).
 *        - [8] alone resets the CPU, which restarts at the reset vector.
 *        - [6] resets the ADC model (dm32_adc), through the device registered
 *          with dm32_sysreg_soft_reset_target().
 *        - [5:0] reset modules that have no model; the pulse is logged under
 *          -d unimp. Stock pulses them at boot and at radio mode changes.
 *      Assumption: the release is immediate; the datasheet gives no duration
 *      and stock never polls the register.
 *
 * A machine reset (power-on, NRESET or the watchdog, which the datasheet
 * calls global resets) returns the block to its reset values.
 *
 * Not modelled: the clock manager has no PLL state and no invalid-divider
 * check, and its registers other than +0x04 are storage. The FM subsystem
 * has no model: its registers inside this window, FM_SQL for example, are
 * ordinary storage. The baseband has no PLL, 4FSK or vocoder model, and
 * covers only the timing and control flow in item (2).
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "system/runstate.h"
#include "hw/csky/dm32.h"

#define TYPE_DM32_SYSREG "dm32_sysreg"
OBJECT_DECLARE_SIMPLE_TYPE(DM32SysregState, DM32_SYSREG)

#define WIN 0x10000

/* CLK_MGR_REG0x04, the PLL lock register (data/registers/HR_C7000-DM32.svd). */
#define OFF_CLK_MGR_REG04  0x04

/*
 * Modem-block offsets (datasheet register table, verified against the
 * firmware trace).
 */
/* Pad control: IO_DIPLEX0-2, then IOMGR_REN_REG0-2 and IOMGR_IE_IE0-2. */
#define OFF_IO_DIPLEX0     0x34
/* Pin mux bank 1; SPI0 sclk, mosi and miso at [8:3]. */
#define OFF_IO_DIPLEX1     0x38
/* Pin mux bank 2; ADC0..7 selects are bits 19..26. */
#define OFF_IO_DIPLEX2     0x3c
#define OFF_IOMGR_REN0     0x40
#define OFF_IOMGR_IE2      0x54
#define OFF_LINEOUT_CTRL   0x88
#define OFF_CODEC_I2C_MUX  0x8c
#define OFF_WORK_MODE      0x100
#define OFF_INTERP_CLEAR   0x3b0
#define OFF_L2_CONTROL     0x400
#define OFF_L2_SLOTON      0x404
#define OFF_L2_TXRX_CTRL   0x408
#define OFF_L2_SLOT_CNT    0x40c
#define OFF_L2_STATUS      0x41c

#define L2_CTRL_TX_MASTER  (1u << 1)   /* active, free-running establishment */
#define L2_SLOTON_TX_ON    (1u << 0)   /* dll_tx_slot_on */
#define INTERP_TXSLOT      (1u << 0)   /* clear_time_txslot_inter */

/* The counter cycles 0..287, which is one DMR frame. */
#define SLOT_MAX           287
#define SLOT_PERIOD_NS     (30 * 1000 * 1000ULL)   /* 30 ms per slot */

struct DM32SysregState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t reg[WIN / 4];

    QEMUTimer *slot_timer;
    qemu_irq   txslot_irq;      /* to PIC source 30 */
    bool       counting;        /* slot counter running */
    /* TX-slot interrupt asserted, awaiting INTERP_CLEAR. */
    bool       irq_pending;
    uint32_t   slot_cnt;        /* current 0..287 */

    FILE      *log;             /* DM32_MODEM_LOG, or NULL */
    /* DM32_MODEM_LEADER: force active establish once. */
    bool       leader_selftest;
    bool       leader_armed;

    /* The next machine reset is SYS_SOFT_RSTN[7]: keep the registers. */
    bool       soft_reset;
};

static void mlog(DM32SysregState *s, const char *fmt, ...)
    G_GNUC_PRINTF(2, 3);
static void mlog(DM32SysregState *s, const char *fmt, ...)
{
    if (!s->log) {
        return;
    }
    va_list ap;

    va_start(ap, fmt);
    vfprintf(s->log, fmt, ap);
    va_end(ap);
    fflush(s->log);
}

static void dm32_slot_rearm(DM32SysregState *s)
{
    if (s->counting) {
        timer_mod(s->slot_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SLOT_PERIOD_NS);
    }
}

static void dm32_slot_start(DM32SysregState *s)
{
    if (s->counting) {
        return;
    }
    s->counting = true;
    s->slot_cnt = 0;
    mlog(s, "[modem] slot counter START (CONTROL=0x%x SLOTON=0x%x)\n",
         s->reg[OFF_L2_CONTROL / 4], s->reg[OFF_L2_SLOTON / 4]);
    dm32_slot_rearm(s);
}

static void dm32_slot_stop(DM32SysregState *s)
{
    if (!s->counting) {
        return;
    }
    s->counting = false;
    timer_del(s->slot_timer);
    mlog(s, "[modem] slot counter STOP\n");
}

static void dm32_slot_tick(void *opaque)
{
    DM32SysregState *s = opaque;

    s->slot_cnt++;
    if (s->slot_cnt > SLOT_MAX) {
        s->slot_cnt = 0;
        /*
         * A full DMR frame has elapsed: raise the held-level TX-slot
         * interrupt.
         */
        s->irq_pending = true;
        qemu_set_irq(s->txslot_irq, 1);
        mlog(s, "[modem] slot wrap -> TX-slot IRQ raised (t=%" PRId64 "ns)\n",
             qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
    dm32_slot_rearm(s);
}

static uint32_t dm32_sysreg_read_reg(DM32SysregState *s, hwaddr off)
{

    if (off + 4 > WIN) {
        return 0;
    }
    uint32_t v = s->reg[off / 4];

    if (off == OFF_CLK_MGR_REG04) {
        /* report the PLL as locked and ready */
        v = (v & 0x3fffffff) | 0xc0000000;
    } else if (off == OFF_LINEOUT_CTRL) {
        /*
         * standby_lo [31], read-only, reset 1: the codec's DAC standby
         * (v2.7 EN 4.6.5.5), from dm32_codec.
         */
        v = (v & 0x3) | (dm32_codec_standby_lo() ? 0x80000000u : 0);
    } else if (off == OFF_L2_STATUS) {
        /*
         * Live slot counter in the low bits. Keep any status bits set by
         * the firmware.
         */
        v = (v & ~0x1ffu) | (s->slot_cnt & 0x1ff);
    }
    return v;
}

/* Users of the pad state, told after every pad-control write. */
static NotifierList dm32_pad_notifiers =
    NOTIFIER_LIST_INITIALIZER(dm32_pad_notifiers);

#define OFF_SOFT_RSTN      0x00
#define SOFT_RSTN_BITS     9
#define SOFT_RSTN_RESET    0x1ffu
#define SOFT_RSTN_SYS      7
#define SOFT_RSTN_CPU      8

static const char *const dm32_soft_rstn_name[SOFT_RSTN_BITS] = {
    "protocol layer", "physical layer", "FM", "audio interface", "codec",
    "baseband ADC", "CPU-side ADC", "system", "CPU",
};

/* The model each SYS_SOFT_RSTN bit resets, where one exists. */
static DeviceState *dm32_soft_rstn_dev[SOFT_RSTN_BITS];

void dm32_sysreg_soft_reset_target(int bit, DeviceState *dev)
{
    assert(bit >= 0 && bit < SOFT_RSTN_BITS);
    dm32_soft_rstn_dev[bit] = dev;
}

static void dm32_soft_rstn_write(DM32SysregState *s, uint32_t val)
{
    uint32_t pulse = ~val & SOFT_RSTN_RESET;
    int bit;

    s->reg[OFF_SOFT_RSTN / 4] = SOFT_RSTN_RESET;
    if (!pulse) {
        return;
    }
    mlog(s, "[sysreg] SYS_SOFT_RSTN <- 0x%x (reset pulse 0x%x)\n", val, pulse);
    if (pulse & BIT(SOFT_RSTN_SYS)) {
        /* Everything, the CPU included, except this block's registers. */
        s->soft_reset = true;
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        return;
    }
    for (bit = 0; bit < SOFT_RSTN_SYS; bit++) {
        if (!(pulse & BIT(bit))) {
            continue;
        }
        if (dm32_soft_rstn_dev[bit]) {
            device_cold_reset(dm32_soft_rstn_dev[bit]);
        } else {
            qemu_log_mask(LOG_UNIMP, "dm32_sysreg: SYS_SOFT_RSTN[%d] (%s) "
                          "reset: module not modelled\n", bit,
                          dm32_soft_rstn_name[bit]);
        }
    }
    if (pulse & BIT(SOFT_RSTN_CPU)) {
        dm32_cpu_soft_reset();
    }
}

static void dm32_sysreg_write_reg(DM32SysregState *s, hwaddr off,
                                  uint64_t val)
{

    if (off + 4 > WIN) {
        return;
    }
    if (off == OFF_SOFT_RSTN) {
        dm32_soft_rstn_write(s, val);
        return;
    }
    s->reg[off / 4] = val;
    if (off >= OFF_IO_DIPLEX0 && off < OFF_IOMGR_IE2 + 4) {
        notifier_list_notify(&dm32_pad_notifiers, NULL);
    }

    if (s->log && (off == OFF_L2_CONTROL || off == OFF_L2_SLOTON ||
                   off == OFF_WORK_MODE || off == OFF_L2_TXRX_CTRL)) {
        const char *n = off == OFF_L2_CONTROL ? "L2_CONTROL" :
                        off == OFF_L2_SLOTON  ? "L2_SLOTON"  :
                        off == OFF_WORK_MODE  ? "WORK_MODE"  : "L2_TXRX_CTRL";

        mlog(s, "[modem] WRITE %s(+0x%03x) = 0x%08x\n", n, (unsigned)off,
             (unsigned)val);
    }

    if (s->leader_selftest && !s->leader_armed && off == OFF_WORK_MODE &&
        (val & 0x18)) {
        /*
         * One-shot: emulate a beacon leader forcing active free-run
         * establishment.
         */
        s->leader_armed = true;
        s->reg[OFF_L2_CONTROL / 4] = 0xcb;   /* tx_master_mode=1 */
        s->reg[OFF_L2_SLOTON / 4] = 0x05;    /* dll_tx_slot_on=1 */
        mlog(s, "[modem] LEADER self-test: forcing active establish "
             "(CONTROL=0xcb SLOTON=0x5)\n");
        dm32_slot_start(s);
    }

    switch (off) {
    case OFF_L2_SLOTON: {
        /*
         * Start the counter when TX slot-on is set and the mode is active
         * (tx_master_mode). Passive, RX-slaved establishment (tx_master_mode=0)
         * is not started here, because it would begin on an RX-slot event,
         * which this timing model does not drive. The active leader path is
         * the one FHSS uses, so that is the case the model runs.
         */
        bool tx_on = (val & L2_SLOTON_TX_ON) != 0;
        bool active = (s->reg[OFF_L2_CONTROL / 4] & L2_CTRL_TX_MASTER) != 0;

        if (s->leader_armed) {
            /*
             * Self-test: keep the leader establishment latched despite the
             * firmware's boot-time SLOTON churn, so that the free-run
             * mechanics can be observed to completion.
             */
            break;
        }
        if (tx_on && active) {
            dm32_slot_start(s);
        } else if (!tx_on) {
            dm32_slot_stop(s);
        }
        break;
    }
    case OFF_INTERP_CLEAR:
        if (val & INTERP_TXSLOT) {
            if (s->irq_pending) {
                s->irq_pending = false;
                /* Deassert the held level. */
                qemu_set_irq(s->txslot_irq, 0);
                mlog(s, "[modem] INTERP_CLEAR TX-slot -> IRQ cleared\n");
            }
        }
        break;
    case OFF_L2_TXRX_CTRL: {
        /*
         * On-air command. 0x40 or 0x41 while a TX slot is established is
         * the keying trigger.
         */
        uint32_t cmd = val & 0xff;

        if ((cmd == 0x40 || cmd == 0x41) && s->counting) {
            mlog(s, "[modem] *** TX-ORIGINATE: COMMAND=0x%02x at slot=%u ***\n",
                 cmd, s->slot_cnt);
        }
        break;
    }
    default:
        break;
    }
}

/*
 * Pin-mux queries for other device models. The ADC needs to know whether its
 * pads have been handed over from GPIO before it reports a conversion; see
 * dm32_adc.c. A single global is sufficient, because the board instantiates
 * exactly one sysreg block.
 */
static DM32SysregState *dm32_sysreg_singleton;

bool dm32_codec_mc_selected(void)
{
    /* CODEC_I2C_MUX codec_si2c_reg [7]: 0 MC interface (v2.7 EN 4.6.5.6) */
    return !dm32_sysreg_singleton ||
           !(dm32_sysreg_singleton->reg[OFF_CODEC_I2C_MUX / 4] & 0x80);
}

/*
 * Pad control (HR_C7000 v2.7 sections 4.4.4.4-4.4.4.12).
 *
 * IO_DIPLEX0-2 (+0x34, +0x38, +0x3c) select each multiplexed pad's function.
 * Every select field's all-ones value is the pad's GPIO function, except the
 * JTAG selects, whose GPIO value is also 1 but which reset to 0 (JTAG). The
 * field positions and the GPIO line each selects are the SVD's (dm32uv-re
 * data/registers/HR_C7000-DM32.svd, IO_DIPLEX0-2), with one correction: the
 * guide gives LCD_DB7's GPIO as PTC24, which DAC_VOUT_MCUB also claims; the
 * DB7 pad is PTC14 (dm32uv-re specs/dts/dm32uv.dts names GPIOC 12-14 as
 * LCD_DB5-7).
 *
 * IOMGR_REN_REG0-2 (+0x40, +0x44, +0x48) enable each pad's pull resistor, one
 * bit per pad, 0 = enabled. Whether a pad's resistor pulls up or down is fixed
 * per pad (sections 4.4.4.7-4.4.4.9; the English and Chinese editions agree on
 * every pad). IOMGR_IE_IE0-2 (+0x4c, +0x50, +0x54) enable each pad's input,
 * 1 = enabled (sections 4.4.4.10-4.4.4.12). The SVD names +0x40 and +0x48
 * RESERVED; the datasheet's register table and descriptions are followed.
 *
 * The UART0 and NFC (program-flash) pads have REN and IE bits but no GPIO
 * line, so they have no entry here. LCD_NRESET's bits are taken to be pad
 * PTC2. Assumption: the datasheet names the pad, not its GPIO line; the DTS
 * names GPIOC 2 LCD_RESET_N.
 */

typedef struct DM32PadField {
    uint8_t off;            /* register offset; 0 = no such field */
    uint8_t lsb;
    uint8_t width;
    uint8_t gpio;           /* select value of the GPIO function */
} DM32PadField;

typedef struct DM32PadBit {
    uint8_t off;            /* register offset; 0 = no such bit */
    uint8_t bit;
} DM32PadBit;

typedef struct DM32PadDesc {
    const char *name;       /* the pad's non-GPIO signal */
    DM32PadField mux;
    DM32PadBit ren;
    DM32PadPull pull;
    DM32PadBit ie;
} DM32PadDesc;

static const DM32PadDesc dm32_pads[DM32_NPADS] = {
    [DM32_PTA(0)] = {
        .name = "JTG_TMS",
        .mux = { 0x34, 0, 1, 1 },
        .ren = { 0x40, 0 }, .pull = DM32_PULL_UP,
        .ie = { 0x4c, 0 },
    },
    [DM32_PTA(1)] = {
        .name = "JTG_TCK",
        .mux = { 0x34, 1, 1, 1 },
        .ren = { 0x40, 1 }, .pull = DM32_PULL_UP,
        .ie = { 0x4c, 1 },
    },
    [DM32_PTA(2)] = {
        .name = "JTG_TDI",
        .mux = { 0x34, 2, 1, 1 },
        .ren = { 0x40, 2 }, .pull = DM32_PULL_DOWN,
        .ie = { 0x4c, 2 },
    },
    [DM32_PTA(3)] = {
        .name = "JTG_TDO",
        .mux = { 0x34, 3, 1, 1 },
        .ren = { 0x40, 3 }, .pull = DM32_PULL_DOWN,
        .ie = { 0x4c, 3 },
    },
    [DM32_PTA(4)] = {
        .name = "JTG_RST_N",
        .mux = { 0x34, 4, 1, 1 },
        .ren = { 0x40, 4 }, .pull = DM32_PULL_UP,
        .ie = { 0x4c, 4 },
    },
    [DM32_PTA(5)] = {
        .name = "I2C_SCL_0",
        .mux = { 0x34, 5, 1, 1 },
        .ren = { 0x40, 5 }, .pull = DM32_PULL_UP,
        .ie = { 0x4c, 5 },
    },
    [DM32_PTA(6)] = {
        .name = "I2C_SDA_0",
        .mux = { 0x34, 6, 1, 1 },
        .ren = { 0x40, 6 }, .pull = DM32_PULL_UP,
        .ie = { 0x4c, 6 },
    },
    [DM32_PTA(7)] = {
        .name = "I2C_SCL_1",
        .mux = { 0x34, 7, 1, 1 },
        .ren = { 0x40, 7 }, .pull = DM32_PULL_UP,
        .ie = { 0x4c, 7 },
    },
    [DM32_PTA(8)] = {
        .name = "I2C_SDA_1",
        .mux = { 0x34, 8, 1, 1 },
        .ren = { 0x40, 8 }, .pull = DM32_PULL_UP,
        .ie = { 0x4c, 8 },
    },
    [DM32_PTA(9)] = {
        .name = "UART1_RXD",
        .mux = { 0x34, 9, 1, 1 },
        .ren = { 0x40, 9 }, .pull = DM32_PULL_UP,
        .ie = { 0x4c, 9 },
    },
    [DM32_PTA(10)] = {
        .name = "UART1_TXD",
        .mux = { 0x34, 10, 1, 1 },
        .ren = { 0x40, 10 }, .pull = DM32_PULL_UP,
        .ie = { 0x4c, 10 },
    },
    [DM32_PTA(11)] = {
        .name = "UART2_RXD",
        .mux = { 0x34, 11, 1, 1 },
        .ren = { 0x40, 11 }, .pull = DM32_PULL_UP,
        .ie = { 0x4c, 11 },
    },
    [DM32_PTA(12)] = {
        .name = "UART2_TXD",
        .mux = { 0x34, 12, 1, 1 },
        .ren = { 0x40, 12 }, .pull = DM32_PULL_UP,
        .ie = { 0x4c, 12 },
    },
    [DM32_PTA(13)] = {
        .name = "UART3_RXD",
        .mux = { 0x34, 13, 2, 3 },
        .ren = { 0x40, 13 }, .pull = DM32_PULL_UP,
        .ie = { 0x4c, 13 },
    },
    [DM32_PTA(14)] = {
        .name = "UART3_TXD",
        .mux = { 0x34, 15, 2, 3 },
        .ren = { 0x40, 14 }, .pull = DM32_PULL_UP,
        .ie = { 0x4c, 14 },
    },
    [DM32_PTA(15)] = {
        .name = "PWM_0",
        .mux = { 0x34, 17, 1, 1 },
        .ren = { 0x40, 15 }, .pull = DM32_PULL_UP,
        .ie = { 0x4c, 15 },
    },
    [DM32_PTA(16)] = {
        .name = "PWM_1",
        .mux = { 0x34, 18, 1, 1 },
        .ren = { 0x40, 16 }, .pull = DM32_PULL_UP,
        .ie = { 0x4c, 16 },
    },
    [DM32_PTA(17)] = {
        .name = "PWM_2",
        .mux = { 0x34, 19, 1, 1 },
        .ren = { 0x40, 17 }, .pull = DM32_PULL_DOWN,
        .ie = { 0x4c, 17 },
    },
    [DM32_PTA(18)] = {
        .name = "SPI0_CSN_0",
        .mux = { 0x38, 0, 2, 3 },
        .ren = { 0x40, 18 }, .pull = DM32_PULL_UP,
        .ie = { 0x4c, 18 },
    },
    [DM32_PTA(19)] = {
        .name = "SPI0_CSN_1",
        .mux = { 0x38, 2, 1, 1 },
        .ren = { 0x40, 19 }, .pull = DM32_PULL_UP,
        .ie = { 0x4c, 19 },
    },
    [DM32_PTA(20)] = {
        .name = "SPI0_SCLK",
        .mux = { 0x38, 3, 2, 3 },
        .ren = { 0x40, 20 }, .pull = DM32_PULL_DOWN,
        .ie = { 0x4c, 20 },
    },
    [DM32_PTA(21)] = {
        .name = "SPI0_MOSI",
        .mux = { 0x38, 5, 2, 3 },
        .ren = { 0x40, 21 }, .pull = DM32_PULL_UP,
        .ie = { 0x4c, 21 },
    },
    [DM32_PTA(22)] = {
        .name = "SPI0_MISO",
        .mux = { 0x38, 7, 2, 3 },
        .ren = { 0x40, 22 }, .pull = DM32_PULL_DOWN,
        .ie = { 0x4c, 22 },
    },
    [DM32_PTA(23)] = {
        .name = "SPI1_CSN_0",
        .mux = { 0x38, 9, 2, 3 },
        .ren = { 0x40, 23 }, .pull = DM32_PULL_UP,
        .ie = { 0x4c, 23 },
    },
    [DM32_PTA(24)] = {
        .name = "SPI1_CSN_1",
        .mux = { 0x38, 11, 2, 3 },
        .ren = { 0x40, 24 }, .pull = DM32_PULL_UP,
        .ie = { 0x4c, 24 },
    },
    [DM32_PTA(25)] = {
        .name = "SPI1_SCLK",
        .mux = { 0x38, 13, 2, 3 },
        .ren = { 0x40, 25 }, .pull = DM32_PULL_DOWN,
        .ie = { 0x4c, 25 },
    },
    [DM32_PTA(26)] = {
        .name = "SPI1_MOSI",
        .mux = { 0x38, 15, 2, 3 },
        .ren = { 0x40, 26 }, .pull = DM32_PULL_UP,
        .ie = { 0x4c, 26 },
    },
    [DM32_PTA(27)] = {
        .name = "SPI1_MISO",
        .mux = { 0x38, 17, 2, 3 },
        .ren = { 0x40, 27 }, .pull = DM32_PULL_UP,
        .ie = { 0x4c, 27 },
    },
    [DM32_PTA(28)] = {
        .name = "BS_INTER_IN",
        .mux = { 0x38, 19, 2, 3 },
        .ren = { 0x40, 28 }, .pull = DM32_PULL_UP,
        .ie = { 0x4c, 28 },
    },
    [DM32_PTA(29)] = {
        .name = "BS_INTER_OUT",
        .mux = { 0x38, 21, 2, 3 },
        .ren = { 0x40, 29 }, .pull = DM32_PULL_UP,
        .ie = { 0x4c, 29 },
    },
    [DM32_PTA(30)] = {
        .name = "TIME_SLOT_R_INTER",
        .mux = { 0x38, 23, 2, 3 },
        .ren = { 0x40, 31 }, .pull = DM32_PULL_DOWN,
        .ie = { 0x4c, 31 },
    },
    [DM32_PTA(31)] = {
        .name = "TIME_SLOT_T_INTER",
        .mux = { 0x38, 25, 2, 3 },
        .ren = { 0x40, 30 }, .pull = DM32_PULL_DOWN,
        .ie = { 0x4c, 30 },
    },
    [DM32_PTB(0)] = {
        .name = "PTB0",
        .ren = { 0x48, 0 }, .pull = DM32_PULL_UP,
        .ie = { 0x54, 0 },
    },
    [DM32_PTB(1)] = {
        .name = "PTB1",
        .ren = { 0x48, 1 }, .pull = DM32_PULL_UP,
        .ie = { 0x54, 1 },
    },
    [DM32_PTB(2)] = {
        .name = "AK_DAC_FS",
        .mux = { 0x38, 27, 1, 1 },
        .ren = { 0x48, 2 }, .pull = DM32_PULL_UP,
        .ie = { 0x54, 2 },
    },
    [DM32_PTB(3)] = {
        .name = "AK_DAC_SCLK",
        .mux = { 0x38, 28, 1, 1 },
        .ren = { 0x48, 3 }, .pull = DM32_PULL_UP,
        .ie = { 0x54, 3 },
    },
    [DM32_PTB(4)] = {
        .name = "AK_DAC_SDI",
        .mux = { 0x38, 29, 1, 1 },
        .ren = { 0x48, 4 }, .pull = DM32_PULL_DOWN,
        .ie = { 0x54, 4 },
    },
    [DM32_PTB(5)] = {
        .name = "AK_ADC_FS",
        .mux = { 0x38, 30, 1, 1 },
        .ren = { 0x48, 5 }, .pull = DM32_PULL_DOWN,
        .ie = { 0x54, 5 },
    },
    [DM32_PTB(6)] = {
        .name = "AK_ADC_SCLK",
        .mux = { 0x38, 31, 1, 1 },
        .ren = { 0x48, 6 }, .pull = DM32_PULL_DOWN,
        .ie = { 0x54, 6 },
    },
    [DM32_PTB(7)] = {
        .name = "AK_ADC_SDO",
        .mux = { 0x34, 31, 1, 1 },
        .ren = { 0x48, 7 }, .pull = DM32_PULL_UP,
        .ie = { 0x54, 7 },
    },
    [DM32_PTB(8)] = {
        .name = "PTB8",
        .ren = { 0x48, 8 }, .pull = DM32_PULL_UP,
        .ie = { 0x54, 8 },
    },
    [DM32_PTB(9)] = {
        .name = "PTB9",
        .ren = { 0x48, 9 }, .pull = DM32_PULL_UP,
        .ie = { 0x54, 9 },
    },
    [DM32_PTB(10)] = {
        .name = "PTB10",
        .ren = { 0x48, 10 }, .pull = DM32_PULL_UP,
        .ie = { 0x54, 10 },
    },
    [DM32_PTB(11)] = {
        .name = "PTB11",
        .ren = { 0x48, 11 }, .pull = DM32_PULL_DOWN,
        .ie = { 0x54, 11 },
    },
    [DM32_PTB(12)] = {
        .name = "PTB12",
        .ren = { 0x48, 12 }, .pull = DM32_PULL_DOWN,
        .ie = { 0x54, 12 },
    },
    [DM32_PTB(13)] = {
        .name = "PTB13",
        .ren = { 0x48, 13 }, .pull = DM32_PULL_UP,
        .ie = { 0x54, 13 },
    },
    [DM32_PTB(14)] = {
        .name = "PTB14",
        .ren = { 0x48, 14 }, .pull = DM32_PULL_UP,
        .ie = { 0x54, 14 },
    },
    [DM32_PTB(15)] = {
        .name = "PTB15",
        .ren = { 0x48, 15 }, .pull = DM32_PULL_UP,
        .ie = { 0x54, 15 },
    },
    [DM32_PTB(16)] = {
        .name = "PTB16",
        .ren = { 0x48, 16 }, .pull = DM32_PULL_UP,
        .ie = { 0x54, 16 },
    },
    [DM32_PTB(17)] = {
        .name = "PTB17",
        .ren = { 0x48, 17 }, .pull = DM32_PULL_DOWN,
        .ie = { 0x54, 17 },
    },
    [DM32_PTB(18)] = {
        .name = "PTB18",
        .ren = { 0x48, 18 }, .pull = DM32_PULL_DOWN,
        .ie = { 0x54, 18 },
    },
    [DM32_PTB(19)] = {
        .name = "PTB19",
        .ren = { 0x48, 19 }, .pull = DM32_PULL_UP,
        .ie = { 0x54, 19 },
    },
    [DM32_PTB(20)] = {
        .name = "PTB20",
        .ren = { 0x48, 20 }, .pull = DM32_PULL_UP,
        .ie = { 0x54, 20 },
    },
    [DM32_PTB(21)] = {
        .name = "PTB21",
        .ren = { 0x48, 21 }, .pull = DM32_PULL_UP,
        .ie = { 0x54, 21 },
    },
    [DM32_PTB(22)] = {
        .name = "PTB22",
        .ren = { 0x48, 22 }, .pull = DM32_PULL_UP,
        .ie = { 0x54, 22 },
    },
    [DM32_PTB(23)] = {
        .name = "PTB23",
        .ren = { 0x48, 23 }, .pull = DM32_PULL_DOWN,
        .ie = { 0x54, 23 },
    },
    [DM32_PTB(24)] = {
        .name = "PTB24",
        .ren = { 0x48, 24 }, .pull = DM32_PULL_DOWN,
        .ie = { 0x54, 24 },
    },
    [DM32_PTB(25)] = {
        .name = "PTB25",
        .ren = { 0x48, 25 }, .pull = DM32_PULL_UP,
        .ie = { 0x54, 25 },
    },
    [DM32_PTB(26)] = {
        .name = "PTB26",
        .ren = { 0x48, 26 }, .pull = DM32_PULL_UP,
        .ie = { 0x54, 26 },
    },
    [DM32_PTB(27)] = {
        .name = "PTB27",
        .ren = { 0x48, 27 }, .pull = DM32_PULL_UP,
        .ie = { 0x54, 27 },
    },
    [DM32_PTB(28)] = {
        .name = "PTB28",
        .ren = { 0x48, 28 }, .pull = DM32_PULL_UP,
        .ie = { 0x54, 28 },
    },
    [DM32_PTB(29)] = {
        .name = "PTB29",
        .ren = { 0x48, 29 }, .pull = DM32_PULL_DOWN,
        .ie = { 0x54, 29 },
    },
    [DM32_PTC(0)] = {
        .name = "CLK_OUT",
        .mux = { 0x3c, 0, 2, 3 },
        .ren = { 0x48, 30 }, .pull = DM32_PULL_DOWN,
        .ie = { 0x54, 30 },
    },
    [DM32_PTC(2)] = {
        .name = "LCD_NRESET",
        .ren = { 0x44, 0 }, .pull = DM32_PULL_UP,
        .ie = { 0x50, 0 },
    },
    [DM32_PTC(3)] = {
        .name = "LCD_CS",
        .mux = { 0x3c, 3, 2, 3 },
        .ren = { 0x44, 1 }, .pull = DM32_PULL_UP,
        .ie = { 0x50, 1 },
    },
    [DM32_PTC(4)] = {
        .name = "LCD_RS",
        .mux = { 0x3c, 5, 2, 3 },
        .ren = { 0x44, 2 }, .pull = DM32_PULL_UP,
        .ie = { 0x50, 2 },
    },
    [DM32_PTC(5)] = {
        .name = "LCD_WR",
        .mux = { 0x3c, 7, 1, 1 },
        .ren = { 0x44, 3 }, .pull = DM32_PULL_UP,
        .ie = { 0x50, 3 },
    },
    [DM32_PTC(6)] = {
        .name = "LCD_RD",
        .mux = { 0x3c, 8, 1, 1 },
        .ren = { 0x44, 4 }, .pull = DM32_PULL_UP,
        .ie = { 0x50, 4 },
    },
    [DM32_PTC(7)] = {
        .name = "LCD_DB0",
        .mux = { 0x3c, 9, 2, 3 },
        .ren = { 0x44, 5 }, .pull = DM32_PULL_DOWN,
        .ie = { 0x50, 5 },
    },
    [DM32_PTC(8)] = {
        .name = "LCD_DB1",
        .mux = { 0x3c, 11, 2, 3 },
        .ren = { 0x44, 6 }, .pull = DM32_PULL_DOWN,
        .ie = { 0x50, 6 },
    },
    [DM32_PTC(9)] = {
        .name = "LCD_DB2",
        .mux = { 0x3c, 13, 1, 1 },
        .ren = { 0x44, 7 }, .pull = DM32_PULL_DOWN,
        .ie = { 0x50, 7 },
    },
    [DM32_PTC(10)] = {
        .name = "LCD_DB3",
        .mux = { 0x3c, 14, 1, 1 },
        .ren = { 0x44, 8 }, .pull = DM32_PULL_UP,
        .ie = { 0x50, 8 },
    },
    [DM32_PTC(11)] = {
        .name = "LCD_DB4",
        .mux = { 0x3c, 15, 1, 1 },
        .ren = { 0x44, 9 }, .pull = DM32_PULL_UP,
        .ie = { 0x50, 9 },
    },
    [DM32_PTC(12)] = {
        .name = "LCD_DB5",
        .mux = { 0x3c, 16, 1, 1 },
        .ren = { 0x44, 10 }, .pull = DM32_PULL_UP,
        .ie = { 0x50, 10 },
    },
    [DM32_PTC(13)] = {
        .name = "LCD_DB6",
        .mux = { 0x3c, 17, 1, 1 },
        .ren = { 0x44, 11 }, .pull = DM32_PULL_UP,
        .ie = { 0x50, 11 },
    },
    [DM32_PTC(14)] = {
        .name = "LCD_DB7",
        .mux = { 0x3c, 18, 1, 1 },
        .ren = { 0x44, 12 }, .pull = DM32_PULL_DOWN,
        .ie = { 0x50, 12 },
    },
    [DM32_PTC(15)] = {
        .name = "ADC0_IN",
        .mux = { 0x3c, 19, 1, 1 },
        .ren = { 0x44, 13 }, .pull = DM32_PULL_DOWN,
        .ie = { 0x50, 13 },
    },
    [DM32_PTC(16)] = {
        .name = "ADC1_IN",
        .mux = { 0x3c, 20, 1, 1 },
        .ren = { 0x44, 14 }, .pull = DM32_PULL_DOWN,
        .ie = { 0x50, 14 },
    },
    [DM32_PTC(17)] = {
        .name = "ADC2_IN",
        .mux = { 0x3c, 21, 1, 1 },
        .ren = { 0x44, 15 }, .pull = DM32_PULL_DOWN,
        .ie = { 0x50, 15 },
    },
    [DM32_PTC(18)] = {
        .name = "ADC3_IN",
        .mux = { 0x3c, 22, 1, 1 },
        .ren = { 0x44, 16 }, .pull = DM32_PULL_DOWN,
        .ie = { 0x50, 16 },
    },
    [DM32_PTC(19)] = {
        .name = "ADC4_IN",
        .mux = { 0x3c, 23, 1, 1 },
        .ren = { 0x44, 17 }, .pull = DM32_PULL_DOWN,
        .ie = { 0x50, 17 },
    },
    [DM32_PTC(20)] = {
        .name = "ADC5_IN",
        .mux = { 0x3c, 24, 1, 1 },
        .ren = { 0x44, 18 }, .pull = DM32_PULL_DOWN,
        .ie = { 0x50, 18 },
    },
    [DM32_PTC(21)] = {
        .name = "ADC6_IN",
        .mux = { 0x3c, 25, 1, 1 },
        .ren = { 0x44, 19 }, .pull = DM32_PULL_DOWN,
        .ie = { 0x50, 19 },
    },
    [DM32_PTC(22)] = {
        .name = "ADC7_IN",
        .mux = { 0x3c, 26, 1, 1 },
        .ren = { 0x44, 20 }, .pull = DM32_PULL_DOWN,
        .ie = { 0x50, 20 },
    },
    [DM32_PTC(23)] = {
        .name = "DAC_VOUT_MCUA",
        .mux = { 0x3c, 27, 1, 1 },
        .ren = { 0x44, 21 }, .pull = DM32_PULL_UP,
        .ie = { 0x50, 21 },
    },
    [DM32_PTC(24)] = {
        .name = "DAC_VOUT_MCUB",
        .mux = { 0x3c, 28, 1, 1 },
        .ren = { 0x44, 22 }, .pull = DM32_PULL_UP,
        .ie = { 0x50, 22 },
    },
    [DM32_PTC(25)] = {
        .name = "DAC_VOUT_MCUC",
        .mux = { 0x3c, 29, 1, 1 },
        .ren = { 0x44, 23 }, .pull = DM32_PULL_UP,
        .ie = { 0x50, 23 },
    },
};


static uint32_t dm32_pad_reg(uint8_t off)
{
    return dm32_sysreg_singleton ? dm32_sysreg_singleton->reg[off / 4] : 0;
}

int dm32_pad_function(unsigned pad)
{
    const DM32PadField *f;

    if (pad >= DM32_NPADS || !dm32_pads[pad].mux.off) {
        return -1;
    }
    f = &dm32_pads[pad].mux;
    return (dm32_pad_reg(f->off) >> f->lsb) & ((1u << f->width) - 1);
}

const char *dm32_pad_name(unsigned pad)
{
    if (pad >= DM32_NPADS || !dm32_pads[pad].name) {
        return "GPIO";
    }
    return dm32_pads[pad].name;
}

bool dm32_pad_is_gpio(unsigned pad)
{
    int fn = dm32_pad_function(pad);

    return fn < 0 || fn == dm32_pads[pad].mux.gpio;
}

DM32PadPull dm32_pad_pull(unsigned pad)
{
    const DM32PadBit *b;

    if (pad >= DM32_NPADS || !dm32_pads[pad].ren.off) {
        return DM32_PULL_NONE;
    }
    b = &dm32_pads[pad].ren;
    if (dm32_pad_reg(b->off) & (1u << b->bit)) {
        return DM32_PULL_NONE;
    }
    return dm32_pads[pad].pull;
}

bool dm32_pad_input_enabled(unsigned pad)
{
    const DM32PadBit *b;

    if (pad >= DM32_NPADS || !dm32_pads[pad].ie.off) {
        return true;
    }
    b = &dm32_pads[pad].ie;
    return (dm32_pad_reg(b->off) >> b->bit) & 1;
}

void dm32_pad_add_notifier(Notifier *n)
{
    notifier_list_add(&dm32_pad_notifiers, n);
}

/* Registers are 32 bits; narrow accesses act on the containing register. */
static uint64_t dm32_sysreg_read(void *opaque, hwaddr addr, unsigned size)
{
    return dm32_reg_lane_read(dm32_sysreg_read_reg(opaque, addr & ~3), addr,
                              size);
}

static void dm32_sysreg_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    DM32SysregState *s = opaque;
    hwaddr off = addr & ~3;

    if (size < 4 && off + 4 <= WIN) {
        val = dm32_reg_lane_write(s->reg[off / 4], addr, size, val);
    }
    dm32_sysreg_write_reg(s, off, val);
}

static const MemoryRegionOps dm32_sysreg_ops = {
    .read = dm32_sysreg_read,
    .write = dm32_sysreg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void dm32_sysreg_reset(DeviceState *dev)
{
    DM32SysregState *s = DM32_SYSREG(dev);

    timer_del(s->slot_timer);
    s->counting = false;
    s->slot_cnt = 0;
    if (s->irq_pending) {
        s->irq_pending = false;
        qemu_set_irq(s->txslot_irq, 0);
    }
    s->leader_armed = false;
    if (s->soft_reset) {
        /* SYS_SOFT_RSTN[7]: the register configuration module is spared. */
        s->soft_reset = false;
        return;
    }
    memset(s->reg, 0, sizeof(s->reg));
    s->reg[OFF_SOFT_RSTN / 4] = SOFT_RSTN_RESET;

    /*
     * IO_DIPLEX2 reset value 0x3ffffffb (v2.7 EN 4.4.4.6; SVD): every muxed
     * pad on its GPIO function, bit 2 reserved. Stock claims the LCD bus with
     * IO_DIPLEX2 &= 0xfff80107 (lcd_controller_power_on_init_sequence
     * @0x03017d86) and selects CLK_OUT (bits 1:0 = 0), which gives the
     * 0x3ff80100 read on hardware after boot.
     */
    s->reg[OFF_IO_DIPLEX2 / 4] = 0x3ffffffbu;

    /*
     * IO_DIPLEX1 reset value. Every mux field here also resets to its GPIO
     * selection, including SPI0's sclk, mosi and miso ([8:3], each 0x3, which
     * is GPIO). A zeroed reg[] would claim that all three pads are already
     * handed to the SPI0 master. That would let a driver that never claims
     * SPI0's pads appear to work in emulation and read all-zero on silicon.
     * 0xffffffff sets every select to its highest, GPIO, option, which matches
     * the documented defaults for the fields this model cares about.
     */
    s->reg[OFF_IO_DIPLEX1 / 4] = 0xffffffffu;

    /*
     * IO_DIPLEX0 reset value 0x800fffe0 (v2.7 EN 4.4.4.4; SVD): the JTAG
     * selects [4:0] reset to 0 (JTAG), the I2C, UART and PWM selects and PTB7
     * to their GPIO values.
     */
    s->reg[OFF_IO_DIPLEX0 / 4] = 0x800fffe0u;

    /*
     * IOMGR_REN_REG0-2 and IOMGR_IE_IE0-2 reset with every defined bit 1
     * (v2.7 EN 4.4.4.7-4.4.4.12): all pull resistors off, all inputs enabled.
     * Bit 31 of REN_REG2 and IE_IE2 is reserved and reads 0.
     */
    s->reg[(OFF_IOMGR_REN0 + 0x0) / 4] = 0xffffffffu;
    s->reg[(OFF_IOMGR_REN0 + 0x4) / 4] = 0xffffffffu;
    s->reg[(OFF_IOMGR_REN0 + 0x8) / 4] = 0x7fffffffu;
    s->reg[(OFF_IOMGR_REN0 + 0xc) / 4] = 0xffffffffu;
    s->reg[(OFF_IOMGR_REN0 + 0x10) / 4] = 0xffffffffu;
    s->reg[(OFF_IOMGR_REN0 + 0x14) / 4] = 0x7fffffffu;

    notifier_list_notify(&dm32_pad_notifiers, NULL);
}

static void dm32_sysreg_init(Object *obj)
{
    DM32SysregState *s = DM32_SYSREG(obj);
    const char *logpath = getenv("DM32_MODEM_LOG");

    dm32_sysreg_singleton = s;

    memory_region_init_io(&s->iomem, obj, &dm32_sysreg_ops, s,
                          TYPE_DM32_SYSREG, WIN);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->txslot_irq);

    s->slot_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dm32_slot_tick, s);
    if (logpath && *logpath) {
        s->log = fopen(logpath, "w");
        if (!s->log) {
            error_report("DM32_MODEM_LOG=%s: cannot open: %s", logpath,
                         strerror(errno));
        }
    }
    s->leader_selftest = dm32_env_flag("DM32_MODEM_LEADER");
    if (s->log) {
        mlog(s, "[modem] DM32_MODEM_LOG active; slot=30ms cycle 0..%d, "
             "TX-slot IRQ->PIC src30\n", SLOT_MAX);
    }
    dm32_sysreg_reset(DEVICE(obj));
}

static void dm32_sysreg_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), dm32_sysreg_reset);
}

static const TypeInfo dm32_sysreg_type = {
    .name = TYPE_DM32_SYSREG,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DM32SysregState),
    .instance_init = dm32_sysreg_init,
    .class_init = dm32_sysreg_class_init,
};

static void dm32_sysreg_register(void)
{
    type_register_static(&dm32_sysreg_type);
}

type_init(dm32_sysreg_register)
