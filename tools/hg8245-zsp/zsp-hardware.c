// SPDX-License-Identifier: GPL-2.0-only
#define pr_fmt(fmt) "hg8245-zsp: " fmt

#include <crypto/sha2.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/ktime.h>
#include <linux/mfd/syscon.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/regmap.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <asm/unaligned.h>

#include "zsp-loader.h"
#include "zsp-hif.h"
#include "zsp-hardware.h"

static const u8 stock_sha256[SHA256_DIGEST_SIZE] = {
	0x82, 0xe9, 0x01, 0x29, 0xa0, 0x4c, 0x4f, 0x82,
	0x9a, 0x5e, 0x9d, 0x7b, 0x1d, 0x6d, 0xee, 0x33,
	0xc5, 0xee, 0xcc, 0x80, 0x24, 0xf8, 0x13, 0x35,
	0x59, 0xd6, 0x55, 0xe9, 0x64, 0xa9, 0xe8, 0x15,
};

int zsp_window_open(struct zsp_window *w)
{
	if (!w->size)
		return -EINVAL;
	if (region_intersects(w->base, w->size, IORESOURCE_SYSTEM_RAM,
			      IORES_DESC_NONE) != REGION_DISJOINT) {
		pr_err("window %08x+%x intersects Linux System RAM\n", w->base, w->size);
		return -EBUSY;
	}
	if (!request_mem_region(w->base, w->size, KBUILD_MODNAME))
		return -EBUSY;
	w->io = ioremap(w->base, w->size);
	if (!w->io) {
		release_mem_region(w->base, w->size);
		return -ENOMEM;
	}
	return 0;
}

void zsp_window_close(struct zsp_window *w)
{
	if (w->io) {
		iounmap(w->io);
		release_mem_region(w->base, w->size);
		w->io = NULL;
	}
}

static bool zsp_window_contains(const struct zsp_window *w, u32 addr, size_t len)
{
	u32 offset;

	if (addr < w->base)
		return false;
	offset = addr - w->base;
	return offset <= w->size && len <= w->size - offset;
}

int zsp_readback_write(void *ctx, enum zsp_target target, u32 addr,
			      const void *payload, size_t len)
{
	struct zsp_hardware *hw = ctx;
	const struct zsp_window *w;
	const u8 *src = payload;
	void __iomem *dest;
	size_t i;

	if (!len || !IS_ALIGNED(len, 4) || !IS_ALIGNED(addr, 4))
		return -EINVAL;
	switch (target) {
	case ZSP_TARGET_STAGE:
		if (addr < ZSP_STAGE_PHYS)
			return -EINVAL;
		w = &hw->shared;
		break;
	case ZSP_TARGET_LOCAL:
		w = addr < ZSP_DATA_PHYS ? &hw->inst : &hw->data;
		break;
	case ZSP_TARGET_MAILBOX:
		if ((addr != ZSP_MAILBOX_PHYS + 0x24 || len != 8) &&
		    (addr != ZSP_MAILBOX_PHYS + 0x4c || len != 4))
			return -EINVAL;
		w = &hw->data;
		break;
	case ZSP_TARGET_CURSOR:
		if (len != 4 || (addr != ZSP_HIF_DOWN_CURSOR && addr != ZSP_HIF_UP_CURSOR))
			return -EINVAL;
		w = &hw->shared;
		break;
	case ZSP_TARGET_BOOTINFO:
		if (addr != ZSP_BOOTINFO_PHYS || len != ZSP_HIF_BOOTINFO_LEN)
			return -EINVAL;
		w = &hw->shared;
		break;
	default:
		return -EINVAL;
	}
	if (!zsp_window_contains(w, addr, len))
		return -ERANGE;

	dest = w->io + (addr - w->base);
	for (i = 0; i < len; i += 4) {
		writel_relaxed(get_unaligned_le32(src + i), dest + i);
		if (!(i & (SZ_4K - 1)))
			cond_resched();
	}
	wmb();
	for (i = 0; i < len; i += 4) {
		u32 value = readl_relaxed(dest + i);
		u32 expected = get_unaligned_le32(src + i);

		if (value != expected) {
			pr_err("readback %08x: got %08x, expected %08x\n",
			       addr + (u32)i, value, expected);
			return -EIO;
		}
		if (!(i & (SZ_4K - 1)))
			cond_resched();
	}
	hw->verified_writes++;
	return 0;
}

