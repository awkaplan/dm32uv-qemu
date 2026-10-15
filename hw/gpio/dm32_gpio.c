/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Baofeng DM-32UV GPIO controller (three DesignWare APB GPIO banks)
 *
 * Copyright (c) 2026 Alex Kaplan
 *
 * The banks sit at 0x14020000 (A), 0x14100000 (B) and 0x14110000 (C).
 *
 * The boot ROM's mode selector (flash gpio_read_pin @0x030015b0) reads the
 * port DATA register at offset +0x50 and tests one bit per pin: pins 0-31
 * map to bank A, 32-63 to bank B (bit = pin - 32), and 64 and above to bank
 * C. gpio_read_pin returns 1 when the bit is set. The stock boot decision
 * (flash 0x030017d0) reads pin 43 (bank B bit 11) and pin 51 (bank B bit
 * 19). If either is set, it performs the normal jsr 0x0300c000. Otherwise it
 * enters the serial recovery console (IRAM 0x12614) and waits for a UART
 * byte. The DM-32's front-panel buttons are active-low with pull-ups, so an
 * idle port (no button held) reads all ones (nothing pressed, normal boot).
 * Offsets with no register are read/write storage.
 *
 * EXT_PORTA (+0x50, read-only, v2.7 EN 5.7.5) is the pad level
 * (dm32_gpio_input_level): high, or the pad's SoC pull when IOMGR_REN enables
 * one (v2.7 EN 4.4.4.7-4.4.4.9), or low for a DM32_GPIO_IDLE_LOW pad; then
 * injected levels; a pin GPIO drives (SWPORTA_DDR set, pad on GPIO) carries
 * its SWPORTA_DR bit; attached devices pull pads low; the keypad injection
 * holds rows low. A pad whose input buffer is off (IOMGR_IE), or which
 * IO_DIPLEX gives to another block, reads 0 and cannot interrupt.
 * Assumptions: the datasheet gives no level for a pad without a pull (the
 * keys and boot-mode pins have board pull-ups, so it reads high), and does
 * not say what GPIO sees on a pad given to another block.
 *
 * Interrupt block: each bank models the Synopsys DesignWare APB GPIO port-A
 * interrupt registers (HR_C7000 DS 5.7.5; dm32uv-re
 * data/registers/HR_C7000-DM32.h GPIO_Type): INTEN +0x30, INTMASK +0x34,
 * INTTYPE_LEVEL +0x38, INT_POLARITY +0x3c, INTSTATUS +0x40 (RAW & ~INTMASK),
 * RAW +0x44, DEBOUNCE +0x48 (see dm32_gpio_debounce), PORTA_EOI +0x4c
 * (write-1-to-clear, edge bits only), EXT_PORTA +0x50 (input), INTR_BOTH_EDGE
 * +0x78 (per bit: either edge; otherwise the edge INT_POLARITY selects).
 * Each bank drives its own IRQ line (= INTSTATUS != 0). The board wires the
 * lines to PIC sources GPIOA=15, GPIOB=14 and GPIOC=17 (dm32.c).
 *
 * The keypad injection (DM32_KEY/KEYSEQ) is clocked by EXT_PORTA reads; the
 * rows it holds persist until the next read. The interrupt block is
 * re-evaluated on every injected change, every EXT_PORTA read, every pad-mux
 * change, the end of each debounce window, and every write to INTEN, INTMASK,
 * INTTYPE_LEVEL, INT_POLARITY, INTR_BOTH_EDGE, DEBOUNCE, SWPORTA_DDR and EOI.
 *
 * Not modelled: synchronisation to the bank clock, and interrupt timing below
 * QEMU's scheduling. The debounce clock is an assumption (the 32.768 kHz RTC
 * clock), as is the level of a pad with no pull.
 */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "qemu/cutils.h"
#include "qemu/host-utils.h"
#include "hw/i2c/bitbang_i2c.h"
#include "hw/csky/dm32.h"

#define TYPE_DM32_GPIO "dm32_gpio"
OBJECT_DECLARE_SIMPLE_TYPE(DM32GpioState, DM32_GPIO)

#define WIN 0x1000
/* EXT_PORTA, the port data input register */
#define DATA_OFF 0x50

struct DM32GpioState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t reg[WIN / 4];
    /* 0=A 1=B 2=C (creation order), -1 for an unexpected extra instance */
    int bank;
    /* this bank's IRQ line, wired to a PIC source in dm32.c */
    qemu_irq irq;
    /* RAW_INTSTATUS (+0x44), kept only for INTEN bits */
    uint32_t raw;
    /* last sampled input word, for edge detection */
    uint32_t last_in;
    /* last_in is valid */
    int primed;
    /* current level driven on irq */
    int irq_level;
    /* pins overridden by injection (DM32_PINSEQ, DM32_KNOB, DM32_PTT) */
    uint32_t force_mask;
    /* forced level for each bit in force_mask */
    uint32_t force_val;
    /* pads on their GPIO function, and pads with the input buffer on */
    uint32_t gpio_mask;
    uint32_t ie_mask;
    /* pads whose IOMGR_REN pull is on, by direction */
    uint32_t pull_up;
    uint32_t pull_down;
    Notifier pad_notifier;
    /* rows the keypad injection holds low, set on each EXT_PORTA read */
    uint32_t key_low;
    /* debounced input word seen by the interrupt block */
    uint32_t db_view;
    /* level a debounced change is settling to, and its timer */
    uint32_t db_cand;
    bool db_armed;
    QEMUTimer *db_timer;
};

/*
 * Trace switches, read once when the first bank is created. They are
 * presence switches: any value other than "" or "0" turns them on.
 */
static bool dm32_keylog;
static bool dm32_gpiolog;

static void dm32_gpio_log_parse(void)
{
    static bool parsed;

    if (parsed) {
        return;
    }
    parsed = true;
    dm32_keylog = dm32_env_flag("DM32_KEYLOG");
    dm32_gpiolog = dm32_env_flag("DM32_GPIOLOG");
}

/* Register offsets (dm32uv-re data/registers/HR_C7000-DM32.h, GPIO_Type). */
#define R_DDR        0x04   /* SWPORTA_DDR, DS 5.7.5.2: 1 = output */
#define R_INTEN      0x30
#define R_INTMASK    0x34
#define R_INTTYPE    0x38   /* 0 = level, 1 = edge */
#define R_INTPOL     0x3c   /* 0 = active-low/falling, 1 = active-high/rising */
#define R_INTSTATUS  0x40
#define R_RAWINTSTAT 0x44
#define R_DEBOUNCE   0x48
#define R_EOI        0x4c
#define R_BOTHEDGE   0x78

