/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Baofeng DM-32UV SSI (SPI master) and W25Q128 SPI-NOR data flash: SSI
 * controller and SPI-NOR flash model
 *
 * Copyright (c) 2026 Alex Kaplan
 *
 * Models the SSI (SPI master) controller and the 16 MB Winbond W25Q128 data
 * chip attached to it. The chip holds the codeplug and configuration storage,
 * and the stock firmware reads and writes it through this model.
 *
 * SSI controller (HR_C7000 v2.7 EN section 5.2, p.243-264; Register Table
 * 11; SVD SPI_Master0). The stock base table at flash 0x030ada00 lists the
 * block at 0x140a0000 (SSI0, the data chip), 0x140b0000 (SSI1, SD card),
 * 0x140e0000 (SSI2), 0x14150000 (SSI3, "EFUSE only") and 0x04000000 (the
 * program flash's controller).
 *   +0x00 CTRLR0   [9:8] TMOD, [7] SCPOL, [6] SCPH, [5] M2L, [4:0] DFS
 *                  (frame size - 1), reset 0x7
 *   +0x04 CTRLR1   [15:0] NDF, frames to receive - 1 in TMOD 2 and 3
 *   +0x08 SSIENR   [0]; 0 flushes both FIFOs and the interrupt status
 *   +0x10 SER      [1:0] the controller's own slave selects
 *   +0x14 BAUDR    [15:0] SCKDV: SCLK = APB clock / SCKDV, even, 0 = off
 *   +0x18 TXFTLR, +0x1c RXFTLR   FIFO thresholds
 *   +0x20 TXFLR, +0x24 RXFLR     FIFO levels
 *   +0x28 SR       [4] RFF, [3] RFNE, [2] TFE, [1] TFNF, [0] BUSY; reset 0x6
 *   +0x2c IMR (reset 0x3f), +0x30 ISR, +0x34 RISR: [4] RXF, [3] RXO,
 *                  [2] RXU, [1] TXO, [0] TXE; ISR = RISR & IMR
 *   +0x38 TXOICR, +0x3c RXOICR, +0x40 RXUICR, +0x48 ICR   read to clear
 *   +0x58 IDR 0x11223344, +0x5c SSI_COMP_VERSION 0x3331312a
 *   +0x60 DR       write: push to the TX FIFO; read: pop the RX FIFO
 *
 * Transfers. Each FIFO holds 128 frames ("FIFO depth 128", p.244). While
 * SSIENR is 1, SER is not 0, BAUDR is not 0 and the instance's pads select it
 * (ssi_pads_muxed), the controller shifts one frame at a time, each taking
 * (DFS + 1) * SCKDV cycles of the 84 MHz APB clock, in virtual time. A frame
 * leaves the TX FIFO when it starts and its received frame enters the RX
 * FIFO when it ends; SR.BUSY is set while frames shift. The progress is
 * computed when the guest next touches the controller; a timer runs only
 * while an interrupt is unmasked. TMOD (p.255): 0 transmit and receive, 1
 * transmit only (received frames dropped), 2 receive only (a write to DR
 * starts NDF + 1 frames, each sending the written value, p.250), 3 EEPROM
 * read (the TX FIFO is sent, received frames dropped, then NDF + 1 frames
 * are received while 0 is sent).
 *
 * Interrupts, on PIC 16 (SSI0), 22 (SSI1) and 23 (SSI2) (Table 30; SSI3 and
 * 0x04000000 have none): RXF while the RX FIFO holds more than RXFTLR frames
 * (p.249); TXE while the TX FIFO holds no more than TXFTLR; TXO on a write to
 * a full TX FIFO (the frame is lost); RXO on a frame received into a full RX
 * FIFO (lost); RXU on a read of an empty RX FIFO, which returns 0xff. The
 * line is ISR != 0.
 *
 * Assumptions, following the DesignWare APB SSI the block is built from (the
 * datasheet does not say): CTRLR0, CTRLR1, BAUDR, TXFTLR and RXFTLR are
 * refused while SSIENR is 1 ("when 1 disable configuration register",
 * p.256; refused writes are logged); TXE and the overflow rules above; a DR
 * write with SSIENR 0 is dropped (ssi_discard); IMR bit 1 enables; DMACR
 * (+0x4c) is storage. With DFS below 3, the frame size is taken from bits
 * [20:16] (the DesignWare 32-bit layout, which the 0x04000000 instance's
 * CTRLR0 0x70000 uses). Devices see the low 8 bits of a frame.
 *
 * Chip selects. The data chip on SSI0 is selected by GPIOA 18 driven low
 * (stock brackets every operation with pin 0x12), not by SER: the pad is
 * attached through dm32_gpio_attach(). The other instances select their
 * device with SER bits 1:0 while SSIENR is 1. Releasing the select ends the
 * SPI-NOR command. A frame shifted with no device selected reads 0xff, and
 * one shifted on pads that do not select the controller is lost
 * (ssi_discard) and reads 0xff (assumption: MISO idles high).
 *
 * SPI-NOR commands:
 *   - READ 0x03 and FAST_READ 0x0b: 24-bit address, then streamed data.
 *   - RDSR 0x05, RDID 0x9f (JEDEC id EF 40 18), WREN 0x06, WRDI 0x04.
 *   - PAGE PROGRAM 0x02, sector erase 0x20 (4 KiB), block erase 0xd8
 *     (64 KiB), chip erase 0xc7 or 0x60.
 *   - WRSR and other commands are accepted with no effect.
 * Programming clears bits only (store &= data). Erase sets bytes to 0xff.
 * Both need the write-enable latch set by WREN, and the chip clears the latch
 * when the operation completes. Writing without erasing first therefore leaves
 * the AND of the old and new bytes, as the real part does.
 * After a completed PROGRAM or ERASE, the next DM32_SPINOR_WIP_READS RDSR
 * commands report WIP=1 (see rdsr_latch()).
 *
 * Backing store: each instance owns a 16 MB store. The store starts as 0xff
 * (erased), which is the state the firmware boots from. If DM32_SPINOR names a
 * file, its contents are loaded over the blank store. A short file is fine.
 *
 * Environment variables:
 *   DM32_SPINOR=<path>         initial contents of the store
 *   DM32_SPINOR_OUT=<path>     at exit, write the store of each instance that
 *                              saw a PROGRAM or ERASE (see spinor_dump_all())
 *   DM32_SPINOR_SYNC=1         also write after every completed PROGRAM or
 *                              ERASE
 *   DM32_SPINOR_WIP_READS=<n>  number of WIP-reporting RDSR reads after a
 *                              completed PROGRAM or ERASE (default 2)
 *   DM32_SPINOR_LOG           log register and SPI-NOR activity to stderr
 *
 * Not modelled: the dual and quad transfer modes, the security registers, the
 * effect of WRSR, and the real duration of busy periods. WIP is reported for a
 * fixed number of RDSR reads instead.
 */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "qemu/notify.h"
#include "system/system.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "hw/csky/dm32.h"

#define TYPE_DM32_SSI "dm32_ssi"
OBJECT_DECLARE_SIMPLE_TYPE(DM32SsiState, DM32_SSI)

#define WIN 0x1000
#define R_CTRLR0   0x00
#define R_CTRLR1   0x04
#define R_SSIENR   0x08
#define R_SER      0x10
#define R_BAUDR    0x14
#define R_TXFTLR   0x18
#define R_RXFTLR   0x1c
#define R_TXFLR    0x20
#define R_RXFLR    0x24
#define R_SR       0x28
#define R_IMR      0x2c
#define R_ISR      0x30
#define R_RISR     0x34
#define R_TXOICR   0x38
#define R_RXOICR   0x3c
#define R_RXUICR   0x40
#define R_ICR      0x48
#define R_DMACR    0x4c
#define R_IDR      0x58
#define R_VERSION  0x5c
#define R_DR       0x60

/* Identification values at R_IDR and R_VERSION, see the header (section 5.2) */
#define SSI_IDR_VALUE      0x11223344
#define SSI_COMP_VERSION   0x3331312a

#define SR_BUSY    (1u << 0)
#define SR_TFNF    (1u << 1)
#define SR_TFE     (1u << 2)
#define SR_RFNE    (1u << 3)
#define SR_RFF     (1u << 4)

#define INT_TXE    (1u << 0)
#define INT_TXO    (1u << 1)
#define INT_RXU    (1u << 2)
#define INT_RXO    (1u << 3)
#define INT_RXF    (1u << 4)

#define TMOD(s)    (((s)->ctrlr0 >> 8) & 3)
#define TMOD_TX    1
#define TMOD_RX    2
#define TMOD_EEPROM 3

#define SSI0_BASE  0x140a0000

/*
 * Size of the backing store: 16 MB, the Winbond W25Q128 data chip. It is
 * reached on SSI0 with chip-select 0x12 and is separate from the 1 MB program
 * flash. nor_byte() and nor_program() wrap addresses with (NOR_SIZE - 1), so
 * a smaller size would fold every access above it onto the low addresses.
 *
 * The size is a fidelity matter as well. A model that implements ERASE with a
 * 1 MB mask would erase the wrong sector and still appear to work.
 */
#define NOR_SIZE (16u << 20)

#define FIFO_DEPTH 128

struct DM32SsiState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer *timer;

    /* controller registers */
    uint32_t ctrlr0, ctrlr1, ssienr, ser, baudr, txftlr, rxftlr, imr, dmacr;
    uint16_t txq[FIFO_DEPTH];
    uint16_t rxq[FIFO_DEPTH];
    int tx_head, tx_count, rx_head, rx_count;
    uint32_t latched;     /* TXO, RXU, RXO */
    bool shifting;        /* a frame is on the wire */
    uint16_t cur;         /* the frame on the wire */
    bool cur_rx;          /* its received frame goes to the RX FIFO */
    int64_t t;            /* start of the frame on the wire, or idle time */
    uint32_t rx_left;     /* frames still to receive (TMOD 2 and 3) */
    uint16_t rx_word;     /* the value sent while receiving */
    bool cs_low;          /* SSI0: GPIOA 18 driven low */

    /* SPI-NOR command state machine */
    uint8_t cmd;          /* current command (0 = idle, awaiting a command) */
    /*
     * 0 = awaiting command, 1..3 = address bytes, 4 = streaming data.
     */
    int phase;
    uint32_t addr;        /* 24-bit address accumulator / read cursor */
    uint8_t rx;           /* byte to return on the next DR read */
    uint8_t *store;       /* backing bytes (NOR_SIZE), or NULL for 0xff */
    /*
     * Write-enable latch: set by WREN, cleared by WRDI and on completion of a
     * PROGRAM or ERASE.
     */
    int wel;
    /* The preloaded byte at addr has not been clocked out yet. */
    int first_data_pending;

    int wrote;            /* the current command modified the store */

    /* Persistence and WIP state */
    hwaddr base;          /* MMIO base of this instance */
    int idx;              /* creation index among the instances */
    int dirty;            /* a PROGRAM or ERASE modified the store */
    int wip_left;         /* RDSR commands that still report WIP=1 */
    /* Status byte latched by the RDSR command byte, shifted out next. */
    uint8_t sr_latched;
};

