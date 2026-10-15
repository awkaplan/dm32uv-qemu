/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Baofeng DM-32UV ADC controller: ADC controller model at 0x140d0000
 *
 * Copyright (c) 2026 Alex Kaplan
 *
 * Modelled against HR_C7000 v2.7 EN section 5.9 ("ADC Controller"), Register
 * Table 24, and the single-scan operating procedure in section 5.9.3.1.
 *
 * This is a behavioural model, not a RAM stub. It enforces the datasheet's
 * preconditions, so a driver that skips the ADC's initialisation reads 0 here
 * as it would on hardware. An uninitialised controller therefore cannot report
 * a plausible voltage.
 *
 * Behaviour:
 *   - PD_FORCE_HIGH (ADC_CTRL[3]) holds the ADC in reset. No conversion
 *     occurs and DATA reads 0.
 *   - A conversion happens only on a write of 1 to ADC_START, and only for the
 *     channels selected in ADC_CH_VLD. In single-scan mode only the lowest
 *     selected channel converts (section 5.9.5.10 note).
 *   - A DATA register reads 0 for any channel that has not converted since
 *     reset. Reading the wrong pair register or the wrong half therefore yields
 *     0, not a plausible voltage.
 *   - ADC_START self-clears on EOC. ADC_CTRL_BUSY and SAMP_FSM_STATE read idle
 *     after the conversion, which is instantaneous. ADC_INTR_STA latches the
 *     completed channel's bit.
 *   - ADC_CTRL_VERID reads 0x20151216, so a driver can check that the block is
 *     alive.
 *   - ADC_CTRL_STOP[1] and ADC_CTRL[3] are the only writes honoured while BUSY,
 *     per note (c).
 *   - A pad left in its reset GPIO state is not connected to the converter, so
 *     its channel reads 0. Channel n's pad is PTC(15 + n), and select value
 *     0 is its analog input (IO_DIPLEX2 adcN_in_sel, v2.7 EN 4.4.4.6); the
 *     pad state comes from dm32_pad_function() in dm32_sysreg.c.
 *
 * Input levels come from the environment, so that a test can place a known
 * voltage on a known input. They are read once, at instance init:
 *   DM32_ADC_CH0..DM32_ADC_CH7  10-bit value for that channel
 *   DM32_ADC_VBAT               10-bit value for the channel named by
 *                               DM32_ADC_VBAT_CH (default 5, channel F)
 *   DM32_ADC_VERBOSE=1          log conversions to stderr
 * The stock adc_read_channel_dispatch(2) reads channel C (the low half of
 * ADC_DATA_CD), not channel F. Set DM32_ADC_CH2 to feed the stock battery
 * reading. The channel F default is kept so that existing OpenRTX-port recipes
 * keep working. Unset channels read as 0. That is deliberate: an unidentified
 * input must not pass for a real one.
 *
 * Not modelled: settling time, noise, conversion timing, and any interrupt.
 */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include <stdlib.h>
#include <stdio.h>
#include "hw/csky/dm32.h"

#define TYPE_DM32_ADC "dm32_adc"
OBJECT_DECLARE_SIMPLE_TYPE(DM32AdcState, DM32_ADC)

#define WIN 0x10000

#define R_CTRL          0x00
#define R_INTR_DELTA    0x04
#define R_INTR          0x08
#define R_INTR_STA      0x0c
#define R_SCAN_TIME     0x10
#define R_START         0x14
#define R_CTRL_STATE    0x18
#define R_INTR_THRES    0x1c
#define R_CTRL_STOP     0x20
#define R_CH_VLD        0x24
#define R_PD_SEOC_TIME  0x28
#define R_P2S_EN        0x2c
#define R_DATA_AB       0x30
#define R_DATA_GH       0x3c
#define R_VERID         0x40
#define R_DIR_DAT_OP    0x44

#define CTRL_PD_FORCE_HIGH  (1u << 3)
#define CTRL_MODE_CONT      (1u << 2)
#define STOP_CTRL_RESET     (1u << 1)
#define VERID_VALUE         0x20151216u

/* Level variables take 0..0x3ff; this value marks a variable that is unset. */
#define LEVEL_UNSET         0x400u
#define LEVEL_MAX           0x3ffu

struct DM32AdcState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;

    uint32_t ctrl, intr_delta, intr, intr_sta, scan_time;
    uint32_t intr_thres, ch_vld, pd_seoc, p2s_en;

    uint16_t data[8];       /* latched conversion result per channel */
    /* Bitmask: the channel has produced a result since reset. */
    uint8_t converted;
    /* ADC_DIR_DAT_OP: the most recent conversion, for debugging. */
    uint32_t last_dir_dat;
    int verbose;
    /* Input level per channel, from DM32_ADC_CH* and DM32_ADC_VBAT */
    uint16_t input[8];
};

