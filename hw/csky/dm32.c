/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Baofeng DM-32UV board model (-M dm32): HR_C7000 / CK803S board for
 * qemu-system-cskyv2
 *
 * Copyright (c) 2026 Alex Kaplan
 *
 * Memory map (from reverse engineering of the stock firmware):
 *
 *   0x00000000  Low RAM (64 KB): CK803S vectors and the bit-banged GPIO
 *               shadow
 *   0x00010000  IRAM (288 KB): loaded from an IRAM image if provided
 *   0x03000000  Flash (16 MB window, 1 MB image): the program image,
 *               executed in place; boot starts here. The 1 MB image is a
 *               read-only ROM device. Writes are dropped unless
 *               DM32_FLASH_WRITABLE=1 is set.
 *   0x04000000  Serial controller (dm32_ssi): the program flash's raw SPI
 *               port, by structural evidence (quad-enable only)
 *   0x11000000  System, clock and modem registers (LAYER2 at 0x400,
 *               WORK_MODE at 0x100, AUDIO at 0x80, ...): dm32_sysreg
 *   Every Table 26 peripheral block: an unimplemented-device window under
 *               its model, if any (reads 0, writes dropped)
 *   0x12000000  LCD (ST7789V-class 8080 bus): dm32_lcd, which renders a
 *               framebuffer
 *   0x13000000  USB (256 KB): unimplemented device
 *   0x14000000  DesignWare APB timer, six channels: dm32_timer
 *   0x14010000  WDG (4 KB window): dm32_wdt (counting; a bite resets the
 *               guest)
 *   0x14020000  GPIOA, 0x14100000 GPIOB, 0x14110000 GPIOC: dm32_gpio
 *               (idle-high, key and pin injection, per-bank DW_apb_gpio
 *               interrupt block on PIC sources 15, 14 and 17)
 *   0x14030000  UART0, 0x14040000 UART1, 0x14050000 UART2,
 *   0x14090000  UART3: csky_uart
 *   0x14060000  I2C0, 0x14070000 I2C1: dm32_i2c, nothing on either bus.
 *               Stock never uses these controllers.
 *   0x14080000  I2C2: dm32_i2c, with the RTC slave in dm32_i2c_rtc.c
 *   0x140a0000, 0x140b0000, 0x140e0000, 0x14150000: SSI0 to SSI3, dm32_ssi
 *   0x140c0000  PWM: dm32_pwm
 *   0x140d0000  ADC: dm32_adc
 *   0x140f0000  DAC: dm32_dac
 *   0x14120000, 0x14130000, 0x14140000: SPI slaves 0 to 2: dm32_spi_slave
 *               (PIC sources 34-36; no master is attached)
 *   0x15000000  SDIO (64 KB): unimplemented device
 *   0x16000000  DMR baseband RAM (64 KB window with the TX and RX RAMs):
 *               dm32_dmrram, capture only
 *   0x160009c0  Codec registers (64 bytes, over that window, PIC source
 *               37): dm32_codec
 *   0x17000000  Programmable interrupt controller: dm32_pic
 *   0x18000000  SRAM (32 KB): loaded from the SRAM dump if provided
 *   0xe000f000  CK803 cache controller (4 KB): dm32_cache, registers only
 *
 * Firmware and dumps are supplied on the command line:
 *   -kernel  <flash image at 0x03000000>
 *   -device loader,file=IRAM.bin,addr=0x00010000
 *   -device loader,file=SRAM.bin,addr=0x18000000
 *
 * This is a partial board model: the core, the memories, and the devices
 * wired below. The devices are described in docs/devices.md and their
 * coverage in docs/coverage.md.
 *
 * No ALPU-MP is modelled. Stock bit-bangs it on GPIOA 5 (SCL) and 6 (SDA)
 * (dm32uv-re specs/dts/dm32uv.dts), and nothing answers there. Splice
 * the ALPU-bypass sector (dm32uv-re
 * data/firmware/patching/sector_030c6000_alpubypass.bin) in, or the stock
 * firmware halts at IRAM 0x169d4.
 *
 * The FD6818 RF transceiver is wired on GPIOA bits 7, 8 and 24
 * (hw/misc/dm32_fd6818.c, forwarded by hw/gpio/dm32_gpio.c). UART0 to
 * UART3 are csky_uart instances. Every peripheral block of the datasheet's
 * address map has an unimplemented-device window (dm32_add_unimp) under its
 * model, so SDIO, USB and the parts of other blocks
 * that no model covers read 0, drop writes and log every access under
 * `-d unimp`. Any other address
 * no region covers falls to a background region (dm32_add_unassigned) that
 * behaves like QEMU's unassigned memory and logs under `-d invalid_mem`.
 * Accesses to both are permissive events (DM32_STRICT). The 0x16000000
 * window (Modem_Buffer TX and RX RAMs) is dm32_dmrram, a capture-only RAM,
 * with dm32_codec over its CODEC registers. The DAC (0x140f0000) is
 * dm32_dac, and the PWM (0x140c0000) is dm32_pwm. The baseband TX DAC
 * (SystemControl.DAC_CONTROL) is not modelled.
 *
 * Not modelled: the ALPU-MP and its I2C slave (the ALPU-bypass sector is
 * needed to boot stock); the baseband TX DAC (SystemControl.DAC_CONTROL); the
 * LCSFC command engine (stores into the flash window are not forwarded to any
 * engine) and the LCSFC interrupt (PIC source 24, not wired); the USB and SDIO
 * blocks, which have no model, read 0 and drop writes; and the distribution of
 * the RAM noise fill and its dependence on the unit and on power-off time.
 */

#undef NEED_CPU_H
#define NEED_CPU_H

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/host-utils.h"
#include "qemu/cutils.h"
#include "target/csky/cpu.h"
#include "hw/csky/csky.h"
#include "hw/core/sysbus.h"
#include "system/system.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "hw/core/boards.h"
#include "system/address-spaces.h"
#include "hw/csky/cskydev.h"
#include "hw/char/csky_uart.h"
#include "hw/csky/dm32.h"
#include "hw/csky/dm32_regfile.h"

/*
 * The addresses and PIC sources wired below agree with the SVD
 * (hw/csky/dm32_svd.h, generated from dm32uv-re
 * data/registers/HR_C7000-DM32.svd). A disagreement fails the build.
 */
