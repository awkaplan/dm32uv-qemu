/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Baofeng DM-32UV SPI slave controllers (DesignWare APB SSI in slave mode)
 *
 * Copyright (c) 2026 Alex Kaplan
 *
 * The HR_C7000 has three SPI slave blocks, SPI Slave 0/1/2 at 0x14120000,
 * 0x14130000 and 0x14140000 (v2.7 EN section 4.5.3, Table 26), on PIC
 * sources 34, 35 and 36, level-triggered, active high (Table 30, p.169). The
 * SPI chapter (section 5.2, p.243-263) describes one SSI that works "as a
 * Master or Slave", with one register set; slave-only fields are CTRLR0
 * SLV_OE [10], FRF [5:4] and DFS [3:0], and CTRLR1, SER and BAUDR are
 * master-only. A slave's FIFOs are 8 entries deep (p.244). Register offsets
 * and reset values are the SVD's (SPI_Slave0-2 derive from SPI_Master0),
 * served by the register file in include/hw/csky/dm32_regfile.h.
 *
 * A slave transfers only when an external master drives its SCLK and chip
 * select. Nothing on the DM-32 board does, and stock never uses these blocks,
 * so no transfer ever happens here. What the model does is what a slave
 * does on its own, following the DesignWare SSI the block is built from
 * (the datasheet gives none of it):
 *   - DR writes, while SSIENR is set, queue in the TX FIFO, up to 8 entries;
 *     a write to a full FIFO is lost and raises TXOI. They wait for a master
 *     that never comes. While SSIENR is clear, DR writes are dropped.
 *   - DR reads find the RX FIFO empty: they return 0 and raise RXUI.
 *   - TXEI is raised while SSIENR is set and the TX FIFO holds no more
 *     entries than TXFTLR. RXFI and RXOI never are, and MSTI is master-only.
 *   - ISR = RISR & IMR, and the interrupt line is ISR != 0.
 *   - TXOICR, RXOICR and RXUICR return their interrupt and clear it; ICR
 *     clears all three and returns their OR.
 *   - Clearing SSIENR empties the TX FIFO and clears the latched interrupts.
 *   - SR: BUSY 0, TFNF while the TX FIFO has room, TFE while it is empty,
 *     RFNE and RFF 0. TXFLR and RXFLR give the FIFO levels.
 * Assumption: configuration writes while SSIENR is set are accepted; the
 * datasheet does not say they are refused. Pad multiplexing (IO_DIPLEX1/2
 * "Slave SPIx" selects) is not consulted, because no master exists.
 *
 * Not modelled: any transfer. No master exists on the board, and stock never
 * uses these blocks.
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "qemu/log.h"
#include "hw/csky/dm32.h"
#include "hw/csky/dm32_regfile.h"

#define TYPE_DM32_SPI_SLAVE "dm32_spi_slave"
OBJECT_DECLARE_SIMPLE_TYPE(DM32SpiSlaveState, DM32_SPI_SLAVE)

#define WIN          0x100
#define FIFO_DEPTH   8

#define R_SSIENR     0x08
#define R_TXFTLR     0x18
#define R_TXFLR      0x20
#define R_RXFLR      0x24
#define R_SR         0x28
#define R_IMR        0x2c
#define R_ISR        0x30
#define R_RISR       0x34
#define R_TXOICR     0x38
#define R_RXOICR     0x3c
#define R_RXUICR     0x40
#define R_ICR        0x48
#define R_DR         0x60

#define SR_TFNF      (1u << 1)
#define SR_TFE       (1u << 2)

#define INT_TXE      (1u << 0)
#define INT_TXO      (1u << 1)
#define INT_RXU      (1u << 2)
#define INT_RXO      (1u << 3)
#define INT_LATCHED  (INT_TXO | INT_RXU | INT_RXO)

static const DM32RegDesc dm32_spi_slave_regs[] = {
    DM32_SVD_SPI_SLAVE0_REGS(DM32_REGDESC)
};
#define NREGS ARRAY_SIZE(dm32_spi_slave_regs)

struct DM32SpiSlaveState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t val[NREGS];
    unsigned txflr;
    uint32_t latched;      /* TXOI, RXUI, RXOI */
};

static uint32_t reg(DM32SpiSlaveState *s, uint32_t off)
{
    uint32_t v = 0;

    dm32_regfile_read(dm32_spi_slave_regs, NREGS, s->val, off, &v);
    return v;
}