static DM32GpioState *dm32_gpio_bank[3];

/*
 * Keypress injection state.
 *
 * DM32_KEY="<colbank>:<colbit>:<rowbank>:<rowbit>"  (banks 0=A 1=B 2=C)
 *
 * The column is sampled when the row is read: the key's row reads low only
 * while its column pad is driven low then (dm32_key_column_low).
 */
static int dm32_key_cb = -2;
static int dm32_key_cbit;
static int dm32_key_rb;
static int dm32_key_rbit;

/*
 * Press and release.
 *
 * Holding a key down forever is not a realistic press, and OpenRTX's input
 * layer discards it: input.c only raises an event on a change in the key
 * bitmap, and input_getPressedNumber() uses __builtin_ctz(), which reports
 * only the lowest set bit. A permanently held key therefore produces one
 * event and then nothing.
 *
 * DM32_KEY_HOLD=N releases the key after N row reads (default: held forever,
 * N=0). A few thousand covers several full scan cycles: long enough to
 * debounce, short enough that the UI sees a release afterwards.
 */
static unsigned long dm32_key_reads;
static unsigned long dm32_key_hold;

/*
 * DM32_KEYSEQ="cb:cbit:rb:rbit:hold:gap;cb:cbit:rb:rbit:hold:gap;..."
 *
 * A scripted press sequence, so that one boot can navigate a menu.
 * DM32_KEY can only hold a single key. Each step forces the key for `hold`
 * row-bank data reads, then releases it for `gap` reads, then advances.
 * cb=-1 means an input outside the matrix scan (for example, the side keys
 * SK1 and SK2 on GPIOB bits 19 and 20, per dm32uv-re specs/dts/dm32uv.dts;
 * Up and Down are matrix keys): rb:rbit is forced low on every read while
 * the step is active, with no column condition.
 * Matrix steps (cb>=0) behave exactly like DM32_KEY. Up to 32 steps. Ignored
 * if DM32_KEY is set.
 */
#define DM32_KEYSEQ_MAX 32

static struct {
    int cb;
    int cbit;
    int rb;
    int rbit;
    unsigned long hold;
    unsigned long gap;
} dm32_seq[DM32_KEYSEQ_MAX];

static int dm32_seq_n = -1;
static int dm32_seq_i;
static unsigned long dm32_seq_reads;

static void dm32_seq_parse(void)
{
    const char *k;

    if (dm32_seq_n != -1) {
        return;
    }
    dm32_seq_n = 0;
    k = getenv("DM32_KEYSEQ");
    if (!k) {
        return;
    }
    while (*k && dm32_seq_n < DM32_KEYSEQ_MAX) {
        int cb;
        int cbit;
        int rb;
        int rbit;
        unsigned long hold;
        unsigned long gap;
        int used = 0;

        if (sscanf(k, "%d:%d:%d:%d:%lu:%lu%n",
                   &cb, &cbit, &rb, &rbit, &hold, &gap, &used) < 6 ||
            cb < -1 || cb > 2 || cbit < 0 || cbit > 31 ||
            rb < 0 || rb > 2 || rbit < 0 || rbit > 31) {
            error_report("DM32_KEYSEQ: step %d (\"%.40s\"): expected "
                         "cb:cbit:rb:rbit:hold:gap, cb -1 (dedicated pin) or "
                         "0-2, rb 0-2, bits 0-31", dm32_seq_n + 1, k);
            exit(1);
        }
        dm32_seq[dm32_seq_n].cb = cb;
        dm32_seq[dm32_seq_n].cbit = cbit;
        dm32_seq[dm32_seq_n].rb = rb;
        dm32_seq[dm32_seq_n].rbit = rbit;
        dm32_seq[dm32_seq_n].hold = hold;
        dm32_seq[dm32_seq_n].gap = gap;
        dm32_seq_n++;
        k += used;
        if (*k == ';') {
            k++;
        }
    }
    if (dm32_keylog) {
        fprintf(stderr, "KEYSEQ parsed %d steps\n", dm32_seq_n);
    }
}

/*
 * Called on every row-bank DATA read. Returns the step whose key must be
 * forced now, or -1.
 */
static int dm32_seq_step(void)
{
    unsigned long span;

    if (dm32_seq_n <= 0 || dm32_seq_i >= dm32_seq_n) {
        return -1;
    }
    span = dm32_seq[dm32_seq_i].hold + dm32_seq[dm32_seq_i].gap;
    if (dm32_seq_reads >= span) {
        dm32_seq_reads = 0;
        dm32_seq_i++;
        if (dm32_keylog) {
            fprintf(stderr, "KEYSEQ -> step %d\n", dm32_seq_i);
        }
        if (dm32_seq_i >= dm32_seq_n) {
            return -1;
        }
    }
    return (dm32_seq_reads < dm32_seq[dm32_seq_i].hold) ? dm32_seq_i : -1;
}

static void dm32_key_parse(void)
{
    const char *k;

    if (dm32_key_cb != -2) {
        /* already parsed */
        return;
    }
    k = getenv("DM32_KEY");
    dm32_key_cb = -1;
    if (k && *k) {
        int n = sscanf(k, "%d:%d:%d:%d", &dm32_key_cb, &dm32_key_cbit,
                       &dm32_key_rb, &dm32_key_rbit);

        if (n != 4 || dm32_key_cb < 0 || dm32_key_cb > 2 ||
            dm32_key_cbit < 0 || dm32_key_cbit > 31 ||
            dm32_key_rb < 0 || dm32_key_rb > 2 ||
            dm32_key_rbit < 0 || dm32_key_rbit > 31) {
            error_report("DM32_KEY=%s: expected colbank:colbit:rowbank:rowbit, "
                         "banks 0-2 (A-C), bits 0-31", k);
            exit(1);
        }
    }
    dm32_key_hold = dm32_env_uint("DM32_KEY_HOLD", 0, 0, ULONG_MAX);
    if (dm32_keylog) {
        fprintf(stderr,
                "KEYPARSE %s -> cb=%d cbit=%d rb=%d rbit=%d hold=%lu\n",
                k ? k : "(unset)", dm32_key_cb, dm32_key_cbit,
                dm32_key_rb, dm32_key_rbit, dm32_key_hold);
    }
}

/*
 * A matrix key closes row and column: its row reads low only while its
 * column pad is driven low, that is, SWPORTA_DDR set, SWPORTA_DR 0 and the
 * pad on its GPIO function, at the moment the row is read. A scan that
 * drives the column high, or leaves it an input, sees no key.
 */
static bool dm32_key_column_low(int cb, int cbit)
{
    bool driven;
    int level = dm32_gpio_output(cb * 32 + cbit, &driven);

    return driven && level == 0;
}