/* Read the input level variables once; each channel's level is stored. */
static void dm32_adc_read_inputs(DM32AdcState *s)
{
    uint64_t vbat_ch = dm32_env_uint("DM32_ADC_VBAT_CH", 5, 0, 7);
    uint64_t vbat = dm32_env_uint("DM32_ADC_VBAT", LEVEL_UNSET, 0, LEVEL_MAX);

    for (unsigned ch = 0; ch < 8; ch++) {
        char name[20];
        uint64_t level;

        snprintf(name, sizeof(name), "DM32_ADC_CH%u", ch);
        level = dm32_env_uint(name, LEVEL_UNSET, 0, LEVEL_MAX);
        /* DM32_ADC_VBAT applies to its channel only when that is unset. */
        if (level == LEVEL_UNSET && ch == vbat_ch) {
            level = vbat;
        }
        s->input[ch] = (level == LEVEL_UNSET) ? 0 : (uint16_t)level;
    }
}

static void dm32_adc_reset_ctrl(DM32AdcState *s)
{
    s->converted = 0;
    memset(s->data, 0, sizeof(s->data));
    s->intr_sta = 0;
}

/* One single-shot conversion: lowest valid channel only (5.9.5.10 note). */
static void dm32_adc_convert(DM32AdcState *s)
{
    unsigned ch;

    if (s->ctrl & CTRL_PD_FORCE_HIGH) {
        if (s->verbose) {
            fprintf(stderr, "[dm32-adc] START ignored: ADC held in reset "
                    "(CTRL[3] set)\n");
        }
        return;
    }
    if (s->ch_vld == 0) {
        if (s->verbose) {
            fprintf(stderr, "[dm32-adc] START with ADC_CH_VLD=0: no channel "
                    "selected\n");
        }
        return;
    }

    for (ch = 0; ch < 8; ch++) {
        if (s->ch_vld & (1u << ch)) {
            /*
             * The pad must be muxed to the analog input. If it is still GPIO,
             * the converter sees nothing. Report a zero conversion rather than
             * the injected level, so that a driver which skips the IO_DIPLEX2
             * claim fails here as it does on silicon.
             */
            if (dm32_pad_function(DM32_PTC(15 + ch)) != 0) {
                dm32_event(DM32_EV_ADC_PAD, "channel %c converted while its "
                           "pad is GPIO (IO_DIPLEX2 bit %u set)", 'A' + ch,
                           19 + ch);
                if (s->verbose) {
                    fprintf(stderr, "[dm32-adc] ch%c(%u) pad is GPIO "
                            "(IO_DIPLEX2 bit %u set): reads 0, claim the pad "
                            "first\n", 'A' + ch, ch, 19 + ch);
                }
                s->data[ch] = 0;
                s->converted |= (1u << ch);
                s->intr_sta |= (1u << ch);
                if (!(s->ctrl & CTRL_MODE_CONT)) {
                    return;
                }
                continue;
            }
            s->data[ch] = s->input[ch];
            s->converted |= (1u << ch);
            /*
             * The stock per-channel reader for channel 2 (0x030ada14 entry 2,
             * 0x0301cbc0) reads the low half of ADC_DATA_CD (channel C)
             * directly. Channel 2 therefore needs no special handling here.
             */
            /*
             * ADC_INTR_STA latches a channel's completion flag only if the
             * channel is enabled in ADC_INTR (bit 16 as the master enable,
             * bits 8:0 per channel), per section 5.9.3.1 step 9. A driver that
             * leaves ADC_INTR at 0 therefore sees no completion flags, as on
             * hardware.
             */
            if ((s->intr & (1u << 16)) && (s->intr & (1u << ch))) {
                s->intr_sta |= (1u << ch);
            } else if (s->verbose) {
                fprintf(stderr, "[dm32-adc] ch%c(%u) converted but "
                        "ADC_INTR_STA NOT latched: ADC_INTR=%08x "
                        "(needs bit16 + bit%u)\n",
                        'A' + ch, ch, (unsigned)s->intr, ch);
            }
            s->last_dir_dat = s->data[ch];
            if (s->verbose) {
                fprintf(stderr, "[dm32-adc] convert ch%c(%u) -> %u\n",
                        'A' + ch, ch, s->data[ch]);
            }
            /* Single scan: only the lowest selected channel produces data. */
            if (!(s->ctrl & CTRL_MODE_CONT)) {
                return;
            }
        }
    }
}

