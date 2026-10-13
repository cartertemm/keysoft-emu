/*
 * i.MX31 Synchronous Serial Interface, transmit path only.
 *
 * Samples that the guest puts in transmit FIFO 0 go to the host audio
 * backend. The codec supplies the bit clock on this board. The sample rate is
 * fixed at the rate that the Apex wave driver runs the codec at. Receive,
 * FIFO 1 and AC97 are not modelled.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "qemu/audio.h"
#include "qemu/bitops.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_IMX31_SSI "imx31.ssi"
OBJECT_DECLARE_SIMPLE_TYPE(IMX31SSIState, IMX31_SSI)

#define SSI_MMIO_SIZE 0x4000
#define SSI_NUM_REGS (0x50 / 4)
#define SSI_FIFO_SIZE 8
#define SSI_OUTPUT_CHANNELS 2
#define SSI_MAX_FRAMES_PER_CALLBACK 1024
#define SSI_SAMPLE_RATE 44100

#define SSI_STX0 0x00
#define SSI_SCR 0x10
#define SSI_SISR 0x14
#define SSI_SIER 0x18
#define SSI_STCR 0x1c
#define SSI_STCCR 0x24
#define SSI_SFCSR 0x2c
#define SSI_STMSK 0x48

#define SSI_SCR_SSIEN BIT(0)
#define SSI_SCR_TE BIT(1)
#define SSI_SISR_TFE0 BIT(0)
#define SSI_SISR_TDE0 BIT(12)
#define SSI_SIER_TDMAE BIT(20)
#define SSI_STCR_TFEN0 BIT(7)
#define SSI_STCCR_DC_SHIFT 8
#define SSI_STCCR_DC_LENGTH 5
#define SSI_SFCSR_TFWM0_LENGTH 4
#define SSI_SFCSR_TFCNT0_SHIFT 8

struct IMX31SSIState {
	SysBusDevice parent_obj;

	MemoryRegion iomem;
	qemu_irq tx_dma_request;
	AudioBackend *audio_be;
	SWVoiceOut *voice;

	uint32_t regs[SSI_NUM_REGS];
	uint16_t fifo[SSI_FIFO_SIZE];
	uint32_t fifo_head;
	uint32_t fifo_count;
	bool active;
};

static bool imx31_ssi_fifo_pop(IMX31SSIState *s, int16_t *sample)
{
	if (s->fifo_count == 0) {
		return false;
	}
	*sample = s->fifo[s->fifo_head];
	s->fifo_head = (s->fifo_head + 1) % SSI_FIFO_SIZE;
	s->fifo_count--;
	return true;
}

static void imx31_ssi_fifo_push(IMX31SSIState *s, uint16_t sample)
{
	if (s->fifo_count < SSI_FIFO_SIZE) {
		s->fifo[(s->fifo_head + s->fifo_count) % SSI_FIFO_SIZE] = sample;
		s->fifo_count++;
	}
}

static uint32_t imx31_ssi_watermark(IMX31SSIState *s)
{
	return MAX(extract32(s->regs[SSI_SFCSR / 4], 0, SSI_SFCSR_TFWM0_LENGTH), 1);
}

static bool imx31_ssi_below_watermark(IMX31SSIState *s)
{
	return SSI_FIFO_SIZE - s->fifo_count >= imx31_ssi_watermark(s);
}

/* The DMA controller fills the FIFO from inside the request callback. */
static void imx31_ssi_request_data(IMX31SSIState *s)
{
	if ((s->regs[SSI_SIER / 4] & SSI_SIER_TDMAE) && imx31_ssi_below_watermark(s)) {
		qemu_irq_pulse(s->tx_dma_request);
	}
}

static int imx31_ssi_slots_per_frame(IMX31SSIState *s)
{
	int slots = extract32(s->regs[SSI_STCCR / 4], SSI_STCCR_DC_SHIFT, SSI_STCCR_DC_LENGTH) + 1;
	int enabled = ctpop32(~s->regs[SSI_STMSK / 4] & MAKE_64BIT_MASK(0, slots));

	return CLAMP(enabled, 1, SSI_OUTPUT_CHANNELS);
}

static bool imx31_ssi_next_sample(IMX31SSIState *s, int16_t *sample)
{
	if (s->fifo_count == 0) {
		imx31_ssi_request_data(s);
	}
	return imx31_ssi_fifo_pop(s, sample);
}

