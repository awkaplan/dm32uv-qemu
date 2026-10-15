/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Baofeng DM-32UV I2C master controller (DesignWare APB I2C, HR_C7000
 * variant): behavioural model.
 *
 * Copyright (c) 2026 Alex Kaplan
 *
 * HR_C7000 v2.7 EN section 5.1 (p.218-243), Register Tables 9 and 10. The
 * block is a DesignWare APB I2C cut down to a bus master with an added start
 * register: IC_DATA_CMD has no STOP or RESTART bits, and a transfer begins
 * when IC_START is set. The board creates three: I2C0 (0x14060000, PIC 8),
 * I2C1 (0x14070000, PIC 7) and I2C2 (0x14080000, PIC 9). Each drives a QEMU
 * I2C bus named "i2c<n>"; the RTC sits on i2c2.
 *
 *   0x00 IC_CON          [6] SLAVE_DISABLE, [5] RESTART_EN, [4] 10BITADDR_
 *                        MASTER, [3] 10BITADDR_SLAVE, [2:1] SPEED, [0]
 *                        MASTER_MODE; reset 0x7d
 *   0x04 IC_TAR          [11] SPECIAL, [10] GC_OR_START, [9:0] target;
 *                        reset 0x55
 *   0x10 IC_DATA_CMD     [8] CMD (1 read, 0 write), [7:0] DAT
 *   0x1c IC_FS_SCL_HCNT  reset 0x5a     0x20 IC_FS_SCL_LCNT  reset 0xa0
 *   0x2c IC_INTR_STAT    0x30 IC_INTR_MASK (reset 0x8ff)
 *   0x34 IC_RAW_INTR_STAT  [10] START_DET, [9] STOP_DET, [8] ACTIVITY,
 *                        [6] TX_ABRT, [4] TX_EMPTY, [3] TX_OVER,
 *                        [2] RX_FULL, [1] RX_OVER, [0] RX_UNDER
 *   0x38 IC_RX_TL, 0x3c IC_TX_TL   0-7, reset 6
 *   0x40 IC_CLR_INTR, 0x44-0x64 IC_CLR_<interrupt>   read to clear
 *   0x6c IC_ENABLE       0x70 IC_STATUS (reset 0x6)
 *   0x74 IC_TXFLR, 0x78 IC_RXFLR    [3:0]
 *   0x80 IC_TX_ABRT_SOURCE          0x9c IC_ENABLE_STATUS [0] IC_EN
 *   0xa0 IC_START        [0]: with data in the TX FIFO, starts sending
 *
 * Transfers. While IC_ENABLE and IC_START are 1 and the TX FIFO holds a
 * command, the controller sends START and the address, then executes the
 * commands in order. A change of direction sends RESTART and the address
 * again. The DS: "once the FIFO is empty, STOP will be triggered" (p.222),
 * so a STOP follows the byte that empties the TX FIFO. A read command
 * receives one byte into the RX FIFO; the byte before a STOP is NACKed.
 * A command leaves the TX FIFO when its byte starts; its result (a sent
 * byte, a received byte, an abort) lands when the byte ends.
 *
 * Timing, in virtual time: one SCL period is (IC_FS_SCL_HCNT + 8) +
 * (IC_FS_SCL_LCNT + 1) cycles of ic_clk, the APB clock (p.220), with the
 * counts held at their minimums of 6 and 8. A byte with its acknowledge is
 * nine periods. Assumption: START, RESTART and STOP take one period each.
 * Stock programs 400 kHz from an 84 MHz clock (i2c2_master_init_400khz
 * @0x0301dacc), so its periods are 2.5 us. Assumption: the timing uses the
 * fast-mode counts whatever IC_CON.SPEED says; the datasheet documents no
 * standard-mode count registers.
 *
 * Interrupts. RAW_INTR_STAT bits latch until their IC_CLR_* read, except
 * TX_EMPTY and RX_FULL, which follow the FIFO levels. TX_EMPTY is set while
 * the TX FIFO holds no more than IC_TX_TL entries. RX_FULL follows IC_RX_TL's
 * own description (p.233): set while the RX FIFO holds more than IC_RX_TL
 * entries, or 7 or more when IC_RX_TL is 7. (The RAW_INTR_STAT text, p.232,
 * says "greater than or equal".) ACTIVITY is the DS's ACTIVITY_NEGATE, set
 * when a transfer ends. IC_CLR_INTR and IC_CLR_TX_ABRT also clear
 * IC_TX_ABRT_SOURCE (p.234, 236). The output is INTR_STAT = RAW & MASK.
 * RD_REQ, RX_DONE and GEN_CALL are slave or general-call events and stay 0.
 *
 * Aborts set IC_TX_ABRT_SOURCE and TX_ABRT, flush the TX FIFO and end the
 * transfer with STOP: ABRT_7B_ADDR_NOACK when no slave acknowledges the
 * address, ABRT_TXDATA_NOACK when a written byte is not acknowledged,
 * ABRT_MASTER_DIS when IC_CON.MASTER_MODE is 0. Assumption: with
 * IC_CON.10BITADDR_MASTER set, no slave acknowledges the first address
 * byte (ABRT_10ADDR1_NOACK); QEMU I2C buses carry 7-bit addresses only.
 *
 * Assumptions following the DesignWare APB I2C the block is built from (the
 * datasheet does not say):
 *   - FIFO depth 8: IC_TXFLR and IC_RXFLR are 4 bits and the thresholds run
 *     0-7. A threshold above 7 reads back 7.
 *   - IC_CON, IC_TAR and the SCL counts are written only while IC_ENABLE is
 *     0, as the initialisation sequence orders (p.221-222); a write while
 *     enabled is refused and logged.
 *   - IC_DATA_CMD writes are dropped while the controller is disabled and
 *     while TX_ABRT is set. A write to a full TX FIFO is dropped and sets
 *     TX_OVER; a received byte with the RX FIFO full is dropped and sets
 *     RX_OVER; a read from an empty RX FIFO returns 0 and sets RX_UNDER.
 *   - Clearing IC_ENABLE lets the byte in flight finish, then sends STOP
 *     ("allows unfinished transfers to continue", p.222); IC_ENABLE_STATUS
 *     stays 1 until then. Both FIFOs are flushed when it drops to 0.
 *   - TX_EMPTY, while disabled, follows activity rather than the level.
 *   - IC_START is stored; it does not clear itself (stock clears it).
 *   - The IC_CLR_* registers read 0.
 * Not modelled: the slave mode, general call and START byte (IC_TAR SPECIAL,
 * "not supported", p.228), high-speed mode, SDA timing.
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/i2c/i2c.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/csky/dm32.h"