/* Process-wide settings, read at instance init. */
static const char *g_spinor_out;      /* DM32_SPINOR_OUT */
static int g_spinor_sync;             /* DM32_SPINOR_SYNC */
static int g_wip_reads = 2;           /* DM32_SPINOR_WIP_READS */
static int g_spinor_log;              /* DM32_SPINOR_LOG */

static uint8_t nor_byte(DM32SsiState *s, uint32_t a)
{
    if (s->store) {
        return s->store[a & (NOR_SIZE - 1)];
    }
    return 0xff;   /* no store: erased flash */
}

/*
 * Page program and erase act on the backing store.
 *
 * NOR semantics that are modelled:
 *   - programming can only clear bits (store &= value), never set them;
 *   - erase sets the region to 0xff;
 *   - both need the write-enable latch (WREN, 0x06), which the chip clears
 *     when the operation completes.
 */
static void spinor_dump_all(int announce);
static void spinor_sync_point(void);

static void nor_program(DM32SsiState *s, uint32_t a, uint8_t b)
{
    if (!s->store) {
        return;
    }
    if (!s->wel) {
        dm32_event(DM32_EV_NOR_WEL, "SSI @0x%x: page program at 0x%06x "
                   "without WREN", (unsigned)s->base, a);
        return;
    }
    s->store[a & (NOR_SIZE - 1)] &= b;   /* programming clears bits only */
    s->wrote = 1;
    s->dirty = 1;
}

