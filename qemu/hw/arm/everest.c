/*
 * HumanWare BrailleNote Apex ("Everest") board
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/arm/fsl-imx31.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "chardev/char.h"
#include "hw/char/imx_serial.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "hw/misc/unimp.h"
#include "hw/net/lan9118.h"
#include "hw/ssi/ssi.h"
#include "hw/usb/chipidea.h"
#include "net/net.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "system/address-spaces.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "system/qtest.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/system.h"

#define EVEREST_RAM_SIZE (256 * MiB)

#define EVEREST_CSPI_COUNT 3
#define EVEREST_PMIC_CSPI 1
#define EVEREST_PMIC_CS 1
#define EVEREST_PMIC_IRQ_GPIO1_PIN 2

/*
 * Start of the NOR flash. It holds the FPGA image that the kernel loads into
 * the FPGA again when the unit wakes. The run script loads the image.
 */
#define EVEREST_NOR_SIZE (1 * MiB)

/* A configured FPGA holds its INIT_B and DONE pins high. */
#define EVEREST_FPGA_GPIO 2
#define EVEREST_FPGA_DONE_PIN 4
#define EVEREST_FPGA_INIT_B_PIN 6

#define EVEREST_IOMUXC_ADDR 0x43fac000
#define EVEREST_IOMUXC_SIZE 0x4000
#define EVEREST_NAND_ADDR (FSL_IMX31_FLASH1_ADDR + 0x400)
#define EVEREST_NAND_SPACING 0x100
#define EVEREST_NAND_COUNT 4
#define EVEREST_USB_ADDR 0x43f88000
#define EVEREST_LAN_ADDR FSL_IMX31_CS4_ADDR
#define EVEREST_KEYS_ADDR (FSL_IMX31_FLASH1_ADDR + 0x800)
#define EVEREST_KEYS_IRQ_GPIO1_PIN 1
/* Key events come from a chardev with this id, for example a TCP socket. */
#define EVEREST_KEYS_CHARDEV "keys"
#define EVEREST_LAN_IRQ_GPIO1_PIN 4
#define EVEREST_USB_HOST_COUNT 2
#define EVEREST_USB_HOST_PRIORITY 1

/*
 * On a power down request, such as Ctrl+C, the board turns the power switch
 * off. KeySoft saves its registry and suspends, and then the machine shuts
 * down. The guest takes about 200 ms. Windows ends the process about 5
 * seconds after its console window closes, so shut down before then even if
 * the guest does not suspend.
 */
#define EVEREST_POWER_DOWN_TIMEOUT_MS 3000

/*
 * State that the boot loader (eboot) leaves for the kernel. The board provides
 * it until eboot itself can run.
 */
#define EVEREST_BOOT_STUB_ADDR 0x80010000
#define EVEREST_KERNEL_ENTRY 0x80200000
#define EVEREST_BOOT_ARGS_MEMORY_SIZE_ADDR 0x80002094
#define EVEREST_BOOT_ARGS_CLOCK_FREQ_ADDR 0x80000044

#define EVEREST_HARP_CFG_ADDR 0x80000094
#define EVEREST_HARP_CFG_SIZE 0x800
#define EVEREST_HARP_CFG_SERIAL_OFFSET 0x18
#define EVEREST_NAND_PRIVATE_MB 20
#define EVEREST_NAND_BINFS_MB 128
#define EVEREST_NAND_OS1_MB 40
#define EVEREST_NAND_OS2_REST_OF_CHIP 0xffffffff
#define EVEREST_NAND_LICENSES_MB 16

/* The Apex QT has a QWERTY keyboard in place of the braille keyboard. */
#define EVEREST_HARP_CFG_KEYBOARD_OFFSET 0x75
#define EVEREST_HARP_CFG_KEYBOARD_QWERTY 1
#define EVEREST_HARP_CFG_MODEL_OFFSET 0x78
#define EVEREST_HARP_CFG_MODEL_QT32 1