#define TYPE_DM32_I2C "dm32_i2c"
OBJECT_DECLARE_SIMPLE_TYPE(DM32I2cState, DM32_I2C)

#define WIN          0x100
#define FIFO_DEPTH   8

#define R_CON        0x00
#define R_TAR        0x04
#define R_DATA_CMD   0x10
#define R_FS_HCNT    0x1c
#define R_FS_LCNT    0x20
#define R_INTR_STAT  0x2c
#define R_INTR_MASK  0x30
#define R_RAW_INTR   0x34
#define R_RX_TL      0x38
#define R_TX_TL      0x3c
#define R_CLR_INTR   0x40
#define R_CLR_RX_UNDER  0x44
#define R_CLR_RX_OVER   0x48
#define R_CLR_TX_OVER   0x4c
#define R_CLR_RD_REQ    0x50
#define R_CLR_TX_ABRT   0x54
#define R_CLR_RX_DONE   0x58
#define R_CLR_ACTIVITY  0x5c
#define R_CLR_STOP_DET  0x60
#define R_CLR_START_DET 0x64
#define R_ENABLE     0x6c
#define R_STATUS     0x70
#define R_TXFLR      0x74
#define R_RXFLR      0x78
#define R_ABRT_SRC   0x80
#define R_EN_STATUS  0x9c
#define R_START      0xa0

#define CON_MASTER_MODE  (1u << 0)
#define CON_10BIT_MASTER (1u << 4)
#define CON_RESTART_EN   (1u << 5)
#define CON_MASK         0x7fu
#define TAR_MASK         0xfffu
#define DATA_CMD_READ    (1u << 8)

#define INT_RX_UNDER   (1u << 0)
#define INT_RX_OVER    (1u << 1)
#define INT_RX_FULL    (1u << 2)
#define INT_TX_OVER    (1u << 3)
#define INT_TX_EMPTY   (1u << 4)
#define INT_TX_ABRT    (1u << 6)
#define INT_ACTIVITY   (1u << 8)
#define INT_STOP_DET   (1u << 9)
#define INT_START_DET  (1u << 10)
#define INT_MASK_BITS  0xfffu