static void nor_erase(DM32SsiState *s, uint32_t a, uint32_t len)
{
    uint32_t i;

    if (!s->store) {
        return;
    }
    if (!s->wel) {
        dm32_event(DM32_EV_NOR_WEL, "SSI @0x%x: erase of 0x%x bytes at "
                   "0x%06x without WREN", (unsigned)s->base, len, a);
        return;
    }
    a &= ~(len - 1);                      /* erase acts on the aligned region */
    for (i = 0; i < len; i++) {
        s->store[(a + i) & (NOR_SIZE - 1)] = 0xff;
    }
    s->wrote = 1;
    s->dirty = 1;
    /* The erase completes here. The chip reports WIP on the next RDSR reads. */
    s->wip_left = g_wip_reads;
    if (g_spinor_log) {
        fprintf(stderr, "dm32_ssi[#%d @0x%x]: ERASE 0x%x len 0x%x "
                "(wip_reads=%d)\n",
                s->idx, (unsigned)s->base, a, len, g_wip_reads);
    }
    spinor_sync_point();
}

/*
 * RDSR status for one RDSR command. WIP (bit 0) is 1 for the next g_wip_reads
 * RDSR commands after a completed PROGRAM or ERASE, then 0. WEL (bit 1) is
 * live. This is called once per RDSR command, on the command byte, so a driver
 * that polls once per command consumes one WIP read per poll.
 */
static uint8_t rdsr_latch(DM32SsiState *s)
{
    uint8_t sr = s->wel ? 0x02 : 0x00;

    if (s->wip_left > 0) {
        sr |= 0x01;
        s->wip_left--;
    }
    s->sr_latched = sr;
    if (g_spinor_log) {
        fprintf(stderr, "dm32_ssi[#%d @0x%x]: RDSR -> 0x%x (WIP=%u, "
                "wip_left=%d)\n",
                s->idx, (unsigned)s->base, sr, sr & 1u, s->wip_left);
    }
    return sr;
}

/*
 * Process one byte written to DR. Sets s->rx to the byte that is shifted back
 * on the same transfer.
 */