typedef struct EverestCfgField {
	uint16_t offset;
	uint8_t size;
	uint32_t value;
} EverestCfgField;

/* The factory default configuration that the boot loader creates. */
static const char everest_harp_cfg_serial[] = "999999999";
static const EverestCfgField everest_harp_cfg[] = {
	{ 0x00, 4, 0x4d583331 },
	{ 0x05, 1, 4 },
	{ 0x06, 2, 0x0500 }, { 0x08, 2, 0xc35d }, { 0x0a, 2, 0x213c },
	{ 0x0c, 4, 0x48554d41 }, { 0x10, 4, 0x43453630 }, { 0x14, 4, 0x45564552 },
	{ 0x22, 1, 0xff }, { 0x23, 1, 0xff },
	{ 0x2c, 4, EVEREST_NAND_PRIVATE_MB },
	{ 0x30, 4, EVEREST_NAND_BINFS_MB },
	{ 0x34, 4, EVEREST_NAND_OS1_MB },
	{ 0x38, 4, EVEREST_NAND_OS2_REST_OF_CHIP },
	{ 0x3c, 4, 0x04000000 },
	{ 0x49, 1, 1 },
	{ 0x4c, 2, 0x0500 }, { 0x4e, 2, 0xc45d }, { 0x50, 2, 0x213c },
	{ 0x52, 2, 0x0500 }, { 0x54, 2, 0xc55d }, { 0x56, 2, 0x213c },
	{ 0x58, 2, 0x0500 }, { 0x5a, 2, 0xc75d }, { 0x5c, 2, 0x213c },
	{ 0x5e, 1, 1 }, { 0x5f, 1, 2 },
	{ 0x60, 4, 0x20090101 },
	{ 0x76, 1, 2 },
	{ 0x7c, 4, EVEREST_NAND_LICENSES_MB },
};

/*
 * Clock rates in Hz, by CSPDDK clock signal number, that eboot computes from
 * the clock controller setup that it and the kernel program. Drivers divide
 * by these, so a zero rate crashes them.
 */
static const uint32_t everest_clock_freq[] = {
	399000000, /* MCU PLL */
	240000000, /* USB PLL */
	220147200, /* serial PLL */
	399000000, /* ARM */
	199500000, /* HSP */
	99750000,  /* AHB */
	49875000,  /* IPG */
	24937500,  /* NAND flash */
	49875000,  /* AHB / 2 */
	1719900,   /* SSI1 */
	1719900,   /* SSI2 */
	1875000,   /* FIRI */
	429975,    /* CSI */
	60000000,  /* USB */
	49875000,  /* PER */
	49875000,  /* PER, used by the UARTs */
	1,
	1,
	0,
};

static bool everest_qt;

static const uint32_t everest_boot_stub[] = {
	0xe3a0060f, /* mov r0, #0x00f00000 */
	0xee010f50, /* mcr p15, 0, r0, c1, c0, 2 (enable VFP coprocessor access) */
	0xe51ff004, /* ldr pc, [pc, #-4] */
	EVEREST_KERNEL_ENTRY,
};

typedef struct EverestState {
	FslIMX31State soc;
	DeviceState *cspi[EVEREST_CSPI_COUNT];
	MemoryRegion iomuxc;
	MemoryRegion nor;
	qemu_irq pmic_power_off;
	Notifier powerdown;
	QEMUTimer *power_down_timer;
	bool suspended;
	bool powering_down;
} EverestState;

static const struct {
	hwaddr addr;
	int irq;
} everest_cspi[EVEREST_CSPI_COUNT] = {
	{ 0x43fa4000, 14 },
	{ 0x50010000, 13 },
	{ 0x53f84000, 17 },
};

/* UART1 and UART2 belong to the SoC model and use host serial ports 0 and 1. */
#define EVEREST_FIRST_BOARD_SERIAL 2

