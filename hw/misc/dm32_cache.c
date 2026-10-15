/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Baofeng DM-32UV CK803 cache controller at 0xe000f000: register model
 *
 * Copyright (c) 2026 Alex Kaplan
 *
 * The CK803 core's cache controller sits in the core's tightly coupled IP
 * space, at TCIP_BASE 0xe000e000 + 0x1000. The HR_C7000 datasheet does not
 * describe it. The layout follows XuanTie's CSI core header (core_803.h,
 * CACHE_Type and the CACHE_* field definitions, in alibaba/AliOS-Things and
 * XUANTIE-RV/open-yoc components/csi, Apache-2.0), which is the only source:
 *
 *   0x000 CER     enable: EN [0], CFIG [1], WB [2], DCW [4], WA [5]
 *   0x004 CIR     operation: INV_ALL [0], INV_ONE [1], CLR_ALL [2],
 *                 CLR_ONE [3], line address [31:4]
 *   0x008 CRCR0-3 cacheable regions: EN [0], SIZE [5:1], base [31:10];
 *                 SIZE code n is a region of 2^(n + 1) bytes (0xb = 4 KB,
 *                 0x1f = 4 GB)
 *   0xff4 CPFCR   profiling: PFEN [0], PFRST [1]
 *   0xff8 CPFATR  access count, 0xffc CPFMTR miss count (units of 256)
 *
 * QEMU does not model caches: every access goes to memory, so cache
 * contents can never be stale here. The model keeps the registers and
 * decodes what the firmware asks for, so a configuration is visible
 * (DM32_CACHE_LOG) and reads back as on the core:
 *   - CER and CRCR keep their defined bits; the others read 0.
 *   - CIR starts an operation that completes at once and reads 0. The
 *     header's helpers write CIR and carry on without polling, which is
 *     consistent with this; it has no status bit.
 *   - CPFCR PFRST clears the counters and reads 0. Assumption: PFRST
 *     self-clears like a CIR operation. The counters read 0, because no
 *     accesses are counted.
 * Assumption: every register resets to 0; the header gives no reset values.
 * The reserved space 0x018-0xff3 reads 0 and ignores writes.
 *
 * Not modelled: caching itself, so neither a stale line after the program
 * flash changes nor a missed clean can be detected, and the profiling counts.
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "qemu/log.h"
#include "hw/csky/dm32.h"

#define TYPE_DM32_CACHE "dm32_cache"
OBJECT_DECLARE_SIMPLE_TYPE(DM32CacheState, DM32_CACHE)

#define WIN          0x1000

#define R_CER        0x000
#define R_CIR        0x004
#define R_CRCR0      0x008
#define R_CRCR3      0x014
#define R_CPFCR      0xff4
#define R_CPFATR     0xff8
#define R_CPFMTR     0xffc

/* CER fields, named as the CACHE_CER_*_Pos fields of core_803.h. */
#define CER_EN       (1u << 0)
#define CER_CFIG     (1u << 1)
#define CER_WB       (1u << 2)
#define CER_DCW      (1u << 4)
#define CER_WA       (1u << 5)
#define CER_MASK     (CER_EN | CER_CFIG | CER_WB | CER_DCW | CER_WA)
#define CIR_INV_ALL  (1u << 0)
#define CIR_INV_ONE  (1u << 1)
#define CIR_CLR_ALL  (1u << 2)
#define CIR_CLR_ONE  (1u << 3)
#define CRCR_MASK    0xfffffc3fu    /* base [31:10], SIZE [5:1], EN [0] */
#define CPFCR_PFEN   (1u << 0)
#define CPFCR_PFRST  (1u << 1)

struct DM32CacheState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t cer;
    uint32_t crcr[4];
    uint32_t cpfcr;
    bool log;
};