QEMU_BUILD_BUG_ON(DM32_SVD_SYSTEMCONTROL_BASE != 0x11000000);
QEMU_BUILD_BUG_ON(DM32_SVD_I8080_BASE != 0x12000000);
QEMU_BUILD_BUG_ON(DM32_SVD_USB_BASE != 0x13000000);
QEMU_BUILD_BUG_ON(DM32_SVD_USB_SIZE != 0x00040000);
QEMU_BUILD_BUG_ON(DM32_SVD_TIMER_BASE != 0x14000000);
QEMU_BUILD_BUG_ON(DM32_SVD_WDG_BASE != 0x14010000);
QEMU_BUILD_BUG_ON(DM32_SVD_GPIOA_BASE != 0x14020000 ||
                  DM32_SVD_GPIOA_IRQ != 15);
QEMU_BUILD_BUG_ON(DM32_SVD_GPIOB_BASE != 0x14100000 ||
                  DM32_SVD_GPIOB_IRQ != 14);
QEMU_BUILD_BUG_ON(DM32_SVD_GPIOC_BASE != 0x14110000 ||
                  DM32_SVD_GPIOC_IRQ != 17);
QEMU_BUILD_BUG_ON(DM32_SVD_UART0_BASE != 0x14030000 ||
                  DM32_SVD_UART0_IRQ != 12);
QEMU_BUILD_BUG_ON(DM32_SVD_UART1_BASE != 0x14040000 ||
                  DM32_SVD_UART1_IRQ != 10);
QEMU_BUILD_BUG_ON(DM32_SVD_UART2_BASE != 0x14050000 ||
                  DM32_SVD_UART2_IRQ != 11);
QEMU_BUILD_BUG_ON(DM32_SVD_UART3_BASE != 0x14090000 ||
                  DM32_SVD_UART3_IRQ != 13);
QEMU_BUILD_BUG_ON(DM32_SVD_I2C0_BASE != 0x14060000 || DM32_SVD_I2C0_IRQ != 8);
QEMU_BUILD_BUG_ON(DM32_SVD_I2C1_BASE != 0x14070000 || DM32_SVD_I2C1_IRQ != 7);
QEMU_BUILD_BUG_ON(DM32_SVD_I2C2_BASE != 0x14080000 || DM32_SVD_I2C2_IRQ != 9);
QEMU_BUILD_BUG_ON(DM32_SVD_SPI_MASTER0_BASE != 0x140a0000);
QEMU_BUILD_BUG_ON(DM32_SVD_SPI_MASTER1_BASE != 0x140b0000);
QEMU_BUILD_BUG_ON(DM32_SVD_SPI2_BASE != 0x140e0000);
QEMU_BUILD_BUG_ON(DM32_SVD_SPI_MASTER3_BASE != 0x14150000);
QEMU_BUILD_BUG_ON(DM32_SVD_PWM_BASE != 0x140c0000);
QEMU_BUILD_BUG_ON(DM32_SVD_ADC_BASE != 0x140d0000);
QEMU_BUILD_BUG_ON(DM32_SVD_DAC_BASE != 0x140f0000);
QEMU_BUILD_BUG_ON(DM32_SVD_SPI_SLAVE0_BASE != 0x14120000 ||
                  DM32_SVD_SPI_SLAVE0_IRQ != 34);
QEMU_BUILD_BUG_ON(DM32_SVD_SPI_SLAVE1_BASE != 0x14130000 ||
                  DM32_SVD_SPI_SLAVE1_IRQ != 35);
QEMU_BUILD_BUG_ON(DM32_SVD_SPI_SLAVE2_BASE != 0x14140000 ||
                  DM32_SVD_SPI_SLAVE2_IRQ != 36);
QEMU_BUILD_BUG_ON(DM32_SVD_SDIO_BASE != 0x15000000 || DM32_SVD_SDIO_IRQ != 20);
QEMU_BUILD_BUG_ON(DM32_SVD_MODEM_BUFFER_BASE != 0x16000000);
QEMU_BUILD_BUG_ON(DM32_SVD_CODEC_BASE != 0x160009c0);
QEMU_BUILD_BUG_ON(DM32_SVD_PIC_BASE != 0x17000000);

/*
 * The flash image starts with the DH_FLAG header. The real entry is the
 * word at flash + 0x04, which is 0x03001000. boot.c's raw-bin path resets
 * the PC to loader_start (0x03000000, the header), so this model installs
 * its own reset hook, which runs after the default reset, to force the true
 * entry.
 */
#define DM32_ENTRY 0x03001000

static CSKYCPU *dm32_cpu;

static void dm32_cpu_reset(void *opaque)
{
    /*
     * The CK803S reset fetches the PC from the vector table: pc = *(vbr + 0).
     * boot.c sets vbr = loader_start = 0x03000000, whose word is the DH_FLAG
     * header (0x0c646874), not code, so reset would jump into nowhere. The
     * real reset vector is the word at flash + 0x04 (0x03001000, the boot
     * ROM's jump target). vbr is pointed at flash + 0x04 so that *(vbr + 0)
     * reads the true entry. This runs after csky_load_kernel's own reset
     * hook, which is registered earlier.
     */
    CSKYCPU *cpu = opaque;

    cpu->env.cp0.vbr = 0x03000004;
    /* Also set directly, in case the vector fetch is skipped. */
    cpu->env.pc = DM32_ENTRY;
}

static void dm32_cpu_soft_reset_work(CPUState *cs, run_on_cpu_data data)
{
    cpu_reset(cs);
    dm32_cpu_reset(CSKY_CPU(cs));
    cs->exception_index = -1;
}

/*
 * SYS_SOFT_RSTN[8]: the CPU restarts at the reset entry, devices and memory
 * untouched. The reset runs on the vCPU thread outside the current TB.
 */
void dm32_cpu_soft_reset(void)
{
    async_run_on_cpu(CPU(dm32_cpu), dm32_cpu_soft_reset_work,
                     RUN_ON_CPU_NULL);
}

/*
 * UART1-3 transmit only while their TXD pad selects the UART: IO_DIPLEX0
 * uart<n>_txd_sel = 0, pads PTA10, PTA12 and PTA14 (v2.7 EN 4.4.4.4). UART0
 * has dedicated pads. csky_uart cannot ask about pads, so a 16-byte window
 * over its THR and LCR, at higher priority, forwards every access to it and
 * drops a transmitted byte (a pad_unclaimed event) while the pad is not the
 * UART's. LCR bit 7 (DLAB) turns offset 0 into the divisor latch, which is
 * forwarded. Receive is not gated: the chardev feeds csky_uart directly.
 */
typedef struct DM32UartGate {
    MemoryRegion *uart;
    MemoryRegion win;
    int n;
    unsigned txd_pad;
    uint32_t lcr;
} DM32UartGate;

static DM32UartGate dm32_uart_gate[4];

