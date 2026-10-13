/*
 * i.MX31 USB module: register blocks for the OTG port and the two host ports.
 *
 * Nothing is ever connected to a port. Controller reset, PHY access, endpoint
 * priming and endpoint flushing complete at once so that the drivers can
 * initialize.
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

#define TYPE_IMX31_USB "imx31.usb"
OBJECT_DECLARE_SIMPLE_TYPE(IMX31USBState, IMX31_USB)

#define USB_MMIO_SIZE 0x1000
#define USB_NUM_REGS (USB_MMIO_SIZE / 4)
#define USB_PORT_SIZE 0x200
#define USB_PORT_COUNT 3

#define USB_OTG_ID 0x000
#define USB_OTG_HCSPARAMS 0x104
#define USB_OTG_DCCPARAMS 0x124
#define USB_OTG_USBCMD 0x140
#define USB_OTG_USBSTS 0x144
#define USB_OTG_ULPIVIEW 0x170
#define USB_OTG_ENDPTSETUPSTAT 0x1ac
#define USB_OTG_ENDPTPRIME 0x1b0
#define USB_OTG_ENDPTFLUSH 0x1b4
#define USB_OTG_ENDPTSTAT 0x1b8
#define USB_OTG_ENDPTCOMPLETE 0x1bc

#define USB_ID_RESET 0x0005fa05
#define USB_HCSPARAMS_RESET 0x00010011
#define USB_DCCPARAMS_RESET 0x00000188
#define USB_USBCMD_RUN BIT(0)
#define USB_USBCMD_RESET_BIT BIT(1)
#define USB_USBSTS_HALTED BIT(12)
#define USB_ULPIVIEW_WAKEUP BIT(31)
#define USB_ULPIVIEW_RUN BIT(30)

struct IMX31USBState {
	SysBusDevice parent_obj;

	MemoryRegion iomem;
	uint32_t regs[USB_NUM_REGS];
};

static void imx31_usb_reset(DeviceState *dev)
{
	IMX31USBState *s = IMX31_USB(dev);
	int port;

	memset(s->regs, 0, sizeof(s->regs));
	for (port = 0; port < USB_PORT_COUNT; port++) {
		uint32_t *regs = s->regs + port * USB_PORT_SIZE / 4;

		regs[USB_OTG_ID / 4] = USB_ID_RESET;
		regs[USB_OTG_HCSPARAMS / 4] = USB_HCSPARAMS_RESET;
		regs[USB_OTG_DCCPARAMS / 4] = USB_DCCPARAMS_RESET;
	}
}

static uint64_t imx31_usb_read(void *opaque, hwaddr offset, unsigned size)
{
	IMX31USBState *s = opaque;
	hwaddr reg = offset & ~3;
	bool in_port = reg < USB_PORT_COUNT * USB_PORT_SIZE;
	uint32_t value = s->regs[reg / 4];

	if (in_port && reg % USB_PORT_SIZE == USB_OTG_USBSTS &&
			!(s->regs[(reg - USB_OTG_USBSTS + USB_OTG_USBCMD) / 4] & USB_USBCMD_RUN)) {
		value |= USB_USBSTS_HALTED;
	}
	return extract32(value, 8 * (offset & 3), 8);
}

static void imx31_usb_write(void *opaque, hwaddr offset, uint64_t byte, unsigned size)
{
	IMX31USBState *s = opaque;
	hwaddr reg = offset & ~3;
	uint32_t mask = 0xff << (8 * (offset & 3));
	uint32_t value = byte << (8 * (offset & 3));
	bool in_port = reg < USB_PORT_COUNT * USB_PORT_SIZE;
	uint32_t *port_regs = s->regs + (reg - reg % USB_PORT_SIZE) / 4;
	uint32_t *target = &s->regs[reg / 4];

	switch (in_port ? reg % USB_PORT_SIZE : USB_MMIO_SIZE) {
	case USB_OTG_ID:
	case USB_OTG_HCSPARAMS:
	case USB_OTG_DCCPARAMS:
	case USB_OTG_ENDPTSTAT:
		break;
	case USB_OTG_USBCMD:
		*target = (*target & ~mask) | (value & ~USB_USBCMD_RESET_BIT);
		break;
	case USB_OTG_USBSTS:
	case USB_OTG_ENDPTSETUPSTAT:
	case USB_OTG_ENDPTCOMPLETE:
		*target &= ~value;
		break;
	case USB_OTG_ULPIVIEW:
		*target = (*target & ~mask) | (value & ~(USB_ULPIVIEW_WAKEUP | USB_ULPIVIEW_RUN));
		break;
	case USB_OTG_ENDPTPRIME:
		port_regs[USB_OTG_ENDPTSTAT / 4] |= value;
		break;
	case USB_OTG_ENDPTFLUSH:
		port_regs[USB_OTG_ENDPTSTAT / 4] &= ~value;
		break;
	default:
		*target = (*target & ~mask) | value;
		break;
	}
}

/* The handlers work on single bytes so that unaligned guest accesses behave as on hardware. */
static const MemoryRegionOps imx31_usb_ops = {
	.read = imx31_usb_read,
	.write = imx31_usb_write,
	.endianness = DEVICE_LITTLE_ENDIAN,
	.valid = {
		.min_access_size = 1,
		.max_access_size = 4,
		.unaligned = true,
	},
	.impl = {
		.min_access_size = 1,
		.max_access_size = 1,
	},
};

static const VMStateDescription vmstate_imx31_usb = {
	.name = TYPE_IMX31_USB,
	.version_id = 1,
	.minimum_version_id = 1,
	.fields = (const VMStateField[]) {
		VMSTATE_UINT32_ARRAY(regs, IMX31USBState, USB_NUM_REGS),
		VMSTATE_END_OF_LIST()
	},
};

static void imx31_usb_realize(DeviceState *dev, Error **errp)
{
	IMX31USBState *s = IMX31_USB(dev);

	memory_region_init_io(&s->iomem, OBJECT(dev), &imx31_usb_ops, s, TYPE_IMX31_USB, USB_MMIO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void imx31_usb_class_init(ObjectClass *klass, const void *data)
{
	DeviceClass *dc = DEVICE_CLASS(klass);

	dc->realize = imx31_usb_realize;
	dc->vmsd = &vmstate_imx31_usb;
	device_class_set_legacy_reset(dc, imx31_usb_reset);
	dc->desc = "i.MX31 USB module";
}

static const TypeInfo imx31_usb_info = {
	.name = TYPE_IMX31_USB,
	.parent = TYPE_SYS_BUS_DEVICE,
	.instance_size = sizeof(IMX31USBState),
	.class_init = imx31_usb_class_init,
};

static void imx31_usb_register_types(void)
{
	type_register_static(&imx31_usb_info);
}

type_init(imx31_usb_register_types)