#define ABRT_7B_ADDR_NOACK   (1u << 0)
#define ABRT_10ADDR1_NOACK   (1u << 1)
#define ABRT_TXDATA_NOACK    (1u << 3)
#define ABRT_MASTER_DIS      (1u << 11)

#define ST_ACTIVITY      (1u << 0)
#define ST_TFNF          (1u << 1)
#define ST_TFE           (1u << 2)
#define ST_RFNE          (1u << 3)
#define ST_RFF           (1u << 4)
#define ST_MST_ACTIVITY  (1u << 5)

typedef enum {
    PH_IDLE,
    PH_BYTE,           /* a command's byte (and address, if any) on the bus */
    PH_STOP,           /* the STOP condition */
} DM32I2cPhase;

struct DM32I2cState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    I2CBus *bus;
    QEMUTimer *timer;
    uint32_t instance;

    uint32_t con, tar, hcnt, lcnt, intr_mask, rx_tl, tx_tl;
    uint32_t enable, start, ic_en;
    uint32_t raw;               /* latched RAW_INTR_STAT bits */
    uint32_t abrt_source;

    uint16_t txfifo[FIFO_DEPTH];
    unsigned tx_head, tx_count;
    uint8_t rxfifo[FIFO_DEPTH];
    unsigned rx_head, rx_count;

    DM32I2cPhase phase;
    bool active;                /* between START and STOP */
    int bus_dir;                /* -1 no bus transaction, 0 write, 1 read */
    uint16_t cur;               /* command whose byte is on the bus */
    bool cur_addr;              /* the byte is preceded by (RE)START+address */
    bool log;
};

static uint32_t dm32_i2c_raw(DM32I2cState *s)
{
    uint32_t raw = s->raw;
    bool tx_empty = s->ic_en ? s->tx_count <= s->tx_tl : s->active;
    bool rx_full = s->rx_tl >= 7 ? s->rx_count >= 7 : s->rx_count > s->rx_tl;

    if (tx_empty) {
        raw |= INT_TX_EMPTY;
    }
    if (rx_full) {
        raw |= INT_RX_FULL;
    }
    return raw;
}

static void dm32_i2c_update(DM32I2cState *s)
{
    qemu_set_irq(s->irq, (dm32_i2c_raw(s) & s->intr_mask) != 0);
}

static int64_t dm32_i2c_period_ns(DM32I2cState *s)
{
    uint64_t cycles = (MAX(s->hcnt, 6) + 8) + (MAX(s->lcnt, 8) + 1);

    return muldiv64(cycles, NANOSECONDS_PER_SECOND, DM32_APB_HZ);
}

static void dm32_i2c_schedule(DM32I2cState *s, DM32I2cPhase phase,
                              unsigned periods)
{
    s->phase = phase;
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              periods * dm32_i2c_period_ns(s));
}

static void dm32_i2c_flush(DM32I2cState *s)
{
    s->tx_head = s->tx_count = 0;
    s->rx_head = s->rx_count = 0;
}

static void dm32_i2c_end_bus(DM32I2cState *s)
{
    if (s->bus_dir >= 0) {
        i2c_end_transfer(s->bus);
        s->bus_dir = -1;
    }
}

static void dm32_i2c_abort(DM32I2cState *s, uint32_t src)
{
    s->abrt_source |= src;
    s->raw |= INT_TX_ABRT;
    s->tx_head = s->tx_count = 0;
    if (s->log) {
        fprintf(stderr, "dm32_i2c%u: abort 0x%x (target 0x%02x)\n",
                s->instance, src, s->tar & 0x3ff);
    }
    dm32_i2c_schedule(s, PH_STOP, 1);
}

/* Takes the next command onto the bus, or sends STOP when none is left. */
static void dm32_i2c_next(DM32I2cState *s)
{
    int dir;

    if (!s->tx_count || !s->enable) {
        dm32_i2c_schedule(s, PH_STOP, 1);
        return;
    }
    s->cur = s->txfifo[s->tx_head];
    s->tx_head = (s->tx_head + 1) % FIFO_DEPTH;
    s->tx_count--;
    dir = (s->cur & DATA_CMD_READ) ? 1 : 0;
    s->cur_addr = s->bus_dir != dir;
    dm32_i2c_schedule(s, PH_BYTE, s->cur_addr ? 1 + 9 + 9 : 9);
}

