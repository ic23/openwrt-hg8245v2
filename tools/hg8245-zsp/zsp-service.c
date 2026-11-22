// SPDX-License-Identifier: GPL-2.0-only
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <asm/unaligned.h>
#include "zsp-service.h"

static const u16 timer_regs[] = { 0x120, 0x11c, 0x118, 0x124, 0x114 };

static int update_checked(struct regmap *crg, u32 reg, u32 mask, u32 value)
{
	u32 readback;
	int err = regmap_update_bits(crg, reg, mask, value);

	err = err ?: regmap_read(crg, reg, &readback);
	return err ?: (((readback ^ value) & mask) ? -EIO : 0);
}

int zsp_service_stop(struct zsp_service *voice)
{
	struct zsp_hardware *hw = &voice->hw;
	int err = 0, i;

	if (voice->changed) {
		err = update_checked(hw->crg, ZSP_CRG_CONTROL, ZSP_CONTROL_MASK, ZSP_CONTROL_STOP);
		if (err)
			return err; /* Retain the mappings until reset can be confirmed. */
		hw->run_unverified = false;
		if (voice->started_hw) {
			for (i = 0; i < ARRAY_SIZE(timer_regs); i++)
				writel(voice->timers[i], hw->hw.io + timer_regs[i]);
			writel(voice->init, hw->hw.io + ZSP_HW_INIT);
			err = regmap_write(hw->crg, ZSP_CRG_IRQ_CTRL, voice->irq);
		}
	}
	if (voice->pcm.armed)
		err = hg8245_pcm_finish(&voice->pcm, true) ?: err;
	if (voice->changed) {
		err = update_checked(hw->crg, ZSP_CRG_SRAM_CFG, ZSP_SRAM_CFG_MASK,
				     voice->config) ?: err;
		err = update_checked(hw->crg, ZSP_CRG_SRAM, ZSP_SRAM_BIT, voice->sram) ?: err;
		err = update_checked(hw->crg, ZSP_CRG_SELECT, ZSP_SELECT_BIT, voice->select) ?: err;
		if (!err)
			err = update_checked(hw->crg, ZSP_CRG_CONTROL, ZSP_CONTROL_MASK | ZSP_HW_ENABLE,
					     voice->control);
		if (err)
			return err;
	}
	voice->changed = false;
	voice->started_hw = false;
	zsp_unmap_firmware(hw);
	return 0;
}

