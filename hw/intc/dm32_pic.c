/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Baofeng DM-32UV programmable interrupt controller (PIC): 64-source
 * interrupt controller at base 0x17000000
 *
 * Copyright (c) 2026 Alex Kaplan
 *
 * HR_C7000 v2.7 EN section 4.7 (p.166-183), register reset values as the
 * datasheet and the SVD (peripheral PIC) give them:
 *   0x00 PIC_MODE        sources 0-31 trigger: 1 edge, 0 level (0xffffffff)
 *   0x04 PIC_PO          sources 0-31 polarity: 1 rising/high, 0 falling/low
 *                        (0xffffffff)
 *   0x08 PIC_MASK        sources 0-31: 1 masked, 0 enabled (0xffffffff)
 *   0x0c PIC_VECTOR      vector0 [6:0], 32-112: source x is vector x +
 *                        vector0 (0x20)
 *   0x10 PIC_COW1        write-only: eoi [2], feoi [3]
 *   0x14-0x30            PIC_PRIOR0_3 .. PIC_PRIOR28_31, one byte per source
 *   0x34 PIC_COW2        avec [1] (1), priority [0] (0)
 *   0x38 PIC_SYNC        min_interval [7:0]
 *   0x3c PIC_FFLAG_L     fast-interrupt flags, sources 0-31
 *   0x40 PIC_RECORD_SEL  source counted by PIC_INT_CNT [6:0]
 *   0x44 PIC_INT_ST      request records, sources 0-31, write 1 to clear
 *   0x48 PIC_INT_ST_1    request records, sources 32-63, write 1 to clear
 *   0x4c PIC_INT_CNT     read-only request count of the selected source
 *   0x60/0x64/0x68       PIC_MODE_1, PIC_PO_1, PIC_MASK_1 (sources 32-63)
 *   0x6c-0x88            PIC_PRIOR32_35 .. PIC_PRIOR60_63
 *   0x8c PIC_FFLAG_H     fast-interrupt flags, sources 32-63
 * PIC_PRIOR32_35 resets to 0x23222120 as the SVD gives it; the datasheet
 * prints 0x27262524, a copy of PIC_PRIOR36_39's row (p.183). Every other
 * PIC_PRIORn byte resets to n in both.
 *
 * Requests. A source's line is active when it is at the level PIC_PO
 * selects. PIC_MASK enables "interrupt request detection" (p.170, step 5),
 * so a masked source requests nothing and records nothing. A request
 * starts when an unmasked line becomes active, or when a source whose line
 * is active is unmasked (assumption: detection follows the mask, as step 5
 * says, so unmasking an active line is a request in either mode).
 *   - Level sources (PIC_MODE 0) request for as long as the line stays
 *     active and unmasked.
 *   - Edge sources (PIC_MODE 1) latch the request; the latch is consumed
 *     when the source enters service. Assumption: a latch survives a later
 *     mask and is delivered once unmasked.
 * Each request start sets the source's PIC_INT_ST bit, which "can only
 * record an interrupt request once" until software writes 1 to it (p.170),
 * and adds 1 to PIC_INT_CNT when PIC_RECORD_SEL names the source. A
 * PIC_INT_ST write that clears the selected source's bit also clears
 * PIC_INT_CNT (p.180-181; assumption: other sources' bits leave it alone).
 * The records are status only: clearing one does not withdraw a request.
 *
 * Delivery. Arbitration picks among the requesting sources: by source
 * number, IRQ0 first, when PIC_COW2.priority is 0, or by the smallest
 * PIC_PRIOR byte when it is 1 (p.179). Assumption: equal PIC_PRIOR bytes
 * go to the lower source number, the default order; the datasheet gives no
 * tie rule, and the bytes are used as static priorities (the datasheet
 * calls them "priority counter initial values" but describes no counting).
 * The winner enters service and the PIC drives INT and VEC to the core
 * until the CPU writes PIC_COW1.eoi, which clears the in-service register
 * (p.174). There is one in-service register and no acknowledge from the
 * core (figure 20 shows only INT_n, FINT_n and VEC), so a source in
 * service is never preempted: a higher-priority request waits for its EOI,
 * and an EOI always ends the one source in service. Stock's dispatcher
 * (IRAM 0x1064a) keeps interrupts disabled until rte, so it never nests.
 * Assumption: a level source leaves service when its request ends (line
 * inactive or masked) before the EOI. Stock writes the EOI before it calls
 * the ISR, while the source's line is still active; the source re-enters
 * service then, and leaves it when the ISR clears the device, so the core
 * is not interrupted again.
 *
 * VEC is source + vector0 while PIC_COW2.avec is 1. With avec 0 the core
 * uses its autovector, which the datasheet gives as vector-table offset
 * 0x28 (p.179): vector 10, csky-qemu's EXCP_CSKY_IRQ. The core input is
 * csky-qemu's csky_vic_v1 level encoding: the vector in bits 7:0, ORed
 * with bit 10 (interrupt request). Its value is also the read-only QOM
 * property "cpu-line".
 *
 * Not modelled: fast interrupts. PIC_FFLAG_L/H and PIC_COW1.feoi are stored
 * with no effect ("The C7000 only supports normal interrupts and does not
 * support fast interrupts", p.170). PIC_SYNC, which spaces asynchronous input
 * pulses (p.180), is also stored with no effect; the board's inputs are
 * synchronous levels.
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "qemu/log.h"
#include "qom/object.h"
#include "hw/csky/dm32.h"

#define NSRC 64

#define TYPE_DM32_PIC "dm32_pic"
OBJECT_DECLARE_SIMPLE_TYPE(DM32PicState, DM32_PIC)

#define R_MODE        0x00
#define R_PO          0x04
#define R_MASK        0x08
#define R_VECTOR      0x0c
#define R_COW1        0x10
#define R_PRIOR_LO    0x14
#define R_PRIOR_LO_END 0x30
#define R_COW2        0x34
#define R_SYNC        0x38
#define R_FFLAG_L     0x3c
#define R_RECORD_SEL  0x40
#define R_INT_ST      0x44
#define R_INT_ST_1    0x48
#define R_INT_CNT     0x4c
#define R_MODE_1      0x60
#define R_PO_1        0x64
#define R_MASK_1      0x68
#define R_PRIOR_HI    0x6c
#define R_PRIOR_HI_END 0x88
#define R_FFLAG_H     0x8c

#define COW1_EOI      (1u << 2)
#define COW2_PRIORITY (1u << 0)
#define COW2_AVEC     (1u << 1)
#define COW2_MASK     0x3u
#define VECTOR_MASK   0x7fu
#define SYNC_MASK     0xffu
#define RECSEL_MASK   0x7fu

#define VEC_AUTO      10            /* offset 0x28 / 4; EXCP_CSKY_IRQ */
#define VIC_ISR_INT   (1u << 10)    /* csky_tcip_v1.c */

struct DM32PicState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq cpu_irq;              /* to cpu_intc[0] */

    uint64_t level;                /* input lines, bit n = source n */
    uint64_t mode;                 /* PIC_MODE_1:PIC_MODE, 1 = edge */
    uint64_t po;                   /* PIC_PO_1:PIC_PO, 1 = rising/high */
    uint64_t mask;                 /* PIC_MASK_1:PIC_MASK, 1 = masked */
    uint64_t fflag;                /* PIC_FFLAG_H:PIC_FFLAG_L */
    uint64_t record;               /* PIC_INT_ST_1:PIC_INT_ST */
    uint64_t detect;               /* active and unmasked, last update */
    uint64_t edge;                 /* latched edge requests */
    uint8_t prior[NSRC];
    uint32_t vector0;
    uint32_t cow2;
    uint32_t sync;
    uint32_t record_sel;
    uint32_t int_cnt;
    int isr;                       /* source in service, -1 if none */
    uint32_t line;                 /* value driven on cpu_irq */
};

static uint64_t dm32_pic_requests(DM32PicState *s)
{
    return (s->detect & ~s->mode) | (s->edge & ~s->mask);
}

static int dm32_pic_arbitrate(DM32PicState *s, uint64_t req)
{
    int best = -1;
    int n;

    if (!(s->cow2 & COW2_PRIORITY)) {
        return ctz64(req);
    }
    for (n = 0; n < NSRC; n++) {
        if (((req >> n) & 1) && (best < 0 || s->prior[n] < s->prior[best])) {
            best = n;
        }
    }
    return best;
}

static void dm32_pic_update(DM32PicState *s)
{
    uint64_t detect = ~(s->level ^ s->po) & ~s->mask;
    uint64_t start = detect & ~s->detect;
    uint64_t req;
    uint32_t line = 0;

    /* Request detection: records, the selected count, edge latches. */
    s->detect = detect;
    s->record |= start;
    s->edge |= start & s->mode;
    if (s->record_sel < NSRC && ((start >> s->record_sel) & 1)) {
        s->int_cnt++;
    }

    /* A level source leaves service when its request ends. */
    if (s->isr >= 0 && !((s->mode >> s->isr) & 1) &&
        !((s->detect >> s->isr) & 1)) {
        s->isr = -1;
    }

    req = dm32_pic_requests(s);
    if (s->isr < 0 && req) {
        s->isr = dm32_pic_arbitrate(s, req);
        s->edge &= ~(1ull << s->isr);
    }

    if (s->isr >= 0) {
        uint32_t vec = (s->cow2 & COW2_AVEC) ? s->isr + s->vector0 : VEC_AUTO;

        line = (vec & 0xff) | VIC_ISR_INT;
    }
    if (line != s->line) {
        s->line = line;
        qemu_set_irq(s->cpu_irq, line);
    }
}

/* Device IRQ lines: qdev gpio-in 0..63, one per source. */
static void dm32_pic_set_irq(void *opaque, int src, int level)
{
    DM32PicState *s = opaque;

    if (src < 0 || src >= NSRC) {
        return;
    }
    s->level = deposit64(s->level, src, 1, level != 0);
    dm32_pic_update(s);
}

static uint32_t dm32_pic_prior_word(DM32PicState *s, int first)
{
    return ldl_le_p(&s->prior[first]);
}

static uint64_t dm32_pic_read(void *opaque, hwaddr off, unsigned size)
{
    DM32PicState *s = opaque;

    switch (off) {
    case R_MODE:
        return (uint32_t)s->mode;
    case R_PO:
        return (uint32_t)s->po;
    case R_MASK:
        return (uint32_t)s->mask;
    case R_VECTOR:
        return s->vector0;
    case R_COW1:
        return 0;                                  /* write-only */
    case R_PRIOR_LO ... R_PRIOR_LO_END:
        return dm32_pic_prior_word(s, off - R_PRIOR_LO);
    case R_COW2:
        return s->cow2;
    case R_SYNC:
        return s->sync;
    case R_FFLAG_L:
        return (uint32_t)s->fflag;
    case R_RECORD_SEL:
        return s->record_sel;
    case R_INT_ST:
        return (uint32_t)s->record;
    case R_INT_ST_1:
        return (uint32_t)(s->record >> 32);
    case R_INT_CNT:
        return s->int_cnt;
    case R_MODE_1:
        return (uint32_t)(s->mode >> 32);
    case R_PO_1:
        return (uint32_t)(s->po >> 32);
    case R_MASK_1:
        return (uint32_t)(s->mask >> 32);
    case R_PRIOR_HI ... R_PRIOR_HI_END:
        return dm32_pic_prior_word(s, 32 + off - R_PRIOR_HI);
    case R_FFLAG_H:
        return (uint32_t)(s->fflag >> 32);
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "dm32_pic: read of undefined offset "
                      "0x%02" HWADDR_PRIx "\n", off);
        return 0;
    }
}