/*
 * Inputs that idle low.
 *
 * The idle-high default is right for the keypad, which has pull-ups and
 * switches to ground. It is wrong for GPIOA bit 16, which is stock pin 0x10,
 * "Power check OFF" in the HR_C7000 pin map. Stock's power-off watcher
 * (FUN_0301b990) posts flag 0x40000 whenever gpio_read_input_bit(0x10) reads
 * nonzero. The power-off task (upgrade_mode_enter_and_reset_task @0x0301b83c)
 * then clears the screen, draws the dialog panel and the "Low battery
 * voltage" string pair (0x030ba108) when its shutdown-reason flag is set,
 * drops the power-hold output (pin 0x11), and spins in
 * `while (!(flags & 0x10)) { adc_rssi_rolling_average_tick(); delay(100); }`
 * waiting for the switch to cut power. Under this model that is a permanent
 * park. The ADC conversions seen in that loop (about 34 Hz on channel 2) are
 * the rssi tick, and the ADC level does not affect the outcome.
 *
 * The power-sense line must therefore idle low ("switch on").
 * DM32_GPIO_IDLE_LOW="bank:bit,..." replaces the default (banks 0=A 1=B
 * 2=C). DM32_GPIO_IDLE_LOW=none clears it, which reproduces the park.
 */
static uint32_t dm32_idle_low[3] = { 1u << 16, 0, 0 };
static int dm32_idle_low_parsed;

static void dm32_idle_low_parse(void)
{
    const char *e;

    if (dm32_idle_low_parsed) {
        return;
    }
    dm32_idle_low_parsed = 1;
    e = getenv("DM32_GPIO_IDLE_LOW");
    if (!e || !*e) {
        return;
    }
    dm32_idle_low[0] = 0;
    dm32_idle_low[1] = 0;
    dm32_idle_low[2] = 0;
    if (!strcmp(e, "none")) {
        return;
    }
    while (*e) {
        const char *end;
        long bank;
        long bit;

        if (qemu_strtol(e, &end, 0, &bank) < 0 || *end != ':') {
            break;
        }
        /* a bit that does not parse reads as 0, as it always has */
        qemu_strtol(end + 1, &end, 0, &bit);
        if (bank >= 0 && bank < 3 && bit >= 0 && bit < 32) {
            dm32_idle_low[bank] |= 1u << bit;
        }
        if (dm32_keylog) {
            fprintf(stderr, "GPIO idle-low: bank%ld bit%ld\n", bank, bit);
        }
        if (*end != ',') {
            break;
        }
        e = end + 1;
    }
}

/*
 * Devices attached to pads (dm32_gpio_attach, include/hw/csky/dm32.h): the
 * FD6818 transceiver and the bit-banged I2C buses. A pad has at most
 * DM32_PAD_ATTACH_MAX devices.
 *
 * An attached device hears about a pad only when the SoC side of it changes,
 * that is, the pad's SWPORTA_DDR or SWPORTA_DR bit, so a read-modify-write of
 * a whole register that leaves the pad alone does not reach it. Both
 * registers reset to 0: undriven, low.
 *
 * An attached pad reads wired-AND: the SoC's SWPORTA_DR bit while SWPORTA_DDR
 * drives the pad, otherwise its idle or injected level, and low if any
 * attached device pulls it low. The stock bit-bang drivers drive both levels
 * rather than releasing the line (dm32uv-re specs/dts/dm32uv.dts), so an
 * open-drain configuration is not required. Unattached pads read as before.
 */
#define DM32_PAD_ATTACH_MAX 2

typedef struct DM32PadAttach {
    const DM32GpioPinOps *ops;
    void *opaque;
    bool last_driven;
    int last_level;
} DM32PadAttach;

static DM32PadAttach dm32_pad_attach[DM32_NPADS][DM32_PAD_ATTACH_MAX];
static uint8_t dm32_pad_nattach[DM32_NPADS];
/* Per bank, the pads with an attached device. */
static uint32_t dm32_attached[3];

void dm32_gpio_attach(unsigned pad, const DM32GpioPinOps *ops, void *opaque)
{
    DM32PadAttach *a;

    assert(pad < DM32_NPADS && dm32_pad_nattach[pad] < DM32_PAD_ATTACH_MAX);
    a = &dm32_pad_attach[pad][dm32_pad_nattach[pad]++];
    a->ops = ops;
    a->opaque = opaque;
    a->last_driven = false;
    a->last_level = 0;
    dm32_attached[pad / 32] |= 1u << (pad % 32);
}

int dm32_gpio_output(unsigned pad, bool *driven)
{
    DM32GpioState *s = pad < DM32_NPADS ? dm32_gpio_bank[pad / 32] : NULL;
    unsigned bit = pad % 32;

    if (!s) {
        *driven = false;
        return 0;
    }
    /* A pin reaches its pad only while IO_DIPLEX routes the pad to GPIO. */
    *driven = ((s->reg[R_DDR / 4] & s->gpio_mask) >> bit) & 1;
    return (s->reg[0x0 / 4] >> bit) & 1;
}

/* Tell attached devices about each pad whose SoC side changed. */
static void dm32_gpio_outputs_changed(DM32GpioState *s)
{
    uint32_t m;

    if (s->bank < 0) {
        return;
    }
    for (m = dm32_attached[s->bank]; m; m &= m - 1) {
        unsigned pad = s->bank * 32 + ctz32(m);
        bool driven;
        int level = dm32_gpio_output(pad, &driven);
        int i;

        for (i = 0; i < dm32_pad_nattach[pad]; i++) {
            DM32PadAttach *a = &dm32_pad_attach[pad][i];

            if (a->last_driven == driven && a->last_level == level) {
                continue;
            }
            a->last_driven = driven;
            a->last_level = level;
            if (a->ops->output) {
                a->ops->output(a->opaque, pad, driven, level);
            }
        }
    }
}

/* Resolve the attached pads of input word @v, as described above. */
static uint32_t dm32_gpio_attached_input(DM32GpioState *s, uint32_t v)
{
    uint32_t m;

    if (s->bank < 0) {
        return v;
    }
    for (m = dm32_attached[s->bank]; m; m &= m - 1) {
        unsigned bit = ctz32(m);
        unsigned pad = s->bank * 32 + bit;
        int i;

        for (i = 0; i < dm32_pad_nattach[pad]; i++) {
            DM32PadAttach *a = &dm32_pad_attach[pad][i];

            if (a->ops->input && a->ops->input(a->opaque, pad) == 0) {
                v &= ~(1u << bit);
            }
        }
    }
    return v;
}

