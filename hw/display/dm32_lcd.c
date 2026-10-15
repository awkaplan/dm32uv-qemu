/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Baofeng DM-32UV LCD controller (ST7789V-class, 8080 bus): QEMU device model
 *
 * Copyright (c) 2026 Alex Kaplan
 *
 * The DM-32UV firmware maps the panel command register at 0x12000000 and the
 * data register at 0x12000004 (8-bit writes). This model decodes the MIPI-DCS
 * command stream the firmware emits (CASET 0x2A, RASET 0x2B, RAMWR 0x2C,
 * MADCTL 0x36) into an RGB565 framebuffer shown on a QEMU graphic console, so
 * display and UI patches can be checked visually under qemu-system-cskyv2.
 *
 * Pixel format: the firmware writes each 16-bit pixel as two data bytes, high
 * byte first (big-endian), RGB565. Window commands set the active rectangle;
 * RAMWR streams pixels into it, auto-advancing.
 *
 * Port note: this model targets modern QEMU. The older API created the console
 * with graphic_console_init() and flushed with a global dpy_gfx_update().
 * Modern QEMU creates the console with qemu_graphic_console_create(), whose
 * owner is a DeviceState. GraphicHwOps.gfx_update returns bool, and dirty
 * rectangles are flushed with qemu_console_update() from inside that
 * callback. The structure follows hw/display/tcx.c. See docs/port-notes.md.
 *
 * Not modelled: bus timing; reads from the panel path, which return 0 except
 * for +0x10 and +0x28; any effect of MADCTL on the displayed image; and the
 * panel's own reset input (no SoC reset is assumed to reach the panel).
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include <stdlib.h>
#include "hw/csky/dm32.h"

/*
 * Panel view size.
 *
 * With DM32_LCDLOG=1, the live firmware's only window is
 * LCD CASET 0..239 and LCD RASET 0..319, so RAMWR addresses
 * x[0..239] y[0..319].
 * That is a full 240x320 GRAM. MADCTL=0xC0 (MY=1 MX=1 MV=0) mirrors both axes
 * with no row and column exchange, so the panel is not transposed, and 240x320
 * is the portrait panel view directly.
 *
 * A 160x128 view would crop the 240x320 image, so gfx_update would show only a
 * corner of it.
 *
 * Override with DM32_LCD_W and DM32_LCD_H if an image drives a smaller panel.
 */
#define DM32_LCD_W 240
#define DM32_LCD_H 320

/*
 * Controller GRAM size in memory coordinates: 256 x 320, which covers the full
 * 240x320 panel the stock firmware addresses, with headroom.
 */
#define GRAM_W 256
#define GRAM_H 320   /* firmware RASETs up to 319 (240x320-class controller) */

#define TYPE_DM32_LCD "dm32_lcd"
OBJECT_DECLARE_SIMPLE_TYPE(DM32LcdState, DM32_LCD)