static const struct {
	const char *type;
	hwaddr addr;
	int irq;
} everest_devices[] = {
	{ "imx31.rtc", 0x53fd8000, 25 },
	{ "imx31.sdhc", 0x50004000, 9 },
	{ "imx31.sdhc", 0x50008000, 8 },
};

#define EVEREST_SDMA_ADDR 0x53fd4000
#define EVEREST_SDMA_IRQ 34

static const struct {
	hwaddr addr;
	int tx_dma_event;
} everest_ssi[] = {
	{ 0x43fa0000, 29 },
	{ 0x50014000, 25 },
};

static const struct {
	hwaddr addr;
	int irq;
} everest_uart[] = {
	{ 0x5000c000, 18 },
	{ 0x43fb0000, 46 },
	{ 0x43fb4000, 47 },
};

/* UART4 talks to the Bluetooth chip and sends through SDMA. */
#define EVEREST_BLUETOOTH_UART 1
#define EVEREST_BLUETOOTH_UART_TX_DMA_EVENT 13
#define EVEREST_UART_TX_DMA_REQUEST 1

static const struct {
	hwaddr addr;
	int irq;
} everest_usb_host[EVEREST_USB_HOST_COUNT] = {
	{ 0x43f88200, 35 },
	{ 0x43f88400, 36 },
};

static struct arm_boot_info everest_binfo = {
	.loader_start = FSL_IMX31_SDRAM0_ADDR,
};

/*
 * Changes to the firmware image in guest memory. Each entry replaces one
 * instruction, and only when the original instruction is there.
 */
typedef struct EverestFirmwarePatch {
	hwaddr addr;
	uint32_t original;
	uint32_t replacement;
} EverestFirmwarePatch;

static const EverestFirmwarePatch everest_firmware_patches[] = {
	/*
	 * VAFramework.dll, CVALicenseMgr::ValidateInfo: "mov r0, r4" becomes
	 * "mov r0, #0", so every license query reports a valid license. The
	 * license keys of a real unit are in its NOR flash, which is not available.
	 */
	{ 0x81ddc534, 0xe1a00004, 0xe3a00000 },
	/*
	 * telnetd.dll: the two reads of the "IsEnabled" registry value become
	 * "mov r0, #1". The shipped registry turns the telnet server off, and it
	 * is the only way to get a command prompt inside the guest.
	 */
	{ 0x813aee14, 0xebfffc58, 0xe3a00001 },
	{ 0x813af370, 0xebfffb01, 0xe3a00001 },
	/*
	 * VASystemServ.dll, device notification thread: "bne loop" becomes
	 * "bne exit" after WaitForMultipleObjects. Without the license keys the
	 * service never creates the two events this thread waits on, so the wait
	 * fails at once and the thread spins, which starves KeySoft's install
	 * thread. With the patch the thread ends when the wait fails.
	 */
	{ 0x8239dc0c, 0x1a000003, 0x1a000002 },
};

static void everest_reset(MachineState *machine, ResetType type)
{
	FslIMX31State *soc = FSL_IMX31(object_resolve_path_component(OBJECT(machine), "soc"));
	DeviceState *fpga_gpio = DEVICE(&soc->gpio[EVEREST_FPGA_GPIO]);
	int i;

	qemu_devices_reset(type);
	qemu_set_irq(qdev_get_gpio_in(fpga_gpio, EVEREST_FPGA_DONE_PIN), 1);
	qemu_set_irq(qdev_get_gpio_in(fpga_gpio, EVEREST_FPGA_INIT_B_PIN), 1);
	for (i = 0; i < ARRAY_SIZE(everest_firmware_patches); i++) {
		const EverestFirmwarePatch *patch = &everest_firmware_patches[i];
		uint32_t word = 0;

		address_space_read(&address_space_memory, patch->addr, MEMTXATTRS_UNSPECIFIED, &word, sizeof(word));
		if (le32_to_cpu(word) == patch->original) {
			word = cpu_to_le32(patch->replacement);
			address_space_write(&address_space_memory, patch->addr, MEMTXATTRS_UNSPECIFIED, &word, sizeof(word));
		}
	}
}