static uint64_t dm32_uart_gate_read(void *opaque, hwaddr off, unsigned size)
{
    DM32UartGate *g = opaque;
    uint64_t v = 0;

    memory_region_dispatch_read(g->uart, off, &v, size_memop(size) | MO_LE,
                                MEMTXATTRS_UNSPECIFIED);
    return v;
}

static void dm32_uart_gate_write(void *opaque, hwaddr off, uint64_t val,
                                 unsigned size)
{
    DM32UartGate *g = opaque;

    if (off == 0x0c) {
        g->lcr = val;
    } else if (off == 0x00 && !(g->lcr & 0x80) &&
               dm32_pad_function(g->txd_pad) != 0) {
        dm32_event(DM32_EV_PAD_UNCLAIMED, "UART%d byte 0x%02x sent while "
                   "pad PTA%u is not %s", g->n, (unsigned)(val & 0xff),
                   g->txd_pad, dm32_pad_name(g->txd_pad));
        return;
    }
    memory_region_dispatch_write(g->uart, off, val, size_memop(size) | MO_LE,
                                 MEMTXATTRS_UNSPECIFIED);
}

static const MemoryRegionOps dm32_uart_gate_ops = {
    .read = dm32_uart_gate_read,
    .write = dm32_uart_gate_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void dm32_uart_gate_create(MemoryRegion *sysmem, int n, hwaddr base,
                                  DeviceState *uart, unsigned txd_pad)
{
    DM32UartGate *g = &dm32_uart_gate[n];

    g->uart = sysbus_mmio_get_region(SYS_BUS_DEVICE(uart), 0);
    g->n = n;
    g->txd_pad = txd_pad;
    memory_region_init_io(&g->win, NULL, &dm32_uart_gate_ops, g,
                          "dm32.uart-pads", 0x10);
    memory_region_add_subregion_overlap(sysmem, base, &g->win, 1);
}

static struct csky_boot_info dm32_binfo = {
    .loader_start = 0x03000000,   /* flash exec base */
    .freq         = 192000000ll,  /* HR_C7000 max 192MHz */
};

/*
 * Permissive events (include/hw/csky/dm32.h). Every event is counted, and
 * one "dm32-events:" line with all counts is printed to stderr at exit.
 *
 * DM32_STRICT selects the classes that stop the guest: "1" or "all" for
 * every class, or a comma-separated list of class names. The first event in
 * a selected class prints "dm32: strict: <class>: <detail>" and stops the
 * guest. DM32_STRICT_ACTION=exit (the default) shuts QEMU down with exit
 * status DM32_STRICT_EXIT; DM32_STRICT_ACTION=pause stops the VM in the
 * guest-panicked state instead, so that a gdb stub (-s) can inspect it.
 * Later events are still counted but do not stop anything again.
 *
 * Debugger accesses (MemTxAttrs.debug) are not events.
 */
#define DM32_STRICT_EXIT 3

static const char *const dm32_event_names[DM32_EV__COUNT] = {
    [DM32_EV_UNIMP]       = "unimp",
    [DM32_EV_UNASSIGNED]  = "unassigned",
    [DM32_EV_FLASH_STORE] = "flash_store",
    [DM32_EV_DAC_PD]      = "dac_pd",
    [DM32_EV_LCD_MUX]     = "lcd_mux",
    [DM32_EV_LCD_OFF]     = "lcd_off",
    [DM32_EV_SSI_DISCARD] = "ssi_discard",
    [DM32_EV_NOR_WEL]     = "nor_wel",
    [DM32_EV_ADC_PAD]     = "adc_pad",
    [DM32_EV_WDT_LOCKED]  = "wdt_locked",
    [DM32_EV_PAD_CONFLICT] = "pad_conflict",
    [DM32_EV_PAD_UNCLAIMED] = "pad_unclaimed",
    [DM32_EV_I2C_NAK] = "i2c_nak",
};

static uint64_t dm32_event_count[DM32_EV__COUNT];
static uint32_t dm32_strict_mask;
static bool dm32_strict_pause;
static int dm32_strict_tripped = -1;

static void dm32_events_report_at_exit(void)
{
    g_autoptr(GString) s = g_string_new("dm32-events:");
    int i;

    for (i = 0; i < DM32_EV__COUNT; i++) {
        g_string_append_printf(s, " %s=%" PRIu64, dm32_event_names[i],
                               dm32_event_count[i]);
    }
    if (dm32_strict_mask) {
        g_string_append_printf(s, " strict_stop=%s",
                               dm32_strict_tripped < 0 ? "none" :
                               dm32_event_names[dm32_strict_tripped]);
    }
    fprintf(stderr, "%s\n", s->str);
}

void dm32_event(DM32Event ev, const char *fmt, ...)
{
    g_autofree char *detail = NULL;
    uint32_t pc = 0;
    va_list ap;

    assert(ev < DM32_EV__COUNT);
    dm32_event_count[ev]++;
    if (!(dm32_strict_mask & (1u << ev)) || dm32_strict_tripped >= 0) {
        return;
    }
    dm32_strict_tripped = ev;

    va_start(ap, fmt);
    detail = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    if (current_cpu) {
        pc = CSKY_CPU(current_cpu)->env.pc;
    }
    error_report("dm32: strict: %s: %s (pc~0x%08x)", dm32_event_names[ev],
                 detail, pc);
    if (dm32_strict_pause) {
        qemu_system_vmstop_request_prepare();
        qemu_system_vmstop_request(RUN_STATE_GUEST_PANICKED);
    } else {
        qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_PANIC,
                                               DM32_STRICT_EXIT);
    }
    if (current_cpu) {
        cpu_exit(current_cpu);
    }
}

bool dm32_env_flag(const char *name)
{
    const char *v = getenv(name);

    return v && *v && strcmp(v, "0") != 0;
}

uint64_t dm32_env_uint(const char *name, uint64_t def, uint64_t min,
                       uint64_t max)
{
    const char *v = getenv(name);
    uint64_t n;

    if (!v || !*v) {
        return def;
    }
    if (*v == '-' || qemu_strtou64(v, NULL, 0, &n) < 0 || n < min ||
        n > max) {
        error_report("%s=%s: expected an integer from %" PRIu64 " to %" PRIu64,
                     name, v, min, max);
        exit(1);
    }
    return n;
}

int64_t dm32_env_int(const char *name, int64_t def, int64_t min, int64_t max)
{
    const char *v = getenv(name);
    int64_t n;

    if (!v || !*v) {
        return def;
    }
    if (qemu_strtoi64(v, NULL, 0, &n) < 0 || n < min || n > max) {
        error_report("%s=%s: expected an integer from %" PRId64 " to %" PRId64,
                     name, v, min, max);
        exit(1);
    }
    return n;
}