static void dm32_i2c_kick(DM32I2cState *s)
{
    if (s->active || !s->ic_en || !s->enable || !(s->start & 1) ||
        !s->tx_count || (s->raw & INT_TX_ABRT)) {
        return;
    }
    s->active = true;
    s->bus_dir = -1;
    if (!(s->con & CON_MASTER_MODE)) {
        dm32_i2c_abort(s, ABRT_MASTER_DIS);
        return;
    }
    dm32_i2c_next(s);
}

/*
 * I2C0 and I2C1 reach the bus only while IO_DIPLEX0 gives them their pads:
 * i2c0_scl/sda_sel (PTA5, PTA6) and i2c1_scl/sda_sel (PTA7, PTA8), value 0
 * (v2.7 EN 4.4.4.4). I2C2 is internal ("RTC only", pin table) and has none.
 */
static bool dm32_i2c_pads_muxed(DM32I2cState *s)
{
    switch (s->instance) {
    case 0:
        return dm32_pad_function(DM32_PTA(5)) == 0 &&
               dm32_pad_function(DM32_PTA(6)) == 0;
    case 1:
        return dm32_pad_function(DM32_PTA(7)) == 0 &&
               dm32_pad_function(DM32_PTA(8)) == 0;
    default:
        return true;
    }
}

static void dm32_i2c_byte_done(DM32I2cState *s)
{
    int dir = (s->cur & DATA_CMD_READ) ? 1 : 0;
    uint8_t addr = s->tar & 0x7f;

    if (s->cur_addr) {
        if (s->bus_dir >= 0 && !(s->con & CON_RESTART_EN)) {
            /* No RESTART: end the transaction and start a new one. */
            dm32_i2c_end_bus(s);
        }
        s->raw |= INT_START_DET;
        if (!dm32_i2c_pads_muxed(s)) {
            /* The address goes out on no pad, so nothing acknowledges it. */
            dm32_event(DM32_EV_PAD_UNCLAIMED, "I2C%u transfer while its "
                       "SCL/SDA pads are not I2C", s->instance);
            dm32_i2c_end_bus(s);
            dm32_i2c_abort(s, ABRT_7B_ADDR_NOACK);
            return;
        }
        if (s->con & CON_10BIT_MASTER) {
            dm32_i2c_end_bus(s);
            dm32_i2c_abort(s, ABRT_10ADDR1_NOACK);
            return;
        }
        if (i2c_start_transfer(s->bus, addr, dir)) {
            s->bus_dir = -1;
            dm32_i2c_abort(s, ABRT_7B_ADDR_NOACK);
            return;
        }
        s->bus_dir = dir;
    }

    if (!dir) {
        if (i2c_send(s->bus, s->cur & 0xff)) {
            dm32_i2c_end_bus(s);
            dm32_i2c_abort(s, ABRT_TXDATA_NOACK);
            return;
        }
    } else {
        uint8_t b = i2c_recv(s->bus);

        if (!s->tx_count || !s->enable) {
            i2c_nack(s->bus);
        }
        if (s->rx_count < FIFO_DEPTH) {
            s->rxfifo[(s->rx_head + s->rx_count) % FIFO_DEPTH] = b;
            s->rx_count++;
        } else {
            s->raw |= INT_RX_OVER;
        }
    }
    dm32_i2c_next(s);
}

static void dm32_i2c_stop_done(DM32I2cState *s)
{
    dm32_i2c_end_bus(s);
    s->active = false;
    s->phase = PH_IDLE;
    s->raw |= INT_STOP_DET | INT_ACTIVITY;
    if (!s->enable) {
        s->ic_en = 0;
        dm32_i2c_flush(s);
    }
    dm32_i2c_kick(s);
}

static void dm32_i2c_timer(void *opaque)
{
    DM32I2cState *s = opaque;

    switch (s->phase) {
    case PH_BYTE:
        dm32_i2c_byte_done(s);
        break;
    case PH_STOP:
        dm32_i2c_stop_done(s);
        break;
    default:
        break;
    }
    dm32_i2c_update(s);
}