static void spi_tx(DM32SsiState *s, uint8_t b)
{
    switch (s->phase) {
    case 0: /* awaiting command */
        s->cmd = b;
        s->rx = 0xff;
        switch (b) {
        case 0x03: /* READ */
        case 0x0b: /* FAST_READ: collect three address bytes next */
            s->phase = 1;
            s->addr = 0;
            break;
        case 0x05: /* RDSR: WIP per the post-PROGRAM/ERASE window, WEL live */
            s->rx = rdsr_latch(s);
            s->phase = 4;
            break;
        case 0x9f: /* RDID: stream the JEDEC id */
            s->phase = 4;
            s->addr = 0;
            break;
        case 0x06: /* WREN: set the write-enable latch */
            s->wel = 1;
            s->phase = 0;
            break;
        case 0x04: /* WRDI: clear it */
            s->wel = 0;
            s->phase = 0;
            break;
        case 0x02: /* PAGE PROGRAM: three address bytes, then data */
        case 0x20: /* SECTOR ERASE (4 KiB) */
        case 0xd8: /* BLOCK ERASE (64 KiB) */
            s->phase = 1;
            s->addr = 0;
            s->wrote = 0;
            break;
        case 0xc7: /* CHIP ERASE */
        case 0x60:
            nor_erase(s, 0, NOR_SIZE);
            s->wel = 0;
            s->phase = 0;
            break;
        default: /* WRSR and others: accepted, no meaningful RX */
            s->phase = 0;
            break;
        }
        break;
    case 1:
        s->addr = (uint32_t)b << 16;
        s->phase = 2;
        s->rx = 0xff;
        break;
    case 2:
        s->addr |= (uint32_t)b << 8;
        s->phase = 3;
        s->rx = 0xff;
        break;
    case 3:
        s->addr |= b;
        s->phase = 4;
        if (g_spinor_log) {
            fprintf(stderr, "dm32_ssi: cmd 0x%x addr 0x%06x\n",
                    s->cmd, s->addr);
        }
        if (s->cmd == 0x20) {
            /* sector erase: acts on the full address */
            nor_erase(s, s->addr, 0x1000);
            s->wel = 0;
            s->phase = 0;
        } else if (s->cmd == 0xd8) {
            /* block erase */
            nor_erase(s, s->addr, 0x10000);
            s->wel = 0;
            s->phase = 0;
        } else if (s->cmd == 0x02) {
            /* page program: data bytes follow */
            s->rx = 0xff;
        } else {
            /*
             * Preload the first data byte without marking it consumed.
             *
             * The driver's sendCmdAddr() calls xferByte() four times (the
             * command and three address bytes), and it reads the RX register
             * after each call. The fourth read, after the last address byte,
             * already returns this preloaded byte 0. If the next streaming TX
             * advanced the address, byte 0 would be delivered during the
             * address phase and the data would start at byte 1.
             * first_data_pending makes the first streaming TX return the
             * preloaded byte and advance only from the second one.
             */
            s->rx = nor_byte(s, s->addr);
            s->first_data_pending = 1;
        }
        break;
    case 4:
        /*
         * Streaming. Each further TX byte (usually 0xff) clocks out the next
         * byte.
         */
        if (s->cmd == 0x02) {
            /* page program: each TX byte is written */
            nor_program(s, s->addr, b);
            s->addr++;
            s->rx = 0xff;
        } else if (s->cmd == 0x03 || s->cmd == 0x0b) {
            if (s->first_data_pending) {
                /* byte 0 was preloaded: deliver it, do not skip it */
                s->first_data_pending = 0;
            } else {
                s->addr++;
                s->rx = nor_byte(s, s->addr);
            }
        } else if (s->cmd == 0x9f) {
            /*
             * JEDEC id of the W25Q128: manufacturer 0xef (Winbond), memory
             * type 0x40, capacity 0x18 (128 Mbit = 16 MB).
             */
            static const uint8_t jedec[3] = { 0xef, 0x40, 0x18 };

            s->rx = jedec[(s->addr++) % 3];
        } else if (s->cmd == 0x05) {
            s->rx = s->sr_latched;   /* status latched by the command byte */
        } else {
            s->rx = 0xff;
        }
        break;
    }
}

/* End of a transfer: return to awaiting a command. */
static void spi_cs_reset(DM32SsiState *s)
{
    if (s->cmd == 0x02) {
        if (s->wrote) {
            /* the program completes: busy for the next RDSR reads */
            s->wip_left = g_wip_reads;
            if (g_spinor_log) {
                fprintf(stderr, "dm32_ssi[#%d @0x%x]: PROGRAM completed near "
                        "0x%x (wip_reads=%d)\n",
                        s->idx, (unsigned)s->base, s->addr, g_wip_reads);
            }
            spinor_sync_point();
        }
        s->wel = 0;   /* the chip clears the latch when a program finishes */
    }
    s->phase = 0;
    s->cmd = 0;
    s->addr = 0;
    s->rx = 0xff;
    s->wrote = 0;
    s->first_data_pending = 0;
}

