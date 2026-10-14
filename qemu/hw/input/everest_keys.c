/*
 * BrailleNote Apex ("Everest") key controller in the board FPGA.
 *
 * Key events wait in FIFO 2. Each entry holds a key number in bits 6..0 and
 * bit 7 set for a release. Bytes received on the "chardev" are queued as
 * entries, so a host program can press keys.
 *
 * The controller has one interrupt output. A source becomes pending when an
 * entry arrives in its FIFO and stays pending until the kernel acknowledges it. The kernel enables sources through two registers, disables
 * them through a third, and reads the pending register to tell them apart.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "qemu/bitops.h"
#include "qemu/fifo8.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_EVEREST_KEYS "everest.keys"
OBJECT_DECLARE_SIMPLE_TYPE(EverestKeysState, EVEREST_KEYS)

#define KEYS_MMIO_SIZE 0x400
#define KEYS_NUM_REGS (KEYS_MMIO_SIZE / 2)
#define KEYS_FIFO1_DATA 0x000
#define KEYS_FIFO2_DATA 0x100
#define KEYS_STATUS 0x300
#define KEYS_IRQ_PENDING 0x302
#define KEYS_IRQ_ENABLE 0x304
#define KEYS_IRQ_DISABLE 0x306
#define KEYS_IRQ_ENABLE2 0x30a
#define KEYS_STATUS_FIFO1 BIT(1)
#define KEYS_STATUS_FIFO2 BIT(2)
#define KEYS_FIFO_SIZE 64

struct EverestKeysState {
	SysBusDevice parent_obj;

	MemoryRegion iomem;
	qemu_irq irq;
	CharFrontend chr;
	Fifo8 fifo;
	uint16_t irq_enabled;
	uint16_t irq_pending;
	uint16_t regs[KEYS_NUM_REGS];
};

static void everest_keys_update_irq(EverestKeysState *s)
{
	qemu_set_irq(s->irq, (s->irq_pending & s->irq_enabled) != 0);
}

static uint64_t everest_keys_read(void *opaque, hwaddr offset, unsigned size)
{
	EverestKeysState *s = opaque;
	uint16_t value;

	switch (offset) {
	case KEYS_FIFO2_DATA:
		value = fifo8_is_empty(&s->fifo) ? 0 : fifo8_pop(&s->fifo);
		everest_keys_update_irq(s);
		return value;
	case KEYS_STATUS:
		return fifo8_is_empty(&s->fifo) ? 0 : KEYS_STATUS_FIFO2;
	case KEYS_IRQ_PENDING:
		return s->irq_pending;
	default:
		return s->regs[offset / 2];
	}
}

static void everest_keys_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
	EverestKeysState *s = opaque;

	switch (offset) {
	case KEYS_IRQ_ENABLE:
	case KEYS_IRQ_ENABLE2:
		s->irq_enabled |= value;
		break;
	case KEYS_IRQ_DISABLE:
		s->irq_enabled &= ~value;
		break;
	case KEYS_IRQ_PENDING:
		s->irq_pending &= ~value;
		break;
	default:
		s->regs[offset / 2] = value;
		break;
	}
	everest_keys_update_irq(s);
}

static const MemoryRegionOps everest_keys_ops = {
	.read = everest_keys_read,
	.write = everest_keys_write,
	.endianness = DEVICE_LITTLE_ENDIAN,
	.impl = {
		.min_access_size = 2,
		.max_access_size = 2,
	},
};

static int everest_keys_can_receive(void *opaque)
{
	EverestKeysState *s = opaque;

	return fifo8_num_free(&s->fifo);
}

static void everest_keys_receive(void *opaque, const uint8_t *buf, int size)
{
	EverestKeysState *s = opaque;

	fifo8_push_all(&s->fifo, buf, MIN(size, fifo8_num_free(&s->fifo)));
	s->irq_pending |= KEYS_STATUS_FIFO2;
	everest_keys_update_irq(s);
}

static void everest_keys_reset(DeviceState *dev)
{
	EverestKeysState *s = EVEREST_KEYS(dev);

	memset(s->regs, 0, sizeof(s->regs));
	s->irq_enabled = 0;
	s->irq_pending = 0;
	fifo8_reset(&s->fifo);
	everest_keys_update_irq(s);
}

static void everest_keys_realize(DeviceState *dev, Error **errp)
{
	EverestKeysState *s = EVEREST_KEYS(dev);

	memory_region_init_io(&s->iomem, OBJECT(dev), &everest_keys_ops, s, TYPE_EVEREST_KEYS, KEYS_MMIO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
	sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
	fifo8_create(&s->fifo, KEYS_FIFO_SIZE);
	qemu_chr_fe_set_handlers(&s->chr, everest_keys_can_receive, everest_keys_receive,
			NULL, NULL, s, NULL, true);
}

static const Property everest_keys_properties[] = {
	DEFINE_PROP_CHR("chardev", EverestKeysState, chr),
};

static void everest_keys_class_init(ObjectClass *klass, const void *data)
{
	DeviceClass *dc = DEVICE_CLASS(klass);

	dc->realize = everest_keys_realize;
	device_class_set_legacy_reset(dc, everest_keys_reset);
	device_class_set_props(dc, everest_keys_properties);
	dc->desc = "BrailleNote Apex key controller";
}

static const TypeInfo everest_keys_info = {
	.name = TYPE_EVEREST_KEYS,
	.parent = TYPE_SYS_BUS_DEVICE,
	.instance_size = sizeof(EverestKeysState),
	.class_init = everest_keys_class_init,
};

static void everest_keys_register_types(void)
{
	type_register_static(&everest_keys_info);
}

type_init(everest_keys_register_types)
