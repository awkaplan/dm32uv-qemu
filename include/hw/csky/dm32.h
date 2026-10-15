/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Baofeng DM-32UV board model: functions the board's devices call in each
 * other.
 *
 * Copyright (c) 2026 Alex Kaplan
 *
 * The DM-32UV devices are not independent. The FD6818 transceiver's bus and
 * the SPI chip select are bit-banged through GPIO, and the pad multiplexing
 * that decides whether the SPI, ADC and LCD pads are connected lives in the
 * system registers. These hooks connect the devices. Each device that
 * provides one exists once per board.
 */

#ifndef HW_CSKY_DM32_H
#define HW_CSKY_DM32_H

#include "exec/hwaddr.h"
#include "qemu/bitops.h"
#include "qemu/notify.h"
#include "hw/core/irq.h"
#include "hw/core/qdev.h"
#include "hw/i2c/i2c.h"

/*
 * hw/csky/dm32.c: configuration from DM32_* environment variables
 * (docs/environment.md). Read them once, when the device is created, not on
 * every access.
 *
 * dm32_env_flag(): a flag is on when the variable is set to anything other
 * than "" or "0".
 *
 * dm32_env_uint(), dm32_env_int(): an unset or empty variable gives @def.
 * Otherwise the value is a C integer literal (decimal, 0x hex or 0 octal)
 * that must lie in [@min, @max]; anything else is a fatal configuration
 * error, reported with the variable's name.
 */
bool dm32_env_flag(const char *name);
uint64_t dm32_env_uint(const char *name, uint64_t def, uint64_t min,
                       uint64_t max);
int64_t dm32_env_int(const char *name, int64_t def, int64_t min, int64_t max);

/*
 * Narrow accesses to a bank of 32-bit registers. QEMU passes a 1- or 2-byte
 * access through at its own address (system/memory.c does not widen it), so
 * a handler that works on whole registers reads the register at the aligned
 * offset and returns the addressed bytes with dm32_reg_lane_read(), and
 * merges a narrow write into the register's current value with
 * dm32_reg_lane_write() before acting on it as a full-word write. Accesses
 * are aligned to their size (MemoryRegionOps.valid.unaligned is false), so
 * an access never spans two registers.
 */
static inline uint32_t dm32_reg_lane_read(uint32_t word, hwaddr addr,
                                          unsigned size)
{
    return extract32(word, (addr & 3) * 8, size * 8);
}

static inline uint32_t dm32_reg_lane_write(uint32_t word, hwaddr addr,
                                           unsigned size, uint64_t val)
{
    return deposit32(word, (addr & 3) * 8, size * 8, val);
}

/*
 * hw/csky/dm32.c: permissive events. Each is a guest access the model lets
 * through, or quietly drops, where the silicon would refuse it, behave
 * differently, or where no model exists. Every event is counted, and the
 * counts are printed at exit. With DM32_STRICT naming the event's class, the
 * first one also stops the guest (docs/environment.md).
 */
typedef enum DM32Event {
    DM32_EV_UNIMP,          /* access to an unimplemented-device window */
    DM32_EV_UNASSIGNED,     /* access to an address no region covers */
    DM32_EV_FLASH_STORE,    /* guest store to the read-only program flash */
    DM32_EV_DAC_PD,         /* DAC data write to a powered-down channel */
    DM32_EV_LCD_MUX,        /* LCD data write while the keypad owns DB0-DB4 */
    DM32_EV_LCD_OFF,        /* LCD write before the controller is enabled */
    DM32_EV_SSI_DISCARD,    /* SSI byte dropped: block, slave or pads off */
    DM32_EV_NOR_WEL,        /* SPI-NOR program or erase without WREN */
    DM32_EV_ADC_PAD,        /* ADC conversion of a pad still muxed to GPIO */
    DM32_EV_WDT_LOCKED,     /* watchdog register write while locked */
    DM32_EV_PAD_CONFLICT,   /* GPIO drives a pad IO_DIPLEX gives elsewhere */
    DM32_EV_PAD_UNCLAIMED,  /* a block transfers on pads it does not have */
    DM32_EV_I2C_NAK,        /* a bit-banged master clocks on after a NAK */
    DM32_EV__COUNT
} DM32Event;

void dm32_event(DM32Event ev, const char *fmt, ...) G_GNUC_PRINTF(2, 3);

/* hw/misc/dm32_ssi.c */
DeviceState *dm32_ssi_create(hwaddr base, qemu_irq irq);

/*
 * hw/misc/dm32_sysreg.c: pad control. A pad is numbered as its GPIO line,
 * bank * 32 + bit with bank 0 = GPIOA, as DM32_PINSEQ numbers pins. The
 * multiplexing comes from IO_DIPLEX0-2, the pull resistors from
 * IOMGR_REN_REG0-2 and the input enables from IOMGR_IE_IE0-2 (HR_C7000 v2.7
 * sections 4.4.4.4-4.4.4.12).
 */