/*
 * Input level (pure: no side effects): idle levels, then injected pin
 * levels. The keypad injection (DM32_KEY and DM32_KEYSEQ) is applied on top
 * of this only in the EXT_PORTA read path, because it is clocked by reads
 * (see dm32_gpio_read). This function must stay side-effect free so that the
 * interrupt block and the virtual-time injectors can call it.
 */
static uint32_t dm32_gpio_input_level(DM32GpioState *s)
{
    uint32_t drive = s->reg[R_DDR / 4] & s->gpio_mask;
    uint32_t v;

    /* timers may fire before the first guest read */
    dm32_idle_low_parse();
    /*
     * An undriven pad: its SoC pull when IOMGR_REN enables one, otherwise
     * high. Assumption: the datasheet gives no level for a pad with no pull;
     * the keys and the boot-mode pins have board pull-ups.
     */
    v = (0xffffffffu | s->pull_up) & ~s->pull_down;
    if (s->bank >= 0) {
        /* board-level pulls the SoC does not know about, see above */
        v &= ~dm32_idle_low[s->bank];
    }
    /* injected pins (DM32_PINSEQ and related) */
    v = (v & ~s->force_mask) | (s->force_val & s->force_mask);
    /* a pin GPIO drives carries SWPORTA_DR */
    v = (v & ~drive) | (s->reg[0x0 / 4] & drive);
    /* attached devices pull their pads low (wired-AND) */
    v = dm32_gpio_attached_input(s, v);
    /* keypad rows held by the injection */
    v &= ~s->key_low;
    /*
     * A pad whose input buffer is off (IOMGR_IE bit 0) reads 0, and so does a
     * pad IO_DIPLEX gives to another block. Assumption: the datasheet does
     * not say what GPIO sees on such a pad.
     */
    return v & s->ie_mask & s->gpio_mask;
}

/* Recompute the bank's view of IO_DIPLEX and IOMGR_IE. */
static void dm32_gpio_pads_refresh(DM32GpioState *s)
{
    uint32_t gpio = 0, ie = 0, up = 0, down = 0;
    int bit;

    for (bit = 0; bit < 32; bit++) {
        unsigned pad = s->bank * 32 + bit;
        DM32PadPull pull = s->bank < 0 ? DM32_PULL_NONE : dm32_pad_pull(pad);

        if (s->bank < 0 || dm32_pad_is_gpio(pad)) {
            gpio |= 1u << bit;
        }
        if (s->bank < 0 || dm32_pad_input_enabled(pad)) {
            ie |= 1u << bit;
        }
        if (pull == DM32_PULL_UP) {
            up |= 1u << bit;
        } else if (pull == DM32_PULL_DOWN) {
            down |= 1u << bit;
        }
    }
    s->gpio_mask = gpio;
    s->ie_mask = ie;
    s->pull_up = up;
    s->pull_down = down;
}

/*
 * Pins the firmware drives as outputs (@drive) on pads IO_DIPLEX gives to
 * another block: the level never reaches the pad, and the two blocks claim
 * one pad.
 */
static void dm32_gpio_check_conflict(DM32GpioState *s, uint32_t drive)
{
    uint32_t m;

    if (s->bank < 0) {
        return;
    }
    for (m = drive & ~s->gpio_mask; m; m &= m - 1) {
        static bool warned[DM32_NPADS];
        unsigned bit = ctz32(m);
        unsigned pad = s->bank * 32 + bit;

        dm32_event(DM32_EV_PAD_CONFLICT, "GPIO%c%u driven while IO_DIPLEX "
                   "gives the pad to %s (select %d)",
                   "ABC"[s->bank], bit, dm32_pad_name(pad),
                   dm32_pad_function(pad));
        if (!warned[pad]) {
            warned[pad] = true;
            fprintf(stderr, "dm32_gpio: GPIO%c%u driven while IO_DIPLEX "
                    "gives the pad to %s (select %d) -- the level does not "
                    "reach the pad\n", "ABC"[s->bank], bit,
                    dm32_pad_name(pad), dm32_pad_function(pad));
        }
    }
}

static char dm32_bank_letter(DM32GpioState *s)
{
    return (s->bank >= 0 && s->bank < 3) ? "ABC"[s->bank] : '?';
}

/*
 * Interrupt block, evaluated with the current input word `in`.
 *
 *   Level-type bit (TYPE=0): the RAW bit is (input == POLARITY) while INTEN
 *   is set. It follows the pin.
 *   Edge-type bit (TYPE=1): the RAW bit latches on the selected edge
 *   (POLARITY 1 = rising, 0 = falling; INTR_BOTH_EDGE=1 means either edge).
 *   It is cleared only by PORTA_EOI.
 *
 * RAW is kept only for INTEN bits. INTSTATUS = RAW & ~INTMASK. The bank's
 * IRQ line is (INTSTATUS != 0). Edge detection needs the previous sample,
 * and `primed` seeds it on the first evaluation.
 */
/*
 * Debounce (DEBOUNCE, +0x48). The datasheet says only that the block
 * "supports de-jitter" with a configurable sampling time (section 5.7.2).
 * Assumptions, from the DesignWare APB GPIO the block is built from: a
 * debounced bit feeds the interrupt block only, not EXT_PORTA; a change must
 * hold for two cycles of the debounce clock before the interrupt block sees
 * it, and a shorter pulse is lost; the debounce clock is the 32.768 kHz RTC
 * clock, so two cycles are 61 us.
 */
#define DM32_GPIO_DB_NS 61035

static void dm32_gpio_db_tick(void *opaque);

static uint32_t dm32_gpio_debounce(DM32GpioState *s, uint32_t in)
{
    uint32_t deb = s->reg[R_DEBOUNCE / 4];

    if (!s->primed) {
        s->db_view = in;
    }
    if ((in ^ s->db_view) & deb) {
        if (!s->db_armed || ((in ^ s->db_cand) & deb)) {
            /* a new level: it must now hold for the whole window */
            s->db_cand = in;
            s->db_armed = true;
            timer_mod(s->db_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      DM32_GPIO_DB_NS);
        }
    } else if (s->db_armed) {
        /* back to the settled level before the window closed: a glitch */
        s->db_armed = false;
        timer_del(s->db_timer);
    }
    s->db_view = (s->db_view & deb) | (in & ~deb);
    return s->db_view;
}