static void dm32_pic_clear_records(DM32PicState *s, uint64_t bits)
{
    s->record &= ~bits;
    if (s->record_sel < NSRC && ((bits >> s->record_sel) & 1)) {
        s->int_cnt = 0;
    }
}

static void dm32_pic_write(void *opaque, hwaddr off, uint64_t val,
                           unsigned size)
{
    DM32PicState *s = opaque;
    uint32_t v = val;

    switch (off) {
    case R_MODE:
        s->mode = deposit64(s->mode, 0, 32, v);
        break;
    case R_PO:
        s->po = deposit64(s->po, 0, 32, v);
        break;
    case R_MASK:
        s->mask = deposit64(s->mask, 0, 32, v);
        break;
    case R_VECTOR:
        s->vector0 = v & VECTOR_MASK;
        if (s->vector0 < 32 || s->vector0 > 112) {
            qemu_log_mask(LOG_GUEST_ERROR, "dm32_pic: PIC_VECTOR %u is "
                          "outside 32-112\n", s->vector0);
        }
        break;
    case R_COW1:
        if (v & COW1_EOI) {
            s->isr = -1;
        }
        break;
    case R_PRIOR_LO ... R_PRIOR_LO_END:
        stl_le_p(&s->prior[off - R_PRIOR_LO], v);
        break;
    case R_COW2:
        s->cow2 = v & COW2_MASK;
        break;
    case R_SYNC:
        s->sync = v & SYNC_MASK;
        break;
    case R_FFLAG_L:
        s->fflag = deposit64(s->fflag, 0, 32, v);
        break;
    case R_RECORD_SEL:
        s->record_sel = v & RECSEL_MASK;
        break;
    case R_INT_ST:
        dm32_pic_clear_records(s, v);
        break;
    case R_INT_ST_1:
        dm32_pic_clear_records(s, (uint64_t)v << 32);
        break;
    case R_INT_CNT:
        break;                                     /* read-only */
    case R_MODE_1:
        s->mode = deposit64(s->mode, 32, 32, v);
        break;
    case R_PO_1:
        s->po = deposit64(s->po, 32, 32, v);
        break;
    case R_MASK_1:
        s->mask = deposit64(s->mask, 32, 32, v);
        break;
    case R_PRIOR_HI ... R_PRIOR_HI_END:
        stl_le_p(&s->prior[32 + off - R_PRIOR_HI], v);
        break;
    case R_FFLAG_H:
        s->fflag = deposit64(s->fflag, 32, 32, v);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "dm32_pic: write to undefined offset "
                      "0x%02" HWADDR_PRIx "\n", off);
        return;
    }
    dm32_pic_update(s);
}