static uint64_t dm32_adc_read(void *opaque, hwaddr off, unsigned size)
{
    DM32AdcState *s = opaque;

    switch (off) {
    case R_CTRL:
        return s->ctrl;
    case R_INTR_DELTA:
        return s->intr_delta;
    case R_INTR:
        return s->intr;
    case R_INTR_STA:
        return s->intr_sta;
    case R_SCAN_TIME:
        return s->scan_time;
    case R_START:
        /* Self-clears on EOC. The conversion is immediate. */
        return 0;
    case R_CTRL_STATE:
        /* BUSY=0, FSM=IDLE: conversions complete at once. */
        return 0;
    case R_INTR_THRES:
        return s->intr_thres;
    case R_CTRL_STOP:
        /* Both bits are write-only and self-clearing. */
        return 0;
    case R_CH_VLD:
        return s->ch_vld;
    case R_PD_SEOC_TIME:
        return s->pd_seoc;
    case R_P2S_EN:
        return s->p2s_en;
    case R_VERID:
        return VERID_VALUE;
    case R_DIR_DAT_OP:
        return s->last_dir_dat & 0x3ff;
    default:
        break;
    }

    if (off >= R_DATA_AB && off <= R_DATA_GH) {
        unsigned pair = (unsigned)((off - R_DATA_AB) / 4);
        unsigned even = pair * 2;
        unsigned odd = pair * 2 + 1;
        uint32_t lo = (s->converted & (1u << even)) ? s->data[even] : 0;
        uint32_t hi = (s->converted & (1u << odd)) ? s->data[odd] : 0;

        /*
         * [9:0] is the even channel and [25:16] the odd channel. [15:10] and
         * [31:26] are reserved and read 0.
         */
        return ((hi & 0x3ff) << 16) | (lo & 0x3ff);
    }

    return 0;
}

static void dm32_adc_write(void *opaque, hwaddr off, uint64_t val,
                           unsigned size)
{
    DM32AdcState *s = opaque;
    uint32_t v = (uint32_t)val;

    switch (off) {
    case R_CTRL: {
        uint32_t was = s->ctrl;

        s->ctrl = v;
        /* Entering reset clears the ADC and the controller. */
        if ((v & CTRL_PD_FORCE_HIGH) && !(was & CTRL_PD_FORCE_HIGH)) {
            dm32_adc_reset_ctrl(s);
        }
        break;
    }
    case R_INTR_DELTA:
        s->intr_delta = v;
        break;
    case R_INTR:
        s->intr = v;
        break;
    case R_INTR_STA:
        /* Write-to-clear: clears the latched channel flags. */
        s->intr_sta &= ~v;
        break;
    case R_SCAN_TIME:
        s->scan_time = v;
        break;
    case R_START:
        if (v & 1u) {
            dm32_adc_convert(s);
        }
        break;
    case R_CTRL_STATE:
        /* The ADC_CTRL_STATE [8:6] error flags clear on any write. */
        break;
    case R_INTR_THRES:
        s->intr_thres = v;
        break;
    case R_CTRL_STOP:
        if (v & STOP_CTRL_RESET) {
            dm32_adc_reset_ctrl(s);
        }
        break;
    case R_CH_VLD:
        s->ch_vld = v & 0xffu;
        break;
    case R_PD_SEOC_TIME:
        s->pd_seoc = v;
        break;
    case R_P2S_EN:
        s->p2s_en = v;
        break;
    default:
        if (off >= R_DATA_AB && off <= R_DATA_GH) {
            fprintf(stderr, "[dm32-adc] write to RO data register "
                    "+0x%02x ignored\n", (unsigned)off);
        }
        break;
    }
}

static const MemoryRegionOps dm32_adc_ops = {
    .read = dm32_adc_read,
    .write = dm32_adc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/*
 * Machine reset and SYS_SOFT_RSTN[6]: reset values from v2.7 EN 5.9.5,
 * PD_SEOC_TIME 0x0a20 and every other register 0; no conversion latched.
 */
static void dm32_adc_reset(DeviceState *dev)
{
    DM32AdcState *s = DM32_ADC(dev);

    s->ctrl = s->intr_delta = s->intr = s->scan_time = 0;
    s->intr_thres = s->ch_vld = s->p2s_en = 0;
    s->pd_seoc = 0x0a20u;
    s->last_dir_dat = 0;
    dm32_adc_reset_ctrl(s);
}

static void dm32_adc_init(Object *obj)
{
    DM32AdcState *s = DM32_ADC(obj);

    s->verbose = dm32_env_flag("DM32_ADC_VERBOSE");
    dm32_adc_read_inputs(s);
    memory_region_init_io(&s->iomem, obj, &dm32_adc_ops, s, TYPE_DM32_ADC,
                          WIN);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    dm32_adc_reset(DEVICE(obj));
}

static void dm32_adc_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), dm32_adc_reset);
}

static const TypeInfo dm32_adc_type = {
    .name = TYPE_DM32_ADC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DM32AdcState),
    .instance_init = dm32_adc_init,
    .class_init = dm32_adc_class_init,
};

static void dm32_adc_register(void)
{
    type_register_static(&dm32_adc_type);
}

type_init(dm32_adc_register)