/*
 * The stock firmware reaches the W25Q128 through this model: SSI0 at
 * 0x140a0000, with GPIO pin 0x12 (GPIOA bit 18) as chip-select. DM32_SPINOR
 * therefore also backs the stock firmware's codeplug access at boot. That
 * consists of READ transactions for the tag byte at the end of each 4 KB page,
 * plus nine 4 KB erases and three page programs at 0x6dc000-0x6e4fff.
 */

/*
 * The controller can transfer only when SSIENR and SER are set, as required by
 * HR_C7000 v2.7 EN section 5.2.3.3, and when its own SCLK, MOSI and MISO pads
 * select its master function. Otherwise the block is enabled and connected to
 * nothing. The selects (v2.7 EN 4.4.4.5 and 4.4.4.6):
 *   - SPI0 (0x140a0000): IO_DIPLEX1 spi0_sclk/mosi/miso_sel, pads PTA20-22,
 *     value 0.
 *   - SPI1 (0x140b0000): IO_DIPLEX1 spi1_sclk/mosi/miso_sel, pads PTA25-27,
 *     value 0.
 *   - SPI2 (0x140e0000): IO_DIPLEX2 lcd_rs/db1/db0_sel, pads PTC4, PTC8 and
 *     PTC7, value 1; the pads are shared with the LCD bus.
 *   - SPI3 (0x14150000), "EFUSE only", and the program flash's controller at
 *     0x04000000 have dedicated connections and no select field.
 */
static bool ssi_pads_muxed(DM32SsiState *s)
{
    switch (s->base) {
    case 0x140a0000:
        return dm32_pad_function(DM32_PTA(20)) == 0 &&
               dm32_pad_function(DM32_PTA(21)) == 0 &&
               dm32_pad_function(DM32_PTA(22)) == 0;
    case 0x140b0000:
        return dm32_pad_function(DM32_PTA(25)) == 0 &&
               dm32_pad_function(DM32_PTA(26)) == 0 &&
               dm32_pad_function(DM32_PTA(27)) == 0;
    case 0x140e0000:
        return dm32_pad_function(DM32_PTC(4)) == 1 &&
               dm32_pad_function(DM32_PTC(8)) == 1 &&
               dm32_pad_function(DM32_PTC(7)) == 1;
    default:
        return true;
    }
}

static int64_t ssi_frame_ns(DM32SsiState *s)
{
    uint32_t bits = (s->ctrlr0 & 0x1f) + 1;
    uint32_t sckdv = s->baudr & 0xfffe;

    if (bits < 4) {
        bits = ((s->ctrlr0 >> 16) & 0x1f) + 1;
    }
    if (!sckdv) {
        return 0;                   /* BAUDR 0: the clock is off */
    }
    return muldiv64((uint64_t)bits * sckdv, NANOSECONDS_PER_SECOND,
                    DM32_APB_HZ);
}

static bool ssi_can_shift(DM32SsiState *s)
{
    return (s->ssienr & 1) && (s->ser & 3) && ssi_frame_ns(s) > 0;
}

/* Is the instance's device selected? */
static bool ssi_selected(DM32SsiState *s)
{
    if (s->base == SSI0_BASE) {
        return s->cs_low;
    }
    return (s->ssienr & 1) && (s->ser & 3);
}

/* One frame on the wire: the device sees @out and answers. */
static uint16_t ssi_device(DM32SsiState *s, uint16_t out)
{
    if (!ssi_pads_muxed(s)) {
        dm32_event(DM32_EV_SSI_DISCARD, "SSI @0x%x: frame 0x%02x on pads that "
                   "do not select the controller", (unsigned)s->base,
                   (unsigned)(out & 0xff));
        return 0xff;
    }
    if (!ssi_selected(s)) {
        return 0xff;
    }
    spi_tx(s, (uint8_t)out);
    return s->rx;
}

static void ssi_rx_push(DM32SsiState *s, uint16_t v)
{
    if (s->rx_count == FIFO_DEPTH) {
        s->latched |= INT_RXO;
        if (g_spinor_log) {
            fprintf(stderr, "dm32_ssi[#%d @0x%x]: RX FIFO overflow\n",
                    s->idx, (unsigned)s->base);
        }
        return;
    }
    s->rxq[(s->rx_head + s->rx_count) % FIFO_DEPTH] = v;
    s->rx_count++;
}

/* Take the next frame onto the wire, if there is one. */
static bool ssi_begin(DM32SsiState *s)
{
    int mode = TMOD(s);

    if (!ssi_can_shift(s)) {
        return false;
    }
    if (s->tx_count && (mode != TMOD_RX || !s->rx_left)) {
        s->cur = s->txq[s->tx_head];
        s->tx_head = (s->tx_head + 1) % FIFO_DEPTH;
        s->tx_count--;
        s->cur_rx = mode == 0;
        if (mode == TMOD_RX) {
            /* a write starts the reception; the value is sent each frame */
            s->rx_word = s->cur;
            s->rx_left = (s->ctrlr1 & 0xffff) + 1;
            s->cur_rx = true;
            s->rx_left--;
        } else if (mode == TMOD_EEPROM && !s->tx_count) {
            s->rx_left = (s->ctrlr1 & 0xffff) + 1;
            s->rx_word = 0;
        }
    } else if (s->rx_left) {
        s->cur = s->rx_word;
        s->cur_rx = true;
        s->rx_left--;
    } else {
        return false;
    }
    s->shifting = true;
    return true;
}

