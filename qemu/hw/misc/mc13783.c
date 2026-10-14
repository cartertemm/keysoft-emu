/*
 * Freescale MC13783 power management and audio IC (SPI register interface)
 *
 * The ADC converts at once and reports the fixed readings below.
 * The "power-off" input is the power switch on the ON1B pin: high turns the
 * device off, low (the default) keeps it on.
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
#define MC13783_REG_INT_SENSE0 2
#define MC13783_REG_INT_STATUS1 3
#define MC13783_REG_INT_MASK1 4
#define MC13783_REG_INT_SENSE1 5
#define MC13783_REG_REVISION 7
#define MC13783_REG_ADC0 43
#define MC13783_REG_ADC1 44
#define MC13783_REG_ADC2 45

#define MC13783_INT0_ADCDONE BIT(0)
#define MC13783_INT1_ONOFD1 BIT(3)
/* The battery is above the low battery high threshold. */
#define MC13783_SENSE0_LOBATHS BIT(14)

#define MC13783_ADC0_ADINC1 BIT(16)
#define MC13783_ADC0_ADINC2 BIT(17)
#define MC13783_ADC1_RAND BIT(1)
#define MC13783_ADC1_ADSEL BIT(3)
#define MC13783_ADC1_ADA1_SHIFT 5
#define MC13783_ADC1_ADA2_SHIFT 8
#define MC13783_ADC1_ADA_LENGTH 3
#define MC13783_ADC1_ASC BIT(20)
#define MC13783_ADC2_ADD1_SHIFT 2
#define MC13783_ADC2_ADD2_SHIFT 14
#define MC13783_ADC_CHANNELS_PER_BANK 8
#define MC13783_ADC_CHANNELS 16
#define MC13783_ADC_ALL_CHANNELS (-1)

/* IC ID 2 (MC13783), silicon revision 3.2 */
#define MC13783_REVISION 0x00009a

/*
 * 10-bit readings of a unit on battery at room temperature: 4.0 V, about
 * 200 mA drawn, 25 C at the battery thermistor, no charger. Unused inputs
 * read half scale.
 */
static const uint16_t mc13783_adc_readings[MC13783_ADC_CHANNELS] = {
	[0] = 714,  /* battery voltage */
	[1] = 36,   /* battery current */
	[2] = 714,  /* application supply */
	[3] = 0,    /* charger voltage */
	[4] = 0,    /* charger current */
	[5] = 87,   /* battery thermistor */
	[6 ... MC13783_ADC_CHANNELS - 1] = 512,
};

struct MC13783State {
	SSIPeripheral parent_obj;

	qemu_irq irq;
	uint32_t regs[MC13783_NUM_REGS];
	/* The channel that a single-channel conversion sampled 8 times. */
	int32_t adc_single_channel;
};

static void mc13783_update_irq(MC13783State *s)
{
	bool pending = (s->regs[MC13783_REG_INT_STATUS0] & ~s->regs[MC13783_REG_INT_MASK0]) ||
		(s->regs[MC13783_REG_INT_STATUS1] & ~s->regs[MC13783_REG_INT_MASK1]);

	qemu_set_irq(s->irq, pending);
}

static int mc13783_adc_channel(uint32_t adc1, int ada_shift)
{
	int channel = extract32(adc1, ada_shift, MC13783_ADC1_ADA_LENGTH);

	if (adc1 & MC13783_ADC1_ADSEL) {
		channel += MC13783_ADC_CHANNELS_PER_BANK;
	}
	return channel;
}

static uint32_t mc13783_adc_reading(MC13783State *s, int ada_shift)
{
	if (s->adc_single_channel != MC13783_ADC_ALL_CHANNELS) {
		return mc13783_adc_readings[s->adc_single_channel];
	}
	return mc13783_adc_readings[mc13783_adc_channel(s->regs[MC13783_REG_ADC1], ada_shift)];
}

static uint32_t mc13783_read(MC13783State *s, int reg)
{
	if (reg == MC13783_REG_ADC2) {
		return mc13783_adc_reading(s, MC13783_ADC1_ADA1_SHIFT) << MC13783_ADC2_ADD1_SHIFT |
			mc13783_adc_reading(s, MC13783_ADC1_ADA2_SHIFT) << MC13783_ADC2_ADD2_SHIFT;
	}
	return s->regs[reg];
}