static void dm32_gpio_irq_eval(DM32GpioState *s, uint32_t pad_in)
{
    uint32_t in = dm32_gpio_debounce(s, pad_in);
    /*
     * Only input-configured pins can interrupt (DW_apb_gpio: an output
     * pin's INTEN is inert). The keypad scan drives the columns as outputs,
     * so INTEN on those bits (stock enables bits 19 and 20 on bank B) must
     * not turn every scan step into an interrupt.
     */
    uint32_t inten = s->reg[R_INTEN / 4] & ~s->reg[R_DDR / 4] & s->gpio_mask;
    uint32_t mask  = s->reg[R_INTMASK / 4];
    uint32_t typ   = s->reg[R_INTTYPE / 4];
    uint32_t pol   = s->reg[R_INTPOL / 4];
    uint32_t both  = s->reg[R_BOTHEDGE / 4];
    uint32_t lvl   = inten & ~typ;
    uint32_t edg   = inten & typ;
    uint32_t prev  = s->primed ? s->last_in : in;
    uint32_t rise  = in & ~prev;
    uint32_t fall  = prev & ~in;
    /* bits where the input is at the active level */
    uint32_t act   = ~(in ^ pol);
    /* per bit: both edges, else the edge INT_POLARITY selects */
    uint32_t trig  = (((rise | fall) & both) |
                      (((rise & pol) | (fall & ~pol)) & ~both)) & edg;
    uint32_t raw   = (act & lvl) | ((s->raw | trig) & edg);
    uint32_t status;
    int level;

    raw &= inten;
    s->raw = raw;
    s->last_in = in;
    s->primed = 1;
    status = raw & ~mask;
    level = (status != 0);
    if (level != s->irq_level) {
        s->irq_level = level;
        if (dm32_keylog) {
            fprintf(stderr,
                    "GPIO IRQ bank %c %s status=0x%x raw=0x%x in=0x%08x\n",
                    dm32_bank_letter(s), level ? "raise" : "lower",
                    status, raw, in);
        }
        qemu_set_irq(s->irq, level);
    }
}

/*
 * Virtual-time pin injection.
 *
 * DM32_PINSEQ="t_ms:pin:level;t_ms:pin:level;..."
 *     t_ms may have a fraction (for example 0.03 for 30 us).
 *     Level 0 or 1 forces the logical pin. Level "-" or "x" releases it to
 *     its idle level. Logical pin = bank * 32 + bit (A=0, B=1, C=2). A later
 *     step for the same pin (at a later time) overrides an earlier one.
 *
 * DM32_KNOB="t_ms:+N" or "t_ms:-N"
 *     N detents of quadrature on pins 0x26 (A) and 0x27 (B), one 4-edge
 *     cycle per detent, with edges every DM32_KNOB_EDGE_MS ms (default 5).
 *     The "+" order is: A falls, B falls, A rises, B rises (A leads B). The
 *     "-" order reverses it (B leads A). Which direction the firmware calls
 *     "+" is decided by the firmware, not by this model. Both pins are
 *     released (idle high) after the last edge.
 *
 * DM32_PTT="t_ms:hold_ms"
 *     Forces pin 0x2b low at t_ms and releases it hold_ms later. Pin 0x2b
 *     is read by the runtime-lib function bitwise_not_of_boolean_result
 *     (dm32uv-re src/runtime-lib/bitwise_not_of_boolean_result_03012f48.c),
 *     which returns ~(pin != 0), that is, 1 when the pin reads low. Low
 *     therefore means pressed, which assumes an active-low PTT.
 *
 * The keypad matrix pins (GPIOB 17-20, logical 49-52, and GPIOC 7-11,
 * logical 71-75) are not touched by any of these unless a PINSEQ names them
 * explicitly, and that prints a warning.
 *
 * Every injected change prints a PINSEQ, KNOB or PTT line under DM32_KEYLOG.
 */
#define DM32_PIN_EV_MAX 8192
#define DM32_KNOB_MAX   1000

typedef struct {
    int kind;                 /* 0 PINSEQ, 1 KNOB, 2 PTT */
    int64_t t_ns;
    int pin;                  /* logical pin */
    int level;                /* 0, 1, or -1 = release */
    QEMUTimer *timer;
} DM32PinEv;

static DM32PinEv dm32_pin_ev[DM32_PIN_EV_MAX];
static int dm32_pin_nev;
static const char *dm32_pin_kind_name[3] = { "PINSEQ", "KNOB", "PTT" };

static void dm32_pin_add(int kind, int64_t t_ns, int pin, int level)
{
    DM32PinEv *ev;

    if (dm32_pin_nev >= DM32_PIN_EV_MAX) {
        fprintf(stderr,
                "DM32 PIN INJECT: event table full, dropping %s at %" PRId64
                " ns\n", dm32_pin_kind_name[kind], t_ns);
        return;
    }
    ev = &dm32_pin_ev[dm32_pin_nev++];
    ev->kind = kind;
    ev->t_ns = t_ns;
    ev->pin = pin;
    ev->level = level;
    ev->timer = NULL;
}

static int dm32_pin_valid(int pin)
{
    return pin >= 0 && pin < 96;
}

static int dm32_pin_is_matrix(int pin)
{
    return (pin >= 49 && pin <= 52) || (pin >= 71 && pin <= 75);
}

static void dm32_pin_parse_seq(const char *k)
{
    while (k && *k) {
        double t;
        int pin;
        char lv[16];
        int used = 0;
        int level;

        /* t_ms may have a fraction, for steps shorter than a millisecond */
        if (sscanf(k, "%lf:%i:%15[^;]%n", &t, &pin, lv, &used) < 3 ||
            t < 0) {
            fprintf(stderr, "DM32_PINSEQ: cannot parse at \"%s\"\n", k);
            break;
        }
        if (lv[0] == '-' || lv[0] == 'x' || lv[0] == 'X') {
            level = -1;
        } else if (lv[0] == '0' || lv[0] == '1') {
            level = lv[0] - '0';
        } else {
            level = -2;
        }
        if (!dm32_pin_valid(pin) || level == -2) {
            fprintf(stderr,
                    "DM32_PINSEQ: bad step pin=%d level=%s "
                    "(pins 0-95, level 0/1/-/x)\n", pin, lv);
        } else {
            if (dm32_pin_is_matrix(pin)) {
                fprintf(stderr,
                        "DM32 WARNING: DM32_PINSEQ drives keypad matrix "
                        "pin %d (bank %c bit %d)\n",
                        pin, "ABC"[pin / 32], pin % 32);
            }
            dm32_pin_add(0, (int64_t)(t * SCALE_MS + 0.5), pin, level);
        }
        k += used;
        if (*k == ';') {
            k++;
        } else if (*k) {
            break;
        }
    }
}

