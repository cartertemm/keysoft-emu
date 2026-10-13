/*
 * Freescale MC13783 power management and audio IC (SPI register interface)
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/ssi/ssi.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_MC13783 "mc13783"
OBJECT_DECLARE_SIMPLE_TYPE(MC13783State, MC13783)

#define MC13783_NUM_REGS 64
#define MC13783_FRAME_WRITE BIT(31)
#define MC13783_FRAME_ADDR_SHIFT 25
#define MC13783_FRAME_ADDR_LENGTH 6
#define MC13783_DATA_MASK 0x00ffffff

#define MC13783_REG_INT_STATUS0 0
#define MC13783_REG_INT_MASK0 1
#define MC13783_REG_INT_STATUS1 3
#define MC13783_REG_INT_MASK1 4
#define MC13783_REG_REVISION 7

/* IC ID 2 (MC13783), silicon revision 3.2 */
#define MC13783_REVISION 0x00009a

struct MC13783State {
	SSIPeripheral parent_obj;

	qemu_irq irq;
	uint32_t regs[MC13783_NUM_REGS];
};

static void mc13783_update_irq(MC13783State *s)
{
	bool pending = (s->regs[MC13783_REG_INT_STATUS0] & ~s->regs[MC13783_REG_INT_MASK0]) ||
		(s->regs[MC13783_REG_INT_STATUS1] & ~s->regs[MC13783_REG_INT_MASK1]);

	qemu_set_irq(s->irq, pending);
}

static uint32_t mc13783_transfer(SSIPeripheral *dev, uint32_t frame)
{
	MC13783State *s = MC13783(dev);
	int reg = extract32(frame, MC13783_FRAME_ADDR_SHIFT, MC13783_FRAME_ADDR_LENGTH);
	uint32_t data = frame & MC13783_DATA_MASK;

	if (!(frame & MC13783_FRAME_WRITE)) {
		return s->regs[reg];
	}
	switch (reg) {
	case MC13783_REG_INT_STATUS0:
	case MC13783_REG_INT_STATUS1:
		s->regs[reg] &= ~data;
		break;
	case MC13783_REG_REVISION:
		break;
	default:
		s->regs[reg] = data;
		break;
	}
	mc13783_update_irq(s);
	return s->regs[reg];
}

static void mc13783_reset(DeviceState *dev)
{
	MC13783State *s = MC13783(dev);

	memset(s->regs, 0, sizeof(s->regs));
	s->regs[MC13783_REG_INT_MASK0] = MC13783_DATA_MASK;
	s->regs[MC13783_REG_INT_MASK1] = MC13783_DATA_MASK;
	s->regs[MC13783_REG_REVISION] = MC13783_REVISION;
	mc13783_update_irq(s);
}

static void mc13783_realize(SSIPeripheral *dev, Error **errp)
{
	MC13783State *s = MC13783(dev);

	qdev_init_gpio_out(DEVICE(dev), &s->irq, 1);
}

static const VMStateDescription vmstate_mc13783 = {
	.name = TYPE_MC13783,
	.version_id = 1,
	.minimum_version_id = 1,
	.fields = (const VMStateField[]) {
		VMSTATE_SSI_PERIPHERAL(parent_obj, MC13783State),
		VMSTATE_UINT32_ARRAY(regs, MC13783State, MC13783_NUM_REGS),
		VMSTATE_END_OF_LIST()
	},
};

static void mc13783_class_init(ObjectClass *klass, const void *data)
{
	DeviceClass *dc = DEVICE_CLASS(klass);
	SSIPeripheralClass *spc = SSI_PERIPHERAL_CLASS(klass);

	spc->realize = mc13783_realize;
	spc->transfer = mc13783_transfer;
	spc->cs_polarity = SSI_CS_HIGH;
	dc->vmsd = &vmstate_mc13783;
	device_class_set_legacy_reset(dc, mc13783_reset);
	dc->desc = "Freescale MC13783 PMIC";
}

static const TypeInfo mc13783_info = {
	.name = TYPE_MC13783,
	.parent = TYPE_SSI_PERIPHERAL,
	.instance_size = sizeof(MC13783State),
	.class_init = mc13783_class_init,
};

static void mc13783_register_types(void)
{
	type_register_static(&mc13783_info);
}

type_init(mc13783_register_types)