static void dm32_events_init(void)
{
    const char *strict = getenv("DM32_STRICT");
    const char *action = getenv("DM32_STRICT_ACTION");
    int i;

    if (strict && *strict && strcmp(strict, "0") != 0) {
        if (!strcmp(strict, "1") || !strcmp(strict, "all")) {
            dm32_strict_mask = (1u << DM32_EV__COUNT) - 1;
        } else {
            g_auto(GStrv) names = g_strsplit(strict, ",", -1);
            char **n;

            for (n = names; *n; n++) {
                g_strstrip(*n);
                for (i = 0; i < DM32_EV__COUNT; i++) {
                    if (!strcmp(*n, dm32_event_names[i])) {
                        break;
                    }
                }
                if (i == DM32_EV__COUNT) {
                    g_autoptr(GString) known = g_string_new("all");

                    for (i = 0; i < DM32_EV__COUNT; i++) {
                        g_string_append_printf(known, ", %s",
                                               dm32_event_names[i]);
                    }
                    error_report("DM32_STRICT: unknown event class '%s' "
                                 "(known: %s)", *n, known->str);
                    exit(EXIT_FAILURE);
                }
                dm32_strict_mask |= 1u << i;
            }
        }
    }
    if (action && *action) {
        if (!strcmp(action, "pause")) {
            dm32_strict_pause = true;
        } else if (strcmp(action, "exit") != 0) {
            error_report("DM32_STRICT_ACTION: '%s' is not exit or pause",
                         action);
            exit(EXIT_FAILURE);
        }
    }
    atexit(dm32_events_report_at_exit);
}

/*
 * Unimplemented-device window: reads return 0 and writes are dropped, as with
 * create_unimplemented_device(), and each access is logged under -d unimp in
 * the same format as hw/misc/unimp.c. Each guest access is also a
 * DM32_EV_UNIMP event.
 */
typedef struct DM32UnimpWindow {
    MemoryRegion mr;
    const char *name;
    int offset_fmt_width;
} DM32UnimpWindow;

static MemTxResult dm32_unimp_read(void *opaque, hwaddr offset,
                                   uint64_t *data, unsigned size,
                                   MemTxAttrs attrs)
{
    DM32UnimpWindow *w = opaque;

    *data = 0;
    if (attrs.debug) {
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "%s: unimplemented device read  "
                  "(size %d, offset 0x%0*" HWADDR_PRIx ")\n",
                  w->name, size, w->offset_fmt_width, offset);
    dm32_event(DM32_EV_UNIMP, "%s read, offset 0x%" HWADDR_PRIx
               ", size %u", w->name, offset, size);
    return MEMTX_OK;
}

static MemTxResult dm32_unimp_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size,
                                    MemTxAttrs attrs)
{
    DM32UnimpWindow *w = opaque;

    if (attrs.debug) {
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "%s: unimplemented device write "
                  "(size %d, offset 0x%0*" HWADDR_PRIx
                  ", value 0x%0*" PRIx64 ")\n",
                  w->name, size, w->offset_fmt_width, offset, size << 1,
                  value);
    dm32_event(DM32_EV_UNIMP, "%s write, offset 0x%" HWADDR_PRIx
               ", size %u, value 0x%" PRIx64, w->name, offset, size, value);
    return MEMTX_OK;
}