static void dm32_pin_parse_knob(const char *k, unsigned long edge_ms)
{
    unsigned long t;
    int n;
    int used = 0;
    int i;
    int j;

    if (sscanf(k, "%lu:%d%n", &t, &n, &used) < 2 || n == 0 ||
        n > DM32_KNOB_MAX || n < -DM32_KNOB_MAX) {
        fprintf(stderr,
                "DM32_KNOB: cannot parse \"%s\" "
                "(want t_ms:+N or t_ms:-N, 1..%d detents)\n",
                k, DM32_KNOB_MAX);
        return;
    }
    {
        /*
         * "+": A falls, B falls, A rises, B rises (A leads B).
         * "-": B falls, A falls, B rises, A rises.
         */
        static const int plus_pin[4] = { 38, 39, 38, 39 };
        static const int plus_lv[4] = { 0, 0, 1, 1 };
        static const int minus_pin[4] = { 39, 38, 39, 38 };
        static const int minus_lv[4] = { 0, 0, 1, 1 };
        int dets = n > 0 ? n : -n;
        const int *pp = n > 0 ? plus_pin : minus_pin;
        const int *pl = n > 0 ? plus_lv : minus_lv;
        unsigned long tt = t;

        for (i = 0; i < dets; i++) {
            for (j = 0; j < 4; j++) {
                dm32_pin_add(1, (int64_t)tt * SCALE_MS, pp[j], pl[j]);
                tt += edge_ms;
            }
        }
        /* back to idle high */
        dm32_pin_add(1, (int64_t)tt * SCALE_MS, 38, -1);
        dm32_pin_add(1, (int64_t)tt * SCALE_MS, 39, -1);
    }
}

static void dm32_pin_parse_all(void)
{
    const char *e;
    /* the upper bound keeps the event times within int64 nanoseconds */
    unsigned long edge_ms = dm32_env_uint("DM32_KNOB_EDGE_MS", 5, 1, 1000000);

    e = getenv("DM32_PINSEQ");
    if (e && *e) {
        dm32_pin_parse_seq(e);
    }
    e = getenv("DM32_KNOB");
    if (e && *e) {
        dm32_pin_parse_knob(e, edge_ms);
    }
    e = getenv("DM32_PTT");
    if (e && *e) {
        unsigned long t;
        unsigned long hold;

        if (sscanf(e, "%lu:%lu", &t, &hold) == 2) {
            /* logical pin 0x2b, bank B bit 11 */
            dm32_pin_add(2, (int64_t)t * SCALE_MS, 43, 0);
            dm32_pin_add(2, (int64_t)(t + hold) * SCALE_MS, 43, -1);
        } else {
            fprintf(stderr,
                    "DM32_PTT: cannot parse \"%s\" (want t_ms:hold_ms)\n", e);
        }
    }
}

static void dm32_pin_ev_cb(void *opaque)
{
    DM32PinEv *ev = opaque;
    int bank = ev->pin / 32, bit = ev->pin % 32;
    DM32GpioState *s = (bank >= 0 && bank < 3) ? dm32_gpio_bank[bank] : NULL;
    uint32_t b = 1u << bit;

    if (!s) {
        return;
    }
    if (ev->level < 0) {
        s->force_mask &= ~b;
    } else {
        s->force_mask |= b;
        if (ev->level) {
            s->force_val |= b;
        } else {
            s->force_val &= ~b;
        }
    }
    if (dm32_keylog) {
        fprintf(stderr, "%s t=%lld pin=%d (bank %c bit %d) %s\n",
                dm32_pin_kind_name[ev->kind],
                (long long)qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL),
                ev->pin, dm32_bank_letter(s), bit,
                ev->level < 0 ? "released"
                              : (ev->level ? "forced high" : "forced low"));
    }
    dm32_gpio_irq_eval(s, dm32_gpio_input_level(s));
}

static void dm32_pin_inject_init(void)
{
    int i;

    dm32_pin_parse_all();
    if (dm32_keylog && dm32_pin_nev) {
        fprintf(stderr, "PIN INJECT: %d scheduled events\n", dm32_pin_nev);
    }
    for (i = 0; i < dm32_pin_nev; i++) {
        DM32PinEv *ev = &dm32_pin_ev[i];

        ev->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dm32_pin_ev_cb, ev);
        timer_mod(ev->timer, ev->t_ns);
    }
}

static uint32_t dm32_gpio_read_reg(DM32GpioState *s, hwaddr off)
{

    if (off + 4 > WIN) {
        return 0;
    }
    dm32_key_parse();
    dm32_idle_low_parse();
    if (off == DATA_OFF && dm32_gpiolog) {
        fprintf(stderr, "GPIOR bank=%c off=0x%x\n", dm32_bank_letter(s),
                (unsigned)off);
    }
    switch (off) {
    case R_INTSTATUS:
        if (dm32_keylog) {
            fprintf(stderr, "GPIO INTSTATUS read bank %c = 0x%x\n",
                    dm32_bank_letter(s),
                    s->raw & ~s->reg[R_INTMASK / 4]);
        }
        return s->raw & ~s->reg[R_INTMASK / 4];
    case R_RAWINTSTAT:
        if (dm32_keylog) {
            fprintf(stderr, "GPIO RAW_INTSTATUS read bank %c = 0x%x\n",
                    dm32_bank_letter(s), s->raw);
        }
        return s->raw;
    case R_EOI:
        /* write-only in the datasheet */
        return 0;
    default:
        break;
    }
    /*
     * EXT_PORTA (0x50) is the DesignWare external-input register. Stock
     * reads inputs there (IRAM gpio_read_input_bit @0x2c588, ld.w
     * (bank, 0x50)). Returning the raw reg[] made every input read low and
     * disabled key injection for stock. The pad levels (see
     * dm32_gpio_input_level) and key injection are applied here. The keypad
     * injection is clocked by these reads; the rows it holds low persist in
     * key_low until the next read, so the interrupt block sees them too.
     */
    if (off == DATA_OFF) {
        uint32_t v;
        uint32_t key_low = 0;
        int me = s->bank;

        /* Keypress injection (DM32_KEY), column sampled now. */
        if (dm32_key_cb >= 0 &&
            dm32_key_column_low(dm32_key_cb, dm32_key_cbit) &&
            (dm32_key_hold == 0 || dm32_key_reads < dm32_key_hold)) {
            dm32_key_reads++;

            if (me == dm32_key_rb) {
                key_low |= 1u << dm32_key_rbit;
                if (dm32_keylog) {
                    fprintf(stderr, "KEYINJ row bank%d bit%d forced low\n",
                            dm32_key_rb, dm32_key_rbit);
                }
            }
        }
        /* Scripted sequence (DM32_KEYSEQ). Row-bank data reads drive it. */
        dm32_seq_parse();
        if (dm32_key_cb < 0 && dm32_seq_n > 0) {
            int st = dm32_seq_step();

            if (st >= 0 && me == dm32_seq[st].rb) {
                int dedicated = (dm32_seq[st].cb < 0);

                if (dedicated ||
                    dm32_key_column_low(dm32_seq[st].cb, dm32_seq[st].cbit)) {
                    key_low |= 1u << dm32_seq[st].rbit;
                    if (dm32_keylog) {
                        fprintf(stderr,
                                "KEYSEQ step %d: bank%d bit%d low (%s)\n",
                                st, dm32_seq[st].rb, dm32_seq[st].rbit,
                                dedicated ? "dedicated" : "matrix");
                    }
                }
            }
            /* clock: count reads of the bank(s) the sequence targets */
            if (st >= 0 ? (me == dm32_seq[st].rb) :
                (dm32_seq_i < dm32_seq_n &&
                 me == dm32_seq[dm32_seq_i].rb)) {
                dm32_seq_reads++;
            }
        }
        s->key_low = key_low;
        v = dm32_gpio_input_level(s);
        /* Reads are an evaluation point for the interrupt block too. */
        dm32_gpio_irq_eval(s, v);
        return v;
    }
    return s->reg[off / 4];
}

