/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Copyright (c) 2026 Alex Kaplan
 *
 * Host test for include/hw/csky/dm32_regfile.h, driven by the register
 * lists generated into dm32_svd.h. Not part of the QEMU build.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -I include tests/svd/test_regfile.c
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "hw/csky/dm32_regfile.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define CHECK_EQ(got, want) do { \
    unsigned long g_ = (unsigned long)(got); \
    unsigned long w_ = (unsigned long)(want); \
    if (g_ != w_) { \
        printf("FAIL %s:%d: %s = 0x%lx, want 0x%lx\n", __FILE__, __LINE__, \
               #got, g_, w_); \
        failures++; \
    } \
} while (0)

static const DM32RegDesc wdg[] = { DM32_SVD_WDG_REGS(DM32_REGDESC) };
static const DM32RegDesc uart[] = { DM32_SVD_UART0_REGS(DM32_REGDESC) };
static const DM32RegDesc pic[] = { DM32_SVD_PIC_REGS(DM32_REGDESC) };
static const DM32RegDesc adc[] = { DM32_SVD_ADC_REGS(DM32_REGDESC) };
static const DM32RegDesc codec[] = { DM32_SVD_CODEC_REGS(DM32_REGDESC) };
static const DM32RegDesc timer[] = { DM32_SVD_TIMER_REGS(DM32_REGDESC) };

/* Hand-built: no real SVD register has these access combinations. */
static const DM32RegDesc hand[] = {
    { "W1S", 0x0, 32, DM32_SVD_RESET_EXPLICIT, 0x0, 0, 0, 0, 0xf0, 0 },
    { "RC", 0x4, 32, 0, 0x5, 0, 0, 0, 0, 0xffffffff },
};

static uint32_t wdg_val[ARRAY_SIZE(wdg)];
static uint32_t pic_val[ARRAY_SIZE(pic)];
static uint32_t adc_val[ARRAY_SIZE(adc)];
static uint32_t codec_val[ARRAY_SIZE(codec)];
static uint32_t hand_val[ARRAY_SIZE(hand)];

