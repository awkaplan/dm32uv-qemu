/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Baofeng DM-32UV RDA5807M broadcast-FM tuner: register-level model
 *
 * Copyright (c) 2026 Alex Kaplan
 *
 * RDA5807M datasheet v1.0 (RDA Microelectronics, May 2011): 2.4-2.5 control
 * interface (p.5), 6 register definition (p.9-13). dm32uv-re
 * specs/rdl/rda5807m.rdl describes the registers and the stock driver. The
 * chip sits on the bit-banged "fm" bus, SCL GPIOA 26 and SDA GPIOA 25, at
 * 7-bit address 0x10 (command bytes 0x20 write, 0x21 read).
 *
 * Bus protocol (2.5): no register address is sent. A write transfer starts at
 * register 02H, a read transfer at 0AH; each register goes high byte first and
 * an internal counter then moves to the next, wrapping from 3AH to 00H. Every
 * byte is acknowledged. Assumption: a register takes the written value when
 * its low byte arrives, and a read latches a register when its high byte is
 * sent.
 *
 * Registers, reset values from section 6 (16 bits, reset 0 unless noted):
 *   00H CHIPID [15:8] 0x58, read-only (assumption: the undocumented low byte
 *       is 0)
 *   02H DHIZ [15], DMUTE [14], MONO [13], BASS [12], SEEKUP [9], SEEK [8],
 *       SKMODE [7], CLK_MODE [6:4], RDS_EN [3], NEW_METHOD [2], SOFT_RESET
 *       [1], ENABLE [0]
 *   03H CHAN [15:6], DIRECT_MODE [5], TUNE [4], BAND [3:2], SPACE [1:0]
 *   04H reset 0x0200 (SOFTMUTE_EN)   05H reset 0x880f (SEEKTH 8, VOLUME 15)
 *   06H OPEN_MODE [14:13]            07H reset 0x4202
 *   0AH STC [14], SF [13], ST [10], READCHAN [9:0]   read-only status
 *   0BH RSSI [15:9], FM_TRUE [8], FM_READY [7]      read-only status
 *   0CH-0FH RDS blocks, resets 0x5803, 0x5804, 0x5808, 0x5804, read-only
 *   10H-3AH hidden registers, writable only while 06H OPEN_MODE is 11
 * Other registers (01H, 08H, 09H) are storage.
 *
 * Tune and seek. A channel is spacing * CHAN above the band base: BAND 00
 * 87-108 MHz, 01 76-91, 10 76-108, 11 65-76 (07H bit 9 = 1) or 50-76 (0);
 * SPACE 00 100 kHz, 01 200, 10 50, 11 25. With ENABLE set, TUNE = 1 tunes to
 * CHAN: TUNE clears, STC sets and READCHAN = CHAN. SEEK = 1 searches from
 * CHAN up (SEEKUP 1) or down for the next station: at a band edge it wraps
 * (SKMODE 0) or stops (1); finding nothing sets SF. Either way SEEK clears and
 * STC sets. Starting a tune or seek clears STC and SF. ENABLE = 0 (stock's
 * power-down writes 02H = 0x8000) cancels an operation, and 0AH and 0BH then
 * read 0. A 0 -> 1 edge of SOFT_RESET returns every register to its reset
 * value (2.4).
 *
 * The RF input is configured: DM32_FM_STATIONS="kHz[:rssi],..." lists the
 * stations (RSSI default 60); every other channel reads DM32_FM_RSSI
 * (default 0). FM_TRUE is set on a listed station, ST on a station while MONO
 * is clear, FM_READY while enabled and idle.
 *
 * Assumptions (the datasheet gives no times): a tune takes 50 ms; a seek
 * takes 10 ms per channel stepped; the seek starts from the CHAN value at its
 * first step, so a transfer that writes 02H then 03H seeks from the new CHAN,
 * as stock's seek does; SEEKTH is not compared (a seek stops at any listed
 * station); a failed seek leaves READCHAN at the channel it stopped on. Not
 * modelled: audio, RDS, CLK_MODE, the de-emphasis and blend settings.
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "qemu/cutils.h"
#include "hw/i2c/i2c.h"
#include "hw/csky/dm32.h"