int zsp_service_start(struct zsp_service *voice, struct device *dev)
{
	struct zsp_hardware *hw = &voice->hw;
	const struct firmware *fw;
	const char *phase = "firmware request";
	u32 control;
	unsigned int i;
	int err, cleanup;

	if (hw->shared.io)
		return -EBUSY;
	err = request_firmware_direct(&fw, ZSP_FIRMWARE, dev);
	if (err) {
		dev_err(dev, "%s: %d\n", phase, err);
		return err;
	}
	hw->verified_writes = 0;
	phase = "DSP windows and reset baseline";
	err = zsp_map_firmware(fw, hw, true);
	err = err ?: zsp_hold_for_staging(hw->crg, true);
	err = err ?: regmap_read(hw->crg, ZSP_CRG_CONTROL, &voice->control);
	err = err ?: regmap_read(hw->crg, ZSP_CRG_SELECT, &voice->select);
	err = err ?: regmap_read(hw->crg, ZSP_CRG_SRAM, &voice->sram);
	err = err ?: regmap_read(hw->crg, ZSP_CRG_SRAM_CFG, &voice->config);
	err = err ?: regmap_read(hw->crg, ZSP_CRG_IRQ_CTRL, &voice->irq);
	if (err)
		goto out;
	phase = "HIGHWAY setup";
	err = hg8245_pcm_prepare(&voice->pcm, hw->crg, hw->hw.io, true);
	if (err)
		goto out;
	voice->changed = true;
	phase = "DSP staging reset and clock";
	err = zsp_hold_for_staging(hw->crg, false);
	if (err)
		goto out;
	phase = "DSP firmware staging";
	zsp_zero_boot_scratch(&hw->shared);
	err = zsp_hif_prepare_cursors(zsp_readback_write, hw);
	err = err ?: zsp_hif_prepare_bootinfo(zsp_readback_write, hw);
	err = err ?: zsp_stage(fw->data, fw->size, zsp_readback_write, hw);
	if (err)
		goto out;

	phase = "pre-release IRQ and reset check";
	err = zsp_boot_preflight(hw);
	err = err ?: regmap_read(hw->crg, ZSP_CRG_CONTROL, &control);
	if (!err && (control & ZSP_CONTROL_MASK)) {
		dev_err(dev, "unexpected staging reset: CRG1c=%08x\n", control);
		err = -EIO;
	}
	if (err)
		goto out;
	voice->init = readl(hw->hw.io + ZSP_HW_INIT);
	for (i = 0; i < ARRAY_SIZE(timer_regs); i++) {
		voice->timers[i] = readl(hw->hw.io + timer_regs[i]);
		writel(0, hw->hw.io + timer_regs[i]);
	}
	voice->started_hw = true;
	writel(voice->init | ZSP_HW_INIT_BITS, hw->hw.io + ZSP_HW_INIT);
	phase = "DSP IRQ enable";
	err = zsp_enable_boot_irqs(hw);
	if (err)
		goto out;
	phase = "DSP reset release";
	err = zsp_enter_run(hw);
	if (err)
		goto out;
	phase = "DSP boot acknowledgement";
	err = zsp_wait_hif(hw, ZSP_WAIT_BOOT, 0);
	if (err)
		goto out;
	phase = "DSP TDM acknowledgement";
	err = zsp_apply_stock_tdm(hw);
out:
	release_firmware(fw);
	if (err) {
		dev_err(dev, "%s failed: %d (%u verified writes)\n",
			phase, err, hw->verified_writes);
		cleanup = zsp_service_stop(voice);
		if (cleanup)
			dev_err(dev, "DSP rollback failed: %d\n", cleanup);
	}
	return err;
}

/* A service owns both HIF cursors under the SLIC mutex. */
static int receive_frame(struct zsp_service *voice, u8 frame[ZSP_HIF_SLOT_SIZE])
{
	struct zsp_window *shared = &voice->hw.shared;
	void __iomem *cursor = shared->io + (ZSP_HIF_UP_CURSOR - shared->base);
	u32 addr, cursors = readl(cursor);
	unsigned int consumer = cursors >> 16;
	int ret = zsp_hif_up_slot(cursors & 0xffff, consumer, &addr);

	if (ret <= 0)
		return ret;
	if (addr < shared->base || addr - shared->base > shared->size - ZSP_HIF_SLOT_SIZE)
		return -ERANGE;
	rmb();
	memcpy_fromio(frame, shared->io + (addr - shared->base), ZSP_HIF_SLOT_SIZE);
	mb();
	writew((consumer + 1) % ZSP_HIF_UP_SLOTS, cursor + 2);
	ret = zsp_hif_is_boot_complete(frame, ZSP_HIF_SLOT_SIZE);
	return ret < 0 ? ret : ret ? -EPIPE : 1;
}

int zsp_service_poll(struct zsp_service *voice)
{
	unsigned int i;

	for (i = 0; i < ZSP_HIF_UP_SLOTS; i++) {
		u8 frame[ZSP_HIF_SLOT_SIZE];
		int ret = receive_frame(voice, frame);

		if (ret <= 0)
			return ret;
	}
	return 0;
}