static void dm32_gpio_write_reg(DM32GpioState *s, hwaddr off,
                                uint64_t val)
{
    uint32_t old;

    if (off + 4 > WIN) {
        return;
    }
    if (dm32_gpiolog) {
        fprintf(stderr, "GPIOW bank=%c off=0x%x val=0x%08x\n",
                dm32_bank_letter(s), (unsigned)off, (uint32_t)val);
    }

    dm32_key_parse();
    dm32_seq_parse();
    if (off == DATA_OFF || off == R_INTSTATUS || off == R_RAWINTSTAT) {
        return;             /* read-only (v2.7 EN 5.7.5; SVD) */
    }
    old = s->reg[off / 4];
    s->reg[off / 4] = val;
    if (off == 0x0) {
        dm32_gpio_check_conflict(s, s->reg[R_DDR / 4] & (old ^ (uint32_t)val));
    } else if (off == R_DDR) {
        dm32_gpio_check_conflict(s, (uint32_t)val & ~old);
    }
    if (off == 0x0 || off == R_DDR) {
        /* the SoC side of the pads moved: tell attached devices */
        dm32_gpio_outputs_changed(s);
    }

    switch (off) {
    case R_INTEN:
    case R_INTMASK:
    case R_INTPOL:
    case R_BOTHEDGE:
        if (dm32_keylog) {
            fprintf(stderr, "GPIO reg 0x%x write bank %c = 0x%x\n",
                    (unsigned)off, dm32_bank_letter(s), (uint32_t)val);
        }
        dm32_gpio_irq_eval(s, dm32_gpio_input_level(s));
        break;
    case R_DEBOUNCE:
    case R_DDR:
        /* direction change: the pin's interrupt eligibility changes */
        dm32_gpio_irq_eval(s, dm32_gpio_input_level(s));
        break;
    case R_INTTYPE:
        if (dm32_keylog) {
            fprintf(stderr, "GPIO reg 0x%x write bank %c = 0x%x\n",
                    (unsigned)off, dm32_bank_letter(s), (uint32_t)val);
        }
        /* a bit whose trigger type changed loses its latched RAW state */
        s->raw &= ~(old ^ (uint32_t)val);
        dm32_gpio_irq_eval(s, dm32_gpio_input_level(s));
        break;
    case R_EOI: {
        uint32_t clr = (uint32_t)val & s->reg[R_INTTYPE / 4] &
                       s->reg[R_INTEN / 4];

        if (dm32_keylog) {
            fprintf(stderr, "GPIO EOI bank %c write=0x%x cleared=0x%x\n",
                    dm32_bank_letter(s), (uint32_t)val, clr);
        }
        /* write-1-to-clear, edge-latched bits only */
        s->raw &= ~clr;
        dm32_gpio_irq_eval(s, dm32_gpio_input_level(s));
        break;
    }
    default:
        break;
    }
}

void dm32_gpio_input_changed(unsigned pad)
{
    DM32GpioState *s = pad < DM32_NPADS ? dm32_gpio_bank[pad / 32] : NULL;

    if (s) {
        dm32_gpio_irq_eval(s, dm32_gpio_input_level(s));
    }
}

/*
 * Bit-banged I2C on two pads (dm32_gpio_i2c_create), bridged to QEMU's
 * bitbang_i2c so that any I2CSlave model can sit on the bus. Each line's
 * level is the SoC's SWPORTA_DR bit while it drives the pad and high (the bus
 * pull-up) while it does not. A slave pulling SDA low (an ACK, or a 0 data
 * bit) pulls the SDA pad low. SCL is never stretched.
 *
 * Users: the ALPU-MP on GPIOA 5 (SCL) / 6 (SDA) (stock i2c_read_block_0x7a
 * @0x000144fc), not modelled, so every address NAKs there; and the RDA5807M FM
 * tuner on GPIOA 26 (SCL) / 25 (SDA) (rda5807m_i2c_send_byte @0x03065548),
 * dm32_rda5807m.c.
 *
 * A master that clocks on after a slave's NAK, instead of sending STOP or a
 * repeated START, gets nowhere: bitbang_i2c ignores the rest of the transfer.
 * That is counted as an i2c_nak event, with one stderr warning per bus. A NAK
 * the master itself sends at the end of a read is not counted.
 */
typedef struct DM32GpioI2C {
    bitbang_i2c_interface bb;
    unsigned scl;
    unsigned sda;
    /* the line levels last given to bb */
    int scl_line;
    int sda_line;
    const char *name;
    /* the slave NAKed and the master has not ended the transfer */
    bool slave_nak;
    bool warned;
} DM32GpioI2C;

/*
 * Pass an SCL edge to bb, noting a slave's NAK and a master ignoring it. On a
 * slave's NAK bitbang_i2c returns to STOPPED at once (a master's NACK at the
 * end of a read leaves it in SENT_NACK instead), so a rising SCL after it,
 * with no START or STOP in between, is the master clocking on.
 */
static void dm32_gpio_i2c_scl(DM32GpioI2C *g, int level)
{
    bitbang_i2c_state before = g->bb.state;

    if (level && g->slave_nak) {
        dm32_event(DM32_EV_I2C_NAK, "bus %s: master clocks on after a NAK",
                   g->name);
        if (!g->warned) {
            g->warned = true;
            fprintf(stderr, "dm32_gpio: I2C bus %s: master clocks on after a "
                    "slave NAK; the rest of the transfer is lost\n", g->name);
        }
        g->slave_nak = false;
    }
    bitbang_i2c_set(&g->bb, BITBANG_I2C_SCL, level);
    if (level && before == WAITING_FOR_ACK && g->bb.state == STOPPED) {
        g->slave_nak = true;
    }
}