struct DM32LcdState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    QemuConsole *con;
    /*
     * Controller GRAM in memory coordinates, as addressed by CASET, RASET and
     * RAMWR. A pixel is stored in the controller's final, MADCTL-applied
     * orientation, so gfx_update reads fb straight through, offset only by
     * off_x and off_y. It does not apply MADCTL again; see the comment in
     * dm32_lcd_gfx_update().
     */
    uint16_t fb[GRAM_W * GRAM_H];           /* RGB565 GRAM, memory coords */
    /*
     * Last MADCTL (0x36) argument: bit 7 MY, bit 6 MX, bit 5 MV. It is decoded
     * and logged but not applied to the view; see gfx_update.
     */
    uint8_t  madctl;
    /* Panel origin offset within GRAM. */
    int      off_x, off_y;
    int      pw, ph;                        /* panel view size (240x320) */
    /* Command decode state. */
    uint8_t cmd;             /* last command byte */
    /*
     * Controller enable handshake. The firmware writes +0x10 (config) and sets
     * +0x28 bit0, then waits for +0x28 bit1. Reads of bit1 are set only after
     * bit0 has been written, and command and data writes are dropped until
     * then, as on hardware.
     */
    uint32_t cfg10;
    uint32_t ctrl28;
    int argn;                /* data-byte index within the current command */
    uint8_t args[8];
    /* Active window (inclusive). */
    uint16_t xs, xe, ys, ye;
    uint16_t cx, cy;         /* current pixel cursor during RAMWR */
    uint8_t pix_hi;
    int pix_have_hi;
    int invalidate;
    /* Quiescent GRAM dump (DM32_GRAMDUMP). */
    /* Virtual-clock time of the most recent RAMWR pixel. */
    int64_t  last_px_ns;
    int      gram_dirty;     /* pixels written since the last dump */
    QEMUTimer *quiesce_timer;
    int64_t  quiesce_ns;     /* how long drawing must be idle before we dump */
    const char *dump_path;
    int      dump_seq;
    /* Read once at realize: DM32_LCDLOG and DM32_GRAMSEQ. */
    bool     lcdlog;
    bool     gramseq;
    /* DM32_LCDLOG: RAMWR window tracking, see dm32_lcd_log_ramwr(). */
    int      lg_nlog;
    unsigned long lg_wpix;
    uint16_t lg_lxs, lg_lxe, lg_lys, lg_lye;
    /* DM32_LCDLOG: command-path counters and the command-byte histogram. */
    unsigned long log_n_cmd, log_n_dat, log_n_oth;
    int      log_n_mad, log_n_win, log_n_ctl;
    unsigned long log_hist[256];
    /* Warn-once flags for the two panel-path warnings. */
    int      warned_disabled, warned_mux;
};

/* ST7789V-class and MIPI-DCS opcodes used by the model. */
enum {
    CMD_CASET = 0x2A,
    CMD_RASET = 0x2B,
    CMD_RAMWR = 0x2C,
    CMD_MADCTL = 0x36,
};

/*
 * DM32_LCDLOG: report each RAMWR window when its first pixel arrives, with the
 * pixel count of the window before it. Called for every RAMWR pixel while
 * logging is on.
 */
static void dm32_lcd_log_ramwr(DM32LcdState *s, uint16_t px)
{
    if (s->cx == s->xs && s->cy == s->ys) {
        if (s->lg_wpix && s->lg_nlog < 200) {
            unsigned exp = (unsigned)(s->lg_lxe - s->lg_lxs + 1) *
                           (s->lg_lye - s->lg_lys + 1);

            fprintf(stderr,
                    "  ^prev win x[%u..%u] y[%u..%u] "
                    "got %lu px (expected %u)%s\n",
                    s->lg_lxs, s->lg_lxe, s->lg_lys, s->lg_lye, s->lg_wpix,
                    exp, s->lg_wpix == exp ? "" : "  <<< MISMATCH");
        }
        if (s->lg_nlog++ < 200) {
            fprintf(stderr,
                    "RAMWR win x[%u..%u] y[%u..%u] "
                    "first_px=0x%04x\n",
                    s->xs, s->xe, s->ys, s->ye, px);
        }
        s->lg_wpix = 0;
        s->lg_lxs = s->xs;
        s->lg_lxe = s->xe;
        s->lg_lys = s->ys;
        s->lg_lye = s->ye;
    }
    s->lg_wpix++;
}