static void mc13783_adc_advance(MC13783State *s, uint32_t adinc, int ada_shift)
{
	uint32_t *adc1 = &s->regs[MC13783_REG_ADC1];

	if (s->regs[MC13783_REG_ADC0] & adinc) {
		*adc1 = deposit32(*adc1, ada_shift, MC13783_ADC1_ADA_LENGTH,
				extract32(*adc1, ada_shift, MC13783_ADC1_ADA_LENGTH) + 1);
	}
}

/* Reading the result register moves to the next channels when ADC0 asks for it. */
static uint32_t mc13783_read_frame(MC13783State *s, int reg)
{
	uint32_t value = mc13783_read(s, reg);

	if (reg == MC13783_REG_ADC2) {
		mc13783_adc_advance(s, MC13783_ADC0_ADINC1, MC13783_ADC1_ADA1_SHIFT);
		mc13783_adc_advance(s, MC13783_ADC0_ADINC2, MC13783_ADC1_ADA2_SHIFT);
	}
	return value;
}

static uint32_t mc13783_transfer(SSIPeripheral *dev, uint32_t frame)
{
	MC13783State *s = MC13783(dev);
	int reg = extract32(frame, MC13783_FRAME_ADDR_SHIFT, MC13783_FRAME_ADDR_LENGTH);
	uint32_t data = frame & MC13783_DATA_MASK;

	if (!(frame & MC13783_FRAME_WRITE)) {
		return mc13783_read_frame(s, reg);
	}
	switch (reg) {
	case MC13783_REG_INT_STATUS0:
	case MC13783_REG_INT_STATUS1:
		s->regs[reg] &= ~data;
		break;
	case MC13783_REG_INT_SENSE0:
	case MC13783_REG_INT_SENSE1:
	case MC13783_REG_REVISION:
		break;
	case MC13783_REG_ADC1:
		s->regs[reg] = data & ~MC13783_ADC1_ASC;
		if (data & MC13783_ADC1_ASC) {
			s->adc_single_channel = data & MC13783_ADC1_RAND ?
				mc13783_adc_channel(data, MC13783_ADC1_ADA1_SHIFT) : MC13783_ADC_ALL_CHANNELS;
			s->regs[MC13783_REG_INT_STATUS0] |= MC13783_INT0_ADCDONE;
		}
		break;
	default:
		s->regs[reg] = data;
		break;
	}
	mc13783_update_irq(s);
	return mc13783_read(s, reg);
}

/* The sense bit follows the ON1B pin, which is low while the switch is on. */
static void mc13783_power_off(void *opaque, int n, int off)
{
	MC13783State *s = opaque;
	uint32_t sense = off ? MC13783_INT1_ONOFD1 : 0;

	if ((s->regs[MC13783_REG_INT_SENSE1] & MC13783_INT1_ONOFD1) == sense) {
		return;
	}
	s->regs[MC13783_REG_INT_SENSE1] ^= MC13783_INT1_ONOFD1;
	s->regs[MC13783_REG_INT_STATUS1] |= MC13783_INT1_ONOFD1;
	mc13783_update_irq(s);
}

static void mc13783_reset(DeviceState *dev)
{
	MC13783State *s = MC13783(dev);

	memset(s->regs, 0, sizeof(s->regs));
	s->regs[MC13783_REG_INT_MASK0] = MC13783_DATA_MASK;
	s->regs[MC13783_REG_INT_MASK1] = MC13783_DATA_MASK;
	s->regs[MC13783_REG_INT_SENSE0] = MC13783_SENSE0_LOBATHS;
	s->regs[MC13783_REG_REVISION] = MC13783_REVISION;
	s->adc_single_channel = MC13783_ADC_ALL_CHANNELS;
	mc13783_update_irq(s);
}

static void mc13783_realize(SSIPeripheral *dev, Error **errp)
{
	MC13783State *s = MC13783(dev);

	qdev_init_gpio_out(DEVICE(dev), &s->irq, 1);
	qdev_init_gpio_in_named(DEVICE(dev), mc13783_power_off, "power-off", 1);
}

static const VMStateDescription vmstate_mc13783 = {
	.name = TYPE_MC13783,
	.version_id = 2,
	.minimum_version_id = 2,
	.fields = (const VMStateField[]) {
		VMSTATE_SSI_PERIPHERAL(parent_obj, MC13783State),
		VMSTATE_UINT32_ARRAY(regs, MC13783State, MC13783_NUM_REGS),
		VMSTATE_INT32(adc_single_channel, MC13783State),
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
