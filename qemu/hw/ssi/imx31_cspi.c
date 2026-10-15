/*
 * i.MX31 CSPI controller
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/fifo32.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_IMX31_CSPI "imx31.cspi"
OBJECT_DECLARE_SIMPLE_TYPE(IMX31CSPIState, IMX31_CSPI)

#define CSPI_MMIO_SIZE 0x1000
#define CSPI_FIFO_SIZE 8
#define CSPI_NUM_CS 4

#define CSPI_RXDATA 0x00
#define CSPI_TXDATA 0x04
#define CSPI_CONREG 0x08
#define CSPI_INTREG 0x0c
#define CSPI_DMAREG 0x10
#define CSPI_STATREG 0x14
#define CSPI_PERIODREG 0x18
#define CSPI_TESTREG 0x1c

#define CSPI_CONREG_EN BIT(0)
#define CSPI_CONREG_MODE BIT(1)
#define CSPI_CONREG_XCH BIT(2)
#define CSPI_CONREG_SMC BIT(3)
#define CSPI_CONREG_SSPOL BIT(7)
#define CSPI_CONREG_BITCOUNT_SHIFT 8
#define CSPI_CONREG_BITCOUNT_LENGTH 5
#define CSPI_CONREG_CS_SHIFT 24
#define CSPI_CONREG_CS_LENGTH 2

#define CSPI_STATREG_TE BIT(0)
#define CSPI_STATREG_TH BIT(1)
#define CSPI_STATREG_TF BIT(2)
#define CSPI_STATREG_RR BIT(3)
#define CSPI_STATREG_RH BIT(4)
#define CSPI_STATREG_RF BIT(5)
#define CSPI_STATREG_RO BIT(6)
#define CSPI_STATREG_BO BIT(7)
#define CSPI_STATREG_TC BIT(8)
#define CSPI_STATREG_W1C (CSPI_STATREG_RO | CSPI_STATREG_BO | CSPI_STATREG_TC)

struct IMX31CSPIState {
	SysBusDevice parent_obj;

	MemoryRegion iomem;
	qemu_irq irq;
	qemu_irq cs_lines[CSPI_NUM_CS];
	SSIBus *bus;

	uint32_t conreg;
	uint32_t intreg;
	uint32_t dmareg;
	uint32_t periodreg;
	uint32_t testreg;
	uint32_t sticky_status;
	bool burst;
	Fifo32 tx_fifo;
	Fifo32 rx_fifo;
};

static uint32_t imx31_cspi_status(IMX31CSPIState *s)
{
	uint32_t status = s->sticky_status;
	uint32_t tx_used = fifo32_num_used(&s->tx_fifo);
	uint32_t rx_used = fifo32_num_used(&s->rx_fifo);

	if (tx_used == 0) {
		status |= CSPI_STATREG_TE;
	}
	if (tx_used <= CSPI_FIFO_SIZE / 2) {
		status |= CSPI_STATREG_TH;
	}
	if (tx_used == CSPI_FIFO_SIZE) {
		status |= CSPI_STATREG_TF;
	}
	if (rx_used > 0) {
		status |= CSPI_STATREG_RR;
	}
	if (rx_used >= CSPI_FIFO_SIZE / 2) {
		status |= CSPI_STATREG_RH;
	}
	if (rx_used == CSPI_FIFO_SIZE) {
		status |= CSPI_STATREG_RF;
	}
	return status;
}

static void imx31_cspi_update_irq(IMX31CSPIState *s)
{
	qemu_set_irq(s->irq, (imx31_cspi_status(s) & s->intreg) != 0);
}

static void imx31_cspi_set_cs(IMX31CSPIState *s, bool selected)
{
	int active_level = (s->conreg & CSPI_CONREG_SSPOL) != 0;
	int channel = extract32(s->conreg, CSPI_CONREG_CS_SHIFT, CSPI_CONREG_CS_LENGTH);
	int i;

	for (i = 0; i < CSPI_NUM_CS; i++) {
		bool active = selected && i == channel;

		qemu_set_irq(s->cs_lines[i], active ? active_level : !active_level);
	}
}

static void imx31_cspi_exchange(IMX31CSPIState *s)
{
	int bits = extract32(s->conreg, CSPI_CONREG_BITCOUNT_SHIFT, CSPI_CONREG_BITCOUNT_LENGTH) + 1;
	uint32_t mask = MAKE_64BIT_MASK(0, bits);

	imx31_cspi_set_cs(s, true);
	while (!fifo32_is_empty(&s->tx_fifo)) {
		uint32_t rx = ssi_transfer(s->bus, fifo32_pop(&s->tx_fifo) & mask) & mask;

		if (fifo32_is_full(&s->rx_fifo)) {
			s->sticky_status |= CSPI_STATREG_RO;
		} else {
			fifo32_push(&s->rx_fifo, rx);
		}
	}
	imx31_cspi_set_cs(s, false);
	s->conreg &= ~CSPI_CONREG_XCH;
	s->sticky_status |= CSPI_STATREG_TC;
}

static bool imx31_cspi_is_active_master(IMX31CSPIState *s)
{
	uint32_t required = CSPI_CONREG_EN | CSPI_CONREG_MODE;

	return (s->conreg & required) == required;
}

static void imx31_cspi_reset(DeviceState *dev)
{
	IMX31CSPIState *s = IMX31_CSPI(dev);

	s->conreg = 0;
	s->intreg = 0;
	s->dmareg = 0;
	s->periodreg = 0;
	s->testreg = 0;
	s->sticky_status = 0;
	s->burst = false;
	fifo32_reset(&s->tx_fifo);
	fifo32_reset(&s->rx_fifo);
	imx31_cspi_update_irq(s);
}

static uint64_t imx31_cspi_read(void *opaque, hwaddr offset, unsigned size)
{
	IMX31CSPIState *s = opaque;
	uint32_t value = 0;

	switch (offset) {
	case CSPI_RXDATA:
		if (!fifo32_is_empty(&s->rx_fifo)) {
			value = fifo32_pop(&s->rx_fifo);
		}
		break;
	case CSPI_CONREG:
		value = s->conreg;
		break;
	case CSPI_INTREG:
		value = s->intreg;
		break;
	case CSPI_DMAREG:
		value = s->dmareg;
		break;
	case CSPI_STATREG:
		value = imx31_cspi_status(s);
		break;
	case CSPI_PERIODREG:
		value = s->periodreg;
		break;
	case CSPI_TESTREG:
		value = s->testreg;
		break;
	default:
		qemu_log_mask(LOG_GUEST_ERROR, "[%s]%s: Bad register at offset 0x%"
				HWADDR_PRIx "\n", TYPE_IMX31_CSPI, __func__, offset);
		break;
	}
	imx31_cspi_update_irq(s);
	return value;
}

static void imx31_cspi_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
	IMX31CSPIState *s = opaque;

	switch (offset) {
	case CSPI_TXDATA:
		if (fifo32_is_full(&s->tx_fifo)) {
			break;
		}
		fifo32_push(&s->tx_fifo, value);
		if (((s->conreg & CSPI_CONREG_SMC) || s->burst) && imx31_cspi_is_active_master(s)) {
			imx31_cspi_exchange(s);
		}
		break;
	case CSPI_CONREG:
		s->conreg = value;
		s->burst = false;
		if (!(value & CSPI_CONREG_EN)) {
			fifo32_reset(&s->tx_fifo);
			fifo32_reset(&s->rx_fifo);
			s->sticky_status = 0;
			s->conreg &= ~CSPI_CONREG_XCH;
		} else if ((value & CSPI_CONREG_XCH) && imx31_cspi_is_active_master(s)) {
			/* Words written while the exchange runs go out in the same burst. */
			s->burst = true;
			imx31_cspi_exchange(s);
		}
		break;
	case CSPI_INTREG:
		s->intreg = value;
		break;
	case CSPI_DMAREG:
		s->dmareg = value;
		break;
	case CSPI_STATREG:
		s->sticky_status &= ~(value & CSPI_STATREG_W1C);
		/* Clearing the transfer complete flag means the guest saw the burst end. */
		if (value & CSPI_STATREG_TC) {
			s->burst = false;
		}
		break;
	case CSPI_PERIODREG:
		s->periodreg = value;
		break;
	case CSPI_TESTREG:
		s->testreg = value;
		break;
	default:
		qemu_log_mask(LOG_GUEST_ERROR, "[%s]%s: Bad register at offset 0x%"
				HWADDR_PRIx "\n", TYPE_IMX31_CSPI, __func__, offset);
		break;
	}
	imx31_cspi_update_irq(s);
}

