/*
 * BrailleNote Apex ("Everest") key controller in the board FPGA.
 *
 * Key events wait in two FIFOs: FIFO 1 holds the braille keyboard (dots 1-8
 * and space), FIFO 2 the other keys. Each entry holds a key number in bits
 * 6..0 and bit 7 set for a release.
 *
 * Host programs send events through the "chardev" as two-byte messages: the
 * FIFO number (1 or 2), then the entry.
 *
 * The controller has one interrupt output. A source becomes pending when an
 * entry arrives in its FIFO and stays pending until the kernel acknowledges
 * it. The kernel enables sources through two registers, disables them through
 * a third, and reads the pending register to tell them apart.
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
#define KEYS_FIFO_SPACING 0x100
#define KEYS_STATUS 0x300
#define KEYS_IRQ_PENDING 0x302
#define KEYS_IRQ_ENABLE 0x304
#define KEYS_IRQ_DISABLE 0x306
#define KEYS_IRQ_ENABLE2 0x30a
#define KEYS_FIFO_COUNT 2
#define KEYS_FIFO_SIZE 64
#define KEYS_MESSAGE_SIZE 2

struct EverestKeysState {
	SysBusDevice parent_obj;

	MemoryRegion iomem;
	qemu_irq irq;
	CharFrontend chr;
	Fifo8 fifos[KEYS_FIFO_COUNT];
	uint8_t message[KEYS_MESSAGE_SIZE];
	uint32_t message_length;
	uint16_t irq_enabled;
	uint16_t irq_pending;
	uint16_t regs[KEYS_NUM_REGS];
};

/* FIFO n (counting from 1) reports in status and interrupt bit n. */
static uint16_t everest_keys_fifo_bit(int index)
{
	return BIT(index + 1);
}

static void everest_keys_update_irq(EverestKeysState *s)
{
	qemu_set_irq(s->irq, (s->irq_pending & s->irq_enabled) != 0);
}

static uint16_t everest_keys_status(EverestKeysState *s)
{
	uint16_t status = 0;
	int i;

	for (i = 0; i < KEYS_FIFO_COUNT; i++) {
		if (!fifo8_is_empty(&s->fifos[i])) {
			status |= everest_keys_fifo_bit(i);
		}
	}
	return status;
}

static uint64_t everest_keys_read(void *opaque, hwaddr offset, unsigned size)
{
	EverestKeysState *s = opaque;
	int index = offset / KEYS_FIFO_SPACING;

	if (offset % KEYS_FIFO_SPACING == 0 && index < KEYS_FIFO_COUNT) {
		return fifo8_is_empty(&s->fifos[index]) ? 0 : fifo8_pop(&s->fifos[index]);
	}
	switch (offset) {
	case KEYS_STATUS:
		return everest_keys_status(s);
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

static void everest_keys_queue(EverestKeysState *s, int index, uint8_t entry)
{
	if (fifo8_is_full(&s->fifos[index])) {
		return;
	}
	fifo8_push(&s->fifos[index], entry);
	s->irq_pending |= everest_keys_fifo_bit(index);
	everest_keys_update_irq(s);
}

static int everest_keys_can_receive(void *opaque)
{
	return KEYS_MESSAGE_SIZE;
}

static void everest_keys_receive(void *opaque, const uint8_t *buf, int size)
{
	EverestKeysState *s = opaque;
	int i;

	for (i = 0; i < size; i++) {
		s->message[s->message_length++] = buf[i];
		if (s->message_length < KEYS_MESSAGE_SIZE) {
			continue;
		}
		s->message_length = 0;
		if (s->message[0] >= 1 && s->message[0] <= KEYS_FIFO_COUNT) {
			everest_keys_queue(s, s->message[0] - 1, s->message[1]);
		}
	}
}

static void everest_keys_reset(DeviceState *dev)
{
	EverestKeysState *s = EVEREST_KEYS(dev);
	int i;

	memset(s->regs, 0, sizeof(s->regs));
	s->irq_enabled = 0;
	s->irq_pending = 0;
	s->message_length = 0;
	for (i = 0; i < KEYS_FIFO_COUNT; i++) {
		fifo8_reset(&s->fifos[i]);
	}
	everest_keys_update_irq(s);
}

static void everest_keys_realize(DeviceState *dev, Error **errp)
{
	EverestKeysState *s = EVEREST_KEYS(dev);
	int i;

	memory_region_init_io(&s->iomem, OBJECT(dev), &everest_keys_ops, s, TYPE_EVEREST_KEYS, KEYS_MMIO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
	sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
	for (i = 0; i < KEYS_FIFO_COUNT; i++) {
		fifo8_create(&s->fifos[i], KEYS_FIFO_SIZE);
	}
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