#define TYPE_DM32_RDA5807M "dm32_rda5807m"
OBJECT_DECLARE_SIMPLE_TYPE(DM32Rda5807mState, DM32_RDA5807M)

#define RDA_ADDR7       0x10
#define RDA_NREGS       0x3b            /* 00H-3AH, then the counter wraps */
#define RDA_WRITE_FIRST 0x02
#define RDA_READ_FIRST  0x0a

#define R02_DHIZ        (1u << 15)
#define R02_MONO        (1u << 13)
#define R02_SEEKUP      (1u << 9)
#define R02_SEEK        (1u << 8)
#define R02_SKMODE      (1u << 7)
#define R02_SOFT_RESET  (1u << 1)
#define R02_ENABLE      (1u << 0)
#define R03_TUNE        (1u << 4)
#define R0A_STC         (1u << 14)
#define R0A_SF          (1u << 13)
#define R0A_ST          (1u << 10)

#define RDA_TUNE_NS     (50 * SCALE_MS)
#define RDA_STEP_NS     (10 * SCALE_MS)
#define RDA_MAX_STATIONS 64

typedef enum {
    RDA_IDLE,
    RDA_TUNING,
    RDA_SEEKING,
} DM32RdaOp;

struct DM32Rda5807mState {
    I2CSlave parent_obj;
    QEMUTimer *timer;

    uint16_t regs[RDA_NREGS];
    uint8_t ptr;                /* the register counter */
    bool high;                  /* the next byte is a high byte */
    uint8_t hi_byte;            /* a written high byte awaiting its low byte */
    uint16_t latch;             /* the register being read */

    DM32RdaOp op;
    bool seek_started;          /* the seek has taken its first step */
    uint16_t chan;              /* the seek's current channel */
    uint16_t start_chan;
    uint16_t readchan;
    bool stc, sf;

    uint32_t station_khz[RDA_MAX_STATIONS];
    uint8_t station_rssi[RDA_MAX_STATIONS];
    int nstations;
    uint8_t noise_rssi;
    bool log;
};

static const uint16_t rda_reset[RDA_NREGS] = {
    [0x00] = 0x5800,
    [0x04] = 0x0200,
    [0x05] = 0x880f,
    [0x07] = 0x4202,
    [0x0c] = 0x5803, [0x0d] = 0x5804, [0x0e] = 0x5808, [0x0f] = 0x5804,
};

static bool rda_enabled(DM32Rda5807mState *s)
{
    return s->regs[0x02] & R02_ENABLE;
}

/* Band edges in kHz for the current 03H BAND and 07H bit 9. */
static void rda_band(DM32Rda5807mState *s, uint32_t *lo, uint32_t *hi)
{
    switch ((s->regs[0x03] >> 2) & 3) {
    case 0:
        *lo = 87000;
        *hi = 108000;
        break;
    case 1:
        *lo = 76000;
        *hi = 91000;
        break;
    case 2:
        *lo = 76000;
        *hi = 108000;
        break;
    default:
        *lo = (s->regs[0x07] & (1u << 9)) ? 65000 : 50000;
        *hi = 76000;
        break;
    }
}

static uint32_t rda_space_khz(DM32Rda5807mState *s)
{
    static const uint32_t space[4] = { 100, 200, 50, 25 };

    return space[s->regs[0x03] & 3];
}

static uint32_t rda_chan_khz(DM32Rda5807mState *s, uint16_t chan)
{
    uint32_t lo, hi;

    rda_band(s, &lo, &hi);
    return lo + chan * rda_space_khz(s);
}

static uint16_t rda_max_chan(DM32Rda5807mState *s)
{
    uint32_t lo, hi;

    rda_band(s, &lo, &hi);
    return MIN((hi - lo) / rda_space_khz(s), 0x3ff);
}

static int rda_station(DM32Rda5807mState *s, uint16_t chan)
{
    uint32_t khz = rda_chan_khz(s, chan);
    int i;

    for (i = 0; i < s->nstations; i++) {
        if (s->station_khz[i] == khz) {
            return i;
        }
    }
    return -1;
}