static void imx31_ssi_audio_callback(void *opaque, int avail)
{
	IMX31SSIState *s = opaque;
	int16_t out[SSI_MAX_FRAMES_PER_CALLBACK * SSI_OUTPUT_CHANNELS];
	int wanted = MIN(avail / (int)(sizeof(int16_t) * SSI_OUTPUT_CHANNELS), SSI_MAX_FRAMES_PER_CALLBACK);
	int slots = imx31_ssi_slots_per_frame(s);
	int frames;

	for (frames = 0; frames < wanted; frames++) {
		int16_t left, right;

		if (!imx31_ssi_next_sample(s, &left)) {
			break;
		}
		right = left;
		if (slots > 1 && !imx31_ssi_next_sample(s, &right)) {
			break;
		}
		out[frames * SSI_OUTPUT_CHANNELS] = cpu_to_le16(left);
		out[frames * SSI_OUTPUT_CHANNELS + 1] = cpu_to_le16(right);
	}
	if (frames > 0) {
		audio_be_write(s->audio_be, s->voice, out, frames * sizeof(int16_t) * SSI_OUTPUT_CHANNELS);
	}
}

static void imx31_ssi_open_voice(IMX31SSIState *s)
{
	struct audsettings settings = {
		.freq = SSI_SAMPLE_RATE,
		.nchannels = SSI_OUTPUT_CHANNELS,
		.fmt = AUDIO_FORMAT_S16,
		.big_endian = false,
	};

	if (s->voice) {
		return;
	}
	s->voice = audio_be_open_out(s->audio_be, s->voice, TYPE_IMX31_SSI, s,
			imx31_ssi_audio_callback, &settings);
}

static void imx31_ssi_update_active(IMX31SSIState *s)
{
	uint32_t required = SSI_SCR_SSIEN | SSI_SCR_TE;
	bool active = (s->regs[SSI_SCR / 4] & required) == required &&
		(s->regs[SSI_STCR / 4] & SSI_STCR_TFEN0);

	if (active == s->active) {
		return;
	}
	s->active = active;
	if (active) {
		imx31_ssi_open_voice(s);
	}
	if (s->voice) {
		audio_be_set_active_out(s->audio_be, s->voice, active);
	}
}

static void imx31_ssi_reset(DeviceState *dev)
{
	IMX31SSIState *s = IMX31_SSI(dev);

	memset(s->regs, 0, sizeof(s->regs));
	s->fifo_head = 0;
	s->fifo_count = 0;
	imx31_ssi_update_active(s);
}

static uint64_t imx31_ssi_read(void *opaque, hwaddr offset, unsigned size)
{
	IMX31SSIState *s = opaque;

	switch (offset) {
	case SSI_SISR:
		return imx31_ssi_below_watermark(s) ? SSI_SISR_TFE0 | SSI_SISR_TDE0 : 0;
	case SSI_SFCSR:
		return s->regs[offset / 4] | (s->fifo_count << SSI_SFCSR_TFCNT0_SHIFT);
	default:
		return offset / 4 < SSI_NUM_REGS ? s->regs[offset / 4] : 0;
	}
}

static void imx31_ssi_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
	IMX31SSIState *s = opaque;

	if (offset / 4 >= SSI_NUM_REGS) {
		return;
	}
	if (offset == SSI_STX0) {
		imx31_ssi_fifo_push(s, value);
		return;
	}
	s->regs[offset / 4] = value;
	imx31_ssi_update_active(s);
}

static const MemoryRegionOps imx31_ssi_ops = {
	.read = imx31_ssi_read,
	.write = imx31_ssi_write,
	.endianness = DEVICE_NATIVE_ENDIAN,
	.valid = {
		.min_access_size = 4,
		.max_access_size = 4,
		.unaligned = false,
	},
};

static void imx31_ssi_realize(DeviceState *dev, Error **errp)
{
	IMX31SSIState *s = IMX31_SSI(dev);

	if (!audio_be_check(&s->audio_be, errp)) {
		return;
	}
	memory_region_init_io(&s->iomem, OBJECT(dev), &imx31_ssi_ops, s, TYPE_IMX31_SSI, SSI_MMIO_SIZE);
	sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
	sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->tx_dma_request);
}

static const Property imx31_ssi_properties[] = {
	DEFINE_AUDIO_PROPERTIES(IMX31SSIState, audio_be),
};

static void imx31_ssi_class_init(ObjectClass *klass, const void *data)
{
	DeviceClass *dc = DEVICE_CLASS(klass);

	dc->realize = imx31_ssi_realize;
	device_class_set_legacy_reset(dc, imx31_ssi_reset);
	device_class_set_props(dc, imx31_ssi_properties);
	dc->desc = "i.MX31 Synchronous Serial Interface";
}

static const TypeInfo imx31_ssi_info = {
	.name = TYPE_IMX31_SSI,
	.parent = TYPE_SYS_BUS_DEVICE,
	.instance_size = sizeof(IMX31SSIState),
	.class_init = imx31_ssi_class_init,
};

static void imx31_ssi_register_types(void)
{
	type_register_static(&imx31_ssi_info);
}

type_init(imx31_ssi_register_types)
