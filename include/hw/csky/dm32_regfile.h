/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Baofeng DM-32UV board model: SVD-described register files
 *
 * Copyright (c) 2026 Alex Kaplan
 *
 * A register file built from a DM32_SVD_<P>_REGS list in dm32_svd.h. It
 * serves the SVD reset values and enforces the SVD access masks, so a block
 * with no behavioural model still answers as the silicon does at reset
 * instead of reading 0:
 *
 *   static const DM32RegDesc usb_regs[] = {
 *       DM32_SVD_USB_REGS(DM32_REGDESC)
 *   };
 *   uint32_t usb_val[ARRAY_SIZE(usb_regs)];
 *
 *   dm32_regfile_reset(usb_regs, ARRAY_SIZE(usb_regs), usb_val);
 *   dm32_regfile_read(usb_regs, ARRAY_SIZE(usb_regs), usb_val, off, &v);
 *
 * Access rules, per bit:
 *   - read: write-only bits read 0; clear-on-read bits clear after the read.
 *   - write: read-only bits keep their value; write-1-to-clear bits clear
 *     where the data has a 1; write-1-to-set bits set where it has a 1; all
 *     other bits (read-write and write-only) take the data.
 * A register flagged DM32_SVD_ALTERNATE (one sharing its offset with
 * another) is skipped when looking up an offset; the first unflagged one
 * answers. Accesses are whole registers: the caller handles sub-word access
 * and decides what an offset with no register does.
 *
 * The functions use only standard C types, so the file can be compiled
 * outside QEMU (tests/svd/).
 */

#ifndef HW_CSKY_DM32_REGFILE_H
#define HW_CSKY_DM32_REGFILE_H

#include "hw/csky/dm32_svd.h"

typedef struct DM32RegDesc {
    const char *name;
    uint32_t offset;
    uint8_t size;           /* bits: 8, 16 or 32 */
    uint8_t flags;          /* DM32_SVD_RESET_EXPLICIT, DM32_SVD_ALTERNATE */
    uint32_t reset;
    uint32_t ro;            /* read-only bits */
    uint32_t wo;            /* write-only bits */
    uint32_t w1c;           /* write-1-to-clear bits */
    uint32_t w1s;           /* write-1-to-set bits */
    uint32_t rc;            /* clear-on-read bits */
} DM32RegDesc;

/* X() for a DM32_SVD_<P>_REGS list: one DM32RegDesc initialiser. */
#define DM32_REGDESC(n, off, sz, rst, fl, ro_, wo_, w1c_, w1s_, rc_) \
    { #n, off, sz, fl, rst, ro_, wo_, w1c_, w1s_, rc_ },

static inline uint32_t dm32_regdesc_mask(const DM32RegDesc *d)
{
    return d->size >= 32 ? 0xffffffffu : (1u << d->size) - 1;
}

/* Index of the register answering at @off, or -1 if there is none. */
static inline int dm32_regfile_index(const DM32RegDesc *d, size_t n,
                                     uint32_t off)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (d[i].offset == off && !(d[i].flags & DM32_SVD_ALTERNATE)) {
            return (int)i;
        }
    }
    return -1;
}

static inline void dm32_regfile_reset(const DM32RegDesc *d, size_t n,
                                      uint32_t *val)
{
    size_t i;

    for (i = 0; i < n; i++) {
        val[i] = d[i].reset & dm32_regdesc_mask(&d[i]);
    }
}

/* Read the register at @off into *@out. Returns false if there is none. */
static inline bool dm32_regfile_read(const DM32RegDesc *d, size_t n,
                                     uint32_t *val, uint32_t off,
                                     uint32_t *out)
{
    int i = dm32_regfile_index(d, n, off);

    if (i < 0) {
        return false;
    }
    *out = val[i] & ~d[i].wo & dm32_regdesc_mask(&d[i]);
    val[i] &= ~d[i].rc;
    return true;
}

/* Write @data to the register at @off. Returns false if there is none. */
static inline bool dm32_regfile_write(const DM32RegDesc *d, size_t n,
                                      uint32_t *val, uint32_t off,
                                      uint32_t data)
{
    int i = dm32_regfile_index(d, n, off);
    uint32_t mask, plain, v;

    if (i < 0) {
        return false;
    }
    mask = dm32_regdesc_mask(&d[i]);
    data &= mask;
    plain = mask & ~(d[i].ro | d[i].w1c | d[i].w1s);
    v = (val[i] & ~plain) | (data & plain);
    v &= ~(data & d[i].w1c);
    v |= data & d[i].w1s;
    val[i] = v;
    return true;
}

#endif /* HW_CSKY_DM32_REGFILE_H */
