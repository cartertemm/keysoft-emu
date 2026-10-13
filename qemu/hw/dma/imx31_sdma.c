/*
 * i.MX31 Smart DMA controller
 *
 * Scripts are not executed. The command channel (channel 0) completes at
 * once. A data channel copies its buffer descriptors to the peripheral address
 * from its context each time one of its DMA events fires, which is what the
 * ROM scripts for memory-to-peripheral transfers do.
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
#define SDMA_NUM_EVENTS 32
#define SDMA_CHNENBL 0x80

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
#define SDMA_BD_COUNT_MASK 0xffff
#define SDMA_BD_COMMAND_SHIFT 24
#define SDMA_BD_BUFFER_OFFSET 4
#define SDMA_MAX_BDS 256

#define SDMA_C0_SETCTX 0x07
#define SDMA_C0_COMMAND_MASK 0x07
#define SDMA_C0_CHANNEL_SHIFT 3
#define SDMA_CONTEXT_PER_ADDR_OFFSET (4 * 4)
#define SDMA_CONTEXT_SHP_ADDR_OFFSET (8 * 4)
#define SDMA_CONTEXT_WATERMARK_OFFSET (9 * 4)
#define SDMA_DATA_SIZE_MASK 0x3
#define SDMA_WORD_BYTES 4
#define SDMA_DEFAULT_BURST_BYTES 4

typedef struct IMX31SDMAChannel {
	uint32_t peripheral;
	uint32_t burst_bytes;
	uint32_t bd_offset;
} IMX31SDMAChannel;

struct IMX31SDMAState {
	SysBusDevice parent_obj;

	MemoryRegion iomem;
	qemu_irq irq;
	uint32_t regs[SDMA_NUM_REGS];
	IMX31SDMAChannel channels[SDMA_NUM_CHANNELS];
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

static void imx31_sdma_set_context(IMX31SDMAState *s, int channel, hwaddr context)
{
	IMX31SDMAChannel *c = &s->channels[channel];
	uint32_t shared_peripheral = imx31_sdma_load(context + SDMA_CONTEXT_SHP_ADDR_OFFSET);
	uint32_t watermark = imx31_sdma_load(context + SDMA_CONTEXT_WATERMARK_OFFSET);

	c->peripheral = shared_peripheral ? shared_peripheral :
		imx31_sdma_load(context + SDMA_CONTEXT_PER_ADDR_OFFSET);
	c->burst_bytes = watermark ? watermark : SDMA_DEFAULT_BURST_BYTES;
	c->bd_offset = 0;
}

static void imx31_sdma_command(IMX31SDMAState *s, hwaddr bd, uint32_t mode)
{
	uint32_t command = mode >> SDMA_BD_COMMAND_SHIFT;

	if ((command & SDMA_C0_COMMAND_MASK) == SDMA_C0_SETCTX) {
		imx31_sdma_set_context(s, (command >> SDMA_C0_CHANNEL_SHIFT) % SDMA_NUM_CHANNELS,
				imx31_sdma_load(bd + SDMA_BD_BUFFER_OFFSET));
	}
}

/* Returns the bytes moved, or 0 when the host has not handed over a buffer. */
static uint32_t imx31_sdma_transfer_word(IMX31SDMAState *s, int channel)
{
	IMX31SDMAChannel *c = &s->channels[channel];
	hwaddr ccb = s->regs[SDMA_MC0PTR / 4] + channel * SDMA_CCB_SIZE;
	hwaddr bd = imx31_sdma_load(ccb);
	uint32_t mode = imx31_sdma_load(bd);
	uint32_t data_size = (mode >> SDMA_BD_COMMAND_SHIFT) & SDMA_DATA_SIZE_MASK;
	uint32_t bytes = data_size ? data_size : SDMA_WORD_BYTES;
	uint32_t word = 0;

	if (!(mode & SDMA_BD_DONE) || !c->peripheral) {
		return 0;
	}
	address_space_read(&address_space_memory, imx31_sdma_load(bd + SDMA_BD_BUFFER_OFFSET) + c->bd_offset,
			MEMTXATTRS_UNSPECIFIED, &word, bytes);
	imx31_sdma_store(c->peripheral, le32_to_cpu(word));
	c->bd_offset += bytes;
	if (c->bd_offset < (mode & SDMA_BD_COUNT_MASK)) {
		return bytes;
	}
	c->bd_offset = 0;
	imx31_sdma_store(bd, mode & ~SDMA_BD_DONE);
	if (mode & SDMA_BD_INTR) {
		s->regs[SDMA_INTR / 4] |= BIT(channel);
	}
	if (mode & SDMA_BD_WRAP) {
		bd = imx31_sdma_load(ccb + SDMA_CCB_BASE_BD_OFFSET);
	} else {
		bd += (mode & SDMA_BD_EXTD) ? SDMA_BD_EXTENDED_SIZE : SDMA_BD_SIZE;
	}
	imx31_sdma_store(ccb, bd);
	return bytes;
}

static void imx31_sdma_event(void *opaque, int event, int level)
{
	IMX31SDMAState *s = opaque;
	uint32_t channels = s->regs[(SDMA_CHNENBL + event * 4) / 4] & s->regs[SDMA_STOP_STAT / 4];
	int channel;
	uint32_t moved, total;

	if (!level) {
		return;
	}
	for (channel = 1; channel < SDMA_NUM_CHANNELS; channel++) {
		if (!(channels & BIT(channel))) {
			continue;
		}
		for (total = 0; total < s->channels[channel].burst_bytes; total += moved) {
			moved = imx31_sdma_transfer_word(s, channel);
			if (!moved) {
				break;
			}
		}
	}
	imx31_sdma_update_irq(s);
}

static void imx31_sdma_complete_channel(IMX31SDMAState *s, int channel)
{
	hwaddr ccb = s->regs[SDMA_MC0PTR / 4] + channel * SDMA_CCB_SIZE;
	hwaddr bd = imx31_sdma_load(ccb + SDMA_CCB_BASE_BD_OFFSET);
	bool interrupt = false;
	int i;

	for (i = 0; i < SDMA_MAX_BDS; i++) {
		uint32_t mode = imx31_sdma_load(bd);

		imx31_sdma_command(s, bd, mode);
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
	s->regs[SDMA_STOP_STAT / 4] |= channels;
}

static void imx31_sdma_reset(DeviceState *dev)
{
	IMX31SDMAState *s = IMX31_SDMA(dev);

	memset(s->regs, 0, sizeof(s->regs));
	memset(s->channels, 0, sizeof(s->channels));
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
	qdev_init_gpio_in(dev, imx31_sdma_event, SDMA_NUM_EVENTS);
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