static int media_command(struct zsp_service *voice, u8 channel, u16 seq,
			 const u8 *command, size_t len)
{
	u8 tx[64] = { channel, 0x7f };
	u8 rx[ZSP_HIF_SLOT_SIZE];
	ktime_t until;
	int ret;

	if (len < 4 || ALIGN(4 + len, 4) > sizeof(tx))
		return -EINVAL;
	put_unaligned_le16(seq, tx + 2);
	memcpy(tx + 4, command, len);
	ret = zsp_send_hif(&voice->hw, tx, ALIGN(4 + len, 4));
	if (ret)
		return ret;
	until = ktime_add_ms(ktime_get(), ZSP_HIF_REPLY_TIMEOUT_MS);
	do {
		bool matched = false;
		unsigned int pos;

		ret = receive_frame(voice, rx);
		if (ret < 0)
			return ret;
		if (!ret) {
			usleep_range(1000, 1500);
			continue;
		}
		if (rx[0] != channel || get_unaligned_le16(rx + 2) != seq)
			continue;
		/* receive_frame already validated length, termination and class. */
		for (pos = 4; pos <= sizeof(rx) - 4; pos += rx[pos]) {
			const u8 *tlv = rx + pos;

			if (tlv[3] == 2 || tlv[3] == 0x12) {
				if (tlv[0] < 6)
					return -EPROTO;
				if (get_unaligned_le16(tlv + 4)) {
					pr_err("le88266: DSP channel %u seq %u request %02x reply %02x status %04x\n",
					       channel, seq, command[2], tlv[2],
					       get_unaligned_le16(tlv + 4));
					return -EREMOTEIO;
				}
				matched |= tlv[2] == command[2];
			}
			if (tlv[1])
				break;
		}
		if (matched)
			return 0;
	} while (ktime_before(ktime_get(), until));
	pr_err("le88266: DSP channel %u tag %02x timed out\n", channel, command[2]);
	return -ETIMEDOUT;
}

int zsp_service_channels(struct zsp_service *voice)
{
	/* Wire TLVs emitted by the HG8245V2 SDK for its default narrowband
	 * channel. Its class 0x10 RTP configuration runs on ARM, outside HIF.
	 */
	static const u8 open[] = {
		8, 0, 0, 0, 0, 0, 0, 0,
		6, 0, 2, 0, 0x34, 0x10,
		10, 0, 0x0f, 0, 2, 0, 0x1c, 0, 0x1c, 0,
		22, 1, 0x16, 0, 1, 0, 3, 0, 0, 0, 1, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0
	};
	u8 slot[] = { 8, 1, 0x1f, 0, 0, 0, 0, 0 };
	unsigned int ch, seq = ZSP_TDM_SEQUENCE + 1;
	int ret;

	void __iomem *stack = voice->hw.shared.io + (ZSP_BOOTINFO_PHYS - voice->hw.shared.base);
	unsigned int i;

	memset(voice->audio, 0, sizeof(voice->audio));
	voice->media_clock = 0;
	voice->media_epoch = 0;
	for (ch = 0; ch < 2; ch++) {
		voice->next_play[ch] = 0;
		for (i = 0; i < 32; i++)
			writew(0xffff, stack + 0x5db24 + ch * 256 + i * 8 + 2);
		writel(U32_MAX, stack + 0x65b24 + ch * 8);
	}
	writew(32, stack + 0xf024 + 64 + 8);
	for (ch = 0; ch < 2; ch++) {
		slot[4] = ch;
		/* Open already supplies gain in TLV 0x0f. Basic PCMA has no
		 * requested VQE (0x1d) or optional filter (0x2c) update.
		 */
		ret = media_command(voice, ch, seq++, open, sizeof(open));
		ret = ret ?: media_command(voice, ch, seq++, slot, sizeof(slot));
		if (ret)
			return ret;
	}
	pr_info("le88266: DSP channels 0/1 and PCM slots ready\n");
	return 0;
}

/* Drop speech collected before the second handset answered. */
int zsp_service_flush_capture(struct zsp_service *voice)
{
	void __iomem *base = voice->hw.shared.io + (ZSP_BOOTINFO_PHYS - voice->hw.shared.base);
	unsigned int ch;

	for (ch = 0; ch < 2; ch++) {
		void __iomem *ring = base + 0x24 + ch * 0x880;
		u16 producer = readw(ring + 6);

		if (producer >= 13)
			return -EPROTO;
		writew(producer, ring + 8);
	}
	return 0;
}