static const MemoryRegionOps imx31_cspi_ops = {
	.read = imx31_cspi_read,
	.write = imx31_cspi_write,
	.endianness = DEVICE_NATIVE_ENDIAN,
	.valid = {
		.min_access_size = 4,
		.max_access_size = 4,
		.unaligned = false,
	},
};

static const VMStateDescription vmstate_imx31_cspi = {
	.name = TYPE_IMX31_CSPI,
	.version_id = 1,
	.minimum_version_id = 1,
	.fields = (const VMStateField[]) {
		VMSTATE_UINT32(conreg, IMX31CSPIState),
		VMSTATE_UINT32(intreg, IMX31CSPIState),
		VMSTATE_UINT32(dmareg, IMX31CSPIState),
		VMSTATE_UINT32(periodreg, IMX31CSPIState),
		VMSTATE_UINT32(testreg, IMX31CSPIState),
		VMSTATE_UINT32(sticky_status, IMX31CSPIState),
		VMSTATE_FIFO32(tx_fifo, IMX31CSPIState),
		VMSTATE_FIFO32(rx_fifo, IMX31CSPIState),
		VMSTATE_END_OF_LIST()
	},
};

static void imx31_cspi_realize(DeviceState *dev, Error **errp)
{
	IMX31CSPIState *s = IMX31_CSPI(dev);
	int i;

	s->bus = ssi_create_bus(dev, "spi");
	memory_region_init_io(&s->iomem, OBJECT(dev), &imx31_cspi_ops, s,
			TYPE_IMX31_CSPI, CSPI_MMIO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
	sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
	for (i = 0; i < CSPI_NUM_CS; i++) {
		sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->cs_lines[i]);
	}
	fifo32_create(&s->tx_fifo, CSPI_FIFO_SIZE);
	fifo32_create(&s->rx_fifo, CSPI_FIFO_SIZE);
}

static void imx31_cspi_class_init(ObjectClass *klass, const void *data)
{
	DeviceClass *dc = DEVICE_CLASS(klass);

	dc->realize = imx31_cspi_realize;
	dc->vmsd = &vmstate_imx31_cspi;
	device_class_set_legacy_reset(dc, imx31_cspi_reset);
	dc->desc = "i.MX31 CSPI Controller";
}

static const TypeInfo imx31_cspi_info = {
	.name = TYPE_IMX31_CSPI,
	.parent = TYPE_SYS_BUS_DEVICE,
	.instance_size = sizeof(IMX31CSPIState),
	.class_init = imx31_cspi_class_init,
};

static void imx31_cspi_register_types(void)
{
	type_register_static(&imx31_cspi_info);
}

type_init(imx31_cspi_register_types)