static void dm32_cache_log_crcr(DM32CacheState *s, int n)
{
    uint32_t v = s->crcr[n];
    unsigned code = (v >> 1) & 0x1f;

    if (!(v & 1)) {
        fprintf(stderr, "[dm32-cache] region %d off\n", n);
    } else if (code < 0xb) {
        fprintf(stderr, "[dm32-cache] region %d: base 0x%08x, reserved size "
                "code 0x%x\n", n, v & 0xfffffc00u, code);
    } else {
        fprintf(stderr, "[dm32-cache] region %d: 0x%08x, %" PRIu64
                " bytes\n", n, v & 0xfffffc00u, (uint64_t)1 << (code + 1));
    }
}

static uint64_t dm32_cache_read(void *opaque, hwaddr off, unsigned size)
{
    DM32CacheState *s = opaque;

    switch (off) {
    case R_CER:
        return s->cer;
    case R_CRCR0 ... R_CRCR3:
        return s->crcr[(off - R_CRCR0) / 4];
    case R_CPFCR:
        return s->cpfcr;
    case R_CIR:         /* operations complete at once */
    case R_CPFATR:      /* no accesses are counted */
    case R_CPFMTR:
    default:
        return 0;
    }
}

static void dm32_cache_write(void *opaque, hwaddr off, uint64_t val,
                             unsigned size)
{
    DM32CacheState *s = opaque;
    uint32_t v = val;

    switch (off) {
    case R_CER:
        s->cer = v & CER_MASK;
        if (s->log) {
            fprintf(stderr, "[dm32-cache] CER = 0x%02x (%s%s%s%s%s)\n",
                    s->cer, (s->cer & CER_EN) ? "on" : "off",
                    (s->cer & CER_CFIG) ? " CFIG" : "",
                    (s->cer & CER_WB) ? " WB" : "",
                    (s->cer & CER_DCW) ? " DCW" : "",
                    (s->cer & CER_WA) ? " WA" : "");
        }
        break;
    case R_CIR:
        if (s->log) {
            fprintf(stderr, "[dm32-cache] CIR:%s%s%s%s line 0x%08x\n",
                    (v & CIR_INV_ALL) ? " invalidate-all" : "",
                    (v & CIR_INV_ONE) ? " invalidate-line" : "",
                    (v & CIR_CLR_ALL) ? " clean-all" : "",
                    (v & CIR_CLR_ONE) ? " clean-line" : "", v & ~0xfu);
        }
        break;
    case R_CRCR0 ... R_CRCR3: {
        int n = (off - R_CRCR0) / 4;

        s->crcr[n] = v & CRCR_MASK;
        if (s->log) {
            dm32_cache_log_crcr(s, n);
        }
        break;
    }
    case R_CPFCR:
        /* PFRST clears the counters, which are always 0 here. */
        s->cpfcr = v & CPFCR_PFEN;
        break;
    case R_CPFATR:
    case R_CPFMTR:
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "dm32_cache: write to reserved "
                      "offset 0x%03" HWADDR_PRIx "\n", off);
        break;
    }
}

static const MemoryRegionOps dm32_cache_ops = {
    .read = dm32_cache_read,
    .write = dm32_cache_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void dm32_cache_reset(DeviceState *dev)
{
    DM32CacheState *s = DM32_CACHE(dev);

    s->cer = 0;
    memset(s->crcr, 0, sizeof(s->crcr));
    s->cpfcr = 0;
}

static void dm32_cache_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), dm32_cache_reset);
}

static void dm32_cache_init(Object *obj)
{
    DM32CacheState *s = DM32_CACHE(obj);

    memory_region_init_io(&s->iomem, obj, &dm32_cache_ops, s,
                          TYPE_DM32_CACHE, WIN);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    s->log = dm32_env_flag("DM32_CACHE_LOG");
    dm32_cache_reset(DEVICE(obj));
}

static const TypeInfo dm32_cache_type = {
    .name = TYPE_DM32_CACHE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DM32CacheState),
    .instance_init = dm32_cache_init,
    .class_init = dm32_cache_class_init,
};

static void dm32_cache_register(void)
{
    type_register_static(&dm32_cache_type);
}

type_init(dm32_cache_register)
