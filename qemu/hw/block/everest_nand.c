/*
 * BrailleNote Apex ("Everest") NAND flash: a Micron MT29F16G08ABABA behind
 * the board's memory-mapped command, address and data latches.
 *
 * The image file stores the bitwise inverse of the flash contents, so that a
 * new, zero-filled image reads as erased flash (0xff).
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "system/block-backend.h"

#define TYPE_EVEREST_NAND "everest.nand"
OBJECT_DECLARE_SIMPLE_TYPE(EverestNANDState, EVEREST_NAND)

#define NAND_MMIO_SIZE 0x100
#define NAND_REG_DESELECT 0x44
#define NAND_REG_IDLE 0x60
#define NAND_REG_SELECT 0x64
#define NAND_REG_DATA 0xa0
#define NAND_REG_ADDRESS 0xe8
#define NAND_REG_COMMAND 0xf0

#define NAND_PAGE_SIZE 4096
#define NAND_OOB_SIZE 224
#define NAND_PAGE_TOTAL (NAND_PAGE_SIZE + NAND_OOB_SIZE)
#define NAND_PAGES_PER_BLOCK 128
#define NAND_BLOCK_COUNT 4096
#define NAND_BLOCK_BYTES ((int64_t)NAND_PAGES_PER_BLOCK * NAND_PAGE_TOTAL)
#define NAND_IMAGE_BYTES (NAND_BLOCK_COUNT * NAND_BLOCK_BYTES)
#define NAND_COLUMN_ADDRESS_BYTES 2
#define NAND_ADDRESS_BYTES 5
#define NAND_ERASED_BYTE 0xff

#define NAND_CMD_READ 0x00
#define NAND_CMD_RANDOM_OUTPUT 0x05
#define NAND_CMD_PROGRAM_CONFIRM 0x10
#define NAND_CMD_READ_CONFIRM 0x30
#define NAND_CMD_ERASE 0x60
#define NAND_CMD_STATUS 0x70
#define NAND_CMD_PROGRAM 0x80
#define NAND_CMD_RANDOM_INPUT 0x85
#define NAND_CMD_READ_ID 0x90
#define NAND_CMD_ERASE_CONFIRM 0xd0
#define NAND_CMD_RANDOM_OUTPUT_CONFIRM 0xe0
#define NAND_CMD_RESET 0xff

#define NAND_STATUS_FAIL 0x01
#define NAND_STATUS_READY 0x40
#define NAND_STATUS_NOT_PROTECTED 0x80

typedef enum {
	NAND_OUTPUT_NONE,
	NAND_OUTPUT_DATA,
	NAND_OUTPUT_ID,
	NAND_OUTPUT_STATUS,
} EverestNANDOutput;

static const uint8_t everest_nand_id[] = { 0x2c, 0x48, 0x00, 0x26, 0x89 };

struct EverestNANDState {
	SysBusDevice parent_obj;

	MemoryRegion iomem;
	BlockBackend *blk;

	EverestNANDOutput output;
	uint8_t command;
	uint8_t address[NAND_ADDRESS_BYTES];
	uint32_t address_count;
	uint32_t page;
	uint32_t column;
	uint32_t id_position;
	bool failed;
	uint8_t buffer[NAND_PAGE_TOTAL];
};

static uint32_t everest_nand_address_column(EverestNANDState *s)
{
	return s->address[0] | (s->address[1] << 8);
}

static uint32_t everest_nand_address_page(EverestNANDState *s, int first)
{
	return s->address[first] | (s->address[first + 1] << 8) | (s->address[first + 2] << 16);
}

static bool everest_nand_page_valid(EverestNANDState *s, uint32_t page)
{
	return s->blk && page < NAND_BLOCK_COUNT * NAND_PAGES_PER_BLOCK;
}

static void everest_nand_load_page(EverestNANDState *s)
{
	int i;

	memset(s->buffer, 0, sizeof(s->buffer));
	if (everest_nand_page_valid(s, s->page)) {
		blk_pread(s->blk, (int64_t)s->page * NAND_PAGE_TOTAL, NAND_PAGE_TOTAL, s->buffer, 0);
	}
	for (i = 0; i < NAND_PAGE_TOTAL; i++) {
		s->buffer[i] = ~s->buffer[i];
	}
}

static void everest_nand_program_page(EverestNANDState *s)
{
	uint8_t stored[NAND_PAGE_TOTAL];
	int64_t offset = (int64_t)s->page * NAND_PAGE_TOTAL;
	int i;

	s->failed = !everest_nand_page_valid(s, s->page) ||
		blk_pread(s->blk, offset, NAND_PAGE_TOTAL, stored, 0) < 0;
	if (s->failed) {
		return;
	}
	for (i = 0; i < NAND_PAGE_TOTAL; i++) {
		stored[i] |= ~s->buffer[i];
	}
	s->failed = blk_pwrite(s->blk, offset, NAND_PAGE_TOTAL, stored, 0) < 0;
}

static void everest_nand_erase_block(EverestNANDState *s)
{
	uint32_t block = everest_nand_address_page(s, 0) / NAND_PAGES_PER_BLOCK;

	s->failed = !everest_nand_page_valid(s, block * NAND_PAGES_PER_BLOCK) ||
		blk_pwrite_zeroes(s->blk, block * NAND_BLOCK_BYTES, NAND_BLOCK_BYTES, 0) < 0;
}

static void everest_nand_command(EverestNANDState *s, uint8_t command)
{
	switch (command) {
	case NAND_CMD_RESET:
		s->output = NAND_OUTPUT_NONE;
		s->failed = false;
		break;
	case NAND_CMD_READ:
		s->output = NAND_OUTPUT_DATA;
		s->address_count = 0;
		break;
	case NAND_CMD_READ_CONFIRM:
		s->column = everest_nand_address_column(s);
		s->page = everest_nand_address_page(s, NAND_COLUMN_ADDRESS_BYTES);
		everest_nand_load_page(s);
		s->output = NAND_OUTPUT_DATA;
		break;
	case NAND_CMD_RANDOM_OUTPUT:
	case NAND_CMD_RANDOM_INPUT:
	case NAND_CMD_ERASE:
		s->address_count = 0;
		break;
	case NAND_CMD_RANDOM_OUTPUT_CONFIRM:
		s->column = everest_nand_address_column(s);
		s->output = NAND_OUTPUT_DATA;
		break;
	case NAND_CMD_PROGRAM:
		s->address_count = 0;
		memset(s->buffer, NAND_ERASED_BYTE, sizeof(s->buffer));
		break;
	case NAND_CMD_PROGRAM_CONFIRM:
		everest_nand_program_page(s);
		break;
	case NAND_CMD_ERASE_CONFIRM:
		everest_nand_erase_block(s);
		break;
	case NAND_CMD_STATUS:
		s->output = NAND_OUTPUT_STATUS;
		return;
	case NAND_CMD_READ_ID:
		s->output = NAND_OUTPUT_ID;
		s->id_position = 0;
		break;
	default:
		qemu_log_mask(LOG_UNIMP, "[%s]%s: command 0x%02x not implemented\n",
				TYPE_EVEREST_NAND, __func__, command);
		break;
	}
	s->command = command;
}

static void everest_nand_address(EverestNANDState *s, uint8_t value)
{
	if (s->address_count < NAND_ADDRESS_BYTES) {
		s->address[s->address_count++] = value;
	}
	if (s->address_count < NAND_COLUMN_ADDRESS_BYTES) {
		return;
	}
	if (s->command == NAND_CMD_PROGRAM || s->command == NAND_CMD_RANDOM_INPUT) {
		s->column = everest_nand_address_column(s);
	}
	if (s->command == NAND_CMD_PROGRAM && s->address_count == NAND_ADDRESS_BYTES) {
		s->page = everest_nand_address_page(s, NAND_COLUMN_ADDRESS_BYTES);
	}
}

static void everest_nand_data_write(EverestNANDState *s, uint8_t value)
{
	bool programming = s->command == NAND_CMD_PROGRAM || s->command == NAND_CMD_RANDOM_INPUT;

	if (programming && s->column < NAND_PAGE_TOTAL) {
		s->buffer[s->column++] = value;
	}
}

static uint8_t everest_nand_data_read(EverestNANDState *s)
{
	switch (s->output) {
	case NAND_OUTPUT_DATA:
		return s->column < NAND_PAGE_TOTAL ? s->buffer[s->column++] : NAND_ERASED_BYTE;
	case NAND_OUTPUT_ID:
		if (!s->blk || s->id_position >= ARRAY_SIZE(everest_nand_id)) {
			return 0;
		}
		return everest_nand_id[s->id_position++];
	case NAND_OUTPUT_STATUS:
		return NAND_STATUS_NOT_PROTECTED | NAND_STATUS_READY | (s->failed ? NAND_STATUS_FAIL : 0);
	default:
		return 0;
	}
}

static uint64_t everest_nand_read(void *opaque, hwaddr offset, unsigned size)
{
	EverestNANDState *s = opaque;
	uint64_t value = 0;
	unsigned i;

	switch (offset) {
	case NAND_REG_DATA:
		for (i = 0; i < size; i++) {
			value |= (uint64_t)everest_nand_data_read(s) << (8 * i);
		}
		break;
	case NAND_REG_IDLE:
		break;
	default:
		qemu_log_mask(LOG_UNIMP, "[%s]%s: read at offset 0x%" HWADDR_PRIx " not implemented\n",
				TYPE_EVEREST_NAND, __func__, offset);
		break;
	}
	return value;
}

static void everest_nand_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
	EverestNANDState *s = opaque;
	unsigned i;

	switch (offset) {
	case NAND_REG_COMMAND:
		everest_nand_command(s, value);
		break;
	case NAND_REG_ADDRESS:
		everest_nand_address(s, value);
		break;
	case NAND_REG_DATA:
		for (i = 0; i < size; i++) {
			everest_nand_data_write(s, value >> (8 * i));
		}
		break;
	case NAND_REG_SELECT:
	case NAND_REG_DESELECT:
		break;
	default:
		qemu_log_mask(LOG_UNIMP, "[%s]%s: write at offset 0x%" HWADDR_PRIx " not implemented\n",
				TYPE_EVEREST_NAND, __func__, offset);
		break;
	}
}

static const MemoryRegionOps everest_nand_ops = {
	.read = everest_nand_read,
	.write = everest_nand_write,
	.endianness = DEVICE_LITTLE_ENDIAN,
	.impl = {
		.min_access_size = 1,
		.max_access_size = 4,
	},
};

static void everest_nand_reset(DeviceState *dev)
{
	EverestNANDState *s = EVEREST_NAND(dev);

	s->output = NAND_OUTPUT_NONE;
	s->command = NAND_CMD_RESET;
	s->address_count = 0;
	s->page = 0;
	s->column = 0;
	s->id_position = 0;
	s->failed = false;
}

static void everest_nand_realize(DeviceState *dev, Error **errp)
{
	EverestNANDState *s = EVEREST_NAND(dev);

	memory_region_init_io(&s->iomem, OBJECT(dev), &everest_nand_ops, s,
			TYPE_EVEREST_NAND, NAND_MMIO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
	if (!s->blk) {
		return;
	}
	if (blk_set_perm(s->blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE, BLK_PERM_ALL, errp) < 0) {
		return;
	}
	if (blk_getlength(s->blk) < NAND_IMAGE_BYTES) {
		error_setg(errp, "NAND image must be at least %" PRId64 " bytes", NAND_IMAGE_BYTES);
	}
}

static const Property everest_nand_properties[] = {
	DEFINE_PROP_DRIVE("drive", EverestNANDState, blk),
};

static void everest_nand_class_init(ObjectClass *klass, const void *data)
{
	DeviceClass *dc = DEVICE_CLASS(klass);

	dc->realize = everest_nand_realize;
	device_class_set_legacy_reset(dc, everest_nand_reset);
	device_class_set_props(dc, everest_nand_properties);
	dc->desc = "BrailleNote Apex NAND flash";
}

static const TypeInfo everest_nand_info = {
	.name = TYPE_EVEREST_NAND,
	.parent = TYPE_SYS_BUS_DEVICE,
	.instance_size = sizeof(EverestNANDState),
	.class_init = everest_nand_class_init,
};

static void everest_nand_register_types(void)
{
	type_register_static(&everest_nand_info);
}

type_init(everest_nand_register_types)