static uint16_t rda_status_0a(DM32Rda5807mState *s)
{
    uint16_t v;
    int st;

    if (!rda_enabled(s)) {
        return 0;
    }
    st = rda_station(s, s->readchan);
    v = (s->stc ? R0A_STC : 0) | (s->sf ? R0A_SF : 0) | (s->readchan & 0x3ff);
    if (st >= 0 && !(s->regs[0x02] & R02_MONO) && s->op == RDA_IDLE) {
        v |= R0A_ST;
    }
    return v;
}

static uint16_t rda_status_0b(DM32Rda5807mState *s)
{
    int st;
    uint8_t rssi;

    if (!rda_enabled(s)) {
        return 0;
    }
    st = rda_station(s, s->readchan);
    rssi = st >= 0 ? s->station_rssi[st] : s->noise_rssi;
    return (rssi & 0x7f) << 9 | (st >= 0 ? 1u << 8 : 0) |
           (s->op == RDA_IDLE ? 1u << 7 : 0);
}

static void rda_finish(DM32Rda5807mState *s, bool failed)
{
    if (s->op == RDA_TUNING) {
        s->regs[0x03] &= ~R03_TUNE;
    } else {
        s->regs[0x02] &= ~R02_SEEK;
    }
    s->op = RDA_IDLE;
    s->stc = true;
    s->sf = failed;
    if (s->log) {
        fprintf(stderr, "dm32_rda5807m: %s at %u kHz%s\n",
                failed ? "seek failed" : "tuned", rda_chan_khz(s, s->readchan),
                rda_station(s, s->readchan) >= 0 ? " (station)" : "");
    }
}

static void rda_timer(void *opaque)
{
    DM32Rda5807mState *s = opaque;
    uint16_t max;

    if (s->op == RDA_TUNING) {
        s->readchan = (s->regs[0x03] >> 6) & 0x3ff;
        rda_finish(s, false);
        return;
    }
    if (s->op != RDA_SEEKING) {
        return;
    }
    max = rda_max_chan(s);
    if (!s->seek_started) {
        s->chan = MIN((s->regs[0x03] >> 6) & 0x3ff, max);
        s->start_chan = s->chan;
        s->seek_started = true;
    }
    if (s->regs[0x02] & R02_SEEKUP) {
        if (s->chan >= max) {
            if (s->regs[0x02] & R02_SKMODE) {
                s->readchan = s->chan;
                rda_finish(s, true);
                return;
            }
            s->chan = 0;
        } else {
            s->chan++;
        }
    } else {
        if (s->chan == 0) {
            if (s->regs[0x02] & R02_SKMODE) {
                s->readchan = s->chan;
                rda_finish(s, true);
                return;
            }
            s->chan = max;
        } else {
            s->chan--;
        }
    }
    s->readchan = s->chan;
    if (rda_station(s, s->chan) >= 0) {
        rda_finish(s, false);
    } else if (s->chan == s->start_chan) {
        rda_finish(s, true);            /* the whole band, nothing found */
    } else {
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  RDA_STEP_NS);
    }
}

static void rda_start(DM32Rda5807mState *s, DM32RdaOp op)
{
    s->op = op;
    s->stc = false;
    s->sf = false;
    s->seek_started = false;
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              (op == RDA_TUNING ? RDA_TUNE_NS : RDA_STEP_NS));
}

static void rda_cancel(DM32Rda5807mState *s)
{
    timer_del(s->timer);
    s->op = RDA_IDLE;
}

static void rda_reset_regs(DM32Rda5807mState *s)
{
    memcpy(s->regs, rda_reset, sizeof(s->regs));
    rda_cancel(s);
    s->readchan = 0;
    s->stc = s->sf = false;
}

static void rda_write_reg(DM32Rda5807mState *s, uint8_t reg, uint16_t v)
{
    uint16_t old = s->regs[reg];

    switch (reg) {
    case 0x00:
    case 0x0a ... 0x0f:
        return;                         /* read-only */
    case 0x02:
        if ((v & R02_SOFT_RESET) && !(old & R02_SOFT_RESET)) {
            rda_reset_regs(s);
            return;
        }
        s->regs[0x02] = v;
        if (!(v & R02_ENABLE)) {
            rda_cancel(s);
        } else if ((v & R02_SEEK) && s->op != RDA_SEEKING) {
            rda_start(s, RDA_SEEKING);
        }
        break;
    case 0x03:
        s->regs[0x03] = v;
        if ((v & R03_TUNE) && rda_enabled(s)) {
            rda_start(s, RDA_TUNING);
        }
        break;
    case 0x10 ... RDA_NREGS - 1:
        if (((s->regs[0x06] >> 13) & 3) != 3) {
            return;                     /* hidden: OPEN_MODE is not 11 */
        }
        s->regs[reg] = v;
        break;
    default:
        s->regs[reg] = v;
        break;
    }
    if (s->log) {
        fprintf(stderr, "dm32_rda5807m: %02XH <- 0x%04x\n", reg, v);
    }
}

