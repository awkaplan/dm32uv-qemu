/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Baofeng DM-32UV FD6818B RF transceiver (3-wire register interface):
 * register file, status, squelch, sub-audio detection, GPIO pins and an
 * injected RF environment.
 *
 * Copyright (c) 2026 Alex Kaplan
 *
 * Source: the FD6818 programming guide (Chinese, REV 1.0.0; not included in
 * this repository), cited as "guide" with its printed page numbers: the
 * register summary (寄存器汇总, p.56-72), interface timing
 * (接口时序, p.15), configuration flow (p.16), frequency (p.20),
 * sub-audio (亚音频设置, p.42-45), squelch, RSSI and noise (p.47), and
 * GPIO and interrupts (p.50-54).
 * Register addresses and fields are the guide's. The AT1846S, from which the
 * part derives, has a different register map and is not used. dm32uv-re
 * specs/rdl/fd6818.rdl records where the populated FD6818B differs from the
 * guide; those differences are marked DEVIATION below.
 *
 * Bus: 3-wire, not SPI
 * ====================
 * Per 接口时序, a transfer is CS low, then 24 bits clocked MSB-first on the
 * rising edge:
 *
 *     R/W̄ (1 bit)  |  ADDR[6:0]  |  DATA[15:0]
 *
 * R/W̄ = 0 writes, 1 reads. On a read the master releases DATA after the
 * address byte and the slave drives the 16 data bits. The stock firmware's
 * primitives confirm the details a guess would get wrong (fd6818b_reg_read
 * @0x0002c33c, bit loop @0x0002c298): the turnaround happens after the address
 * byte and before the first data clock, and the data bit is sampled while CLK
 * is high.
 *
 * Paging (p.15): registers 0x80-0xFF are reached by WRITE(0x7F, 1), then the
 * register number minus 0x80; WRITE(0x7F, 0) returns to page 0, the default.
 * Wire address 0x7F is the paging command on either page, not a register:
 * reading it returns 0, as on hardware. Bit 0 selects the page.
 *
 * Wiring: bit-banged on GPIOA, CLK bit 7, DATA bit 8, CS bit 24.
 * dm32_fd6818_init(), called by the board (hw/csky/dm32.c), attaches the
 * chip to those three pads (dm32_gpio_attach). Every change of the three
 * effective levels reaches fd6818_bus_update(), and while the chip drives
 * DATA it puts its data bit on pad PTA8. The chip's GPIO0 and GPIO1 pins go
 * to GPIOB 27 and 26 (dm32uv-re specs/dts/dm32uv.dts; Assumption: from a
 * community pin map, not traced on the board).
 *
 * Chip ID: the guide says REG_00H reads 0x6818. The populated radio reads
 * 0x4819, stably (48/48 identical reads across a 32x timing range), so the
 * model defaults to that. DM32_FD6818_ID=0x6818 selects the guide's value.
 *
 * Soft reset (p.16): WRITE(0x00, 0x8000) then WRITE(0x00, 0x0000). A write
 * with bit 15 set returns every register to its reset value and the chip to
 * page 0, with the PLL off. Assumption: the guide does not list what the
 * soft reset restores; the model restores everything, including the chip's
 * detection state.
 *
 * Status (REG_0CH, p.58): pll_lock [9], afc_rail [8], weak_rssi [7], tot_out
 * [3], vox_out [2], sq_out [1], int_out [0]; [15:10] and [6:4] are reserved
 * and read 0. Stock also reads [11:10] and [15:14] (dm32uv-re
 * specs/rdl/fd6818.rdl); what they carry on the FD6818B is unknown. afc_rail,
 * tot_out and vox_out read 0: AFC, TOT and VOX are not modelled.
 *
 * Transmit/receive enable. DEVIATION: the guide's enable word is REG_03H; the
 * FD6818B uses the same layout at REG_30H (stock writes 0xBFF1 for RX and
 * 0xC1FE for TX there and never writes REG_03H). [8:4] pll_en, [1] txon,
 * [0] rxon. The PLL runs while pll_en is 11111 and txon or rxon is set.
 *
 * PLL lock. The frequency is (REG_39H << 16 | REG_38H) * 10 Hz (p.20). A
 * REG_38H write retunes and drops lock until REG_39H is written: hardware
 * left the PLL unlocked when REG_39H was written before REG_38H, and locked
 * with the low word first. Lock then takes DM32_FD6818_LOCK_US (default 40
 * us, the upper bound measured on hardware) after the REG_39H write or after
 * the PLL is switched on. Assumption: the guide gives no lock time; the 40 us
 * default is the model's value. While locked, a REG_38H write is a
 * "PLL lock lost" event.
 *
 * RF environment
 * ==============
 * DM32_RF_ENV names a script of carriers at virtual times (docs/environment.md,
 * "FD6818"). The receiver sees the strongest carrier within 6.25 kHz of the
 * tuned frequency (a carrier at "*" is always on it), over a noise floor
 * (default -127 dBm), while rxon is set and the PLL is locked. From the total
 * in-channel power P (dBm):
 *
 *   rssi_sq   REG_67H[8:0]   2 * (P + 160), 0.5 dB per step (p.47 gives
 *                            RSSI(dB) = rssi_sq / 2 - 160)
 *   agc_rssi  REG_62H[15:8]  P + 160, 1 dB per step, narrowband
 *   lna_peak_rssi REG_62H[7:0]  the same over every carrier within 5 MHz,
 *                            wideband
 *   snr_out   REG_61H[15:8]  carrier minus noise floor, dB
 *   rssi_rel  REG_65H[15:8]  the same as snr_out
 *   noise_sq  REG_65H[7:0]   60 - snr_out, at least 0
 *   weak_rssi REG_0CH[7]     snr_out < 10 dB
 *
 * Assumptions: the guide gives the units of REG_62H and REG_65H but no
 * offsets, so REG_62H uses rssi_sq's -160 dB offset; the channel width, the
 * wideband span, the noise level of 60 at no signal and the 10 dB weak-signal
 * limit are not in the guide. The reported level is the level at the antenna,
 * as if REG_2CH ext_lna_gain matched the external LNA (p.47).
 *
 * Squelch (p.47), with rssi_sq_th_in/out in REG_78H[15:8]/[7:0] (0.5 dB per
 * step) and noise_sq_th_out/in in REG_4FH[14:8]/[6:0]:
 *
 *   if (rssi_sq > rssi_sq_th_in && noise_sq < noise_sq_th_in)        SQ = 1;
 *   else if (rssi_sq < rssi_sq_th_out || noise_sq > noise_sq_th_out) SQ = 0;
 *
 * Sub-audio detection (p.42-45) runs while REG_51H subau_en [15] is set, on a
 * carrier at least 10 dB above the noise floor (Assumption). With
 * ctc_dcs_sel [12] = 1, CTCSS slot k (REG_07H [13] = k, word [12:0] =
 * f * 2^27 / 6500000) is detected when the carrier's tone is within
 * ctc_th_in and lost beyond ctc_th_out (REG_52H [11:6] and [5:0], in 0.1 Hz,
 * or 0.1 % of the tone with ctc_th_mode [12] set); REG_60H [1:0] reports it.
 * With ctc_dcs_sel = 0, the CDCSS code is REG_08H's two halves ([15] = 1:
 * high 12 bits) and REG_60H [11:10] reports a positive or negative code.
 * Assumption: the carrier's code is the 23-bit Golay word of its octal code
 * (data bits 11:9 = 100, generator 0xC75), inverted for a negative code; the
 * configured word matches any cyclic rotation of it, in either bit order,
 * because the guide does not give the bit order.
 *
 * Interrupts (p.50-52). A change of SQ (source 0), CTCSS (5), CDCSS (6) or a
 * PLL lock loss (7) whose REG_3FH irq_mask bit is set latches REG_02H = the
 * event's status bits | source and sets int_out. Any write to REG_02H clears
 * int_out; the vector stays readable. Source numbers follow the interrupt
 * section and the mask bits; the summary table gives 8 for PLL lock lost.
 *
 * GPIO pins (p.53-54). Pin n is driven by the chip while REG_25H
 * gpio_oen_b[n + 8] is 0, with the function REG_26H/27H gpioN_out_sel
 * selects: 0 gpio_out_val[n], 1 INT, 2 SQ, 3 VOX, 4 sub-audio compare; the
 * others (sub-audio code output, serial and clock outputs) drive 0
 * (Assumption). REG_28H gpio_in_val [15:8] reads the pins. DEVIATION: with
 * the guide's REG_25H reset value 0x0000 every pin would drive at reset, but
 * the populated chip read gpio_in_val 0x6b, which no driven combination of
 * the reset functions gives. The model therefore resets REG_25H to 0xff00
 * (no pin driven), and an undriven pin reads the board: GPIO0 and GPIO1 the
 * GPIOB 27 and 26 pad (driven level, else the SoC pull-up), the others the
 * hardware read's bits 0x68 (Assumption).
 *
 * Without DM32_RF_ENV, the band is empty: the receiver sees the noise floor
 * only, except that REG_62H keeps a deterministic jitter stand-in (see
 * fd6818_rssi_jitter()), which DM32_FD6818_RSSI pins and
 * DM32_FD6818_RSSI_JITTER sizes.
 *
 * Not modelled: transmit, audio, DTMF (REG_0BH reads 0), VOX (REG_64H reads
 * 0), the compander (REG_87H reads 0), FSK, AFC and TOT.
 */