/* SD5112ADP_VOIP_AddrCfg, PSTK_PackChannelRtp and JB_FillRtpData.
 * Both directions transfer one 10 ms G.711 A-law block per operation.
 * Each downstream play-list entry owns a separate packet buffer until the
 * DSP releases both the entry and its descriptor.
 */
int zsp_service_audio(struct zsp_service *voice, unsigned int ch, bool playback, u8 frame[80])
{
	void __iomem *base = voice->hw.shared.io + (ZSP_BOOTINFO_PHYS - voice->hw.shared.base);
	void __iomem *ring, *slot, *block, *desc;
	u32 now, idx, offset, state;
	u64 tick, target;

	if (ch > 1)
		return -EINVAL;
	if (!playback) {
		void __iomem *fifo = base + 0xf024 + 64;
		u16 producer, consumer;

		/* Up-channel notifications are hints; the per-channel ring holds the audio. */
		producer = readw(fifo + 12);
		if (producer < 32)
			writew(producer, fifo + 14);
		ring = base + 0x24 + ch * 0x880;
		producer = readw(ring + 6);
		consumer = readw(ring + 8);
		if (producer >= 13 || consumer >= 13)
			return -EPROTO;
		if (producer == consumer)
			return -EAGAIN;
		rmb();
		slot = ring + 0x200 + consumer * 128;
		state = readb(slot + 0x75) == 80 && readb(slot + 0x77) == 0;
		if (state) {
			unsigned int i;
			bool varying = false;

			memcpy_fromio(frame, slot, 80);
			for (i = 0; i < 80; i++) {
				/* A-law positive and negative minimum magnitude. */
				if ((frame[i] & 0x7f) != 0x55)
					voice->audio[ch].mic_nonidle++;
				if (frame[i] != frame[0])
					varying = true;
			}
			voice->audio[ch].mic_varying += varying;
			voice->audio[ch].mic_frames++;
		}
		mb();
		writew((consumer + 1) % 13, ring + 8);
		return state ? 0 : -ENODATA;
	}

	now = readl(voice->hw.hw.io + 0x118);
	if (now < voice->media_clock)
		voice->media_epoch += 1ULL << 32;
	voice->media_clock = now;
	tick = div_u64(voice->media_epoch + now, 80);
	target = max(voice->next_play[ch], tick + 3);
	if (target > tick + 24)
		return -EAGAIN;
	idx = target & 31;
	slot = base + 0x5db24 + ch * 256 + idx * 8;
	offset = 0x141a4 + (ch * 32 + idx) * 0x800;
	block = base + offset;
	desc = block + 0x680;
	if (readw(slot)) {
		voice->audio[ch].playback_busy++;
		return -EAGAIN;
	}
	state = readw(desc + 6);
	if (state != 0 && state != 3)
		return -EAGAIN;
	memcpy_toio(block + 4, frame, 80);
	writel(ZSP_BOOTINFO_PHYS + offset + 4, desc);
	writew(80, desc + 4);
	writew(1, desc + 6);
	writeb(1, desc + 8);
	writeb(0, desc + 10);
	writeb(8, desc + 11);
	writel((u32)(target * 80), desc + 12);
	writel(now, desc + 20);
	writel((u32)target, desc + 24);
	writew(0, desc + 28);
	writew(idx, desc + 30);
	writel(ZSP_BOOTINFO_PHYS + offset + 0x680, slot + 4);
	writew(0, slot + 2);
	wmb();
	writew(1, slot);
	voice->audio[ch].playback_queued++;
	voice->next_play[ch] = target + 1;
	return 0;
}

MODULE_FIRMWARE(ZSP_FIRMWARE);