int zsp_hold_for_staging(struct regmap *crg, bool check_only)
{
	u32 select, control, status, sram, cfg;
	int ret;

	ret = regmap_read(crg, ZSP_CRG_SELECT, &select);
	ret = ret ?: regmap_read(crg, ZSP_CRG_CONTROL, &control);
	ret = ret ?: regmap_read(crg, ZSP_CRG_CLK_STATUS, &status);
	if (ret)
		return ret;
	if (!(select & ZSP_SELECT_BIT) ||
	    (control & ZSP_CONTROL_MASK) != ZSP_CONTROL_UBOOT ||
	    !(status & ZSP_CLOCK_BIT)) {
		pr_warn("not the known RAM boot baseline: CRG18=%08x CRG1c=%08x CRG2c=%08x\n",
			select, control, status);
		return -EAGAIN;
	}
	if (check_only)
		return 0;

	/*
	 * Use the stock reset-state fields needed for memory staging. Only a
	 * separate boot request can enter the run sequence after readback.
	 */
	ret = regmap_clear_bits(crg, ZSP_CRG_CONTROL, ZSP_CONTROL_MASK);
	ret = ret ?: regmap_read(crg, ZSP_CRG_CONTROL, &control);
	if (ret || (control & ZSP_CONTROL_MASK))
		return ret ?: -EIO;
	ret = regmap_clear_bits(crg, ZSP_CRG_SELECT, ZSP_SELECT_BIT);
	ret = ret ?: regmap_write(crg, ZSP_CRG_CLK_SET, ZSP_CLOCK_BIT);
	if (ret)
		return ret;
	ret = regmap_read_poll_timeout(crg, ZSP_CRG_CLK_STATUS, status,
				       status & ZSP_CLOCK_BIT, 10, 1000);
	ret = ret ?: regmap_set_bits(crg, ZSP_CRG_SRAM, ZSP_SRAM_BIT);
	ret = ret ?: regmap_update_bits(crg, ZSP_CRG_SRAM_CFG,
				       ZSP_SRAM_CFG_MASK, ZSP_SRAM_CFG_OLD);
	ret = ret ?: regmap_read(crg, ZSP_CRG_SELECT, &select);
	ret = ret ?: regmap_read(crg, ZSP_CRG_SRAM, &sram);
	ret = ret ?: regmap_read(crg, ZSP_CRG_SRAM_CFG, &cfg);
	if (ret)
		return ret;
	if ((select & ZSP_SELECT_BIT) || !(sram & ZSP_SRAM_BIT) ||
	    (cfg & ZSP_SRAM_CFG_MASK) != ZSP_SRAM_CFG_OLD)
		return -EIO;
	return 0;
}

int zsp_map_irq_guard(struct zsp_hardware *hw)
{
	struct device_node *node;
	struct resource res;
	int ret;

	node = of_find_compatible_node(NULL, NULL, "hisilicon,sd5113-intc");
	if (!node)
		return -ENODEV;
	ret = of_address_to_resource(node, 0, &res);
	if (!ret &&
	    (res.start != ZSP_INTC_BASE ||
	     resource_size(&res) < ZSP_INTC_RAW_STATUS + 4))
		ret = -ENODEV;
	if (!ret) {
		hw->intc = of_iomap(node, 0);
		if (!hw->intc)
			ret = -ENOMEM;
	}
	of_node_put(node);
	return ret;
}

int zsp_boot_preflight(struct zsp_hardware *hw)
{
	u32 route, irq_ctrl;
	int ret;

	/* Poll the mailbox, without changing the irqchip's ownership or mask. */
	if (!(readl(hw->intc + ZSP_INTC_MASK) & ZSP_INTC_IRQ_BIT)) {
		pr_warn("IRQ20 already unmasked; aborting polling-only boot\n");
		return -EBUSY;
	}
	ret = regmap_read(hw->crg, ZSP_CRG_ROUTE, &route);
	ret = ret ?: regmap_read(hw->crg, ZSP_CRG_IRQ_CTRL, &irq_ctrl);
	if (ret)
		return ret;
	if ((route & GENMASK(4, 0)) || (irq_ctrl & ~GENMASK(4, 0))) {
		pr_warn("unexpected route/IRQ state: CRG40=%08x CRG50=%08x\n",
			route, irq_ctrl);
		return -EAGAIN;
	}
	return 0;
}

int zsp_enable_boot_irqs(struct zsp_hardware *hw)
{
	int ret;

	ret = regmap_write(hw->crg, ZSP_CRG_IRQ_CLEAR, 0x003f0000);
	return ret ?: regmap_write(hw->crg, ZSP_CRG_IRQ_CTRL, 0x1f);
}