static const MemoryRegionOps dm32_pic_ops = {
    .read = dm32_pic_read,
    .write = dm32_pic_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void dm32_pic_reset(DeviceState *dev)
{
    DM32PicState *s = DM32_PIC(dev);
    int n;

    s->mode = ~0ull;
    s->po = ~0ull;
    s->mask = ~0ull;
    s->fflag = 0;
    s->record = 0;
    s->detect = 0;
    s->edge = 0;
    for (n = 0; n < NSRC; n++) {
        s->prior[n] = n;
    }
    s->vector0 = 0x20;
    s->cow2 = COW2_AVEC;
    s->sync = 0;
    s->record_sel = 0;
    s->int_cnt = 0;
    s->isr = -1;
    dm32_pic_update(s);
}

static void dm32_pic_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), dm32_pic_reset);
}

static void dm32_pic_init(Object *obj)
{
    DM32PicState *s = DM32_PIC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &dm32_pic_ops, s, TYPE_DM32_PIC,
                          0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    qdev_init_gpio_in(DEVICE(obj), dm32_pic_set_irq, NSRC);
    sysbus_init_irq(sbd, &s->cpu_irq);
    object_property_add_uint32_ptr(obj, "cpu-line", &s->line,
                                   OBJ_PROP_FLAG_READ);
    s->level = 0;
    s->line = 0;
    dm32_pic_reset(DEVICE(obj));
}

static const TypeInfo dm32_pic_type = {
    .name = TYPE_DM32_PIC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DM32PicState),
    .instance_init = dm32_pic_init,
    .class_init = dm32_pic_class_init,
};

static void dm32_pic_register(void)
{
    type_register_static(&dm32_pic_type);
}

type_init(dm32_pic_register)