#include "qemu/osdep.h"
#include <math.h>
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "qemu/cutils.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "hw/csky/dm32.h"

#define FD_NREG 0x100

/* REG_0CH bits */
#define ST_PLL_LOCK  (1u << 9)
#define ST_WEAK_RSSI (1u << 7)
#define ST_SQ_OUT    (1u << 1)
#define ST_INT_OUT   (1u << 0)

/* REG_02H interrupt sources (p.51) */
#define IRQ_SQ    0
#define IRQ_CTCSS 5
#define IRQ_CDCSS 6
#define IRQ_PLL   7

/* RF environment limits and assumptions, see the header */
#define RF_MAX_EVENTS    256
#define RF_MAX_CARRIERS  16
#define RF_CHANNEL_HZ    6250
#define RF_WIDEBAND_HZ   5000000
#define RF_NOISE_FLOOR   (-127.0)
#define RF_NOISE_FULL    60
#define RF_WEAK_SNR      10
#define RF_SUBAU_SNR     10

/* Board level of undriven GPIO2-7, from the hardware read (see header). */
#define FD_GPIO_BOARD    0x68

/* Sentinel for DM32_FD6818_RSSI when it is unset; outside its 0..255 range */
#define FD_RSSI_UNSET    0x100

typedef enum RfMod {
    RF_MOD_NONE,
    RF_MOD_FM,
    RF_MOD_DMR,
} RfMod;

typedef struct RfCarrier {
    uint64_t hz;            /* 0: on the tuned frequency, whatever it is */
    double dbm;
    RfMod mod;
    double ctcss_hz;        /* 0: none */
    int dcs;                /* octal code as a number, -1: none */
    bool dcs_inv;
} RfCarrier;

typedef struct RfEvent {
    int64_t ns;
    bool off;
    RfCarrier c;
} RfEvent;

