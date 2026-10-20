/*
 * Freescale MC13783 power management and audio IC (SPI register interface)
 *
 * The ADC converts at once. The battery and charger readings follow the host
 * battery and AC power status, which the chip polls every few seconds.
 * The "power-off" input is the power switch on the ON1B pin: high turns the
 * device off, low (the default) keeps it on.
 *
 * KeySoft's battery driver counts the charge that goes in and out of the
 * battery, so the battery current tells it how fast the host battery drains
 * or charges. It checks the battery voltage at boot and at low levels.
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
#include "qemu/timer.h"
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
#define MC13783_INT0_CHGDETI BIT(6)
#define MC13783_INT1_ONOFD1 BIT(3)
/* A charger is connected. */
#define MC13783_SENSE0_CHGDETS BIT(6)
/* The charge current is above the threshold. */
#define MC13783_SENSE0_CHGCURRS BIT(11)
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

#define MC13783_ADC_BATTERY_V 0
#define MC13783_ADC_BATTERY_I 1
#define MC13783_ADC_SUPPLY_V 2
#define MC13783_ADC_CHARGER_V 3
#define MC13783_ADC_CHARGER_I 4

/*
 * Scales that KeySoft's battery driver uses to convert 10-bit readings:
 * battery voltage 2400 to 4700 mV, current 0 to 5750 mA (two's complement,
 * negative while charging), charger voltage 0 to 11500 mV.
 */
#define MC13783_ADC_FULL_SCALE 1023
#define MC13783_ADC_MAX_MAGNITUDE 511
#define MC13783_ADC_SIGNED_RANGE 1024
#define MC13783_BATTERY_MV_MIN 2400
#define MC13783_BATTERY_MV_SPAN 2300
#define MC13783_CURRENT_MA_SPAN 5750
#define MC13783_CHARGER_MV_SPAN 11500
#define MC13783_MAX_MA (MC13783_ADC_MAX_MAGNITUDE * MC13783_CURRENT_MA_SPAN / MC13783_ADC_FULL_SCALE)
/* On battery, the driver adds 1 mV for each 8 mA drawn. */
#define MC13783_MA_PER_MV_DROP 8

/* IC ID 2 (MC13783), silicon revision 3.2 */
#define MC13783_REVISION 0x00009a

#define MC13783_HOST_POLL_MS 5000
#define MC13783_CHARGER_MV 5000
/* On AC power, KeySoft starts a charge at 4100 mV or less. */
#define MC13783_AC_MAX_BATTERY_MV 4090

/*
 * KeySoft's battery holds 2400 mAh, so 1 percent is 86400 mA s. It counts 98
 * percent of the charge current. It ignores currents below 30 mA and ends a
 * charge when the current falls below 55 mA. It sets 100 percent at the end
 * of a charge only if it counted the current, so a full battery draws 40 mA.
 */
#define MC13783_MA_S_PER_PERCENT 86400
#define MC13783_CHARGE_EFFICIENCY 98
#define MC13783_MIN_DISCHARGE_MA 30
#define MC13783_FULL_CHARGE_MA 40
#define MC13783_MIN_CHARGE_MA 60
#define MC13783_DEFAULT_DISCHARGE_MA 200
#define MC13783_DEFAULT_CHARGE_MA 500

/*
 * Battery voltage for each host percent. The bends match the voltages that
 * KeySoft's driver checks: it reads 10 percent or less from 3700 mV down,
 * 3 percent or less from 3620 mV down, and a battery at 4000 mV or more as
 * 90 percent or more. It calls a battery above 4100 mV high. Rounding can
 * make the driver read up to 3 mV high, so the points stay that far inside
 * these limits.
 */
static const struct {
	int percent;
	int mv;
} mc13783_battery_curve[] = {
	{ 0, 3500 },
	{ 3, 3617 },
	{ 10, 3701 },
	{ 60, 3800 },
	{ 90, 4000 },
	{ 100, 4200 },
};

/*
 * 10-bit readings of the inputs that do not follow the host: 25 C at the
 * battery thermistor. Unused inputs read half scale.
 */
static const uint16_t mc13783_adc_defaults[MC13783_ADC_CHANNELS] = {
	[5] = 87,   /* battery thermistor */
	[6 ... MC13783_ADC_CHANNELS - 1] = 512,
};