static uint32_t dm32_i2c_clear(DM32I2cState *s, uint32_t bits)
{
    s->raw &= ~bits;
    if (bits & INT_TX_ABRT) {
        s->abrt_source = 0;
    }
    return 0;
}

static uint64_t dm32_i2c_read(void *opaque, hwaddr off, unsigned size)
{
    DM32I2cState *s = opaque;
    uint32_t v = 0;

    switch (off) {
    case R_CON:
        return s->con;
    case R_TAR:
        return s->tar;
    case R_DATA_CMD:
        if (!s->rx_count) {
            s->raw |= INT_RX_UNDER;
            break;
        }
        v = s->rxfifo[s->rx_head];
        s->rx_head = (s->rx_head + 1) % FIFO_DEPTH;
        s->rx_count--;
        break;
    case R_FS_HCNT:
        return s->hcnt;
    case R_FS_LCNT:
        return s->lcnt;
    case R_INTR_STAT:
        return dm32_i2c_raw(s) & s->intr_mask;
    case R_INTR_MASK:
        return s->intr_mask;
    case R_RAW_INTR:
        return dm32_i2c_raw(s);
    case R_RX_TL:
        return s->rx_tl;
    case R_TX_TL:
        return s->tx_tl;
    case R_CLR_INTR:
        v = dm32_i2c_clear(s, INT_MASK_BITS & ~(INT_TX_EMPTY | INT_RX_FULL));
        break;
    case R_CLR_RX_UNDER:
        v = dm32_i2c_clear(s, INT_RX_UNDER);
        break;
    case R_CLR_RX_OVER:
        v = dm32_i2c_clear(s, INT_RX_OVER);
        break;
    case R_CLR_TX_OVER:
        v = dm32_i2c_clear(s, INT_TX_OVER);
        break;
    case R_CLR_TX_ABRT:
        v = dm32_i2c_clear(s, INT_TX_ABRT);
        break;
    case R_CLR_ACTIVITY:
        v = dm32_i2c_clear(s, INT_ACTIVITY);
        break;
    case R_CLR_STOP_DET:
        v = dm32_i2c_clear(s, INT_STOP_DET);
        break;
    case R_CLR_START_DET:
        v = dm32_i2c_clear(s, INT_START_DET);
        break;
    case R_CLR_RD_REQ:
    case R_CLR_RX_DONE:
        break;                          /* slave events, never set */
    case R_ENABLE:
        return s->enable;
    case R_STATUS:
        v = (s->tx_count < FIFO_DEPTH ? ST_TFNF : 0) |
            (s->tx_count == 0 ? ST_TFE : 0) |
            (s->rx_count ? ST_RFNE : 0) |
            (s->rx_count == FIFO_DEPTH ? ST_RFF : 0) |
            (s->active ? ST_ACTIVITY | ST_MST_ACTIVITY : 0);
        return v;
    case R_TXFLR:
        return s->tx_count;
    case R_RXFLR:
        return s->rx_count;
    case R_ABRT_SRC:
        return s->abrt_source;
    case R_EN_STATUS:
        return s->ic_en;
    case R_START:
        return s->start;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "dm32_i2c%u: read of undefined "
                      "offset 0x%02" HWADDR_PRIx "\n", s->instance, off);
        return 0;
    }
    dm32_i2c_update(s);
    return v;
}

static bool dm32_i2c_locked(DM32I2cState *s, hwaddr off)
{
    if (s->enable) {
        qemu_log_mask(LOG_GUEST_ERROR, "dm32_i2c%u: write to offset 0x%02"
                      HWADDR_PRIx " while IC_ENABLE is 1 refused\n",
                      s->instance, off);
        return true;
    }
    return false;
}

static void dm32_i2c_push(DM32I2cState *s, uint32_t v)
{
    if (!s->ic_en || (s->raw & INT_TX_ABRT)) {
        return;
    }
    if (s->tx_count == FIFO_DEPTH) {
        s->raw |= INT_TX_OVER;
        return;
    }
    s->txfifo[(s->tx_head + s->tx_count) % FIFO_DEPTH] = v & 0x1ff;
    s->tx_count++;
}