static const MemoryRegionOps dm32_unimp_ops = {
    .read_with_attrs = dm32_unimp_read,
    .write_with_attrs = dm32_unimp_write,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void dm32_add_unimp(MemoryRegion *sysmem, const char *name,
                           hwaddr base, uint64_t size)
{
    DM32UnimpWindow *w = g_new0(DM32UnimpWindow, 1);

    w->name = name;
    w->offset_fmt_width = DIV_ROUND_UP(64 - clz64(size - 1), 4);
    memory_region_init_io(&w->mr, NULL, &dm32_unimp_ops, w, name, size);
    /* The priority create_unimplemented_device() uses. */
    memory_region_add_subregion_overlap(sysmem, base, &w->mr, -1000);
}

/*
 * Background region under the whole 32-bit address space, below every other
 * region. It stands in for QEMU's unassigned-memory handling, which board
 * code cannot hook: a read returns 0 and a write is dropped, the access ends
 * in a decode error (which the C-SKY target ignores, as before), and it is
 * logged under -d invalid_mem in the format memory_region_access_valid()
 * uses. Each guest access is also a DM32_EV_UNASSIGNED event.
 */
static MemTxResult dm32_unassigned_read(void *opaque, hwaddr addr,
                                        uint64_t *data, unsigned size,
                                        MemTxAttrs attrs)
{
    *data = 0;
    if (!attrs.debug) {
        qemu_log_mask(LOG_INVALID_MEM, "Invalid read at addr 0x%"
                      HWADDR_PRIX ", size %u, region 'dm32.unassigned', "
                      "reason: rejected\n", addr, size);
        dm32_event(DM32_EV_UNASSIGNED, "read at 0x%08" HWADDR_PRIx
                   ", size %u", addr, size);
    }
    return MEMTX_DECODE_ERROR;
}

static MemTxResult dm32_unassigned_write(void *opaque, hwaddr addr,
                                         uint64_t value, unsigned size,
                                         MemTxAttrs attrs)
{
    if (!attrs.debug) {
        qemu_log_mask(LOG_INVALID_MEM, "Invalid write at addr 0x%"
                      HWADDR_PRIX ", size %u, region 'dm32.unassigned', "
                      "reason: rejected\n", addr, size);
        dm32_event(DM32_EV_UNASSIGNED, "write at 0x%08" HWADDR_PRIx
                   ", size %u, value 0x%" PRIx64, addr, size, value);
    }
    return MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps dm32_unassigned_ops = {
    .read_with_attrs = dm32_unassigned_read,
    .write_with_attrs = dm32_unassigned_write,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .valid.unaligned = true,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void dm32_add_unassigned(MemoryRegion *sysmem)
{
    MemoryRegion *r = g_new(MemoryRegion, 1);

    memory_region_init_io(r, NULL, &dm32_unassigned_ops, NULL,
                          "dm32.unassigned", 0x100000000ULL);
    memory_region_add_subregion_overlap(sysmem, 0, r, -2000);
}

/*
 * The 1 MB program flash at 0x03000000 is the GD25Q80, executed in place
 * through the LCSFC controller. On the real radio, a CPU store into this
 * range does not modify the flash, so the model must not accept one. It is
 * a ROM device: reads come directly from the backing RAM ("romd" mode, so
 * the XIP fetch path stays fast), and writes go through the ops handler
 * below and are dropped.
 *
 * Firmware images are loaded into the backing store by csky_load_kernel()'s
 * ROM blob, which writes the host backing directly and does not pass through
 * the write handler.
 *
 * DM32_FLASH_WRITABLE=1 restores the plain RAM behaviour. This is for
 * experiments only, because it makes the model accept stores that the real
 * part would ignore.
 *
 * The PC printed is CPUCSKYState.pc of current_cpu. The CK803S translator
 * only syncs env->pc at branches and helpers (gen_save_pc), so for
 * straight-line code it can lag the store that faulted. Treat it as a hint.
 * This QEMU version has no exact store PC without a retaddr.
 */
static uint64_t dm32_flash_dropped_writes;
static bool dm32_flash_writable_checked;
static bool dm32_flash_writable;

static void dm32_flash_report_at_exit(void)
{
    if (dm32_flash_dropped_writes > 20) {
        fprintf(stderr, "FLASH WRITE DROPPED total=%llu (first 20 printed)\n",
                (unsigned long long)dm32_flash_dropped_writes);
    }
}

static void dm32_flash_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    uint32_t pc = 0;

    if (current_cpu) {
        pc = CSKY_CPU(current_cpu)->env.pc;
    }
    dm32_flash_dropped_writes++;
    dm32_event(DM32_EV_FLASH_STORE, "addr 0x%08" HWADDR_PRIx " size %u "
               "val 0x%llx", 0x03000000 + addr, size, (unsigned long long)val);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "dm32.flash: dropped guest write addr=0x%08"
                  HWADDR_PRIx " size=%u val=0x%llx pc~0x%08x\n",
                  0x03000000 + addr, size, (unsigned long long)val, pc);
    if (dm32_flash_dropped_writes <= 20) {
        fprintf(stderr, "FLASH WRITE DROPPED addr=0x%08" HWADDR_PRIx
                " size=%u val=0x%llx pc~0x%08x\n",
                0x03000000 + addr, size, (unsigned long long)val, pc);
        if (dm32_flash_dropped_writes == 20) {
            fprintf(stderr, "FLASH WRITE DROPPED: further messages "
                    "suppressed; count at exit\n");
        }
    }
}

static uint64_t dm32_flash_read(void *opaque, hwaddr addr, unsigned size)
{
    /*
     * This is never reached while romd mode is on, which is the default.
     * Reads are served directly from the backing RAM. The handler is kept
     * only so that a romd-to-MMIO switch cannot dereference a NULL read op.
     */
    return 0xffffffffu;
}

static const MemoryRegionOps dm32_flash_ops = {
    .read = dm32_flash_read,
    .write = dm32_flash_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void dm32_add_flash(MemoryRegion *sysmem)
{
    MemoryRegion *r = g_new(MemoryRegion, 1);

    if (!dm32_flash_writable_checked) {
        dm32_flash_writable = dm32_env_flag("DM32_FLASH_WRITABLE");
        dm32_flash_writable_checked = true;
        if (!dm32_flash_writable) {
            atexit(dm32_flash_report_at_exit);
        }
    }
    if (dm32_flash_writable) {
        memory_region_init_ram(r, NULL, "dm32.flash", 0x00100000,
                               &error_fatal);
    } else {
        memory_region_init_rom_device(r, NULL, &dm32_flash_ops, NULL,
                                      "dm32.flash", 0x00100000, &error_fatal);
    }
    memory_region_add_subregion(sysmem, 0x03000000, r);
}

/* Add a plain RAM region (used for RAM and for MMIO-window stubs). */
static MemoryRegion *dm32_add_ram(MemoryRegion *sysmem, const char *name,
                                  hwaddr base, uint64_t size)
{
    MemoryRegion *r = g_new(MemoryRegion, 1);

    memory_region_init_ram(r, NULL, name, size, &error_fatal);
    memory_region_add_subregion(sysmem, base, r);
    return r;
}

/*
 * Power-on contents of IRAM and SRAM (DM32_RAM_FILL).
 *
 * QEMU zero-fills RAM. On the radio, SRAM the firmware never initialises
 * holds power-on noise: a live capture of 0x18004c00-0x18007fff has 13,277
 * of its 13,312 bytes non-zero (dm32uv-re docs/live-ram-capture.md). A patch
 * that relies on zeroed RAM therefore works here and fails on hardware.
 *
 * DM32_RAM_FILL=noise fills IRAM and SRAM with pseudo-random bytes when the
 * board is created. -device loader images are written later, at reset, so
 * they overwrite their own ranges and only the bytes they do not cover keep
 * the noise. A system reset does not refill RAM, as a warm reset on the radio
 * does not clear it. The bytes come from splitmix64, so a seed gives the same
 * contents on every run: noise uses DM32_RAM_FILL_SEED, noise:<seed> another
 * seed. Assumption: uniform random bytes; the captured noise's distribution
 * and its dependence on the unit and on power-off time are not modelled.
 * zero, or unset, keeps QEMU's zero fill.
 */
#define DM32_RAM_FILL_SEED 0x444d3332u    /* "DM32" */

static bool dm32_ram_noise;
static uint64_t dm32_ram_seed;

static void dm32_ram_fill_parse(void)
{
    const char *e = getenv("DM32_RAM_FILL");

    if (!e || !*e || !strcmp(e, "zero")) {
        return;
    }
    if (!strcmp(e, "noise")) {
        dm32_ram_noise = true;
        dm32_ram_seed = DM32_RAM_FILL_SEED;
        return;
    }
    if (!strncmp(e, "noise:", 6) &&
        qemu_strtou64(e + 6, NULL, 0, &dm32_ram_seed) == 0) {
        dm32_ram_noise = true;
        return;
    }
    error_report("DM32_RAM_FILL: '%s' is not zero, noise or noise:<seed>", e);
    exit(EXIT_FAILURE);
}

static uint64_t dm32_splitmix64(uint64_t *state)
{
    uint64_t z = (*state += 0x9e3779b97f4a7c15ull);

    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

/* Fill @r with noise. @base keeps regions on one seed from repeating. */
static void dm32_ram_fill(MemoryRegion *r, hwaddr base)
{
    uint8_t *p = memory_region_get_ram_ptr(r);
    uint64_t size = memory_region_size(r);
    uint64_t state = dm32_ram_seed ^ base;
    uint64_t i;

    if (!dm32_ram_noise) {
        return;
    }
    for (i = 0; i < size; i += 8) {
        uint64_t v = dm32_splitmix64(&state);

        memcpy(p + i, &v, MIN(8, size - i));
    }
}

static void dm32_init(MachineState *machine)
{
    Object *cpuobj;
    CSKYCPU *cpu;
    CPUCSKYState *env;
    qemu_irq *cpu_intc;
    qemu_irq intc[64];
    DeviceState *dev;
    DeviceState *gpioa;
    int i;
    MemoryRegion *sysmem = get_system_memory();

    dm32_events_init();
    dm32_ram_fill_parse();

    cpuobj = object_new(machine->cpu_type);
    object_property_set_bool(cpuobj, "realized", true, &error_fatal);
    cpu = CSKY_CPU(cpuobj);
    dm32_cpu = cpu;
    env = &cpu->env;

    /*
     * The DM-32 core is a CK803S with the DSP extension. The firmware
     * executes DSP ops such as `muls` (for example at IRAM 0x250e0 in the
     * audio path). QEMU's stock ck803 model omits ABIV2_DSP, so the
     * translator raises EXCP_UDEF (vector 4), and the firmware halts in the
     * crt0 exception stub at 0x0300c086. The DSP feature bits are granted
     * here. They are additive, since they only gate extra instruction
     * groups.
     */
    env->features |= ABIV2_DSP | ABIV2_DSP2;

    /* Memories. */
    /* Vectors and the GPIO shadow. */
    dm32_add_ram(sysmem, "dm32.lowram", 0x00000000, 0x00010000);
    /* IRAM, 288 KB. */
    dm32_ram_fill(dm32_add_ram(sysmem, "dm32.iram", 0x00010000, 0x00048000),
                  0x00010000);
    /* 1 MB program flash (GD25Q80, XIP, read-only). */
    dm32_add_flash(sysmem);
    /* SRAM, 32 KB. */
    dm32_ram_fill(dm32_add_ram(sysmem, "dm32.sram", 0x18000000, 0x00008000),
                  0x18000000);

    /*
     * The modem and clock registers at 0x11000000 (dm32_sysreg) are created
     * below, after the PIC, because they need the PIC's intc[] lines.
     */

    /*
     * The serial controller at 0x04000000 is used only by IRAM 0x2bb98,
     * which initialises it through 0x2bb28 and sends the GD25Q80 quad-enable
     * sequence (35h, 06h, 01h 00h 02h, 05h). By that structural evidence it
     * is the program flash's raw SPI port (dm32uv-re specs/rdl/gd25q80.rdl).
     * Its STATUS register (+0x28) always reads TX-ready, so the busy-waits
     * complete. Its pads are the program flash's dedicated NFC pads, with no
     * IO_DIPLEX select (v2.7 EN pin table), so it is not pad-gated. Until it
     * has its own GD25Q80 model, the sequence runs against the same SPI-NOR
     * model as the data chip.
     */
    dm32_ssi_create(0x04000000, NULL);

    /*
     * Interrupt controller. The DM-32 uses the external PIC at 0x17000000
     * (HR_C7000 v2.7 section 4.7). The IRQ dispatcher at IRAM 0x1064a
     * confirms this: every vector from 32 upward points to it. It saves the
     * context, calls OSIntEnter, writes the PIC EOI (st.w 4,(0x17000010)),
     * extracts the vector from PSR >> 16, calls the ISR from
     * table[0x45514 + (vec-32)*4], calls OSIntExit, and executes rte. The
     * PIC drives cpu_intc[0]. The RTOS tick arrives as a PIC source, from
     * the DesignWare timer wired below. The tick is neither the CK803 CORET
     * (0xE000E000) nor csky_coret, because the firmware never touches those.
     * With the wiring below, the stock firmware boots to the standby screen.
     */
    cpu_intc = csky_vic_v1_init_cpu(env);
    if (env->cpu_freq != 0) {
        dm32_binfo.freq = env->cpu_freq;
    }
    dev = sysbus_create_simple("dm32_pic", 0x17000000, cpu_intc[0]);
    for (i = 0; i < 64; i++) {
        intc[i] = qdev_get_gpio_in(dev, i);
    }

    /*
     * GPIO ports A, B and C are created after the PIC, so that their
     * interrupt lines can be wired. The creation order A, B, C is kept
     * because dm32_gpio.c indexes its banks by creation order. Each bank has
     * its own DesignWare port-A interrupt block and PIC source: GPIOA is
     * source 15, GPIOB is source 14, and GPIOC is source 17
     * (dm32uv-re data/registers/HR_C7000-DM32.h, PIC_PIC_*_GPIOA/B/C; ISR
     * vectors 0x2f, 0x2e and 0x31 per FUN_0301d50c).
     *
     * dm32_gpio returns idle-high (0xffffffff) for the +0x50 data register,
     * so that no front-panel button reads as pressed. This makes the
     * boot-ROM mode check take the normal-boot branch. The check reads flash
     * 0x030015b0: pin 43 is bank B bit 11, and pin 51 is bank B bit 19. The
     * normal-boot branch jumps to 0x0300c000. The other branch drops into the
     * serial recovery console at IRAM 0x12614.
     */
    /* GPIOA. */
    gpioa = sysbus_create_varargs("dm32_gpio", 0x14020000, intc[15], NULL);
    /* GPIOB (boot-mode pins 43 and 51). */
    sysbus_create_varargs("dm32_gpio", 0x14100000, intc[14], NULL);
    /* GPIOC. */
    sysbus_create_varargs("dm32_gpio", 0x14110000, intc[17], NULL);
    /*
     * The FD6818B RF transceiver has a 3-wire bus: CLK on GPIOA bit 7, DATA
     * on bit 8, and CS on bit 24. It attaches to those pads.
     */
    dm32_fd6818_init();

    /*
     * Two bit-banged I2C buses on GPIOA (dm32uv-re specs/dts/dm32uv.dts):
     * "alpu", SCL GPIOA 5 and SDA GPIOA 6, for the ALPU-MP at 7-bit 0x3d
     * (stock i2c_read_block_0x7a @0x000144fc); and "fm", SCL GPIOA 26 and
     * SDA GPIOA 25, for the RDA5807M at 0x10 (rda5807m_i2c_send_byte
     * @0x03065548), modelled in dm32_rda5807m.c. No ALPU-MP is modelled.
     * Other slaves can be added with -device <type>,bus=alpu or bus=fm.
     */
    dm32_gpio_i2c_create(gpioa, "alpu", DM32_PTA(5), DM32_PTA(6));
    dm32_rda5807m_attach(dm32_gpio_i2c_create(gpioa, "fm", DM32_PTA(26),
                                              DM32_PTA(25)));

    /*
     * Modem and clock registers at 0x11000000 (dm32_sysreg): a PLL-lock stub
     * and a DMR L2 slot-timing model. Its TX-slot interrupt output goes to
     * PIC source 30, which is vector 62 (firmware Time_slot_inter_tx). The
     * ISR-table index is vec - 32 = 30, which is where the firmware installed
     * ISR 0x2c42c.
     */
    sysbus_create_varargs("dm32_sysreg", 0x11000000, intc[30], NULL);

    /*
     * The DesignWare APB timer at 0x14000000 drives PIC hardware line 1,
     * which is the RTOS tick. The vector arithmetic, confirmed from flash, is
     * as follows. The firmware installs the tick ISR with
     * register_isr(handler, logical=33) (flash 0x0306cf70 and 0x0306cf38).
     * That stores it at ISR-table[0x45514 + (33-32)*4], which is index 1. The
     * IRQ dispatcher (IRAM 0x1064a) computes the ISR index as (PSR_vec - 32)
     * and calls table[index], so the tick needs PSR_vec = 33. The PIC encodes
     * the CPU vector as pic_source + 32, so pic_source = 1 gives 33. The
     * firmware's 33 is a logical source ID, and PIC hardware line 1 is what
     * carries it. Wiring the timer to line 33 would give PSR_vec 65, which
     * has no vector-table entry, so no ISR would run.
     *
     * Timer channel 0 is on PIC source 1. The project SVD
     * (dm32uv-re data/registers/HR_C7000-DM32.svd) says the TIMER is routed to
     * PIC source 33. Hardware testing contradicts that. With only source 33
     * unmasked, the timer expired and held its line, the PIC pending registers
     * stayed empty, and no ISR ran. With source 1 unmasked, the ISR fires. The
     * SVD note is wrong on this point.
     *
     * Timer channels 1 to 5 are on PIC sources 2 to 6. TIMER_n is PIC source
     * n, with vector n + 32. HR_C7000 v2.7 EN Table 30 lists vector numbers,
     * not source numbers, so the vector for a source is that source + 32.
     *
     * Stock init enables channel 5 as its UI tick (timer_ch4_enable at
     * 0x0301d784, through timer_channel_enable_dispatch(5)). With only four
     * channels modelled, those writes were dropped, the UI task's flag was
     * never posted, and the keypad was never scanned. All six channels are
     * modelled, on sources 1 to 6.
     */
    sysbus_create_varargs("dm32_timer", 0x14000000,
                          intc[1], intc[2], intc[3],
                          intc[4], intc[5], intc[6], NULL);

    /* LCD (ST7789V-class) at 0x12000000. */
    sysbus_create_simple("dm32_lcd", 0x12000000, NULL);

    /*
     * UART bases and PIC sources come from
     * dm32uv-re data/registers/HR_C7000-DM32.h: UART0 at 0x14030000 is
     * source 12, UART1 at 0x14040000 is source 10, UART2 at 0x14050000 is
     * source 11, and UART3 at 0x14090000 is source 13. They are cross-checked
     * against the <interrupt> values for the same peripherals in
     * dm32uv-re data/registers/HR_C7000-DM32.svd. serial_hd(n) is the nth
     * -serial chardev, so the -serial argument order selects the backend:
     * serial_hd(0) is UART0, serial_hd(1) is UART1, and so on. A NULL chardev
     * (no -serial for that index) is accepted, and the UART then drops TX
     * bytes.
     */
    /* UART0. */
    csky_uart_create(0x14030000, intc[12], serial_hd(0));
    /* UART1 (the GPS port on stock). */
    dm32_uart_gate_create(sysmem, 1, 0x14040000,
                          csky_uart_create(0x14040000, intc[10],
                                           serial_hd(1)), DM32_PTA(10));
    /* UART2. */
    dm32_uart_gate_create(sysmem, 2, 0x14050000,
                          csky_uart_create(0x14050000, intc[11],
                                           serial_hd(2)), DM32_PTA(12));
    /* UART3. */
    dm32_uart_gate_create(sysmem, 3, 0x14090000,
                          csky_uart_create(0x14090000, intc[13],
                                           serial_hd(3)), DM32_PTA(14));

    /*
     * Watchdog at 0x14010000: a counting watchdog that bites, which resets the
     * guest, on expiry. See hw/misc/dm32_wdt.c.
     */
    sysbus_create_simple("dm32_wdt", 0x14010000, NULL);

    /*
     * Remaining SoC peripheral windows that the firmware brings up. The bases
     * come from reverse engineering of the stock firmware. Each is mapped as a
     * RAM stub, so that register reads and writes do not silently hit unmapped
     * space. A window is refined to a real device if a specific one gates
     * boot. Each window is 64 KB.
     */
    /*
     * I2C controllers, each with its own QEMU I2C bus "i2c<n>" and its PIC
     * source (Table 30). I2C0 (0x14060000) and I2C1 (0x14070000) have nothing
     * on their buses, so every address NAKs. Stock never uses either: the
     * ALPU-MP on the I2C0 pads is bit-banged through GPIOA 5 and 6, and the
     * I2C1 pads carry the FD6818's CLK and DATA. I2C2 (0x14080000) is the
     * stock firmware's RTC bus (i2c2_master_read_reg at 0x0301dc1c, a polled
     * transfer with a 0xFFFFF per-byte limit); the RTC slave is
     * dm32_i2c_rtc.c, its alarm on PIC source 19 (Table 30). Without it,
     * every RTC read spins to that limit.
     */
    dm32_i2c_create(0, DM32_SVD_I2C0_BASE, intc[DM32_SVD_I2C0_IRQ]);
    dm32_i2c_create(1, DM32_SVD_I2C1_BASE, intc[DM32_SVD_I2C1_IRQ]);
    dm32_i2c_rtc_attach(dm32_i2c_create(2, DM32_SVD_I2C2_BASE,
                                        intc[DM32_SVD_I2C2_IRQ]), intc[19]);
    /*
     * ADC: dm32_adc models the datasheet state machine. Inputs come from
     * DM32_ADC_CH0 to DM32_ADC_CH7, and an unset input reads 0. The power-sense
     * line on GPIO is idle-low in dm32_gpio.c. It, not the ADC, is the source
     * of the 'Voltage' park. SYS_SOFT_RSTN[6] (adc_ctrl_soft_rstn, v2.7 EN
     * section 4.1.4.1) resets it; stock pulses that bit at boot.
     */
    dm32_sysreg_soft_reset_target(6, sysbus_create_simple("dm32_adc",
                                                           0x140d0000, NULL));
    /*
     * DMR baseband RAM window (TX and RX Modem_Buffer, codec registers at
     * +0x9c0). This is a capture-only model: plain RAM plus observability
     * (DM32_DMR_LOG, DM32_DMR_TRACE, DM32_DMR_DUMP). See
     * hw/misc/dm32_dmrram.c.
     */
    sysbus_create_simple("dm32_dmrram", 0x16000000, NULL);
    /*
     * The codec's registers (dm32_codec.c), 64 bytes at +0x9c0 of that
     * window, mapped over it. Interrupt: PIC source 37 (Table 30). Its
     * SYS_SOFT_RSTN bit is 4 (section 4.1.4.1).
     */
    {
        DeviceState *codec = qdev_new("dm32_codec");

        sysbus_realize_and_unref(SYS_BUS_DEVICE(codec), &error_fatal);
        sysbus_mmio_map_overlap(SYS_BUS_DEVICE(codec), 0,
                                DM32_SVD_CODEC_BASE, 1);
        sysbus_connect_irq(SYS_BUS_DEVICE(codec), 0,
                           intc[DM32_SVD_CODEC_IRQ]);
        dm32_sysreg_soft_reset_target(4, codec);
    }
    /*
     * DAC at 0x140f0000 is dm32_dac, the CPU-side 12-bit DAC with channels A
     * to C (DS 5.10). Stock drives channel 2 (DATA_B) for the TX power ramp,
     * through FUN_0002b364.
     */
    sysbus_create_simple("dm32_dac", 0x140f0000, NULL);
    /*
     * PWM at 0x140c0000 is dm32_pwm. It has three channels with config, period
     * and phase storage, plus decoded logging. It does no waveform generation
     * and wires no IRQ. Channel 0 is thought to drive the LCD backlight (see
     * hw/misc/dm32_pwm.c).
     */
    sysbus_create_simple("dm32_pwm", 0x140c0000, NULL);
    /*
     * SSI and SPI controllers. The SSI driver at IRAM 0x2bfc4 polls at
     * 0x2bf90. It indexes a base table at flash 0x030ada00, which is
     * { 0x140a0000, 0x140b0000, 0x140e0000, 0x14150000, 0x04000000, ... }.
     * Each entry is the same DesignWare-SSI-style block, modelled with
     * dm32_ssi; the driver polls SR (+0x28) for TFNF, TFE and BUSY around
     * each byte. The 0x04000000 entry is created above.
     */
    dm32_ssi_create(0x140a0000, intc[16]);  /* SSI0 (table[0]), Table 30 */
    dm32_ssi_create(0x140b0000, intc[22]);  /* SSI1 (table[1]) */
    dm32_ssi_create(0x140e0000, intc[23]);  /* SSI2 (table[2]) */
    dm32_ssi_create(0x14150000, NULL);      /* SSI3 (table[3]), no source */

    /*
     * Addresses outside every region fall to the background region, which
     * sits below everything else. Table 26 lists no reserved ranges and does
     * not say what the silicon does on such an access (a bus error, a 0 read
     * or a hang). Assumption: a read returns 0 and a write is dropped, as
     * QEMU's unassigned memory does.
     */
    dm32_add_unassigned(sysmem);

    /*
     * Every peripheral block in the datasheet's address map (HR_C7000 v2.7
     * section 4.5.3, Table 26, which lists the same blocks as the SVD) gets
     * an unimplemented-device window named after it, under any model of that
     * block. An access inside a block that the model does not cover, such
     * as a GPIO bank's offsets past its 4 KB model window, or I2C1, USB, SDIO
     * and the SPI slaves, which have no model, is logged under -d unimp as
     * an unimp event and reads 0. Table 26 lists no reserved ranges, so an
     * address between blocks falls to the background region and is an
     * unassigned access. The CODEC block lies inside Modem_Buffer, whose
     * model maps the whole 64 KB, and dm32_codec sits over it. Table 26
     * also gives the LCSFC window 0x03000000-0x03ffffff; the 1 MB
     * program-flash image covers its start.
     *
     * Model windows are mapped at priority 0 and these at -1000, so a model
     * always wins where both exist.
     */
    /*
     * SPI slaves 0-2 (dm32_spi_slave.c), PIC sources 34-36 (Table 30). No
     * device on the board can act as their master.
     */
    sysbus_create_simple("dm32_spi_slave", DM32_SVD_SPI_SLAVE0_BASE,
                         intc[DM32_SVD_SPI_SLAVE0_IRQ]);
    sysbus_create_simple("dm32_spi_slave", DM32_SVD_SPI_SLAVE1_BASE,
                         intc[DM32_SVD_SPI_SLAVE1_IRQ]);
    sysbus_create_simple("dm32_spi_slave", DM32_SVD_SPI_SLAVE2_BASE,
                         intc[DM32_SVD_SPI_SLAVE2_IRQ]);

#define DM32_UNIMP_BLOCK(name, base, size) \
    dm32_add_unimp(sysmem, g_ascii_strdown("dm32." #name, -1), base, size);
    DM32_SVD_PERIPHERALS(DM32_UNIMP_BLOCK)
#undef DM32_UNIMP_BLOCK
    dm32_add_unimp(sysmem, "dm32.lcsfc", 0x03000000, 0x01000000);

    /*
     * CK803 cache controller at 0xe000f000 (dm32_cache.c). Flash crt0 at
     * 0x0300c00c invalidates all (CIR = 1), selects write-back (CER = 4),
     * sets cacheable region 0 (CRCR0) and then enables the cache (CER |= 1).
     */
    sysbus_create_simple("dm32_cache", 0xe000f000, NULL);

    /* Boot the supplied flash image at 0x03000000. */
    dm32_binfo.ram_size = machine->ram_size;
    dm32_binfo.kernel_filename = machine->kernel_filename;
    dm32_binfo.kernel_cmdline = machine->kernel_cmdline;
    dm32_binfo.initrd_filename = machine->initrd_filename;
    csky_load_kernel(cpu, &dm32_binfo);

    /*
     * Force the true entry (0x03001000) at every reset, after boot.c's
     * default reset.
     */
    qemu_register_reset(dm32_cpu_reset, cpu);
    env->pc = DM32_ENTRY;
}

static void dm32_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Baofeng DM-32UV (HR_C7000 / CK803S)";
    mc->init = dm32_init;
    mc->default_cpu_type = CSKY_CPU_TYPE_NAME("ck803");
}

static const TypeInfo dm32_type = {
    .name = MACHINE_TYPE_NAME("dm32"),
    .parent = TYPE_MACHINE,
    .class_init = dm32_class_init,
};

static void dm32_machine_init(void)
{
    type_register_static(&dm32_type);
}

type_init(dm32_machine_init)