typedef struct MC13783HostPower {
	bool ac;
	bool charging;
	int percent;
	/* Seconds of battery time left, or -1 if the host does not know. */
	int seconds_left;
} MC13783HostPower;

struct MC13783State {
	SSIPeripheral parent_obj;

	qemu_irq irq;
	uint32_t regs[MC13783_NUM_REGS];
	/* The channel that a single-channel conversion sampled 8 times. */
	int32_t adc_single_channel;
	uint16_t adc_readings[MC13783_ADC_CHANNELS];

	QEMUTimer *host_timer;
	/* The host percent at the last poll, or -1 before the first poll. */
	int host_percent;
	/* When the host percent last went up while charging, or -1. */
	int64_t charge_rise_ms;
	int charge_ma;
};

/* A host with no battery, or with an unknown status, reads as full on AC. */
static void mc13783_read_host_power(MC13783HostPower *host)
{
	*host = (MC13783HostPower) { .ac = true, .percent = 100, .seconds_left = -1 };
#ifdef _WIN32
	SYSTEM_POWER_STATUS status;
	const BYTE no_battery = 128, charging = 8, unknown = 255;

	if (!GetSystemPowerStatus(&status) || status.BatteryFlag == unknown ||
		(status.BatteryFlag & no_battery) || status.BatteryLifePercent > 100) {
		return;
	}
	host->ac = status.ACLineStatus == 1;
	host->charging = status.BatteryFlag & charging;
	host->percent = status.BatteryLifePercent;
	if (status.BatteryLifeTime != (DWORD)-1) {
		host->seconds_left = status.BatteryLifeTime;
	}
#endif
}

static int mc13783_battery_mv(int percent)
{
	int i;

	for (i = 1; i < ARRAY_SIZE(mc13783_battery_curve) - 1; i++) {
		if (percent <= mc13783_battery_curve[i].percent) {
			break;
		}
	}
	return mc13783_battery_curve[i - 1].mv +
		(percent - mc13783_battery_curve[i - 1].percent) *
		(mc13783_battery_curve[i].mv - mc13783_battery_curve[i - 1].mv) /
		(mc13783_battery_curve[i].percent - mc13783_battery_curve[i - 1].percent);
}

/* Round up, because the driver rounds down when it converts back. */
static uint16_t mc13783_adc_scale(int value, int min, int span)
{
	return MIN(DIV_ROUND_UP(MAX(value - min, 0) * MC13783_ADC_FULL_SCALE, span), MC13783_ADC_FULL_SCALE);
}

/* Positive current flows out of the battery. */
static uint16_t mc13783_adc_current(int ma)
{
	int magnitude = DIV_ROUND_UP(abs(ma) * MC13783_ADC_FULL_SCALE, MC13783_CURRENT_MA_SPAN);

	return ma < 0 ? MC13783_ADC_SIGNED_RANGE - magnitude : magnitude;
}

static int mc13783_rate_ma(int percent, int64_t ms)
{
	return MC13783_MA_S_PER_PERCENT * percent * 1000LL / MAX(ms, 1);
}

/* The host charge rate comes from the time between two rises of its percent. */
static void mc13783_track_charge_rate(MC13783State *s, const MC13783HostPower *host, int64_t now)
{
	if (!host->charging) {
		s->charge_rise_ms = -1;
		s->charge_ma = MC13783_DEFAULT_CHARGE_MA;
	} else if (s->host_percent >= 0 && host->percent > s->host_percent) {
		if (s->charge_rise_ms >= 0) {
			s->charge_ma = mc13783_rate_ma(host->percent - s->host_percent, now - s->charge_rise_ms) *
				100 / MC13783_CHARGE_EFFICIENCY;
		}
		s->charge_rise_ms = now;
	}
	s->host_percent = host->percent;
}

/*
 * On AC power, a full battery draws so little current that KeySoft ends the
 * charge and shows 100 percent. A battery on AC that the host does not charge
 * draws the least current that keeps KeySoft charging.
 */