/* Complete every frame that has ended by @now. */
static void ssi_advance(DM32SsiState *s, int64_t now)
{
    for (;;) {
        int64_t frame = ssi_frame_ns(s);
        uint16_t in;

        if (!s->shifting) {
            if (!ssi_begin(s)) {
                s->t = now;
                return;
            }
        }
        if (frame <= 0 || now - s->t < frame) {
            return;
        }
        in = ssi_device(s, s->cur);
        if (s->cur_rx) {
            ssi_rx_push(s, in);
        }
        s->shifting = false;
        s->t += frame;
    }
}

static uint32_t ssi_risr(DM32SsiState *s)
{
    uint32_t r = s->latched;

    if ((s->ssienr & 1) && (uint32_t)s->tx_count <= (s->txftlr & 0xff)) {
        r |= INT_TXE;
    }
    if ((uint32_t)s->rx_count > (s->rxftlr & 0xff)) {
        r |= INT_RXF;
    }
    return r;
}

static void ssi_update(DM32SsiState *s)
{
    uint32_t isr = ssi_risr(s) & s->imr;

    qemu_set_irq(s->irq, isr != 0);
    /* with an interrupt enabled, wake up when the next frame ends */
    if (s->imr && s->shifting && ssi_frame_ns(s) > 0) {
        timer_mod(s->timer, s->t + ssi_frame_ns(s));
    } else {
        timer_del(s->timer);
    }
}

static void ssi_sync(DM32SsiState *s)
{
    ssi_advance(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

static void ssi_timer(void *opaque)
{
    DM32SsiState *s = opaque;

    ssi_sync(s);
    ssi_update(s);
}

static void ssi_flush(DM32SsiState *s)
{
    s->tx_head = s->tx_count = 0;
    s->rx_head = s->rx_count = 0;
    s->latched = 0;
    s->shifting = false;
    s->rx_left = 0;
}

static uint32_t ssi_clear(DM32SsiState *s, uint32_t bits)
{
    uint32_t was = s->latched & bits;

    s->latched &= ~bits;
    return was ? 1 : 0;
}

static uint64_t dm32_ssi_read(void *opaque, hwaddr off, unsigned size)
{
    DM32SsiState *s = opaque;
    uint32_t v = 0;

    ssi_sync(s);
    switch (off) {
    case R_CTRLR0:
        v = s->ctrlr0;
        break;
    case R_CTRLR1:
        v = s->ctrlr1;
        break;
    case R_SSIENR:
        v = s->ssienr;
        break;
    case R_SER:
        v = s->ser;
        break;
    case R_BAUDR:
        v = s->baudr;
        break;
    case R_TXFTLR:
        v = s->txftlr;
        break;
    case R_RXFTLR:
        v = s->rxftlr;
        break;
    case R_TXFLR:
        v = s->tx_count;
        break;
    case R_RXFLR:
        v = s->rx_count;
        break;
    case R_SR:
        v = (s->tx_count < FIFO_DEPTH ? SR_TFNF : 0) |
            (s->tx_count == 0 ? SR_TFE : 0) |
            (s->rx_count ? SR_RFNE : 0) |
            (s->rx_count == FIFO_DEPTH ? SR_RFF : 0) |
            (s->shifting ? SR_BUSY : 0);
        break;
    case R_IMR:
        v = s->imr;
        break;
    case R_ISR:
        v = ssi_risr(s) & s->imr;
        break;
    case R_RISR:
        v = ssi_risr(s);
        break;
    case R_TXOICR:
        v = ssi_clear(s, INT_TXO);
        break;
    case R_RXOICR:
        v = ssi_clear(s, INT_RXO);
        break;
    case R_RXUICR:
        v = ssi_clear(s, INT_RXU);
        break;
    case R_ICR:
        v = ssi_clear(s, INT_TXO | INT_RXO | INT_RXU);
        break;
    case R_DMACR:
        v = s->dmacr;
        break;
    case R_IDR:
        v = SSI_IDR_VALUE;
        break;
    case R_VERSION:
        v = SSI_COMP_VERSION;
        break;
    case R_DR:
        if (!s->rx_count) {
            s->latched |= INT_RXU;
            v = 0xff;
            break;
        }
        v = s->rxq[s->rx_head];
        s->rx_head = (s->rx_head + 1) % FIFO_DEPTH;
        s->rx_count--;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "dm32_ssi @0x%x: read of undefined "
                      "offset 0x%02" HWADDR_PRIx "\n", (unsigned)s->base, off);
        break;
    }
    ssi_update(s);
    return v;
}

/* Configuration registers are refused while SSIENR is 1. */
static bool ssi_locked(DM32SsiState *s, hwaddr off)
{
    if (s->ssienr & 1) {
        qemu_log_mask(LOG_GUEST_ERROR, "dm32_ssi @0x%x: write to offset 0x%02"
                      HWADDR_PRIx " while SSIENR is 1 refused\n",
                      (unsigned)s->base, off);
        return true;
    }
    return false;
}

static void dm32_ssi_write(void *opaque, hwaddr off, uint64_t val,
                           unsigned size)
{
    DM32SsiState *s = opaque;
    uint32_t v = val;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    ssi_advance(s, now);
    switch (off) {
    case R_CTRLR0:
        if (!ssi_locked(s, off)) {
            s->ctrlr0 = v;
        }
        break;
    case R_CTRLR1:
        if (!ssi_locked(s, off)) {
            s->ctrlr1 = v & 0xffff;
        }
        break;
    case R_BAUDR:
        if (!ssi_locked(s, off)) {
            s->baudr = v & 0xffff;
        }
        break;
    case R_TXFTLR:
        if (!ssi_locked(s, off)) {
            s->txftlr = v & 0xff;
        }
        break;
    case R_RXFTLR:
        if (!ssi_locked(s, off)) {
            s->rxftlr = v & 0xff;
        }
        break;
    case R_SSIENR:
        if (!(v & 1)) {
            /* "when 0 clear FIFO and status register" (p.256) */
            ssi_flush(s);
            if (s->base != SSI0_BASE) {
                spi_cs_reset(s);
            }
        }
        s->ssienr = v & 1;
        break;
    case R_SER:
        if (s->base != SSI0_BASE && (s->ser & 3) && !(v & 3)) {
            spi_cs_reset(s);        /* the controller releases its select */
        }
        s->ser = v & 3;
        break;
    case R_IMR:
        s->imr = v & 0x3f;
        break;
    case R_DMACR:
        s->dmacr = v;
        break;
    case R_DR:
        if (!(s->ssienr & 1)) {
            dm32_event(DM32_EV_SSI_DISCARD, "SSI @0x%x: DR write 0x%02x "
                       "dropped (SSIENR=0)", (unsigned)s->base,
                       (unsigned)(v & 0xff));
            break;
        }
        if (s->tx_count == FIFO_DEPTH) {
            s->latched |= INT_TXO;
            break;
        }
        s->txq[(s->tx_head + s->tx_count) % FIFO_DEPTH] = v;
        s->tx_count++;
        break;
    case R_TXFLR:
    case R_RXFLR:
    case R_SR:
    case R_ISR:
    case R_RISR:
    case R_TXOICR:
    case R_RXOICR:
    case R_RXUICR:
    case R_ICR:
    case R_IDR:
    case R_VERSION:
        break;                      /* read-only */
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "dm32_ssi @0x%x: write to undefined "
                      "offset 0x%02" HWADDR_PRIx "\n", (unsigned)s->base, off);
        break;
    }
    /* frames that could not start before may start now */
    ssi_advance(s, now);
    ssi_update(s);
}