static uint32_t dm32_spi_slave_risr(DM32SpiSlaveState *s)
{
    uint32_t r = s->latched;

    if ((reg(s, R_SSIENR) & 1) && s->txflr <= (reg(s, R_TXFTLR) & 0xff)) {
        r |= INT_TXE;
    }
    return r;
}

static void dm32_spi_slave_update(DM32SpiSlaveState *s)
{
    qemu_set_irq(s->irq, (dm32_spi_slave_risr(s) & reg(s, R_IMR)) != 0);
}

static uint32_t dm32_spi_slave_clear(DM32SpiSlaveState *s, uint32_t bits)
{
    uint32_t was = s->latched & bits;

    s->latched &= ~bits;
    dm32_spi_slave_update(s);
    return was ? 1 : 0;
}

static uint64_t dm32_spi_slave_read(void *opaque, hwaddr off, unsigned size)
{
    DM32SpiSlaveState *s = opaque;
    uint32_t v = 0;

    switch (off) {
    case R_TXFLR:
        return s->txflr;
    case R_RXFLR:
        return 0;
    case R_SR:
        return (s->txflr < FIFO_DEPTH ? SR_TFNF : 0) |
               (s->txflr == 0 ? SR_TFE : 0);
    case R_RISR:
        return dm32_spi_slave_risr(s);
    case R_ISR:
        return dm32_spi_slave_risr(s) & reg(s, R_IMR);
    case R_TXOICR:
        return dm32_spi_slave_clear(s, INT_TXO);
    case R_RXOICR:
        return dm32_spi_slave_clear(s, INT_RXO);
    case R_RXUICR:
        return dm32_spi_slave_clear(s, INT_RXU);
    case R_ICR:
        return dm32_spi_slave_clear(s, INT_LATCHED);
    case R_DR:
        /* No master ever fills the RX FIFO: an empty read underflows. */
        if (reg(s, R_SSIENR) & 1) {
            s->latched |= INT_RXU;
            dm32_spi_slave_update(s);
        }
        return 0;
    default:
        if (!dm32_regfile_read(dm32_spi_slave_regs, NREGS, s->val, off, &v)) {
            qemu_log_mask(LOG_GUEST_ERROR, "dm32_spi_slave: read of "
                          "undefined offset 0x%02" HWADDR_PRIx "\n", off);
        }
        return v;
    }
}

static void dm32_spi_slave_write(void *opaque, hwaddr off, uint64_t val,
                                 unsigned size)
{
    DM32SpiSlaveState *s = opaque;

    switch (off) {
    case R_DR:
        if (!(reg(s, R_SSIENR) & 1)) {
            return;
        }
        if (s->txflr < FIFO_DEPTH) {
            s->txflr++;
        } else {
            s->latched |= INT_TXO;
        }
        break;
    case R_SSIENR:
        dm32_regfile_write(dm32_spi_slave_regs, NREGS, s->val, off, val);
        if (!(val & 1)) {
            s->txflr = 0;
            s->latched = 0;
        }
        break;
    default:
        if (!dm32_regfile_write(dm32_spi_slave_regs, NREGS, s->val, off,
                                val)) {
            qemu_log_mask(LOG_GUEST_ERROR, "dm32_spi_slave: write to "
                          "undefined offset 0x%02" HWADDR_PRIx "\n", off);
        }
        break;
    }
    dm32_spi_slave_update(s);
}

static const MemoryRegionOps dm32_spi_slave_ops = {
    .read = dm32_spi_slave_read,
    .write = dm32_spi_slave_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void dm32_spi_slave_reset(DeviceState *dev)
{
    DM32SpiSlaveState *s = DM32_SPI_SLAVE(dev);

    dm32_regfile_reset(dm32_spi_slave_regs, NREGS, s->val);
    s->txflr = 0;
    s->latched = 0;
    dm32_spi_slave_update(s);
}

static void dm32_spi_slave_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), dm32_spi_slave_reset);
}

static void dm32_spi_slave_init(Object *obj)
{
    DM32SpiSlaveState *s = DM32_SPI_SLAVE(obj);

    memory_region_init_io(&s->iomem, obj, &dm32_spi_slave_ops, s,
                          TYPE_DM32_SPI_SLAVE, WIN);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    dm32_spi_slave_reset(DEVICE(obj));
}

static const TypeInfo dm32_spi_slave_type = {
    .name = TYPE_DM32_SPI_SLAVE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DM32SpiSlaveState),
    .instance_init = dm32_spi_slave_init,
    .class_init = dm32_spi_slave_class_init,
};

static void dm32_spi_slave_register(void)
{
    type_register_static(&dm32_spi_slave_type);
}

type_init(dm32_spi_slave_register)