typedef struct FD6818State {
    uint16_t reg[FD_NREG];  /* page 0 at 0x00-0x7F, page 1 at 0x80-0xFF */
    uint8_t  page;          /* REG_7FH page select */

    /* write-only slot registers (REG_07H [13], REG_08H [15]) */
    uint16_t ctc_word[2];
    bool     ctc_set[2];
    uint16_t dcs_half[2];

    /* PLL */
    bool     retune;        /* REG_38H written, REG_39H not yet */
    int64_t  lock_at;       /* virtual ns at which the PLL locks */
    bool     was_locked;

    /* detection state */
    bool     sq;
    uint8_t  ctc_det;       /* REG_60H [1:0] */
    uint8_t  dcs_det;       /* REG_60H [11:10]: [0] positive, [1] negative */
    bool     int_out;
    uint16_t vector;        /* REG_02H */
    uint8_t  gpio_level;    /* GPIO0/1 as last told to the pads, 2 bits each */

    /* RF environment */
    bool     env;
    double   floor_dbm;
    RfEvent  ev[RF_MAX_EVENTS];
    int      nev, next_ev;
    RfCarrier car[RF_MAX_CARRIERS];
    int      ncar;
    QEMUTimer *timer;

    /* 3-wire shift state */
    int      cs;            /* 1 = idle (deasserted) */
    int      clk;
    int      bit_count;     /* bits clocked in this transfer */
    uint32_t shift_in;      /* bits received from the master */
    uint16_t shift_out;     /* data being driven back on a read */
    int      is_read;
    uint8_t  addr;
    int      dat_out;       /* level the chip drives on DATA, when it drives */
    int      driving;       /* 1 while the chip owns DATA (read data phase) */
    uint32_t rssi_seed;     /* deterministic RSSI jitter, see REG_62H */

    /* environment knobs, read once by dm32_fd6818_init() */
    bool     log;           /* DM32_FD6818_LOG */
    bool     bits;          /* DM32_FD6818_BITS */
    uint16_t chip_id;       /* DM32_FD6818_ID, the reset value of REG_00H */
    int64_t  lock_ns;       /* DM32_FD6818_LOCK_US */
    bool     rssi_fixed;    /* DM32_FD6818_RSSI is set */
    uint8_t  rssi_base;     /* DM32_FD6818_RSSI, else 0x30 */
    uint8_t  rssi_span;     /* DM32_FD6818_RSSI_JITTER */
} FD6818State;

static FD6818State fd6818;

#define FD_GPIO0_PAD DM32_PTB(27)
#define FD_GPIO1_PAD DM32_PTB(26)

static void fd6818_update(FD6818State *s);

static bool fd6818_log(void)
{
    return fd6818.log;
}