/* The power switch from the keys device. Switching on wakes the guest. */
static void everest_power_switch(void *opaque, int n, int off)
{
	EverestState *s = opaque;

	if (!off) {
		s->suspended = false;
	}
	qemu_set_irq(s->pmic_power_off, off);
}

static void everest_stop_mode(void *opaque, int n, int stop)
{
	EverestState *s = opaque;

	if (!stop) {
		return;
	}
	s->suspended = true;
	if (s->powering_down) {
		qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
	}
}

static void everest_power_down_timeout(void *opaque)
{
	qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_SIGNAL);
}

static void everest_powerdown(Notifier *notifier, void *data)
{
	EverestState *s = container_of(notifier, EverestState, powerdown);

	if (s->suspended) {
		qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
		return;
	}
	if (s->powering_down) {
		return;
	}
	s->powering_down = true;
	qemu_set_irq(s->pmic_power_off, 1);
	timer_mod(s->power_down_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + EVEREST_POWER_DOWN_TIMEOUT_MS);
}

static DeviceState *everest_create_device(EverestState *s, const char *type, hwaddr addr, int irq)
{
	DeviceState *dev = qdev_new(type);

	sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
	sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, addr);
	sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, qdev_get_gpio_in(DEVICE(&s->soc.avic), irq));
	return dev;
}