static void dm32_lcd_data(DM32LcdState *s, uint8_t b)
{
    switch (s->cmd) {
    case CMD_CASET: /* x0_hi x0_lo x1_hi x1_lo */
        /*
         * Each coordinate is latched as its bytes arrive, as the real
         * controller does, rather than only once all four are in. The
         * firmware's display_set_cursor_pos (0x03017978), used by
         * display_plot_pixel for every glyph pixel, sends CASET and RASET
         * with only two data bytes each (the start coordinate) and then
         * RAMWR. Waiting for a fourth byte would ignore those cursor sets,
         * so every glyph pixel would land wherever the previous pixel's
         * auto-advance left the cursor, smearing each glyph column into a
         * horizontal streak.
         */
        if (s->argn < 4) {
            s->args[s->argn] = b;
        }
        if (s->argn == 1) {
            s->xs = (s->args[0] << 8) | s->args[1];
            s->cx = s->xs;
        } else if (s->argn == 3) {
            s->xe = (s->args[2] << 8) | s->args[3];
        }
        s->argn++;
        break;
    case CMD_RASET: /* same latch-as-they-arrive rule as CASET */
        if (s->argn < 4) {
            s->args[s->argn] = b;
        }
        if (s->argn == 1) {
            s->ys = (s->args[0] << 8) | s->args[1];
            s->cy = s->ys;
        } else if (s->argn == 3) {
            s->ye = (s->args[2] << 8) | s->args[3];
        }
        s->argn++;
        break;
    case CMD_RAMWR:
        /* Two bytes per pixel, high byte first. */
        if (!s->pix_have_hi) {
            s->pix_hi = b;
            s->pix_have_hi = 1;
        } else {
            uint16_t px = (s->pix_hi << 8) | b;
            s->pix_have_hi = 0;
            if (s->lcdlog) {
                dm32_lcd_log_ramwr(s, px);
            }
            if (s->cx < GRAM_W && s->cy < GRAM_H) {
                s->fb[s->cy * GRAM_W + s->cx] = px;
            }
            /* Advance within the CASET/RASET window. */
            if (s->cx >= s->xe || s->cx >= GRAM_W - 1) {
                s->cx = s->xs;
                s->cy = (s->cy >= s->ye) ? s->ys : s->cy + 1;
            } else {
                s->cx++;
            }
            s->invalidate = 1;
            /*
             * Mark the GRAM dirty and record when the last pixel landed. The
             * dump is taken by dm32_gram_quiesce_tick() once drawing has
             * stopped for a while.
             */
            s->last_px_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            s->gram_dirty = 1;
        }
        break;
    case CMD_MADCTL:
        if (s->argn == 0) {
            s->madctl = b;
            s->invalidate = 1;
        }
        s->argn++;
        break;
    default:
        /* Other config commands: swallow the arguments. */
        if (s->argn < 8) {
            s->args[s->argn] = b;
        }
        s->argn++;
        break;
    }
}

/*
 * Are any of the LCD data-bus pads DB0..DB4 (PTC7..PTC11) away from the LCD
 * function, select value 0 (IO_DIPLEX2 lcd_db0_sel..lcd_db4_sel, v2.7 EN
 * 4.4.4.6)? These pads are shared with the keypad rows. Each stock keypad scan
 * sets IO_DIPLEX2 |= 0xfe00 (rf_modem_mute_during_keypad_scan @0x03012e14),
 * and the stock poll clears bits 15..9 again after the scan (FUN_0301cad0
 * @0x0301cad0). The panel init clears them too (mask 0xfff80107). A patch
 * that scans the keypad and omits the restore leaves the panel unwritable on
 * hardware, with a white screen until reboot.
 */
static bool lcd_db_pads_lent(void)
{
    unsigned pad;

    for (pad = DM32_PTC(7); pad <= DM32_PTC(11); pad++) {
        if (dm32_pad_function(pad) != 0) {
            return true;
        }
    }
    return false;
}