static int64_t fd6818_now(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

/* ---- reset defaults, from the guide's register summary ------------------- */

static void fd6818_reset(FD6818State *s)
{
    memset(s->reg, 0, sizeof(s->reg));

    /* Chip ID: the value this radio reports, not the guide's 0x6818. */
    s->reg[0x00] = s->chip_id;
    s->reg[0x01] = 0x0000;   /* revision_id */

    /* R/W registers with documented defaults. Transcribed verbatim. */
    s->reg[0x03] = 0x0000;
    s->reg[0x04] = 0x0000;
    s->reg[0x18] = 0x4125;
    s->reg[0x19] = 0x900F;
    s->reg[0x23] = 0x8F8F;
    s->reg[0x24] = 0x603F;
    s->reg[0x25] = 0xFF00;   /* DEVIATION: the guide gives 0x0000, see header */
    s->reg[0x26] = 0x0005;
    s->reg[0x27] = 0x3200;
    s->reg[0x28] = 0x003F;   /* [5:0]; [15:8] is the live gpio_in_val */
    s->reg[0x38] = 0x3A98;
    s->reg[0x39] = 0x0271;   /* 409.75 MHz */
    s->reg[0x3B] = 0x3D62;
    s->reg[0x3C] = 0x1000;   /* xtal_freq */
    s->reg[0x3F] = 0x0000;   /* irq_mask */
    s->reg[0x40] = 0x3808;
    s->reg[0x43] = 0x6009;
    s->reg[0x44] = 0x43EC;
    s->reg[0x46] = 0x8050;
    s->reg[0x47] = 0x6140;
    s->reg[0x4F] = 0x7E34;
    s->reg[0x50] = 0x033C;
    s->reg[0x51] = 0x1050;
    s->reg[0x52] = 0x028F;
    s->reg[0x53] = 0x97DD;
    s->reg[0x54] = 0x8517;
    s->reg[0x56] = 0x1021;
    s->reg[0x59] = 0x8078;
    s->reg[0x5A] = 0x85CF;
    s->reg[0x5B] = 0xAB45;
    s->reg[0x5C] = 0x56F9;
    s->reg[0x5D] = 0x3FCC;
    s->reg[0x5E] = 0x0004;
    s->reg[0x70] = 0x7070;
    s->reg[0x71] = 0x2854;
    s->reg[0x72] = 0x3065;
    s->reg[0x73] = 0x568A;
    s->reg[0x75] = 0x0FB4;
    s->reg[0x76] = 0xE307;
    s->reg[0x78] = 0x44FF;
    s->reg[0x79] = 0x1040;
    s->reg[0x7A] = 0x881A;
    s->reg[0x7D] = 0x4200;
    s->reg[0x7E] = 0x341E;
    /* page 1 */
    s->reg[0x90] = 0x8C20;
    s->reg[0x94] = 0x8F5E;
    s->reg[0x96] = 0x1AE8;
    s->reg[0x97] = 0x0AA8;
    s->reg[0x98] = 0x7A14;
    s->reg[0x99] = 0x7855;
    s->reg[0x9A] = 0x0728;
    s->reg[0x9B] = 0x0004;
    s->reg[0x9D] = 0x29AD;
    s->reg[0xB6] = 0x9D08;

    s->page = 0;
    memset(s->ctc_word, 0, sizeof(s->ctc_word));
    memset(s->ctc_set, 0, sizeof(s->ctc_set));
    memset(s->dcs_half, 0, sizeof(s->dcs_half));
    s->retune = false;
    s->lock_at = 0;
    s->was_locked = false;
    s->sq = false;
    s->ctc_det = 0;
    s->dcs_det = 0;
    s->int_out = false;
    s->vector = 0;
}

/* ---- PLL ----------------------------------------------------------------- */

static bool fd6818_pll_on(FD6818State *s)
{
    uint16_t en = s->reg[0x30];

    return ((en >> 4) & 0x1F) == 0x1F && (en & 0x3);
}

static bool fd6818_locked(FD6818State *s)
{
    return fd6818_pll_on(s) && !s->retune && fd6818_now() >= s->lock_at;
}

static void fd6818_start_lock(FD6818State *s)
{
    s->lock_at = fd6818_now() + s->lock_ns;
    if (s->timer) {
        timer_mod(s->timer, MIN(s->lock_at, timer_expire_time_ns(s->timer)));
    }
}

static uint64_t fd6818_freq_hz(FD6818State *s)
{
    return ((uint64_t) s->reg[0x39] << 16 | s->reg[0x38]) * 10u;
}

/* ---- RF environment ------------------------------------------------------ */

/* The DCS Golay (23,12) word of octal @code: data [11:0], parity [22:12] */
static uint32_t fd6818_dcs_word(int code)
{
    uint32_t data = (code & 0x1FF) | 0x800;
    uint32_t rem = data << 11;
    int i;

    for (i = 22; i >= 11; i--) {
        if (rem & (1u << i)) {
            rem ^= 0xC75u << (i - 11);
        }
    }
    return data | (rem & 0x7FF) << 12;
}

static uint32_t fd6818_rev23(uint32_t x)
{
    uint32_t r = 0;
    int i;

    for (i = 0; i < 23; i++) {
        r |= ((x >> i) & 1) << (22 - i);
    }
    return r;
}

/* Is @w any cyclic rotation of @cw, in either bit order? */
static bool fd6818_dcs_match(uint32_t w, uint32_t cw)
{
    int r;

    w &= 0x7FFFFF;
    for (r = 0; r < 23; r++) {
        uint32_t rot = ((cw << r) | (cw >> (23 - r))) & 0x7FFFFF;

        if (w == rot || fd6818_rev23(w) == rot) {
            return true;
        }
    }
    return false;
}

static void fd6818_env_error(const char *path, int line, const char *what)
{
    error_report("DM32_RF_ENV: %s:%d: %s", path, line, what);
    exit(1);
}

static int fd6818_event_cmp(const void *a, const void *b)
{
    const RfEvent *x = a, *y = b;

    return x->ns < y->ns ? -1 : x->ns > y->ns;
}

/*
 * One entry per line: "<t_ms> <freq_hz|*> <dBm|off> [none|fm|dmr]
 * [ctcss=<Hz>] [dcs=<octal>[i]]", or "floor <dBm>". '#' starts a comment.
 */
static void fd6818_env_load(FD6818State *s)
{
    const char *path = getenv("DM32_RF_ENV");
    char line[256];
    FILE *f;
    int n = 0;

    s->floor_dbm = RF_NOISE_FLOOR;
    if (!path) {
        return;
    }
    f = fopen(path, "r");
    if (!f) {
        error_report("DM32_RF_ENV: cannot open '%s'", path);
        exit(1);
    }
    s->env = true;
    while (fgets(line, sizeof(line), f)) {
        char *save = NULL, *tok, *end;
        RfEvent *e;

        n++;
        if (!strchr(line, '\n') && !feof(f)) {
            fd6818_env_error(path, n, "line too long (over 254 characters)");
        }
        *strchrnul(line, '#') = '\0';
        tok = strtok_r(line, " \t\r\n", &save);
        if (!tok) {
            continue;
        }
        if (!strcmp(tok, "floor")) {
            tok = strtok_r(NULL, " \t\r\n", &save);
            if (!tok) {
                fd6818_env_error(path, n, "floor needs a level in dBm");
            }
            s->floor_dbm = strtod(tok, &end);
            if (*end) {
                fd6818_env_error(path, n, "bad floor level");
            }
            continue;
        }
        if (s->nev == RF_MAX_EVENTS) {
            fd6818_env_error(path, n, "too many entries");
        }
        e = &s->ev[s->nev];
        memset(e, 0, sizeof(*e));
        e->c.dcs = -1;
        e->c.mod = RF_MOD_NONE;
        e->ns = (int64_t) (strtod(tok, &end) * SCALE_MS);
        if (*end || e->ns < 0) {
            fd6818_env_error(path, n, "bad time");
        }
        tok = strtok_r(NULL, " \t\r\n", &save);
        if (tok && !strcmp(tok, "*")) {
            e->c.hz = 0;
        } else if (!tok || qemu_strtou64(tok, NULL, 10, &e->c.hz) ||
                   !e->c.hz) {
            fd6818_env_error(path, n, "bad frequency (Hz)");
        }
        tok = strtok_r(NULL, " \t\r\n", &save);
        if (!tok) {
            fd6818_env_error(path, n, "missing level");
        }
        if (!strcmp(tok, "off")) {
            e->off = true;
        } else {
            e->c.dbm = strtod(tok, &end);
            if (*end) {
                fd6818_env_error(path, n, "bad level (dBm)");
            }
        }
        while ((tok = strtok_r(NULL, " \t\r\n", &save))) {
            if (!strcmp(tok, "none")) {
                e->c.mod = RF_MOD_NONE;
            } else if (!strcmp(tok, "fm")) {
                e->c.mod = RF_MOD_FM;
            } else if (!strcmp(tok, "dmr")) {
                e->c.mod = RF_MOD_DMR;
            } else if (!strncmp(tok, "ctcss=", 6)) {
                e->c.ctcss_hz = strtod(tok + 6, &end);
                if (*end || e->c.ctcss_hz <= 0) {
                    fd6818_env_error(path, n, "bad ctcss tone");
                }
            } else if (!strncmp(tok, "dcs=", 4)) {
                e->c.dcs = strtol(tok + 4, &end, 8);
                e->c.dcs_inv = *end == 'i';
                if (*end == 'i' || *end == 'n') {
                    end++;
                }
                if (*end || e->c.dcs < 0 || e->c.dcs > 0777) {
                    fd6818_env_error(path, n, "bad dcs code");
                }
            } else {
                fd6818_env_error(path, n, "unknown keyword");
            }
        }
        if ((e->c.ctcss_hz > 0 || e->c.dcs >= 0) && e->c.mod != RF_MOD_FM) {
            fd6818_env_error(path, n, "ctcss and dcs need fm");
        }
        s->nev++;
    }
    fclose(f);
    qsort(s->ev, s->nev, sizeof(s->ev[0]), fd6818_event_cmp);
}

/* Apply every environment entry due by now; return the next entry's time. */
static int64_t fd6818_env_advance(FD6818State *s)
{
    int64_t now = fd6818_now();

    while (s->next_ev < s->nev && s->ev[s->next_ev].ns <= now) {
        RfEvent *e = &s->ev[s->next_ev++];
        int i;

        for (i = 0; i < s->ncar && s->car[i].hz != e->c.hz; i++) {
            /* find the carrier on this frequency */
        }
        if (e->off) {
            if (i < s->ncar) {
                s->car[i] = s->car[--s->ncar];
            }
        } else if (i < s->ncar || s->ncar < RF_MAX_CARRIERS) {
            s->car[i] = e->c;
            s->ncar += i == s->ncar;
        }
        if (fd6818_log()) {
            fprintf(stderr, "fd6818: rf env @%" PRId64 " ms: %" PRIu64
                    " Hz %s\n", e->ns / SCALE_MS, e->c.hz,
                    e->off ? "off" : "on");
        }
    }
    return s->next_ev < s->nev ? s->ev[s->next_ev].ns : INT64_MAX;
}

static double fd6818_dbm_sum(double a, double b)
{
    return 10.0 * log10(pow(10.0, a / 10.0) + pow(10.0, b / 10.0));
}

static unsigned fd6818_clamp(double v, unsigned max)
{
    return v <= 0 ? 0 : v >= max ? max : (unsigned) lround(v);
}

/* What the receiver measures now. */
typedef struct RfMeasure {
    bool rx;                /* rxon set and the PLL locked */
    const RfCarrier *car;   /* strongest in-channel carrier, or NULL */
    unsigned rssi_sq, agc_rssi, lna_rssi, snr, noise_sq;
} RfMeasure;

static void fd6818_measure(FD6818State *s, RfMeasure *m)
{
    uint64_t f = fd6818_freq_hz(s);
    double nb = s->floor_dbm, wb = s->floor_dbm;
    int i;

    memset(m, 0, sizeof(*m));
    m->rx = (s->reg[0x30] & 1) && fd6818_locked(s);
    if (!m->rx) {
        return;
    }
    for (i = 0; i < s->ncar; i++) {
        const RfCarrier *c = &s->car[i];
        uint64_t hz = c->hz ? c->hz : f;
        uint64_t d = hz > f ? hz - f : f - hz;

        if (d <= RF_CHANNEL_HZ && (!m->car || c->dbm > m->car->dbm)) {
            m->car = c;
        }
        if (d <= RF_WIDEBAND_HZ) {
            wb = fd6818_dbm_sum(wb, c->dbm);
        }
    }
    if (m->car) {
        nb = fd6818_dbm_sum(nb, m->car->dbm);
        m->snr = fd6818_clamp(m->car->dbm - s->floor_dbm, 255);
    }
    m->rssi_sq = fd6818_clamp(2.0 * (nb + 160.0), 511);
    m->agc_rssi = fd6818_clamp(nb + 160.0, 255);
    m->lna_rssi = fd6818_clamp(wb + 160.0, 255);
    m->noise_sq = m->snr >= RF_NOISE_FULL ? 0 : RF_NOISE_FULL - m->snr;
}

/* REG_60H [1:0]: CTCSS slots, with the REG_52H hysteresis */
static uint8_t fd6818_ctcss(FD6818State *s, const RfMeasure *m)
{
    uint16_t th = s->reg[0x52];
    uint8_t det = s->ctc_det;
    int k;

    for (k = 0; k < 2; k++) {
        double fk = (s->ctc_word[k] & 0x1FFF) * 6500000.0 / (1 << 27);
        double unit = (th & 0x1000) ? fk * 0.001 : 0.1;
        double d;

        if (!s->ctc_set[k] || !m->car || m->car->ctcss_hz <= 0) {
            det &= ~(1u << k);
            continue;
        }
        d = fabs(m->car->ctcss_hz - fk);
        if (d <= ((th >> 6) & 0x3F) * unit) {
            det |= 1u << k;
        } else if (d > (th & 0x3F) * unit) {
            det &= ~(1u << k);
        }
    }
    return det;
}

/* REG_60H [11:10]: [0] positive, [1] negative CDCSS code */
static uint8_t fd6818_cdcss(FD6818State *s, const RfMeasure *m)
{
    uint32_t w = (uint32_t) (s->dcs_half[1] & 0xFFF) << 12 |
                 (s->dcs_half[0] & 0xFFF);
    uint32_t rx;

    if (!m->car || m->car->dcs < 0) {
        return 0;
    }
    rx = fd6818_dcs_word(m->car->dcs);
    if (m->car->dcs_inv) {
        rx = ~rx & 0x7FFFFF;
    }
    if (fd6818_dcs_match(w, rx)) {
        return 1;
    }
    if (fd6818_dcs_match(~w, rx)) {
        return 2;
    }
    return 0;
}

static void fd6818_irq(FD6818State *s, int source)
{
    if (!(s->reg[0x3F] & (1u << source))) {
        return;
    }
    s->vector = (s->dcs_det ? 0x800 : 0) | ((s->dcs_det & 2) ? 0x400 : 0) |
                (s->ctc_det & 3) << 6 | (s->sq ? 0x10 : 0) | source;
    s->int_out = true;
    if (fd6818_log()) {
        fprintf(stderr, "fd6818: interrupt, REG_02H = 0x%04x\n", s->vector);
    }
}

/* The level the chip drives on pin @n, or -1 if it does not drive it. */
static int fd6818_pin_out(FD6818State *s, int n)
{
    uint16_t sel = s->reg[n < 4 ? 0x26 : 0x27] >> ((n % 4) * 4) & 0xF;

    if ((s->reg[0x25] >> (8 + n)) & 1) {
        return -1;
    }
    switch (sel) {
    case 0:
        return (s->reg[0x25] >> n) & 1;
    case 1:
        return s->int_out;
    case 2:
        return s->sq;
    case 4:
        return s->ctc_det || s->dcs_det;
    default:
        return 0;   /* VOX, sub-audio code, serial and clock outputs */
    }
}

/* The SoC side of a GPIOB pad: its driven level, else its pull. */
static int fd6818_soc_pad(unsigned pad)
{
    bool driven;
    int v = dm32_gpio_output(pad, &driven);

    return driven ? v : dm32_pad_pull(pad) != DM32_PULL_DOWN;
}

/* REG_28H [15:8] */
static uint8_t fd6818_gpio_in(FD6818State *s)
{
    uint8_t v = 0;
    int n;

    for (n = 0; n < 8; n++) {
        int out = fd6818_pin_out(s, n);
        int lvl;

        if (out >= 0) {
            lvl = out;
        } else if (n == 0) {
            lvl = fd6818_soc_pad(FD_GPIO0_PAD);
        } else if (n == 1) {
            lvl = fd6818_soc_pad(FD_GPIO1_PAD);
        } else {
            lvl = (FD_GPIO_BOARD >> n) & 1;
        }
        v |= (lvl & 1) << n;
    }
    return v;
}

/*
 * Re-evaluate the environment, the squelch and the detectors; raise
 * interrupts and tell the GPIO pads about changed pins.
 */
static void fd6818_update(FD6818State *s)
{
    int64_t next = fd6818_env_advance(s);
    bool locked = fd6818_locked(s);
    uint16_t th = s->reg[0x78], nth = s->reg[0x4F];
    RfMeasure m;
    bool sq = s->sq;
    uint8_t ctc, dcs;
    int n;

    if (s->was_locked && !locked && fd6818_pll_on(s)) {
        fd6818_irq(s, IRQ_PLL);
    }
    s->was_locked = locked;

    fd6818_measure(s, &m);
    if (!m.rx) {
        sq = false;
    } else if (m.rssi_sq > (th >> 8) && m.noise_sq < (nth & 0x7F)) {
        sq = true;
    } else if (m.rssi_sq < (th & 0xFF) || m.noise_sq > ((nth >> 8) & 0x7F)) {
        sq = false;
    }

    ctc = dcs = 0;
    if ((s->reg[0x51] & 0x8000) && m.snr >= RF_SUBAU_SNR) {
        if (s->reg[0x51] & 0x1000) {
            ctc = fd6818_ctcss(s, &m);
        } else {
            dcs = fd6818_cdcss(s, &m);
        }
    }

    if (sq != s->sq) {
        s->sq = sq;
        fd6818_irq(s, IRQ_SQ);
    }
    if (ctc != s->ctc_det) {
        s->ctc_det = ctc;
        fd6818_irq(s, IRQ_CTCSS);
    }
    if (dcs != s->dcs_det) {
        s->dcs_det = dcs;
        fd6818_irq(s, IRQ_CDCSS);
    }

    for (n = 0; n < 2; n++) {
        /* 2 bits per pin: 0 or 1 driven, 2 undriven */
        int out = fd6818_pin_out(s, n);
        unsigned lvl = out < 0 ? 2 : out;

        if (((s->gpio_level >> (n * 2)) & 3) != lvl) {
            s->gpio_level = (s->gpio_level & ~(3u << (n * 2))) | lvl << (n * 2);
            dm32_gpio_input_changed(n ? FD_GPIO1_PAD : FD_GPIO0_PAD);
        }
    }

    if (s->timer) {
        if (fd6818_pll_on(s) && !s->retune && s->lock_at > fd6818_now()) {
            next = MIN(next, s->lock_at);
        }
        if (next == INT64_MAX) {
            timer_del(s->timer);
        } else {
            timer_mod(s->timer, next);
        }
    }
}

static void fd6818_timer(void *opaque)
{
    fd6818_update(opaque);
}

/* ---- register access ----------------------------------------------------- */

static int fd6818_is_readonly(unsigned a)
{
    /* Read-only per the summary table: identity, status and measurements. */
    switch (a) {
    case 0x00:
    case 0x01:
    case 0x02:
    case 0x0B:
    case 0x0C:
    case 0x60:
    case 0x61:
    case 0x62:
    case 0x64:
    case 0x65:
    case 0x67:
    case 0x87:
        return 1;
    default:
        return 0;
    }
}

/*
 * REG_62H without an environment: agc_rssi wanders at the noise floor on
 * hardware, and that wander drives the UI's redraw rate, so a constant would
 * let a redraw filter look right in emulation and redraw far more often on
 * silicon. The stand-in is a deterministic jitter: base 0x30, 12 dB peak to
 * peak. DM32_FD6818_RSSI pins the value and disables the jitter;
 * DM32_FD6818_RSSI_JITTER sets the peak-to-peak span in dB.
 */
static uint16_t fd6818_rssi_jitter(FD6818State *s)
{
    unsigned base = s->rssi_base;
    unsigned span = s->rssi_span;
    unsigned rssi = base;

    if (!s->rssi_fixed && span > 0) {
        /* LCG, so a run is repeatable and does not depend on the host. */
        s->rssi_seed = s->rssi_seed * 1103515245u + 12345u;
        rssi = base + ((s->rssi_seed >> 16) % span) - (span / 2u);
    }
    return (uint16_t) ((rssi & 0xff) << 8);
}

static uint16_t fd6818_read_reg(FD6818State *s, unsigned a)
{
    RfMeasure m;

    fd6818_update(s);
    fd6818_measure(s, &m);

    switch (a) {
    case 0x02:
        return s->vector;
    case 0x0B:
        return 0;
    case 0x0C:
        return (fd6818_locked(s) ? ST_PLL_LOCK : 0) |
               (m.snr < RF_WEAK_SNR ? ST_WEAK_RSSI : 0) |
               (s->sq ? ST_SQ_OUT : 0) | (s->int_out ? ST_INT_OUT : 0);
    case 0x28:
        return (uint16_t) fd6818_gpio_in(s) << 8 | (s->reg[0x28] & 0xFF);
    case 0x60:
        return (uint16_t) s->dcs_det << 10 | s->ctc_det;
    case 0x61:
        return m.snr << 8;
    case 0x62:
        if (!s->env) {
            return fd6818_rssi_jitter(s);
        }
        return m.agc_rssi << 8 | m.lna_rssi;
    case 0x64:
        return 0;
    case 0x65:
        return m.snr << 8 | MIN(m.noise_sq, 255);
    case 0x67:
        return m.rssi_sq;
    case 0x87:
        return 0;
    default:
        return s->reg[a];
    }
}

static void fd6818_write_reg(FD6818State *s, unsigned a, uint16_t v)
{
    if (fd6818_log()) {
        fprintf(stderr, "fd6818: REG_%02XH := 0x%04x", a, v);
        if (a == 0x30) {
            /* Decode the one register that says what the radio is doing. */
            fprintf(stderr, "  [%s%s%s%s%s%s%s]",
                    (v & 0x0001) ? "RXON " : "",
                    (v & 0x0002) ? "TXON " : "",
                    (v & 0x0004) ? "MICIN " : "",
                    (v & 0x0008) ? "PADRV " : "",
                    (((v >> 4) & 0x1F) == 0x1F) ? "PLL " : "",
                    (v & 0x4000) ? "PABIAS " : "",
                    (((v >> 10) & 0xF) == 0xF) ? "RXLINK" : "");
        } else if (a == 0x38 || a == 0x39) {
            uint64_t f = (uint64_t) (a == 0x39 ? v : s->reg[0x39]) << 16 |
                         (a == 0x38 ? v : s->reg[0x38]);

            fprintf(stderr, "  [freq = %" PRIu64 " Hz]", f * 10u);
        }
        fprintf(stderr, "\n");
    }

    switch (a) {
    case 0x00:
        /* Soft reset (p.16); the chip ID itself is read-only. */
        if (v & 0x8000) {
            fd6818_reset(s);
        }
        break;
    case 0x02:
        /* Any write clears the interrupt (p.51). */
        s->int_out = false;
        break;
    case 0x07:
        s->reg[a] = v;
        s->ctc_word[(v >> 13) & 1] = v;
        s->ctc_set[(v >> 13) & 1] = true;
        break;
    case 0x08:
        s->reg[a] = v;
        s->dcs_half[(v >> 15) & 1] = v;
        break;
    case 0x28:
        /* [15:8] is gpio_in_val, read-only */
        s->reg[a] = v & 0xFF;
        break;
    case 0x30: {
        bool was_on = fd6818_pll_on(s);

        s->reg[a] = v;
        if (fd6818_pll_on(s) && !was_on) {
            fd6818_start_lock(s);
        }
        break;
    }
    case 0x38:
        s->reg[a] = v;
        s->retune = true;
        break;
    case 0x39:
        s->reg[a] = v;
        s->retune = false;
        fd6818_start_lock(s);
        break;
    default:
        if (fd6818_is_readonly(a)) {
            if (fd6818_log()) {
                fprintf(stderr, "fd6818: REG_%02XH is read-only, write "
                        "ignored\n", a);
            }
            return;
        }
        s->reg[a] = v;
        break;
    }
    fd6818_update(s);
}

/* Wire address @addr on the current page, or -1 for the paging command. */
static int fd6818_reg_addr(FD6818State *s, uint8_t addr)
{
    return addr == 0x7F ? -1 : (s->page << 7) | addr;
}

static uint16_t fd6818_wire_read(FD6818State *s, uint8_t addr)
{
    int a = fd6818_reg_addr(s, addr);

    /* The paging command is not a register; hardware reads 0. */
    return a < 0 ? 0 : fd6818_read_reg(s, a);
}

static void fd6818_wire_write(FD6818State *s, uint8_t addr, uint16_t v)
{
    int a = fd6818_reg_addr(s, addr);

    if (a < 0) {
        s->page = v & 1;
        if (fd6818_log()) {
            fprintf(stderr, "fd6818: page := %u\n", s->page);
        }
        return;
    }
    fd6818_write_reg(s, a, v);
}

/* ---- 3-wire bus state machine -------------------------------------------- */

/*
 * fd6818_bus_update(clk, dat, cs) receives each change of the effective
 * CLK/DATA/CS levels, where DATA is the SoC's level only while it drives the
 * pad. The chip shifts on the CLK rising edge, as the stock firmware's own
 * bit loops do (fd6818b_shift_out_8bit @0x0002c1f8, fd6818b_shift_in_16bit
 * @0x0002c298).
 */
static void fd6818_bus_update(int clk, int dat, int cs)
{
    FD6818State *s = &fd6818;
    int rising = (!s->clk && clk);

    if (cs != s->cs) {
        s->cs = cs;
        if (cs) {
            /* Deasserted: the transfer is over. */
            if (s->bits) {
                fprintf(stderr, "  TRANSFER END after %d edges\n",
                        s->bit_count);
            }
            s->bit_count = 0;
            s->shift_in = 0;
            s->driving = 0;
        } else {
            /* Asserted: start a fresh transfer. */
            s->bit_count = 0;
            s->shift_in = 0;
            s->driving = 0;
            s->is_read = 0;
        }
    }

    if (!s->cs && rising) {
        if (s->bit_count < 8) {
            /* R/W̄ bit, then ADDR[6:0], MSB first. */
            s->shift_in = (s->shift_in << 1) | (dat ? 1u : 0u);
            s->bit_count++;

            if (s->bit_count == 8) {
                s->is_read = (s->shift_in >> 7) & 1u;
                s->addr    = (uint8_t) (s->shift_in & 0x7F);
                if (s->is_read) {
                    /*
                     * Turnaround. The master released DATA after the address
                     * byte, so the chip takes the line and presents bit 15
                     * immediately, before the read loop's first clkHigh().
                     * No clock edge is consumed here; the next rising edge
                     * is the one on which the master samples bit 15.
                     */
                    s->shift_out = fd6818_wire_read(s, s->addr);
                    s->driving   = 1;
                    s->dat_out   = (s->shift_out >> 15) & 1;
                    if (fd6818_log()) {
                        fprintf(stderr, "fd6818: READ %02XH (page %u) -> "
                                "0x%04x\n", s->addr, s->page, s->shift_out);
                    }
                }
                s->shift_in = 0;
            }
        } else if (s->is_read) {
            /*
             * Data phase of a read: present a bit on this rising edge, and
             * advance on the next one.
             *
             * Bit timing follows the master's actual loop. The master's
             * shiftOut() ends with CLK high, so the 8th address bit's rising
             * edge is consumed there. Its read loop then does, sixteen times:
             *     value <<= 1;  clkHigh();  delay;  sample DATA;  clkLow();
             *     delay;
             * Data bit N is therefore sampled after the rising edge that
             * corresponds to it, while CLK is high. The chip must have bit 15
             * on the line before the loop's first clkHigh(), which the
             * turnaround already provides, and must move to bit 14 only on
             * the edge after the one where bit 15 was sampled.
             */
            if (s->bits) {
                fprintf(stderr,
                        "  edge %2d: master samples dat_out=%d "
                        "(shift_out=0x%04x)\n",
                        s->bit_count, s->dat_out, s->shift_out);
            }

            /*
             * Edge 8 presents without advancing. shiftOut() leaves CLK high
             * after the address byte, so the read loop's opening
             * clkLow()/clkHigh() makes this edge, and the master samples
             * right after it, expecting bit 15. The turnaround has already
             * placed bit 15 on the line, so the shifter must hold there.
             */
            if (s->bit_count == 8) {
                s->bit_count++;
            } else {
                s->bit_count++;
                if (s->bit_count >= 25) {
                    /* 16 data bits delivered (edges 8..24). */
                    s->driving = 0;
                } else {
                    s->shift_out = (uint16_t) (s->shift_out << 1);
                    s->dat_out   = (s->shift_out >> 15) & 1;
                }
            }
        } else {
            /* Data phase of a write. */
            s->shift_in = (s->shift_in << 1) | (dat ? 1u : 0u);
            s->bit_count++;
            if (s->bit_count == 24) {
                fd6818_wire_write(s, s->addr,
                                  (uint16_t) (s->shift_in & 0xFFFF));
                s->bit_count = 0;
                s->shift_in = 0;
            }
        }
    }

    s->clk = clk;
}

/* ---- GPIO attachment ----------------------------------------------------- */

#define FD_CLK_PAD DM32_PTA(7)
#define FD_DAT_PAD DM32_PTA(8)
#define FD_CS_PAD  DM32_PTA(24)

/*
 * The effective levels the chip sees: CLK and CS straight from SWPORTA_DR,
 * and DATA from SWPORTA_DR only while SWPORTA_DDR drives the pad. Stock's bus
 * primitives make the same split: fd6818_reg_read and fd6818b_reg_write set
 * DDR bit 8 (|= 0x100 @0x0002c33c and @0x0002c2f4) to drive the address byte
 * and the write data, and fd6818b_shift_in_16bit (@0x0002c298) clears it
 * (& 0xfffffeff) before sampling DATA. With DDR bit 8 clear the value on DR
 * is not what the chip sees, so DATA is forwarded as 0.
 *
 * Only changes of the three levels are forwarded. fd6818_last starts at 0,
 * the GPIO reset state (all three pins low); the chip's own power-on state is
 * "CS deasserted", and the first forwarded change reconciles the two.
 */
static uint32_t fd6818_last;

static void fd6818_pins_changed(void *opaque, unsigned pad, bool driven,
                                int level)
{
    bool clk_driven, cs_driven, dat_driven;
    int clk = dm32_gpio_output(FD_CLK_PAD, &clk_driven);
    int cs = dm32_gpio_output(FD_CS_PAD, &cs_driven);
    int dat = dm32_gpio_output(FD_DAT_PAD, &dat_driven);
    uint32_t now;

    if (!dat_driven) {
        dat = 0;
    }
    now = (uint32_t)clk | ((uint32_t)dat << 1) | ((uint32_t)cs << 2);
    if (now == fd6818_last) {
        return;
    }
    fd6818_last = now;
    fd6818_bus_update(clk, dat, cs);
}

/* While the chip drives DATA (a read's data phase), it sets pad PTA8. */
static int fd6818_pin_input(void *opaque, unsigned pad)
{
    if (pad != FD_DAT_PAD || !fd6818.driving) {
        return -1;
    }
    return fd6818.dat_out & 1;
}

static const DM32GpioPinOps fd6818_pin_ops = {
    .output = fd6818_pins_changed,
    .input = fd6818_pin_input,
};

/* GPIO0 and GPIO1 on GPIOB 27 and 26: the chip's level while it drives. */
static void fd6818_gpio_pad_output(void *opaque, unsigned pad, bool driven,
                                   int level)
{
    /* The chip reads the pad through gpio_in_val; nothing to latch. */
}

static int fd6818_gpio_pad_input(void *opaque, unsigned pad)
{
    return fd6818_pin_out(&fd6818, pad == FD_GPIO0_PAD ? 0 : 1);
}

static const DM32GpioPinOps fd6818_gpio_pad_ops = {
    .output = fd6818_gpio_pad_output,
    .input = fd6818_gpio_pad_input,
};

/*
 * The FD6818 is a separate chip: SoC resets (machine reset, watchdog,
 * SYS_SOFT_RSTN) do not reset its registers. Assumption: no SoC pin drives
 * its reset; it resets at power-up and by the REG_00H soft reset. A transfer
 * cut short by a SoC reset ends when the firmware next releases CS.
 */
void dm32_fd6818_init(void)
{
    FD6818State *s = &fd6818;
    /* REG_62H is pinned when DM32_FD6818_RSSI is set; the sentinel is unset */
    uint64_t rssi = dm32_env_uint("DM32_FD6818_RSSI", FD_RSSI_UNSET, 0, 0xff);

    s->log = dm32_env_flag("DM32_FD6818_LOG");
    s->bits = dm32_env_flag("DM32_FD6818_BITS");
    s->chip_id = dm32_env_uint("DM32_FD6818_ID", 0x4819, 0, 0xffff);
    s->lock_ns = (int64_t) dm32_env_uint("DM32_FD6818_LOCK_US", 40, 0,
                                         1000000) * SCALE_US;
    s->rssi_fixed = rssi != FD_RSSI_UNSET;
    s->rssi_base = s->rssi_fixed ? rssi : 0x30;
    s->rssi_span = dm32_env_uint("DM32_FD6818_RSSI_JITTER", 12, 0, 0xff);

    fd6818_reset(s);
    s->cs = 1;
    s->gpio_level = 0xA;    /* both pins undriven */
    fd6818_env_load(s);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, fd6818_timer, s);
    if (s->nev) {
        timer_mod(s->timer, s->ev[0].ns);
    }
    dm32_gpio_attach(FD_CLK_PAD, &fd6818_pin_ops, NULL);
    dm32_gpio_attach(FD_DAT_PAD, &fd6818_pin_ops, NULL);
    dm32_gpio_attach(FD_CS_PAD, &fd6818_pin_ops, NULL);
    dm32_gpio_attach(FD_GPIO0_PAD, &fd6818_gpio_pad_ops, NULL);
    dm32_gpio_attach(FD_GPIO1_PAD, &fd6818_gpio_pad_ops, NULL);
}