/* Index of the first register named @name, or -1. */
static int find_name(const DM32RegDesc *d, size_t n, const char *name)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (strcmp(d[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void test_reset(void)
{
    size_t i;

    dm32_regfile_reset(wdg, ARRAY_SIZE(wdg), wdg_val);
    CHECK_EQ(wdg_val[find_name(wdg, ARRAY_SIZE(wdg), "WDG_LOCK")], 1);
    CHECK_EQ(wdg_val[find_name(wdg, ARRAY_SIZE(wdg), "WDG_OT_LOAD_H")], 7);
    CHECK_EQ(wdg_val[find_name(wdg, ARRAY_SIZE(wdg), "WDG_OT_LOAD_L")],
             0xc0000000u);
    for (i = 0; i < ARRAY_SIZE(wdg); i++) {
        CHECK_EQ(wdg_val[i], wdg[i].reset);
    }
}

static void test_read_only(void)
{
    uint32_t v = 0;

    dm32_regfile_reset(wdg, ARRAY_SIZE(wdg), wdg_val);
    CHECK(dm32_regfile_write(wdg, ARRAY_SIZE(wdg), wdg_val, 0x008,
                             0x12345678));
    CHECK(dm32_regfile_read(wdg, ARRAY_SIZE(wdg), wdg_val, 0x008, &v));
    CHECK_EQ(v, 0xc0000000u);

    /* A read-write register next to it still takes writes. */
    CHECK(dm32_regfile_write(wdg, ARRAY_SIZE(wdg), wdg_val, 0x000, 0));
    CHECK(dm32_regfile_read(wdg, ARRAY_SIZE(wdg), wdg_val, 0x000, &v));
    CHECK_EQ(v, 0);
}

static void test_w1c(void)
{
    uint32_t v = 0;
    int i = find_name(pic, ARRAY_SIZE(pic), "PIC_INT_ST");

    CHECK(i >= 0);
    CHECK_EQ(pic[i].offset, 0x044);

    /* Write 0 to bits that are set: nothing is cleared. */
    dm32_regfile_reset(pic, ARRAY_SIZE(pic), pic_val);
    pic_val[i] = 0xfffffffeu;
    CHECK(dm32_regfile_write(pic, ARRAY_SIZE(pic), pic_val, 0x044, 0));
    CHECK(dm32_regfile_read(pic, ARRAY_SIZE(pic), pic_val, 0x044, &v));
    CHECK_EQ(v, 0xfffffffeu);

    /* Write 1s: the w1c bits clear. Bit 0 is not w1c in the SVD. */
    pic_val[i] = 0xffffffffu;
    CHECK(dm32_regfile_write(pic, ARRAY_SIZE(pic), pic_val, 0x044, 0x6));
    CHECK(dm32_regfile_read(pic, ARRAY_SIZE(pic), pic_val, 0x044, &v));
    CHECK_EQ(v & 0x6, 0);
    CHECK_EQ(v & 0xfffffff8u, 0xfffffff8u);

    pic_val[i] = 0xffffffffu;
    CHECK(dm32_regfile_write(pic, ARRAY_SIZE(pic), pic_val, 0x044,
                             0xfffffffeu));
    CHECK(dm32_regfile_read(pic, ARRAY_SIZE(pic), pic_val, 0x044, &v));
    CHECK_EQ(v & 0xfffffffeu, 0);
}

static void test_uart_alternate(void)
{
    int i;
    size_t n = ARRAY_SIZE(uart);

    i = dm32_regfile_index(uart, n, 0x000);
    CHECK(i >= 0);
    CHECK(strcmp(uart[i].name, "RBR") == 0);
    CHECK(!(uart[i].flags & DM32_SVD_ALTERNATE));

    /* IER is the answer at 0x004; DLH shares it and is skipped. */
    i = dm32_regfile_index(uart, n, 0x004);
    CHECK(i >= 0);
    CHECK(strcmp(uart[i].name, "IER") == 0);
}

static void test_write_only(void)
{
    uint32_t v = 0xdeadbeef;
    int i = find_name(adc, ARRAY_SIZE(adc), "ADC_START");

    CHECK(i >= 0);
    CHECK_EQ(adc[i].wo, 0xffffffffu);
    dm32_regfile_reset(adc, ARRAY_SIZE(adc), adc_val);
    CHECK(dm32_regfile_write(adc, ARRAY_SIZE(adc), adc_val, 0x014,
                             0xffffffffu));
    CHECK(dm32_regfile_read(adc, ARRAY_SIZE(adc), adc_val, 0x014, &v));
    CHECK_EQ(v, 0);
}

static void test_8bit(void)
{
    uint32_t v = 0;
    int i = find_name(codec, ARRAY_SIZE(codec), "AICR_DAC");
    int sr1 = find_name(codec, ARRAY_SIZE(codec), "SR1");

    CHECK(i >= 0 && sr1 >= 0);
    CHECK_EQ(codec[i].size, 8);
    dm32_regfile_reset(codec, ARRAY_SIZE(codec), codec_val);
    CHECK_EQ(codec_val[i], 0xd3);

    CHECK(dm32_regfile_write(codec, ARRAY_SIZE(codec), codec_val, 0x008,
                             0xffffff34u));
    CHECK(dm32_regfile_read(codec, ARRAY_SIZE(codec), codec_val, 0x008, &v));
    CHECK_EQ(v, 0x34);

    /* SR1 is read-only: the write is dropped, the value stays 0. */
    CHECK(dm32_regfile_write(codec, ARRAY_SIZE(codec), codec_val, 0x000,
                             0x5a));
    CHECK(dm32_regfile_read(codec, ARRAY_SIZE(codec), codec_val, 0x000, &v));
    CHECK_EQ(v, 0);
}

static void test_timer_cluster(void)
{
    int i = dm32_regfile_index(timer, ARRAY_SIZE(timer), 0x064);

    CHECK(i >= 0);
    CHECK(strcmp(timer[i].name, "TIMER_CH5_LoadCount") == 0);
}

static void test_no_register(void)
{
    uint32_t v = 0;

    CHECK(!dm32_regfile_read(wdg, ARRAY_SIZE(wdg), wdg_val, 0x018, &v));
    CHECK(!dm32_regfile_write(wdg, ARRAY_SIZE(wdg), wdg_val, 0x018, 1));
    CHECK(!dm32_regfile_read(wdg, ARRAY_SIZE(wdg), wdg_val, 0x100, &v));
}

static void test_w1s_and_rc(void)
{
    uint32_t v = 0;
    size_t n = ARRAY_SIZE(hand);

    dm32_regfile_reset(hand, n, hand_val);
    CHECK_EQ(hand_val[0], 0);
    CHECK_EQ(hand_val[1], 5);

    /* W1S: 1s set the w1s bits; 0s leave them alone. */
    CHECK(dm32_regfile_write(hand, n, hand_val, 0x0, 0x30));
    CHECK(dm32_regfile_read(hand, n, hand_val, 0x0, &v));
    CHECK_EQ(v, 0x30);
    CHECK(dm32_regfile_write(hand, n, hand_val, 0x0, 0x00));
    CHECK(dm32_regfile_read(hand, n, hand_val, 0x0, &v));
    CHECK_EQ(v, 0x30);
    CHECK(dm32_regfile_write(hand, n, hand_val, 0x0, 0xc0));
    CHECK(dm32_regfile_read(hand, n, hand_val, 0x0, &v));
    CHECK_EQ(v, 0xf0);

    /* Clear-on-read: the first read returns the value, the next is 0. */
    CHECK(dm32_regfile_read(hand, n, hand_val, 0x4, &v));
    CHECK_EQ(v, 5);
    CHECK(dm32_regfile_read(hand, n, hand_val, 0x4, &v));
    CHECK_EQ(v, 0);
    CHECK(dm32_regfile_write(hand, n, hand_val, 0x4, 0x12345678u));
    CHECK(dm32_regfile_read(hand, n, hand_val, 0x4, &v));
    CHECK_EQ(v, 0x12345678u);
    CHECK(dm32_regfile_read(hand, n, hand_val, 0x4, &v));
    CHECK_EQ(v, 0);
}

int main(void)
{
    test_reset();
    test_read_only();
    test_w1c();
    test_uart_alternate();
    test_write_only();
    test_8bit();
    test_timer_cluster();
    test_no_register();
    test_w1s_and_rc();

    if (failures) {
        printf("test_regfile: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_regfile: PASS\n");
    return 0;
}