static void dm32_lcd_write(void *opaque, hwaddr off, uint64_t val,
                           unsigned size)
{
    DM32LcdState *s = opaque;
    uint8_t b = val & 0xff;

    if (off == 0x0) {
        s->log_n_cmd++;
    } else if (off == 0x4) {
        s->log_n_dat++;
    } else {
        s->log_n_oth++;
    }
    if (s->lcdlog) {
        unsigned long tot = s->log_n_cmd + s->log_n_dat + s->log_n_oth;

        if (tot <= 8 || (tot % 5000) == 0) {
            fprintf(stderr,
                    "LCDW off=0x%lx val=0x%lx  totals cmd=%lu data=%lu "
                    "other=%lu\n",
                    (unsigned long)off, (unsigned long)val,
                    s->log_n_cmd, s->log_n_dat, s->log_n_oth);
        }
        /*
         * Orientation and window evidence: the MADCTL argument byte and
         * the completed CASET and RASET windows.
         */
        if (off == 0x0) {
            s->log_hist[b]++;
            if ((s->log_n_cmd % 20000) == 0) {
                int i;

                fprintf(stderr, "LCD CMDHIST @%lu:", s->log_n_cmd);
                for (i = 0; i < 256; i++) {
                    if (s->log_hist[i]) {
                        fprintf(stderr, " %02x=%lu", i, s->log_hist[i]);
                    }
                }
                fprintf(stderr, "\n");
            }
        }
        /*
         * Display-state commands: 0x28 DISPOFF, 0x29 DISPON, 0x3A COLMOD,
         * 0x20 and 0x21 INV, 0x10 and 0x11 SLPIN and SLPOUT.
         */
        if (off == 0x0 &&
            (b == 0x28 || b == 0x29 || b == 0x3a || b == 0x20 ||
             b == 0x21 || b == 0x10 || b == 0x11) &&
            s->log_n_ctl++ < 200) {
            fprintf(stderr, "LCD CMD 0x%02x (after %lu cmds)\n",
                    b, s->log_n_cmd);
        }
        if (off == 0x4 && s->cmd == 0x3a && s->argn == 0 &&
            s->log_n_ctl++ < 200) {
            fprintf(stderr, "LCD COLMOD arg=0x%02x\n", b);
        }
        if (off == 0x4 && s->cmd == CMD_MADCTL && s->argn == 0 &&
            s->log_n_mad++ < 200) {
            fprintf(stderr, "LCD MADCTL=0x%02x (MY=%d MX=%d MV=%d)\n",
                    b, (b >> 7) & 1, (b >> 6) & 1, (b >> 5) & 1);
        }
        if (off == 0x4 &&
            (s->cmd == CMD_CASET || s->cmd == CMD_RASET) &&
            s->argn == 3 && s->log_n_win++ < 400) {
            fprintf(stderr, "LCD %s window %u..%u\n",
                    s->cmd == CMD_CASET ? "CASET" : "RASET",
                    (s->args[0] << 8) | s->args[1],
                    (s->args[2] << 8) | b);
        }
    }

    if (off == 0x10) {
        s->cfg10 = val;
        return;
    }
    if (off == 0x28) {
        s->ctrl28 = val & 0x1;
        return;
    }
    if (!(s->ctrl28 & 0x1)) {
        /*
         * Controller not enabled: the real block drives nothing to the
         * panel.
         */
        dm32_event(DM32_EV_LCD_OFF, "write +0x%02x = 0x%02x before the "
                   "+0x28 enable", (unsigned)off, b);
        if (!s->warned_disabled++) {
            fprintf(stderr,
                    "dm32_lcd: panel write with controller DISABLED (no +0x28 "
                    "enable) -- dropped\n");
        }
        return;
    }
    if (off == 0x0) {
        /* 0x12000000 is the command register. */
        s->cmd = b;
        s->argn = 0;
        s->pix_have_hi = 0;
    } else if (off == 0x4) {
        /* 0x12000004 is the data register. */
        if (lcd_db_pads_lent()) {
            /*
             * DB0..DB4 are keypad GPIOs while IO_DIPLEX2[15:9] is set, so
             * the panel does not see this byte. Model it as all-ones (white
             * pixels) so that the mistake is visible.
             */
            dm32_event(DM32_EV_LCD_MUX, "DATA write 0x%02x while "
                       "IO_DIPLEX2[15:9] hands DB0-DB4 to the keypad", b);
            if (!s->warned_mux++) {
                fprintf(stderr,
                        "dm32_lcd: panel DATA write while IO_DIPLEX2[15:9] "
                        "hands DB0..DB4 to the keypad -- pixel data forced to "
                        "0xff (missing FUN_0301cad0 restore after a keypad "
                        "scan?)\n");
            }
            b = 0xff;
        }
        dm32_lcd_data(s, b);
    }
    /* Other offsets in the window are ignored. */
}

static uint64_t dm32_lcd_read(void *opaque, hwaddr off, unsigned size)
{
    DM32LcdState *s = opaque;

    /*
     * The 0x12000000 block is an LCD controller, not just the 8080 panel bus.
     * Panel init (flash 0x03017d66) writes control registers at +0x10 and
     * +0x28, then busy-waits on STATUS +0x28 bit1 (transfer and controller
     * ready), the same idiom as the SSI blocks. The read reports ready (0x3)
     * once bit0 has been written, so that the wait completes. Other reads
     * return 0; the panel data path is write-only in the traced code.
     */
    if (off == 0x28) {
        /*
         * Bit1 is ready, but only once the firmware has enabled the
         * controller.
         */
        return (s->ctrl28 & 0x1) ? 0x3 : 0x0;
    }
    if (off == 0x10) {
        return s->cfg10;
    }
    return 0;
}

