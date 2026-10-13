/*
 * i.MX31 SD host controller with an empty card slot.
 *
 * The clock control works and every command ends with a response time-out,
 * which is what the host sees when no card is present.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_IMX31_SDHC "imx31.sdhc"
OBJECT_DECLARE_SIMPLE_TYPE(IMX31SDHCState, IMX31_SDHC)

#define SDHC_MMIO_SIZE 0x4000
#define SDHC_NUM_REGS (0x40 / 4)

#define SDHC_STR_STP_CLK 0x00
#define SDHC_STATUS 0x04
#define SDHC_CMD_DAT_CONT 0x0c
#define SDHC_REV_NO 0x20
#define SDHC_INT_CNTR 0x24

#define SDHC_CLK_STOP BIT(0)
#define SDHC_CLK_START BIT(1)
#define SDHC_CLK_RESET BIT(3)
#define SDHC_STATUS_TIME_OUT_RESP BIT(1)
#define SDHC_STATUS_CLK_RUN BIT(8)
#define SDHC_STATUS_END_CMD_RESP BIT(13)
#define SDHC_STATUS_W1C (~SDHC_STATUS_CLK_RUN)
#define SDHC_INT_END_CMD_RES BIT(2)
#define SDHC_REVISION 0x400

struct IMX31SDHCState {
	SysBusDevice parent_obj;

	MemoryRegion iomem;
	qemu_irq irq;
	uint32_t regs[SDHC_NUM_REGS];
};

static void imx31_sdhc_update_irq(IMX31SDHCState *s)
{
	bool command_done = s->regs[SDHC_STATUS / 4] & SDHC_STATUS_END_CMD_RESP;

	qemu_set_irq(s->irq, command_done && (s->regs[SDHC_INT_CNTR / 4] & SDHC_INT_END_CMD_RES));
}

static void imx31_sdhc_reset(DeviceState *dev)
{
	IMX31SDHCState *s = IMX31_SDHC(dev);

	memset(s->regs, 0, sizeof(s->regs));
	s->regs[SDHC_REV_NO / 4] = SDHC_REVISION;
	imx31_sdhc_update_irq(s);
}

static uint64_t imx31_sdhc_read(void *opaque, hwaddr offset, unsigned size)
{
	IMX31SDHCState *s = opaque;

	return offset / 4 < SDHC_NUM_REGS ? s->regs[offset / 4] : 0;
}

static void imx31_sdhc_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
	IMX31SDHCState *s = opaque;

	if (offset / 4 >= SDHC_NUM_REGS) {
		return;
	}
	switch (offset) {
	case SDHC_STR_STP_CLK:
		if (value & SDHC_CLK_RESET) {
			imx31_sdhc_reset(DEVICE(s));
			return;
		}
		if (value & SDHC_CLK_START) {
			s->regs[SDHC_STATUS / 4] |= SDHC_STATUS_CLK_RUN;
		}
		if (value & SDHC_CLK_STOP) {
			s->regs[SDHC_STATUS / 4] &= ~SDHC_STATUS_CLK_RUN;
		}
		break;
	case SDHC_STATUS:
		s->regs[SDHC_STATUS / 4] &= ~(value & SDHC_STATUS_W1C);
		break;
	case SDHC_CMD_DAT_CONT:
		s->regs[offset / 4] = value;
		s->regs[SDHC_STATUS / 4] |= SDHC_STATUS_END_CMD_RESP | SDHC_STATUS_TIME_OUT_RESP;
		break;
	case SDHC_REV_NO:
		break;
	default:
		s->regs[offset / 4] = value;
		break;
	}
	imx31_sdhc_update_irq(s);
}

static const MemoryRegionOps imx31_sdhc_ops = {
	.read = imx31_sdhc_read,
	.write = imx31_sdhc_write,
	.endianness = DEVICE_NATIVE_ENDIAN,
	.valid = {
		.min_access_size = 4,
		.max_access_size = 4,
		.unaligned = false,
	},
};

static const VMStateDescription vmstate_imx31_sdhc = {
	.name = TYPE_IMX31_SDHC,
	.version_id = 1,
	.minimum_version_id = 1,
	.fields = (const VMStateField[]) {
		VMSTATE_UINT32_ARRAY(regs, IMX31SDHCState, SDHC_NUM_REGS),
		VMSTATE_END_OF_LIST()
	},
};

static void imx31_sdhc_realize(DeviceState *dev, Error **errp)
{
	IMX31SDHCState *s = IMX31_SDHC(dev);

	memory_region_init_io(&s->iomem, OBJECT(dev), &imx31_sdhc_ops, s, TYPE_IMX31_SDHC, SDHC_MMIO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
	sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static void imx31_sdhc_class_init(ObjectClass *klass, const void *data)
{
	DeviceClass *dc = DEVICE_CLASS(klass);

	dc->realize = imx31_sdhc_realize;
	dc->vmsd = &vmstate_imx31_sdhc;
	device_class_set_legacy_reset(dc, imx31_sdhc_reset);
	dc->desc = "i.MX31 SD host controller (empty slot)";
}

static const TypeInfo imx31_sdhc_info = {
	.name = TYPE_IMX31_SDHC,
	.parent = TYPE_SYS_BUS_DEVICE,
	.instance_size = sizeof(IMX31SDHCState),
	.class_init = imx31_sdhc_class_init,
};

static void imx31_sdhc_register_types(void)
{
	type_register_static(&imx31_sdhc_info);
}

type_init(imx31_sdhc_register_types)