int zsp_enter_run(struct zsp_hardware *hw)
{
	u32 control;
	int ret;

	wmb();
	hw->run_unverified = true;
	ret = regmap_update_bits(hw->crg, ZSP_CRG_CONTROL,
				 ZSP_CONTROL_MASK | ZSP_HW_ENABLE,
				 ZSP_CONTROL_RUN | ZSP_HW_ENABLE);
	ret = ret ?: regmap_read(hw->crg, ZSP_CRG_CONTROL, &control);
	if (!ret && (control & ZSP_CONTROL_MASK) != ZSP_CONTROL_RUN) {
		pr_err("RUN did not latch: CRG1c=%08x\n", control);
		return -EIO;
	}
	return ret;
}

void zsp_zero_boot_scratch(struct zsp_window *shared)
{
	u32 i;

	/* This whole 4 MiB window is held by request_mem_region, outside Linux RAM. */
	for (i = 0; i < shared->size; i += SZ_4K) {
		memset_io(shared->io + i, 0, SZ_4K);
		cond_resched();
	}
	wmb();
}

int zsp_wait_hif(struct zsp_hardware *hw, enum zsp_hif_wait what,
			 unsigned int sequence)
{
	void __iomem *cur = hw->shared.io + (ZSP_HIF_UP_CURSOR - hw->shared.base);
	ktime_t deadline = ktime_add_ms(ktime_get(), what == ZSP_WAIT_BOOT ?
					ZSP_BOOT_TIMEOUT_MS : ZSP_HIF_REPLY_TIMEOUT_MS);
	struct zsp_hif_version reply = { };
	unsigned int tdm_status = 0;
	unsigned int frames = 0;
	u32 cursors = 0;

	do {
		u8 frame[ZSP_HIF_SLOT_SIZE];
		unsigned int producer, consumer;
		u32 addr;
		int ret;

		cursors = readl(cur);
		producer = cursors & 0xffff;
		consumer = cursors >> 16;
		ret = zsp_hif_up_slot(producer, consumer, &addr);
		if (ret < 0) {
			pr_warn("invalid HIF cursors: producer=%u consumer=%u\n", producer, consumer);
			return ret;
		}
		if (!ret) {
			usleep_range(1000, 2000);
			continue;
		}
		if (!zsp_window_contains(&hw->shared, addr, sizeof(frame)))
			return -ERANGE;
		/* Producer publishes the cursor only after writing the slot. */
		rmb();
		memcpy_fromio(frame, hw->shared.io + (addr - hw->shared.base),
			      sizeof(frame));
		if (frames < 4)
			pr_info("HIF slot %u producer=%u: %*ph\n",
				consumer, producer, 16, frame);
		frames++;
		if (what == ZSP_WAIT_VERSION)
			ret = zsp_hif_version_reply(frame, sizeof(frame), sequence, &reply);
		else if (what == ZSP_WAIT_TDM)
			ret = zsp_hif_tdm_reply(frame, sizeof(frame), sequence, &tdm_status);
		else
			ret = zsp_hif_is_boot_complete(frame, sizeof(frame));
		if (ret < 0) {
			if (what == ZSP_WAIT_VERSION)
				pr_warn("ZSP version response error: ret=%d DSP status=%04x\n",
					ret, reply.status);
			else if (what == ZSP_WAIT_TDM)
				pr_warn("ZSP TDM response error: ret=%d DSP status=%04x\n",
					ret, tdm_status);
			return ret;
		}

		/* Publish consumer only; the producer belongs to ZSP. */
		mb();
		writew((consumer + 1) % ZSP_HIF_UP_SLOTS, cur + 2);
		if (ret) {
			if (what == ZSP_WAIT_VERSION)
				pr_info("ZSP_VERSION year=%04x date=%04x main=%04x sub=%04x\n",
					reply.year, reply.date, reply.main, reply.sub);
			else if (what == ZSP_WAIT_TDM)
				pr_info("ZSP_TDM_MODE_ACK mode=0 status=0\n");
			else
				pr_info("ZSP_BOOT_COMPLETE after %u HIF frame(s)\n", frames);
			return 0;
		}
		cond_resched();
	} while (ktime_before(ktime_get(), deadline));

	if (what != ZSP_WAIT_BOOT) {
		void __iomem *down = hw->shared.io + (ZSP_HIF_DOWN_CURSOR - hw->shared.base);

		pr_warn("%s: down=%08x up=%08x frames=%u\n",
			what == ZSP_WAIT_TDM ? "ZSP_TDM_TIMEOUT" : "ZSP_VERSION_TIMEOUT",
			readl(down), cursors, frames);
		return -ETIMEDOUT;
	}
	pr_warn("ZSP_BOOT_TIMEOUT after %u ms: cursors=%08x frames=%u\n",
		ZSP_BOOT_TIMEOUT_MS, cursors, frames);
	pr_info("timeout diagnostics: raw IRQ20=%u up-slot0 word0=%08x word1=%08x\n",
		!!(readl(hw->intc + ZSP_INTC_RAW_STATUS) & ZSP_INTC_IRQ_BIT),
		readl(hw->shared.io + (ZSP_HIF_UP_PHYS - hw->shared.base)),
		readl(hw->shared.io + (ZSP_HIF_UP_PHYS - hw->shared.base) + 4));
	return -ETIMEDOUT;
}