static const MemoryRegionOps dm32_lcd_ops = {
    .read = dm32_lcd_read,
    .write = dm32_lcd_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/*
 * Quiescent GRAM dump.
 *
 * The dump is not taken at a fixed pixel count, and it does not keep the
 * fullest frame. The firmware repaints by first clearing a region and then
 * drawing into it, so a snapshot at an arbitrary pixel count can catch a
 * half-erased or half-drawn screen. A screen that is legitimately sparser
 * than an earlier one would never be kept by a "fullest frame" rule.
 *
 * Every RAMWR pixel stamps last_px_ns. This timer polls, and when the GRAM is
 * dirty and no pixel has arrived for DM32_GRAMQUIET ms (default 150 ms of
 * virtual guest time), the repaint has settled and the GRAM is dumped once.
 *
 * Environment:
 *   DM32_GRAMDUMP=<path>  enable; written at each quiescent point (overwritten
 *                         in place)
 *   DM32_GRAMQUIET=<ms>   idle threshold, default 150
 *   DM32_GRAMSEQ=1        also write <path>.NNN for each settled frame
 */
static void dm32_gram_quiesce_tick(void *opaque)
{
    DM32LcdState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (s->gram_dirty && (now - s->last_px_ns) >= s->quiesce_ns) {
        int i, cnt = 0;
        FILE *f;

        for (i = 0; i < GRAM_W * GRAM_H; i++) {
            if (s->fb[i]) {
                cnt++;
            }
        }
        f = fopen(s->dump_path, "wb");
        if (f) {
            fwrite(s->fb, 2, GRAM_W * GRAM_H, f);
            fclose(f);
        }
        if (s->gramseq) {
            char seqpath[1024];

            snprintf(seqpath, sizeof(seqpath), "%s.%03d", s->dump_path,
                     s->dump_seq);
            f = fopen(seqpath, "wb");
            if (f) {
                fwrite(s->fb, 2, GRAM_W * GRAM_H, f);
                fclose(f);
            }
        }
        fprintf(stderr, "GRAMDUMP[%d] settled: nonblack=%d -> %s\n",
                s->dump_seq, cnt, s->dump_path);
        s->dump_seq++;
        s->gram_dirty = 0;
    }
    /* Poll every 20 ms of virtual time. */
    timer_mod(s->quiesce_timer, now + 20 * 1000000LL);
}

/*
 * gfx_update returns bool, reporting whether it drew anything, and flushes
 * with qemu_console_update() (see the port note in the file header).
 */
static bool dm32_lcd_gfx_update(void *opaque)
{
    DM32LcdState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    uint32_t *dest = (uint32_t *)surface_data(surface);
    int x, y;

    if (!s->invalidate) {
        return false;
    }
    /*
     * MADCTL is not applied here. MADCTL configures how the controller maps
     * incoming RAMWR pixels into GRAM, so a pixel stored in fb is already in
     * the controller's final orientation. Applying MADCTL's MV, MX and MY bits
     * again would transform the image twice. With this firmware's MADCTL=0xC0
     * (MY=1 MX=1 MV=0) that is exactly a 180-degree rotation. Reading straight
     * through reproduces the GRAM bounding box. s->madctl is still decoded and
     * logged; it does not change the view.
     */
    for (y = 0; y < s->ph; y++) {
        for (x = 0; x < s->pw; x++) {
            int gx = x + s->off_x;
            int gy = y + s->off_y;
            uint16_t p = (gx >= 0 && gx < GRAM_W &&
                          gy >= 0 && gy < GRAM_H) ?
                         s->fb[gy * GRAM_W + gx] : 0;
            int r = ((p >> 11) & 0x1f) << 3;
            int g = ((p >> 5) & 0x3f) << 2;
            int b = (p & 0x1f) << 3;

            dest[y * s->pw + x] = rgb_to_pixel32(r, g, b);
        }
    }
    qemu_console_update(s->con, 0, 0, s->pw, s->ph);
    s->invalidate = 0;
    return true;
}

/*
 * Full-repaint request from the UI layer. A VNC client's FramebufferUpdate-
 * Request on attach would otherwise be dropped by the early return on
 * !s->invalidate in gfx_update.
 */
static void dm32_lcd_gfx_invalidate(void *opaque)
{
    DM32LcdState *s = opaque;

    s->invalidate = 1;
}

static const GraphicHwOps dm32_lcd_gfx_ops = {
    .gfx_update = dm32_lcd_gfx_update,
    .invalidate = dm32_lcd_gfx_invalidate,
};

static void dm32_lcd_realize(DeviceState *dev, Error **errp)
{
    DM32LcdState *s = DM32_LCD(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &dm32_lcd_ops, s,
                          TYPE_DM32_LCD, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    s->pw = dm32_env_uint("DM32_LCD_W", DM32_LCD_W, 1, 4096);
    s->ph = dm32_env_uint("DM32_LCD_H", DM32_LCD_H, 1, 4096);
    s->con = qemu_graphic_console_create(dev, 0, &dm32_lcd_gfx_ops, s);
    qemu_console_resize(s->con, s->pw, s->ph);
    s->xe = GRAM_W - 1;
    s->ye = GRAM_H - 1;
    s->madctl = 0;
    s->off_x = dm32_env_int("DM32_LCD_OFFX", 0, -4096, 4096);
    s->off_y = dm32_env_int("DM32_LCD_OFFY", 0, -4096, 4096);
    s->invalidate = 1;
    s->lcdlog = dm32_env_flag("DM32_LCDLOG");
    s->gramseq = dm32_env_flag("DM32_GRAMSEQ");

    /* The quiescent-dump timer is armed only when DM32_GRAMDUMP is set. */
    s->dump_path = getenv("DM32_GRAMDUMP");
    if (s->dump_path && *s->dump_path) {
        int ms = dm32_env_uint("DM32_GRAMQUIET", 150, 1, INT32_MAX);

        s->quiesce_ns = (int64_t)ms * 1000000LL;
        s->quiesce_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                        dm32_gram_quiesce_tick, s);
        timer_mod(s->quiesce_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 20 * 1000000LL);
        fprintf(stderr,
                "GRAMDUMP: quiescent mode, idle threshold %d ms -> %s\n",
                ms, s->dump_path);
    }
}

/*
 * Machine reset: the SoC's bus interface (+0x10 configuration, +0x28 enable
 * handshake) returns to 0, so command and data writes are dropped until the
 * firmware enables it again. The panel controller behind it is a separate
 * part, so GRAM, window, MADCTL and command state persist. Assumption: no
 * SoC reset reaches the panel; its own reset input is not modelled.
 *
 * The DM32_LCDLOG counters and the warn-once flags are per-instance state
 * and restart here.
 */
static void dm32_lcd_reset(DeviceState *dev)
{
    DM32LcdState *s = DM32_LCD(dev);

    s->cfg10 = 0;
    s->ctrl28 = 0;
    s->log_n_cmd = 0;
    s->log_n_dat = 0;
    s->log_n_oth = 0;
    s->log_n_mad = 0;
    s->log_n_win = 0;
    s->log_n_ctl = 0;
    memset(s->log_hist, 0, sizeof(s->log_hist));
    s->lg_nlog = 0;
    s->lg_wpix = 0;
    s->warned_disabled = 0;
    s->warned_mux = 0;
}

static void dm32_lcd_realize_class(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = dm32_lcd_realize;
    device_class_set_legacy_reset(dc, dm32_lcd_reset);
}

static const TypeInfo dm32_lcd_reg = {
    .name = TYPE_DM32_LCD,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DM32LcdState),
    .class_init = dm32_lcd_realize_class,
};

static void dm32_lcd_register_types(void)
{
    type_register_static(&dm32_lcd_reg);
}

type_init(dm32_lcd_register_types)