static const MemoryRegionOps dm32_ssi_ops = {
    .read = dm32_ssi_read,
    .write = dm32_ssi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/*
 * Registry of the instances, for the DM32_SPINOR_OUT dump at exit.
 */
#define MAX_SSI 8
static DM32SsiState *g_ssi[MAX_SSI];
static int g_ssi_n;

/* SSI0's data chip select, GPIOA 18: selected while driven low. */
static void ssi_cs_pin(void *opaque, unsigned pad, bool driven, int level)
{
    DM32SsiState *s = opaque;
    bool low = driven && !level;

    if (low == s->cs_low) {
        return;
    }
    ssi_sync(s);                    /* frames before the edge see the old CS */
    s->cs_low = low;
    if (!low) {
        spi_cs_reset(s);
    }
    ssi_update(s);
}

static const DM32GpioPinOps ssi_cs_ops = {
    .output = ssi_cs_pin,
};

static int spinor_write_file(const char *path, const uint8_t *data)
{
    char tmp[4096];
    FILE *f;

    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
        return -1;
    }
    f = fopen(tmp, "wb");
    if (!f) {
        return -1;
    }
    if (fwrite(data, 1, NOR_SIZE, f) != NOR_SIZE) {
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0) {
        return -1;
    }
    return rename(tmp, path);
}

/*
 * Write the dirty instance stores out. The first dirty instance (in creation
 * order) goes to DM32_SPINOR_OUT itself. Any further dirty instances go to
 * DM32_SPINOR_OUT.<idx>. Each file is written to <path>.tmp and renamed into
 * place. If no instance is dirty, nothing is written.
 */