int zsp_send_hif(struct zsp_hardware *hw, const u8 *frame, size_t len)
{
	void __iomem *cur = hw->shared.io + (ZSP_HIF_DOWN_CURSOR - hw->shared.base);
	void __iomem *slot;
	u32 cursors, addr;
	unsigned int next, i;
	int ret;

	if (!len || len > ZSP_HIF_SLOT_SIZE || !IS_ALIGNED(len, 4))
		return -EINVAL;
	cursors = readl(cur);
	ret = zsp_hif_down_slot(cursors & 0xffff, cursors >> 16, &addr, &next);
	if (ret)
		return ret;
	if (!zsp_window_contains(&hw->shared, addr, len))
		return -ERANGE;

	slot = hw->shared.io + (addr - hw->shared.base);
	for (i = 0; i < len; i += 4)
		writel_relaxed(get_unaligned_le32(frame + i), slot + i);
	wmb();
	/* CS_MailboxSend publishes producer; do not change the DSP's cursor. */
	writew(next, cur);
	return 0;
}

int zsp_apply_stock_tdm(struct zsp_hardware *hw)
{
	u8 frame[ZSP_HIF_TDM_REQ_LEN];
	int ret;

	ret = zsp_hif_make_stock_tdm_request(ZSP_TDM_SEQUENCE, frame);
	ret = ret ?: zsp_send_hif(hw, frame, sizeof(frame));
	if (ret)
		return ret;
	pr_info("HIF stock TDM command 0x1b published: %*ph\n", (int)sizeof(frame), frame);
	return zsp_wait_hif(hw, ZSP_WAIT_TDM, ZSP_TDM_SEQUENCE);
}

int zsp_map_firmware(const struct firmware *fw, struct zsp_hardware *hw, bool with_highway)
{
	struct zsp_load_plan plan;
	u8 hash[SHA256_DIGEST_SIZE];
	unsigned int i;
	int ret;

	if (fw->size != ZSP_STOCK_BYTES)
		return -ENOEXEC;
	sha256(fw->data, fw->size, hash);
	if (memcmp(hash, stock_sha256, sizeof(hash)))
		return -EKEYREJECTED;
	ret = zsp_prepare(fw->data, fw->size, &plan);
	if (ret)
		return ret;

	hw->shared.base = ZSP_SHARED_PHYS;
	hw->shared.size = ZSP_SHARED_BYTES;
	hw->inst.base = ZSP_INST_PHYS;
	hw->data.base = ZSP_DATA_PHYS;
	hw->data.size = ZSP_MAILBOX_PHYS + ZSP_MAILBOX_LEN - hw->data.base;
	hw->hw.base = ZSP_HW_BASE;
	hw->hw.size = ZSP_HW_BYTES;
	for (i = 0; i < ZSP_RESIDENT_COUNT; ++i) {
		const struct zsp_resident *section = &plan.resident[i];
		struct zsp_window *w = section->dest < ZSP_DATA_PHYS ?
				       &hw->inst : &hw->data;

		w->size = max(w->size, section->dest - w->base + section->bytes);
	}

	ret = zsp_window_open(&hw->shared);
	ret = ret ?: zsp_window_open(&hw->inst);
	ret = ret ?: zsp_window_open(&hw->data);
	if (!ret && with_highway) {
		ret = zsp_window_open(&hw->hw);
		ret = ret ?: zsp_map_irq_guard(hw);
		ret = ret ?: zsp_boot_preflight(hw);
	}
	return ret;
}

void zsp_unmap_firmware(struct zsp_hardware *hw)
{
	if (hw->intc)
		iounmap(hw->intc);
	zsp_window_close(&hw->hw);
	zsp_window_close(&hw->data);
	zsp_window_close(&hw->inst);
	zsp_window_close(&hw->shared);
	hw->intc = NULL;
}