static int dm32_gpio_i2c_line(unsigned pad)
{
    bool driven;
    int level = dm32_gpio_output(pad, &driven);

    return driven ? level : 1;
}

static void dm32_gpio_i2c_output(void *opaque, unsigned pad, bool driven,
                                 int level)
{
    DM32GpioI2C *g = opaque;
    int scl = dm32_gpio_i2c_line(g->scl);
    int sda = dm32_gpio_i2c_line(g->sda);
    int out = g->bb.device_out;

    /*
     * One register write can move both lines. Order them as a master would:
     * data changes while the clock is low, so a falling clock goes first and
     * a rising clock last.
     */
    if (scl != g->scl_line && !scl) {
        dm32_gpio_i2c_scl(g, 0);
        g->scl_line = 0;
    }
    if (sda != g->sda_line) {
        bitbang_i2c_set(&g->bb, BITBANG_I2C_SDA, sda);
        g->sda_line = sda;
        if (g->scl_line) {
            g->slave_nak = false;       /* a START or STOP ends the transfer */
        }
    }
    if (scl != g->scl_line) {
        dm32_gpio_i2c_scl(g, scl);
        g->scl_line = scl;
    }
    if (g->bb.device_out != out) {
        dm32_gpio_input_changed(g->sda);
    }
}

static int dm32_gpio_i2c_input(void *opaque, unsigned pad)
{
    DM32GpioI2C *g = opaque;

    return (pad == g->sda && !g->bb.device_out) ? 0 : -1;
}

static const DM32GpioPinOps dm32_gpio_i2c_ops = {
    .output = dm32_gpio_i2c_output,
    .input = dm32_gpio_i2c_input,
};

I2CBus *dm32_gpio_i2c_create(DeviceState *parent, const char *name,
                             unsigned scl, unsigned sda)
{
    DM32GpioI2C *g = g_new0(DM32GpioI2C, 1);
    I2CBus *bus = i2c_init_bus(parent, name);

    bitbang_i2c_init(&g->bb, bus);
    g->name = name;
    g->scl = scl;
    g->sda = sda;
    /* bitbang_i2c_init's idle state; both pads reset undriven, so high */
    g->scl_line = 1;
    g->sda_line = 1;
    dm32_gpio_attach(scl, &dm32_gpio_i2c_ops, g);
    dm32_gpio_attach(sda, &dm32_gpio_i2c_ops, g);
    return bus;
}

/* Registers are 32 bits; narrow accesses act on the containing register. */
static uint64_t dm32_gpio_read(void *opaque, hwaddr addr, unsigned size)
{
    return dm32_reg_lane_read(dm32_gpio_read_reg(opaque, addr & ~3), addr,
                              size);
}

static void dm32_gpio_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    DM32GpioState *s = opaque;
    hwaddr off = addr & ~3;

    if (size < 4 && off + 4 <= WIN) {
        val = dm32_reg_lane_write(s->reg[off / 4], addr, size, val);
    }
    dm32_gpio_write_reg(s, off, val);
}

static const MemoryRegionOps dm32_gpio_ops = {
    .read = dm32_gpio_read,
    .write = dm32_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/*
 * Machine reset: every register 0 (SVD), so every pin is an input, and no
 * interrupt pending. Attached devices see the pads released. Injected pin
 * levels (DM32_PINSEQ, DM32_KNOB, DM32_PTT) are host input and persist.
 */
static void dm32_gpio_reset(DeviceState *dev)
{
    DM32GpioState *s = DM32_GPIO(dev);

    memset(s->reg, 0, sizeof(s->reg));
    s->raw = 0;
    s->key_low = 0;
    s->db_armed = false;
    timer_del(s->db_timer);
    s->last_in = 0;
    s->primed = 0;
    s->irq_level = 0;
    qemu_set_irq(s->irq, 0);
    dm32_gpio_pads_refresh(s);
    dm32_gpio_outputs_changed(s);
}

static void dm32_gpio_db_tick(void *opaque)
{
    DM32GpioState *s = opaque;
    uint32_t deb = s->reg[R_DEBOUNCE / 4];

    /* the candidate held for the whole window: the interrupt block sees it */
    s->db_view = (s->db_view & ~deb) | (s->db_cand & deb);
    s->db_armed = false;
    dm32_gpio_irq_eval(s, dm32_gpio_input_level(s));
}

/* IO_DIPLEX or IOMGR_IE changed: pads may have joined or left GPIO. */
static void dm32_gpio_pads_changed(Notifier *n, void *data)
{
    DM32GpioState *s = container_of(n, DM32GpioState, pad_notifier);

    dm32_gpio_pads_refresh(s);
    dm32_gpio_outputs_changed(s);
    dm32_gpio_irq_eval(s, dm32_gpio_input_level(s));
}

static void dm32_gpio_init(Object *obj)
{
    DM32GpioState *s = DM32_GPIO(obj);

    dm32_gpio_log_parse();
    memory_region_init_io(&s->iomem, obj, &dm32_gpio_ops, s,
                          TYPE_DM32_GPIO, WIN);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->gpio_mask = s->ie_mask = 0xffffffff;
    s->db_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dm32_gpio_db_tick, s);
    s->pad_notifier.notify = dm32_gpio_pads_changed;
    dm32_pad_add_notifier(&s->pad_notifier);

    /*
     * Record instances in creation order (A, B, C). dm32.c creates them in
     * that order, after the PIC, so their interrupt lines can be wired. The
     * keypress injector reaches the column bank from the row bank's read
     * handler through this table. The third bank also arms the pin
     * injectors, which need all three banks.
     */
    {
        static int n;

        s->bank = -1;
        if (n < 3) {
            s->bank = n;
            dm32_gpio_bank[n] = s;
            if (dm32_keylog) {
                fprintf(stderr, "GPIOBANK[%d] = %p\n", n, (void *)s);
            }
            n++;
            if (n == 3) {
                dm32_pin_inject_init();
            }
        }
    }
}

static void dm32_gpio_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), dm32_gpio_reset);
}

static const TypeInfo dm32_gpio_type = {
    .name = TYPE_DM32_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DM32GpioState),
    .instance_init = dm32_gpio_init,
    .class_init = dm32_gpio_class_init,
};

static void dm32_gpio_register(void)
{
    type_register_static(&dm32_gpio_type);
}

type_init(dm32_gpio_register)