static int mc13783_battery_ma(MC13783State *s, const MC13783HostPower *host)
{
	if (!host->ac) {
		if (host->seconds_left <= 0) {
			return MC13783_DEFAULT_DISCHARGE_MA;
		}
		return MAX(MC13783_MA_S_PER_PERCENT * host->percent / host->seconds_left,
			MC13783_MIN_DISCHARGE_MA);
	}
	if (host->percent >= 100) {
		return -MC13783_FULL_CHARGE_MA;
	}
	return host->charging ? -MAX(s->charge_ma, MC13783_MIN_CHARGE_MA) : -MC13783_MIN_CHARGE_MA;
}

static void mc13783_update_irq(MC13783State *s)
{
	bool pending = (s->regs[MC13783_REG_INT_STATUS0] & ~s->regs[MC13783_REG_INT_MASK0]) ||
		(s->regs[MC13783_REG_INT_STATUS1] & ~s->regs[MC13783_REG_INT_MASK1]);

	qemu_set_irq(s->irq, pending);
}

static void mc13783_update_power(MC13783State *s)
{
	MC13783HostPower host;
	uint32_t *sense = &s->regs[MC13783_REG_INT_SENSE0];
	uint32_t charger_sense;
	uint16_t battery_v;
	int mv, ma;

	mc13783_read_host_power(&host);
	mc13783_track_charge_rate(s, &host, qemu_clock_get_ms(QEMU_CLOCK_REALTIME));
	ma = MIN(MAX(mc13783_battery_ma(s, &host), -MC13783_MAX_MA), MC13783_MAX_MA);
	mv = mc13783_battery_mv(host.percent);
	if (host.ac) {
		mv = MIN(mv, MC13783_AC_MAX_BATTERY_MV);
	}
	battery_v = mc13783_adc_scale(mv - MAX(ma, 0) / MC13783_MA_PER_MV_DROP, MC13783_BATTERY_MV_MIN,
		MC13783_BATTERY_MV_SPAN);

	s->adc_readings[MC13783_ADC_BATTERY_V] = battery_v;
	s->adc_readings[MC13783_ADC_SUPPLY_V] = battery_v;
	s->adc_readings[MC13783_ADC_BATTERY_I] = mc13783_adc_current(ma);
	s->adc_readings[MC13783_ADC_CHARGER_V] = host.ac ?
		mc13783_adc_scale(MC13783_CHARGER_MV, 0, MC13783_CHARGER_MV_SPAN) : 0;
	s->adc_readings[MC13783_ADC_CHARGER_I] = ma < 0 ? mc13783_adc_current(-ma) : 0;

	charger_sense = host.ac ? MC13783_SENSE0_CHGDETS | MC13783_SENSE0_CHGCURRS : 0;
	if ((*sense & MC13783_SENSE0_CHGDETS) != (charger_sense & MC13783_SENSE0_CHGDETS)) {
		s->regs[MC13783_REG_INT_STATUS0] |= MC13783_INT0_CHGDETI;
	}
	*sense = (*sense & ~(MC13783_SENSE0_CHGDETS | MC13783_SENSE0_CHGCURRS)) | charger_sense;
	mc13783_update_irq(s);
}

static void mc13783_host_poll(void *opaque)
{
	MC13783State *s = opaque;

	mc13783_update_power(s);
	timer_mod(s->host_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + MC13783_HOST_POLL_MS);
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
		return s->adc_readings[s->adc_single_channel];
	}
	return s->adc_readings[mc13783_adc_channel(s->regs[MC13783_REG_ADC1], ada_shift)];
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
	memcpy(s->adc_readings, mc13783_adc_defaults, sizeof(s->adc_readings));
	s->host_percent = -1;
	s->charge_rise_ms = -1;
	s->charge_ma = MC13783_DEFAULT_CHARGE_MA;
	mc13783_host_poll(s);
	s->regs[MC13783_REG_INT_STATUS0] = 0;
	mc13783_update_irq(s);
}

static void mc13783_realize(SSIPeripheral *dev, Error **errp)
{
	MC13783State *s = MC13783(dev);

	qdev_init_gpio_out(DEVICE(dev), &s->irq, 1);
	qdev_init_gpio_in_named(DEVICE(dev), mc13783_power_off, "power-off", 1);
	s->host_timer = timer_new_ms(QEMU_CLOCK_REALTIME, mc13783_host_poll, s);
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