static uint16_t rda_read_reg(DM32Rda5807mState *s, uint8_t reg)
{
    switch (reg) {
    case 0x0a:
        return rda_status_0a(s);
    case 0x0b:
        return rda_status_0b(s);
    default:
        return s->regs[reg];
    }
}

static uint8_t rda_next(uint8_t reg)
{
    return reg == RDA_NREGS - 1 ? 0 : reg + 1;
}

static int rda_event(I2CSlave *i2c, enum i2c_event event)
{
    DM32Rda5807mState *s = DM32_RDA5807M(i2c);

    switch (event) {
    case I2C_START_SEND:
        s->ptr = RDA_WRITE_FIRST;
        s->high = true;
        break;
    case I2C_START_RECV:
        s->ptr = RDA_READ_FIRST;
        s->high = true;
        break;
    default:
        break;
    }
    return 0;
}

static int rda_send(I2CSlave *i2c, uint8_t data)
{
    DM32Rda5807mState *s = DM32_RDA5807M(i2c);

    if (s->high) {
        s->hi_byte = data;
    } else {
        rda_write_reg(s, s->ptr, s->hi_byte << 8 | data);
        s->ptr = rda_next(s->ptr);
    }
    s->high = !s->high;
    return 0;
}

static uint8_t rda_recv(I2CSlave *i2c)
{
    DM32Rda5807mState *s = DM32_RDA5807M(i2c);
    uint8_t v;

    if (s->high) {
        s->latch = rda_read_reg(s, s->ptr);
        v = s->latch >> 8;
    } else {
        v = s->latch & 0xff;
        s->ptr = rda_next(s->ptr);
    }
    s->high = !s->high;
    return v;
}

/* DM32_FM_STATIONS="kHz[:rssi],...", DM32_FM_RSSI=<0..127> */
static void rda_parse_env(DM32Rda5807mState *s)
{
    const char *e = getenv("DM32_FM_STATIONS");

    while (e && *e && s->nstations < RDA_MAX_STATIONS) {
        const char *end;
        unsigned long khz, rssi = 60;

        if (qemu_strtoul(e, &end, 10, &khz) || !khz) {
            break;
        }
        if (*end == ':' && !qemu_strtoul(end + 1, &end, 10, &rssi)) {
            rssi = MIN(rssi, 127);
        }
        s->station_khz[s->nstations] = khz;
        s->station_rssi[s->nstations] = rssi;
        s->nstations++;
        e = *end == ',' ? end + 1 : NULL;
    }
    s->noise_rssi = dm32_env_uint("DM32_FM_RSSI", 0, 0, 127);
}

/*
 * No reset handler: the tuner is a separate chip that SoC resets do not
 * reach; it resets at power-up and by SOFT_RESET.
 */
static void rda_init(Object *obj)
{
    DM32Rda5807mState *s = DM32_RDA5807M(obj);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rda_timer, s);
    s->log = dm32_env_flag("DM32_FM_LOG");
    rda_parse_env(s);
    rda_reset_regs(s);
}

static void rda_class_init(ObjectClass *klass, const void *data)
{
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);

    sc->event = rda_event;
    sc->send = rda_send;
    sc->recv = rda_recv;
}

static const TypeInfo rda_type = {
    .name = TYPE_DM32_RDA5807M,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(DM32Rda5807mState),
    .instance_init = rda_init,
    .class_init = rda_class_init,
};

static void rda_register(void)
{
    type_register_static(&rda_type);
}

type_init(rda_register)

void dm32_rda5807m_attach(I2CBus *bus)
{
    i2c_slave_create_simple(bus, TYPE_DM32_RDA5807M, RDA_ADDR7);
}