static void spinor_dump_all(int announce)
{
    int i;
    int first = -1;
    int ndirty = 0;

    if (!g_spinor_out) {
        return;
    }
    for (i = 0; i < g_ssi_n; i++) {
        if (g_ssi[i]->dirty) {
            ndirty++;
            if (first < 0) {
                first = i;
            }
        }
    }
    if (ndirty == 0) {
        if (announce) {
            fprintf(stderr, "DM32_SPINOR_OUT: no SPI-NOR writes happened, "
                    "nothing written\n");
        }
        return;
    }
    for (i = 0; i < g_ssi_n; i++) {
        DM32SsiState *s = g_ssi[i];
        char path[4096];
        int n, rc;

        if (!s->dirty) {
            continue;
        }
        if (i == first) {
            n = snprintf(path, sizeof(path), "%s", g_spinor_out);
        } else {
            n = snprintf(path, sizeof(path), "%s.%d", g_spinor_out, s->idx);
        }
        /* A truncated path is a write failure, as a long tmp path is */
        rc = n >= (int)sizeof(path) ? -1 : spinor_write_file(path, s->store);
        if (announce || rc != 0) {
            fprintf(stderr, "DM32_SPINOR_OUT: instance #%d @0x%x -> %s %s\n",
                    s->idx, (unsigned)s->base, path,
                    rc == 0 ? "(written)" : "(WRITE FAILED)");
        }
    }
}

static void spinor_sync_point(void)
{
    if (g_spinor_sync && g_spinor_out) {
        spinor_dump_all(0);
    }
}

static void spinor_exit_notify(Notifier *n, void *data)
{
    (void)n;
    (void)data;
    spinor_dump_all(1);
}

static Notifier spinor_exit_notifier = { .notify = spinor_exit_notify };

/*
 * Board code creates instances through this function, which records the MMIO
 * base for the log messages.
 */
DeviceState *dm32_ssi_create(hwaddr base, qemu_irq irq)
{
    DeviceState *dev = sysbus_create_simple(TYPE_DM32_SSI, base, irq);
    DM32SsiState *s = DM32_SSI(dev);

    s->base = base;
    if (base == SSI0_BASE) {
        dm32_gpio_attach(DM32_PTA(18), &ssi_cs_ops, s);
    }
    return dev;
}

/*
 * Machine reset: the controller's registers return to their reset values
 * (CTRLR0 0x7, IMR 0x3f, the rest 0), both FIFOs empty, and any command in
 * progress ends, as chip select is released. The NOR chip behind it is a
 * separate part: its contents, write-enable latch and busy count persist.
 */
static void dm32_ssi_reset(DeviceState *dev)
{
    DM32SsiState *s = DM32_SSI(dev);

    s->ctrlr0 = 0x7;
    s->ctrlr1 = s->ssienr = s->ser = s->baudr = 0;
    s->txftlr = s->rxftlr = s->dmacr = 0;
    s->imr = 0x3f;
    ssi_flush(s);
    s->t = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_del(s->timer);
    spi_cs_reset(s);
    s->sr_latched = 0;
    qemu_set_irq(s->irq, 0);
}

static void dm32_ssi_init(Object *obj)
{
    DM32SsiState *s = DM32_SSI(obj);
    const char *fn;
    static int exit_hooked;

    memory_region_init_io(&s->iomem, obj, &dm32_ssi_ops, s, TYPE_DM32_SSI,
                          WIN);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ssi_timer, s);
    dm32_ssi_reset(DEVICE(obj));
    s->dirty = 0;
    s->wip_left = 0;
    s->idx = g_ssi_n;

    g_spinor_log = dm32_env_flag("DM32_SPINOR_LOG");
    g_spinor_out = getenv("DM32_SPINOR_OUT");
    if (g_spinor_out && !*g_spinor_out) {
        g_spinor_out = NULL;
    }
    g_spinor_sync = dm32_env_flag("DM32_SPINOR_SYNC");
    g_wip_reads = (int)dm32_env_uint("DM32_SPINOR_WIP_READS", 2, 0, 1000);

    /*
     * Every instance gets its own blank (0xff) store. A file named by
     * DM32_SPINOR is loaded over it.
     */
    s->store = g_malloc(NOR_SIZE);
    memset(s->store, 0xff, NOR_SIZE);
    fn = getenv("DM32_SPINOR");
    if (fn && *fn) {
        FILE *f = fopen(fn, "rb");

        if (f) {
            /*
             * A short file is fine. The 0xff fill covers the rest, as on an
             * erased chip that was only partly written.
             */
            size_t n = fread(s->store, 1, NOR_SIZE, f);

            (void)n;
            fclose(f);
        }
    }
    if (g_ssi_n < MAX_SSI) {
        g_ssi[g_ssi_n++] = s;
    }
    if (g_spinor_out && !exit_hooked) {
        qemu_add_exit_notifier(&spinor_exit_notifier);
        exit_hooked = 1;
    }
}

static void dm32_ssi_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), dm32_ssi_reset);
}

static const TypeInfo dm32_ssi_type = {
    .name = TYPE_DM32_SSI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DM32SsiState),
    .instance_init = dm32_ssi_init,
    .class_init = dm32_ssi_class_init,
};

static void dm32_ssi_register(void)
{
    type_register_static(&dm32_ssi_type);
}

type_init(dm32_ssi_register)
