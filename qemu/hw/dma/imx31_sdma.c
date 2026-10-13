/*
 * i.MX31 Smart DMA controller
 *
 * Only the command channel (channel 0) completes. Script execution and data
 * channels are not modelled.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "system/address-spaces.h"

#define TYPE_IMX31_SDMA "imx31.sdma"
OBJECT_DECLARE_SIMPLE_TYPE(IMX31SDMAState, IMX31_SDMA)

#define SDMA_MMIO_SIZE 0x4000
#define SDMA_NUM_REGS (0x180 / 4)
#define SDMA_NUM_CHANNELS 32
#define SDMA_COMMAND_CHANNEL 0

#define SDMA_MC0PTR 0x00
#define SDMA_INTR 0x04
#define SDMA_STOP_STAT 0x08
#define SDMA_HSTART 0x0c
#define SDMA_RESET 0x24

#define SDMA_CCB_SIZE 16
#define SDMA_CCB_BASE_BD_OFFSET 4
#define SDMA_BD_SIZE 8
#define SDMA_BD_EXTENDED_SIZE 12
#define SDMA_BD_DONE BIT(16)
#define SDMA_BD_WRAP BIT(17)
#define SDMA_BD_CONT BIT(18)
#define SDMA_BD_INTR BIT(19)
#define SDMA_BD_EXTD BIT(23)
#define SDMA_MAX_BDS 256

struct IMX31SDMAState {
	SysBusDevice parent_obj;

	MemoryRegion iomem;
	qemu_irq irq;
	uint32_t regs[SDMA_NUM_REGS];
};

static uint32_t imx31_sdma_load(hwaddr addr)
{
	uint32_t value = 0;

	address_space_read(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, &value, sizeof(value));
	return le32_to_cpu(value);
}

static void imx31_sdma_store(hwaddr addr, uint32_t value)
{
	value = cpu_to_le32(value);
	address_space_write(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED, &value, sizeof(value));
}

static void imx31_sdma_update_irq(IMX31SDMAState *s)
{
	qemu_set_irq(s->irq, s->regs[SDMA_INTR / 4] != 0);
}

static void imx31_sdma_complete_channel(IMX31SDMAState *s, int channel)
{
	hwaddr ccb = s->regs[SDMA_MC0PTR / 4] + channel * SDMA_CCB_SIZE;
	hwaddr bd = imx31_sdma_load(ccb + SDMA_CCB_BASE_BD_OFFSET);
	bool interrupt = false;
	int i;

	for (i = 0; i < SDMA_MAX_BDS; i++) {
		uint32_t mode = imx31_sdma_load(bd);

		imx31_sdma_store(bd, mode & ~SDMA_BD_DONE);
		interrupt |= (mode & SDMA_BD_INTR) != 0;
		if ((mode & SDMA_BD_WRAP) || !(mode & SDMA_BD_CONT)) {
			break;
		}
		bd += (mode & SDMA_BD_EXTD) ? SDMA_BD_EXTENDED_SIZE : SDMA_BD_SIZE;
	}
	if (interrupt) {
		s->regs[SDMA_INTR / 4] |= BIT(channel);
	}
}

static void imx31_sdma_start(IMX31SDMAState *s, uint32_t channels)
{
	if (channels & BIT(SDMA_COMMAND_CHANNEL)) {
		imx31_sdma_complete_channel(s, SDMA_COMMAND_CHANNEL);
		channels &= ~BIT(SDMA_COMMAND_CHANNEL);
	}
	if (channels) {
		qemu_log_mask(LOG_UNIMP, "[%s]%s: data channels 0x%08x not implemented\n",
				TYPE_IMX31_SDMA, __func__, channels);
		s->regs[SDMA_STOP_STAT / 4] |= channels;
	}
}

static void imx31_sdma_reset(DeviceState *dev)
{
	IMX31SDMAState *s = IMX31_SDMA(dev);

	memset(s->regs, 0, sizeof(s->regs));
	imx31_sdma_update_irq(s);
}

static uint64_t imx31_sdma_read(void *opaque, hwaddr offset, unsigned size)
{
	IMX31SDMAState *s = opaque;

	if (offset / 4 >= SDMA_NUM_REGS) {
		qemu_log_mask(LOG_GUEST_ERROR, "[%s]%s: Bad register at offset 0x%"
				HWADDR_PRIx "\n", TYPE_IMX31_SDMA, __func__, offset);
		return 0;
	}
	return s->regs[offset / 4];
}

static void imx31_sdma_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
	IMX31SDMAState *s = opaque;

	if (offset / 4 >= SDMA_NUM_REGS) {
		qemu_log_mask(LOG_GUEST_ERROR, "[%s]%s: Bad register at offset 0x%"
				HWADDR_PRIx "\n", TYPE_IMX31_SDMA, __func__, offset);
		return;
	}
	switch (offset) {
	case SDMA_INTR:
	case SDMA_STOP_STAT:
		s->regs[offset / 4] &= ~value;
		break;
	case SDMA_HSTART:
		imx31_sdma_start(s, value);
		break;
	case SDMA_RESET:
		if (value & 1) {
			imx31_sdma_reset(DEVICE(s));
		}
		break;
	default:
		s->regs[offset / 4] = value;
		break;
	}
	imx31_sdma_update_irq(s);
}

static const MemoryRegionOps imx31_sdma_ops = {
	.read = imx31_sdma_read,
	.write = imx31_sdma_write,
	.endianness = DEVICE_NATIVE_ENDIAN,
	.valid = {
		.min_access_size = 4,
		.max_access_size = 4,
		.unaligned = false,
	},
};

static const VMStateDescription vmstate_imx31_sdma = {
	.name = TYPE_IMX31_SDMA,
	.version_id = 1,
	.minimum_version_id = 1,
	.fields = (const VMStateField[]) {
		VMSTATE_UINT32_ARRAY(regs, IMX31SDMAState, SDMA_NUM_REGS),
		VMSTATE_END_OF_LIST()
	},
};

static void imx31_sdma_realize(DeviceState *dev, Error **errp)
{
	IMX31SDMAState *s = IMX31_SDMA(dev);

	memory_region_init_io(&s->iomem, OBJECT(dev), &imx31_sdma_ops, s,
			TYPE_IMX31_SDMA, SDMA_MMIO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
	sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static void imx31_sdma_class_init(ObjectClass *klass, const void *data)
{
	DeviceClass *dc = DEVICE_CLASS(klass);

	dc->realize = imx31_sdma_realize;
	dc->vmsd = &vmstate_imx31_sdma;
	device_class_set_legacy_reset(dc, imx31_sdma_reset);
	dc->desc = "i.MX31 Smart DMA Controller";
}

static const TypeInfo imx31_sdma_info = {
	.name = TYPE_IMX31_SDMA,
	.parent = TYPE_SYS_BUS_DEVICE,
	.instance_size = sizeof(IMX31SDMAState),
	.class_init = imx31_sdma_class_init,
};

static void imx31_sdma_register_types(void)
{
	type_register_static(&imx31_sdma_info);
}

type_init(imx31_sdma_register_types)