#define DM32_PAD(bank, bit) ((bank) * 32 + (bit))

/*
 * hw/misc/dm32_sysreg.c: SYS_SOFT_RSTN bit @bit (0-6) resets @dev with
 * device_cold_reset(). Bits with no target are logged as unmodelled.
 */
void dm32_sysreg_soft_reset_target(int bit, DeviceState *dev);

/* hw/csky/dm32.c: reset the CPU alone (SYS_SOFT_RSTN[8]). */
void dm32_cpu_soft_reset(void);
#define DM32_PTA(n) DM32_PAD(0, n)
#define DM32_PTB(n) DM32_PAD(1, n)
#define DM32_PTC(n) DM32_PAD(2, n)
#define DM32_NPADS 96

typedef enum DM32PadPull {
    DM32_PULL_NONE,
    DM32_PULL_UP,
    DM32_PULL_DOWN,
} DM32PadPull;

/*
 * The pad's IO_DIPLEX select value, or -1 if the pad has no select field
 * (it is always GPIO). Each value's meaning is in the SVD field description.
 */
int dm32_pad_function(unsigned pad);
/* Is the pad routed to its GPIO line? */
bool dm32_pad_is_gpio(unsigned pad);
/* The pad's select-0 signal (for example "SPI1_SCLK"), or "GPIO" if none. */
const char *dm32_pad_name(unsigned pad);
/* The pull resistor in effect: none when disabled or the pad has none. */
DM32PadPull dm32_pad_pull(unsigned pad);
/* Is the pad's input buffer enabled? True for a pad with no enable bit. */
bool dm32_pad_input_enabled(unsigned pad);
/* Called, with NULL data, after any IO_DIPLEX, IOMGR_REN or IOMGR_IE write. */
void dm32_pad_add_notifier(Notifier *n);

/*
 * hw/gpio/dm32_gpio.c: devices on GPIO pads (DM32_PTA(n) numbering). An
 * attached device is told the SoC side of the pad whenever it changes:
 * whether the SoC drives it (SWPORTA_DDR bit set) and the SWPORTA_DR bit.
 * It puts its own level on the pad through input(): 0 pulls the pad low, and
 * 1 or -1 leaves it alone. The pad then reads wired-AND: low if the SoC
 * drives it low or any attached device pulls it low, otherwise the SoC's
 * driven level, or the pad's idle or injected level when the SoC does not
 * drive it. input() must have no side effects; a device whose level changes
 * on its own calls dm32_gpio_input_changed() so that interrupts see it.
 */
typedef struct DM32GpioPinOps {
    void (*output)(void *opaque, unsigned pad, bool driven, int level);
    int (*input)(void *opaque, unsigned pad);
} DM32GpioPinOps;

void dm32_gpio_attach(unsigned pad, const DM32GpioPinOps *ops, void *opaque);
/* The SWPORTA_DR level of @pad; *@driven is whether SWPORTA_DDR drives it. */
int dm32_gpio_output(unsigned pad, bool *driven);
void dm32_gpio_input_changed(unsigned pad);

/*
 * A bit-banged I2C bus on two GPIO pads, as a QEMU I2C bus named @name, so
 * that any I2CSlave model attaches with -device <type>,bus=<name>. A pad the
 * SoC does not drive is high (the bus pull-ups).
 */
I2CBus *dm32_gpio_i2c_create(DeviceState *parent, const char *name,
                             unsigned scl, unsigned sda);

/* hw/misc/dm32_fd6818.c: the FD6818B transceiver on GPIOA 7, 8 and 24 */
void dm32_fd6818_init(void);

/*
 * The APB clock: 84 MHz, the value stock's clock variable (IRAM 0x3a60c)
 * holds and derives its timer loads and I2C SCL counts from. The timer and
 * the I2C controllers count in it.
 */
#define DM32_APB_HZ 84000000

/*
 * hw/misc/dm32_i2c.c: I2C controller @n (0-2) at @base, interrupt @irq. Its
 * bus is named "i2c<n>", so any I2CSlave attaches with -device ...,bus=i2c<n>.
 */
I2CBus *dm32_i2c_create(int n, hwaddr base, qemu_irq irq);

/*
 * hw/misc/dm32_codec.c: the codec's DAC standby, LINEOUT_CTRL standby_lo.
 * hw/misc/dm32_sysreg.c: whether CODEC_I2C_MUX selects the codec's MC
 * (memory-mapped) register interface.
 */
bool dm32_codec_standby_lo(void);
bool dm32_codec_mc_selected(void);

/* hw/misc/dm32_rda5807m.c: the RDA5807M FM tuner, a slave at 0x10 on @bus */
void dm32_rda5807m_attach(I2CBus *bus);

/* hw/misc/dm32_i2c_rtc.c: the RTC, a slave at 0x70 on @bus, alarm on @irq */
void dm32_i2c_rtc_attach(I2CBus *bus, qemu_irq irq);

#endif /* HW_CSKY_DM32_H */