static void dm32_i2c_write(void *opaque, hwaddr off, uint64_t val,
                           unsigned size)
{
    DM32I2cState *s = opaque;
    uint32_t v = val;

    switch (off) {
    case R_CON:
        if (!dm32_i2c_locked(s, off)) {
            s->con = v & CON_MASK;
        }
        break;
    case R_TAR:
        if (!dm32_i2c_locked(s, off)) {
            s->tar = v & TAR_MASK;
        }
        break;
    case R_FS_HCNT:
        if (!dm32_i2c_locked(s, off)) {
            s->hcnt = v & 0xffff;
        }
        break;
    case R_FS_LCNT:
        if (!dm32_i2c_locked(s, off)) {
            s->lcnt = v & 0xffff;
        }
        break;
    case R_DATA_CMD:
        dm32_i2c_push(s, v);
        break;
    case R_INTR_MASK:
        s->intr_mask = v & INT_MASK_BITS;
        break;
    case R_RX_TL:
        s->rx_tl = MIN(v & 0xff, FIFO_DEPTH - 1);
        break;
    case R_TX_TL:
        s->tx_tl = MIN(v & 0xff, FIFO_DEPTH - 1);
        break;
    case R_ENABLE:
        s->enable = v & 1;
        if (s->enable) {
            s->ic_en = 1;
        } else if (!s->active) {
            s->ic_en = 0;
            dm32_i2c_flush(s);
        }
        break;
    case R_START:
        s->start = v & 1;
        break;
    case R_INTR_STAT:
    case R_RAW_INTR:
    case R_CLR_INTR ... R_CLR_START_DET:
    case R_STATUS:
    case R_TXFLR:
    case R_RXFLR:
    case R_ABRT_SRC:
    case R_EN_STATUS:
        break;                          /* read-only */
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "dm32_i2c%u: write to undefined "
                      "offset 0x%02" HWADDR_PRIx "\n", s->instance, off);
        return;
    }
    dm32_i2c_kick(s);
    dm32_i2c_update(s);
}

static const MemoryRegionOps dm32_i2c_ops = {
    .read = dm32_i2c_read,
    .write = dm32_i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void dm32_i2c_reset(DeviceState *dev)
{
    DM32I2cState *s = DM32_I2C(dev);

    timer_del(s->timer);
    dm32_i2c_end_bus(s);
    s->con = 0x7d;
    s->tar = 0x55;
    s->hcnt = 0x5a;
    s->lcnt = 0xa0;
    s->intr_mask = 0x8ff;
    s->rx_tl = s->tx_tl = 6;
    s->enable = s->start = s->ic_en = 0;
    s->raw = 0;
    s->abrt_source = 0;
    dm32_i2c_flush(s);
    s->phase = PH_IDLE;
    s->active = false;
    dm32_i2c_update(s);
}

static void dm32_i2c_realize(DeviceState *dev, Error **errp)
{
    DM32I2cState *s = DM32_I2C(dev);
    g_autofree char *name = g_strdup_printf("i2c%u", s->instance);

    s->bus = i2c_init_bus(dev, name);
}

static void dm32_i2c_init(Object *obj)
{
    DM32I2cState *s = DM32_I2C(obj);

    memory_region_init_io(&s->iomem, obj, &dm32_i2c_ops, s, TYPE_DM32_I2C,
                          WIN);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dm32_i2c_timer, s);
    s->bus_dir = -1;
    s->log = dm32_env_flag("DM32_I2C_LOG");
}

static const Property dm32_i2c_props[] = {
    DEFINE_PROP_UINT32("instance", DM32I2cState, instance, 0),
};

static void dm32_i2c_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = dm32_i2c_realize;
    device_class_set_legacy_reset(dc, dm32_i2c_reset);
    device_class_set_props(dc, dm32_i2c_props);
}

static const TypeInfo dm32_i2c_type = {
    .name = TYPE_DM32_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DM32I2cState),
    .instance_init = dm32_i2c_init,
    .class_init = dm32_i2c_class_init,
};

static void dm32_i2c_register(void)
{
    type_register_static(&dm32_i2c_type);
}

type_init(dm32_i2c_register)

I2CBus *dm32_i2c_create(int n, hwaddr base, qemu_irq irq)
{
    DeviceState *dev = qdev_new(TYPE_DM32_I2C);

    qdev_prop_set_uint32(dev, "instance", n);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, base);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, irq);
    return DM32_I2C(dev)->bus;
}
