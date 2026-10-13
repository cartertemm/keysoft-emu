/*
 * i.MX31 real-time clock
 *
 * The time counter and the alarm interrupt are modelled. The periodic
 * (sampling) interrupts and the stopwatch are not.
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
#include "qemu/timer.h"
#include "qom/object.h"
#include "system/system.h"

#define TYPE_IMX31_RTC "imx31.rtc"
OBJECT_DECLARE_SIMPLE_TYPE(IMX31RTCState, IMX31_RTC)

#define RTC_MMIO_SIZE 0x4000

#define RTC_HOURMIN 0x00
#define RTC_SECONDS 0x04
#define RTC_ALRM_HM 0x08
#define RTC_ALRM_SEC 0x0c
#define RTC_RTCCTL 0x10
#define RTC_RTCISR 0x14
#define RTC_RTCIENR 0x18
#define RTC_STPWCH 0x1c
#define RTC_DAYR 0x20
#define RTC_DAYALARM 0x24

#define RTC_HOUR_SHIFT 8
#define RTC_HOUR_MASK 0x1f
#define RTC_MINUTE_MASK 0x3f
#define RTC_SECOND_MASK 0x3f
#define RTC_DAY_MASK 0xffff
#define RTC_RTCCTL_SWR BIT(0)
#define RTC_INT_ALM BIT(2)

#define SECONDS_PER_MINUTE 60
#define SECONDS_PER_HOUR 3600
#define SECONDS_PER_DAY 86400

struct IMX31RTCState {
	SysBusDevice parent_obj;

	MemoryRegion iomem;
	qemu_irq irq;
	QEMUTimer *alarm_timer;

	int64_t offset;
	uint32_t alarm_hourmin;
	uint32_t alarm_seconds;
	uint32_t alarm_day;
	uint32_t control;
	uint32_t status;
	uint32_t enable;
	uint32_t stopwatch;
};

static int64_t imx31_rtc_seconds_from_fields(uint32_t day, uint32_t hourmin, uint32_t seconds)
{
	return (int64_t)(day & RTC_DAY_MASK) * SECONDS_PER_DAY +
		((hourmin >> RTC_HOUR_SHIFT) & RTC_HOUR_MASK) * SECONDS_PER_HOUR +
		(hourmin & RTC_MINUTE_MASK) * SECONDS_PER_MINUTE +
		(seconds & RTC_SECOND_MASK);
}

static int64_t imx31_rtc_now(IMX31RTCState *s)
{
	return s->offset + qemu_clock_get_ns(rtc_clock) / NANOSECONDS_PER_SECOND;
}

static uint32_t imx31_rtc_hourmin(int64_t time)
{
	int64_t in_day = time % SECONDS_PER_DAY;

	return ((in_day / SECONDS_PER_HOUR) << RTC_HOUR_SHIFT) |
		((in_day % SECONDS_PER_HOUR) / SECONDS_PER_MINUTE);
}

static void imx31_rtc_update(IMX31RTCState *s)
{
	int64_t alarm = imx31_rtc_seconds_from_fields(s->alarm_day, s->alarm_hourmin, s->alarm_seconds);
	int64_t now = imx31_rtc_now(s);

	qemu_set_irq(s->irq, (s->status & s->enable) != 0);
	if ((s->enable & RTC_INT_ALM) && alarm > now) {
		timer_mod(s->alarm_timer, qemu_clock_get_ns(rtc_clock) +
				(alarm - now) * NANOSECONDS_PER_SECOND);
	} else {
		timer_del(s->alarm_timer);
	}
}

static void imx31_rtc_alarm(void *opaque)
{
	IMX31RTCState *s = opaque;

	s->status |= RTC_INT_ALM;
	imx31_rtc_update(s);
}

static void imx31_rtc_set_time(IMX31RTCState *s, uint32_t day, uint32_t hourmin, uint32_t seconds)
{
	s->offset += imx31_rtc_seconds_from_fields(day, hourmin, seconds) - imx31_rtc_now(s);
}

static void imx31_rtc_reset(DeviceState *dev)
{
	IMX31RTCState *s = IMX31_RTC(dev);

	s->alarm_hourmin = 0;
	s->alarm_seconds = 0;
	s->alarm_day = 0;
	s->control = 0;
	s->status = 0;
	s->enable = 0;
	s->stopwatch = 0;
	imx31_rtc_update(s);
}

static uint64_t imx31_rtc_read(void *opaque, hwaddr offset, unsigned size)
{
	IMX31RTCState *s = opaque;
	int64_t now = imx31_rtc_now(s);

	switch (offset) {
	case RTC_HOURMIN:
		return imx31_rtc_hourmin(now);
	case RTC_SECONDS:
		return now % SECONDS_PER_MINUTE;
	case RTC_ALRM_HM:
		return s->alarm_hourmin;
	case RTC_ALRM_SEC:
		return s->alarm_seconds;
	case RTC_RTCCTL:
		return s->control;
	case RTC_RTCISR:
		return s->status;
	case RTC_RTCIENR:
		return s->enable;
	case RTC_STPWCH:
		return s->stopwatch;
	case RTC_DAYR:
		return (now / SECONDS_PER_DAY) & RTC_DAY_MASK;
	case RTC_DAYALARM:
		return s->alarm_day;
	default:
		qemu_log_mask(LOG_GUEST_ERROR, "[%s]%s: Bad register at offset 0x%"
				HWADDR_PRIx "\n", TYPE_IMX31_RTC, __func__, offset);
		return 0;
	}
}

static void imx31_rtc_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
	IMX31RTCState *s = opaque;
	int64_t now = imx31_rtc_now(s);
	uint32_t day = now / SECONDS_PER_DAY;
	uint32_t hourmin = imx31_rtc_hourmin(now);
	uint32_t seconds = now % SECONDS_PER_MINUTE;

	switch (offset) {
	case RTC_HOURMIN:
		imx31_rtc_set_time(s, day, value, seconds);
		break;
	case RTC_SECONDS:
		imx31_rtc_set_time(s, day, hourmin, value);
		break;
	case RTC_ALRM_HM:
		s->alarm_hourmin = value;
		break;
	case RTC_ALRM_SEC:
		s->alarm_seconds = value;
		break;
	case RTC_RTCCTL:
		if (value & RTC_RTCCTL_SWR) {
			imx31_rtc_reset(DEVICE(s));
			return;
		}
		s->control = value;
		break;
	case RTC_RTCISR:
		s->status &= ~value;
		break;
	case RTC_RTCIENR:
		s->enable = value;
		break;
	case RTC_STPWCH:
		s->stopwatch = value;
		break;
	case RTC_DAYR:
		imx31_rtc_set_time(s, value, hourmin, seconds);
		break;
	case RTC_DAYALARM:
		s->alarm_day = value;
		break;
	default:
		qemu_log_mask(LOG_GUEST_ERROR, "[%s]%s: Bad register at offset 0x%"
				HWADDR_PRIx "\n", TYPE_IMX31_RTC, __func__, offset);
		break;
	}
	imx31_rtc_update(s);
}

static const MemoryRegionOps imx31_rtc_ops = {
	.read = imx31_rtc_read,
	.write = imx31_rtc_write,
	.endianness = DEVICE_NATIVE_ENDIAN,
	.valid = {
		.min_access_size = 4,
		.max_access_size = 4,
		.unaligned = false,
	},
};

static const VMStateDescription vmstate_imx31_rtc = {
	.name = TYPE_IMX31_RTC,
	.version_id = 1,
	.minimum_version_id = 1,
	.fields = (const VMStateField[]) {
		VMSTATE_INT64(offset, IMX31RTCState),
		VMSTATE_UINT32(alarm_hourmin, IMX31RTCState),
		VMSTATE_UINT32(alarm_seconds, IMX31RTCState),
		VMSTATE_UINT32(alarm_day, IMX31RTCState),
		VMSTATE_UINT32(control, IMX31RTCState),
		VMSTATE_UINT32(status, IMX31RTCState),
		VMSTATE_UINT32(enable, IMX31RTCState),
		VMSTATE_UINT32(stopwatch, IMX31RTCState),
		VMSTATE_TIMER_PTR(alarm_timer, IMX31RTCState),
		VMSTATE_END_OF_LIST()
	},
};

static void imx31_rtc_realize(DeviceState *dev, Error **errp)
{
	IMX31RTCState *s = IMX31_RTC(dev);

	memory_region_init_io(&s->iomem, OBJECT(dev), &imx31_rtc_ops, s,
			TYPE_IMX31_RTC, RTC_MMIO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
	sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
	s->alarm_timer = timer_new_ns(rtc_clock, imx31_rtc_alarm, s);
}

static void imx31_rtc_class_init(ObjectClass *klass, const void *data)
{
	DeviceClass *dc = DEVICE_CLASS(klass);

	dc->realize = imx31_rtc_realize;
	dc->vmsd = &vmstate_imx31_rtc;
	device_class_set_legacy_reset(dc, imx31_rtc_reset);
	dc->desc = "i.MX31 Real-Time Clock";
}

static const TypeInfo imx31_rtc_info = {
	.name = TYPE_IMX31_RTC,
	.parent = TYPE_SYS_BUS_DEVICE,
	.instance_size = sizeof(IMX31RTCState),
	.class_init = imx31_rtc_class_init,
};

static void imx31_rtc_register_types(void)
{
	type_register_static(&imx31_rtc_info);
}

type_init(imx31_rtc_register_types)