static void everest_init(MachineState *machine)
{
	EverestState *s = g_new0(EverestState, 1);
	DeviceState *pmic;
	DeviceState *nand;
	DeviceState *usb;
	DeviceState *sdma;
	DeviceState *keys;
	DeviceState *uart[ARRAY_SIZE(everest_uart)];
	DriveInfo *nand_drive;
	uint32_t boot_stub[ARRAY_SIZE(everest_boot_stub)];
	uint32_t clock_freq[ARRAY_SIZE(everest_clock_freq)];
	uint8_t harp_cfg[EVEREST_HARP_CFG_SIZE] = { 0 };
	uint32_t memory_size = cpu_to_le32(machine->ram_size);
	int i;

	object_initialize_child(OBJECT(machine), "soc", &s->soc, TYPE_FSL_IMX31);
	qdev_realize(DEVICE(&s->soc), NULL, &error_fatal);

	memory_region_add_subregion(get_system_memory(), FSL_IMX31_SDRAM0_ADDR, machine->ram);

	create_unimplemented_device("everest.unmapped", 0, 4 * GiB);

	for (i = 0; i < EVEREST_CSPI_COUNT; i++) {
		s->cspi[i] = everest_create_device(s, "imx31.cspi", everest_cspi[i].addr, everest_cspi[i].irq);
	}
	for (i = 0; i < ARRAY_SIZE(everest_devices); i++) {
		everest_create_device(s, everest_devices[i].type, everest_devices[i].addr, everest_devices[i].irq);
	}
	for (i = 0; i < ARRAY_SIZE(everest_uart); i++) {
		uart[i] = qdev_new(TYPE_IMX_SERIAL);
		qdev_prop_set_chr(uart[i], "chardev", serial_hd(EVEREST_FIRST_BOARD_SERIAL + i));
		sysbus_realize_and_unref(SYS_BUS_DEVICE(uart[i]), &error_fatal);
		sysbus_mmio_map(SYS_BUS_DEVICE(uart[i]), 0, everest_uart[i].addr);
		sysbus_connect_irq(SYS_BUS_DEVICE(uart[i]), 0,
				qdev_get_gpio_in(DEVICE(&s->soc.avic), everest_uart[i].irq));
	}

	for (i = 0; i < EVEREST_NAND_COUNT; i++) {
		nand = qdev_new("everest.nand");
		nand_drive = drive_get(IF_MTD, 0, i);
		if (nand_drive) {
			qdev_prop_set_drive_err(nand, "drive", blk_by_legacy_dinfo(nand_drive), &error_fatal);
		}
		sysbus_realize_and_unref(SYS_BUS_DEVICE(nand), &error_fatal);
		sysbus_mmio_map(SYS_BUS_DEVICE(nand), 0, EVEREST_NAND_ADDR + i * EVEREST_NAND_SPACING);
	}

	usb = qdev_new("imx31.usb");
	sysbus_realize_and_unref(SYS_BUS_DEVICE(usb), &error_fatal);
	sysbus_mmio_map(SYS_BUS_DEVICE(usb), 0, EVEREST_USB_ADDR);
	for (i = 0; i < EVEREST_USB_HOST_COUNT; i++) {
		usb = qdev_new(TYPE_CHIPIDEA);
		sysbus_realize_and_unref(SYS_BUS_DEVICE(usb), &error_fatal);
		sysbus_mmio_map_overlap(SYS_BUS_DEVICE(usb), 0, everest_usb_host[i].addr, EVEREST_USB_HOST_PRIORITY);
		sysbus_connect_irq(SYS_BUS_DEVICE(usb), 0,
				qdev_get_gpio_in(DEVICE(&s->soc.avic), everest_usb_host[i].irq));
	}

	memory_region_init_rom(&s->nor, NULL, "everest.nor", EVEREST_NOR_SIZE, &error_fatal);
	memory_region_add_subregion(get_system_memory(), FSL_IMX31_FLASH0_ADDR, &s->nor);

	/* The pin multiplexer only stores its configuration. */
	memory_region_init_ram(&s->iomuxc, NULL, "everest.iomuxc", EVEREST_IOMUXC_SIZE, &error_fatal);
	memory_region_add_subregion(get_system_memory(), EVEREST_IOMUXC_ADDR, &s->iomuxc);

	pmic = ssi_create_peripheral((SSIBus *)qdev_get_child_bus(s->cspi[EVEREST_PMIC_CSPI], "spi"), "mc13783");
	sysbus_connect_irq(SYS_BUS_DEVICE(s->cspi[EVEREST_PMIC_CSPI]), 1 + EVEREST_PMIC_CS,
			qdev_get_gpio_in_named(pmic, SSI_GPIO_CS, 0));
	qdev_connect_gpio_out(pmic, 0, qdev_get_gpio_in(DEVICE(&s->soc.gpio[0]), EVEREST_PMIC_IRQ_GPIO1_PIN));

	for (i = 0; i < ARRAY_SIZE(everest_boot_stub); i++) {
		boot_stub[i] = cpu_to_le32(everest_boot_stub[i]);
	}
	rom_add_blob_fixed("everest.boot-stub", boot_stub, sizeof(boot_stub), EVEREST_BOOT_STUB_ADDR);
	for (i = 0; i < ARRAY_SIZE(everest_harp_cfg); i++) {
		stn_le_p(harp_cfg + everest_harp_cfg[i].offset, everest_harp_cfg[i].size, everest_harp_cfg[i].value);
	}
	memcpy(harp_cfg + EVEREST_HARP_CFG_SERIAL_OFFSET, everest_harp_cfg_serial, sizeof(everest_harp_cfg_serial));
	if (everest_qt) {
		harp_cfg[EVEREST_HARP_CFG_KEYBOARD_OFFSET] = EVEREST_HARP_CFG_KEYBOARD_QWERTY;
		harp_cfg[EVEREST_HARP_CFG_MODEL_OFFSET] = EVEREST_HARP_CFG_MODEL_QT32;
	}
	rom_add_blob_fixed("everest.harp-cfg", harp_cfg, sizeof(harp_cfg), EVEREST_HARP_CFG_ADDR);
	rom_add_blob_fixed("everest.boot-args", &memory_size, sizeof(memory_size),
			EVEREST_BOOT_ARGS_MEMORY_SIZE_ADDR);
	for (i = 0; i < ARRAY_SIZE(everest_clock_freq); i++) {
		clock_freq[i] = cpu_to_le32(everest_clock_freq[i]);
	}
	rom_add_blob_fixed("everest.clock-freq", clock_freq, sizeof(clock_freq),
			EVEREST_BOOT_ARGS_CLOCK_FREQ_ADDR);

	if (qemu_find_nic_info("lan9118", true, NULL)) {
		lan9118_init(EVEREST_LAN_ADDR, qdev_get_gpio_in(DEVICE(&s->soc.gpio[0]), EVEREST_LAN_IRQ_GPIO1_PIN));
	}

	keys = qdev_new("everest.keys");
	qdev_prop_set_chr(keys, "chardev", qemu_chr_find(EVEREST_KEYS_CHARDEV));
	sysbus_realize_and_unref(SYS_BUS_DEVICE(keys), &error_fatal);
	sysbus_mmio_map(SYS_BUS_DEVICE(keys), 0, EVEREST_KEYS_ADDR);
	sysbus_connect_irq(SYS_BUS_DEVICE(keys), 0, qdev_get_gpio_in(DEVICE(&s->soc.gpio[0]), EVEREST_KEYS_IRQ_GPIO1_PIN));
	s->pmic_power_off = qdev_get_gpio_in_named(pmic, "power-off", 0);
	qdev_connect_gpio_out_named(keys, "power-off", 0, qemu_allocate_irq(everest_power_switch, s, 0));
	qdev_connect_gpio_out_named(DEVICE(&s->soc.ccm), "stop-mode", 0, qemu_allocate_irq(everest_stop_mode, s, 0));
	s->power_down_timer = timer_new_ms(QEMU_CLOCK_REALTIME, everest_power_down_timeout, s);
	s->powerdown.notify = everest_powerdown;
	qemu_register_powerdown_notifier(&s->powerdown);

	sdma = everest_create_device(s, "imx31.sdma", EVEREST_SDMA_ADDR, EVEREST_SDMA_IRQ);
	sysbus_connect_irq(SYS_BUS_DEVICE(uart[EVEREST_BLUETOOTH_UART]), EVEREST_UART_TX_DMA_REQUEST,
			qdev_get_gpio_in(sdma, EVEREST_BLUETOOTH_UART_TX_DMA_EVENT));
	for (i = 0; i < ARRAY_SIZE(everest_ssi); i++) {
		DeviceState *ssi = qdev_new("imx31.ssi");

		if (machine->audiodev) {
			qdev_prop_set_string(ssi, "audiodev", machine->audiodev);
		}
		sysbus_realize_and_unref(SYS_BUS_DEVICE(ssi), &error_fatal);
		sysbus_mmio_map(SYS_BUS_DEVICE(ssi), 0, everest_ssi[i].addr);
		sysbus_connect_irq(SYS_BUS_DEVICE(ssi), 0, qdev_get_gpio_in(sdma, everest_ssi[i].tx_dma_event));
	}

	everest_binfo.ram_size = machine->ram_size;
	if (!qtest_enabled()) {
		arm_load_kernel(&s->soc.cpu, machine, &everest_binfo);
	}
}

static bool everest_get_qt(Object *obj, Error **errp)
{
	return everest_qt;
}

static void everest_set_qt(Object *obj, bool value, Error **errp)
{
	everest_qt = value;
}

static void everest_machine_init(MachineClass *mc)
{
	mc->desc = "HumanWare BrailleNote Apex (ARM1136)";
	mc->init = everest_init;
	mc->reset = everest_reset;
	mc->ignore_memory_transaction_failures = true;
	mc->default_ram_size = EVEREST_RAM_SIZE;
	mc->default_ram_id = "everest.ram";
	machine_add_audiodev_property(mc);
	object_class_property_add_bool(OBJECT_CLASS(mc), "qt", everest_get_qt, everest_set_qt);
	object_class_property_set_description(OBJECT_CLASS(mc), "qt",
			"Set on to emulate the Apex QT, with a QWERTY keyboard");
}

DEFINE_MACHINE_ARM("everest", everest_machine_init)
